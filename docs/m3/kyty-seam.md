# NPS5_GPU=kyty: KytyPS5's GPU stack inside the ps5recomp runtime (host seam)

Branch `kyty-seam` (ps5recomp, on `astro-boot2` ea42026) and `nps5-kyty-seam` (AnyPS5, on `nps5-astro-int2` 7337657c75).
Spike, 2026-11-16. Off by default. Credit: the idea and the interface it grew from are AstroHybrid's (kyty_gpu.dll, `kytyGpuApi.h`
ABI 1.0-1.5, AnyPS5 patches 0021-0025 of the pcportastrobot project); KytyPS5 is by Jetsku. Both GPL-2.0.

Status: the host side is complete and tested against a stub library. No real `kyty_gpu.dylib` was loaded and no title was run.

## What the switch does

`NPS5_GPU=kyty` (exactly that value; anything else, or unset, is the AnyPS5 renderer as before) makes `nps5_anyps5_startup`:

1. `dlopen` the library: `NPS5_KYTY_GPU_LIB=<path>`, else `@rpath/kyty_gpu.dylib`, `kyty_gpu.dylib`, `libkyty_gpu.dylib` (DYLD_LIBRARY_PATH).
2. `KytyGpuGetApi(1.6)`, check ABI 1.6 and sizes, `init(host callbacks)`.
3. Rebind the 84 graphics exports (66 libSceAgc, 7 libSceAgcDriver, 11 libSceVideoOut: AstroHybrid's export table, `pkg/thunkgen/kyty_nids.tsv`) to the
   library: `nid_entry(nid)`, else `dlsym(lib, nid)`. Everything else (libkernel, pad, user service, libc, the ~1320 other AnyPS5 exports) stays AnyPS5.
4. Become the one owner of the GPU-visible guest ranges: VM hooks -> `register_memory` / `unregister_memory` / `protect_memory`; the runtime's access
   resolver and host access hooks go to the seam's page watch table. AnyPS5's allocation registry and write watch are NOT installed
   (`nps5_anyps5_setup_watch` / `install_hooks` are skipped), and its driver never starts because none of its graphics exports is called.
5. `start_graphics()` (unless `NPS5_KYTY_LAZY_START` is set), on the main thread, before the first guest thread runs.

A failure in any step with the switch on is fatal with a message; there is no silent fallback to another renderer.

Other knobs: `NPS5_KYTY_TRACE=1` (register/unregister/protect lines), `NPS5_KYTY_STATS=1` (watch counters at exit), `NPS5_KYTY_ALIAS=0` (no alias
views), `NPS5_KYTY_NO_SHUTDOWN=1` (skip `shutdown()` at exit).

### Cost when off

The routable thunks used to `bl _sceAgcX` (a dylib stub: load the address, branch). They now load `aps5_rt_sceAgcX` (a pointer statically
initialised with `sceAgcX`, bound by dyld at load) and `blr`. Same work, one branch fewer. The VideoOut thunks already called through a pointer.
Start-up reads one environment variable. Nothing is loaded, hooked or allocated (`TestKytySeam` "default" checks all of it, also that every one
of the 84 pointers still equals its AnyPS5 function and that marshalling is unchanged). The generated diff is the 73 call lines and the route table.

## Decision 1: addresses (measured)

Kyty assumes guest address == host address. That cannot be had here:

| guest range | what | host at that address |
|---|---|---|
| 0x400000 +251 MiB | eboot image | occupied: the 4 GiB `__PAGEZERO` is mandatory on arm64 |
| 0x2_0000_0000 | first unhinted mappings (`dyn_lo`) | occupied: dyld shared cache |
| 0x3_0000_0000 +2 GiB | Astro fixed mapping | free |
| 0x10_0000_0000 +3.2 GB | Astro's FIXED direct-memory block (its GPU memory) | occupied: GPU carve-out 0x10..-0x70.. |

(`mach_vm_map` FIXED without overwrite; `memory_manifest.json` of the title is base mode with carve-out constants, so `-addr-mode identity` is refused.)

So: **host = guest + bias, bias 2^40, one contiguous view of the whole guest space; Kyty translates.** Rule for the Kyty side: every address
Kyty keys by, hashes, compares or writes back into guest data (PM4 words, T#/V#/RT bases, labels, tracker pages) stays a GUEST address; the bias is
added only at the dereference (`Hosted::Ptr<T>(guest)` in `src/hybrid/kytyGpuHosted.h`, Kyty branch `nps5-kyty-kseam`). It is a constant add, never a
lookup. The SRT walker takes it through its `read_memory` callback.

Exports receive what the game passed, untouched: `KytyGpuApi.pointer_mode = KYTY_GPU_POINTERS_GUEST` (0, the default). The thunk still marshals
scalars and floats (also the guest's stack arguments) with the AnyPS5 prototype; the generated adapter re-reads pointer parameters from the guest
registers and gives a pointer result the bias the thunk takes off again. `KYTY_GPU_POINTERS_HOST` is also implemented (the thunk's own
translation: pointer parameters are host pointers, null and out-of-window become null) in case that is less work for a first frame; addresses
inside guest data are guest values in both modes.

ABI note (arm64 only): the library's entry is called with the C prototype AnyPS5 declares for the export. Apple's arm64 ABI packs narrow
stack arguments, so for the six exports with more than eight integer arguments (AcbDmaData, DcbDmaData, AcbCopyData, DcbCopyData, DcbWaitRegMem,
CbReleaseMem) Kyty's parameter types must match; they do today (compared with `src/libs/agc.cpp`), and the test covers a 12-argument call.
Kyty's `KYTY_SYSV_ABI` is ignored on arm64, which is what is wanted: no naked x86 thunks, `nid_entry` returns the HLE function itself.

Guest callbacks: a library that branches to a guest function pointer on a guest thread inside an export faults into the runtime's host-call
bridge as with AnyPS5 (`nps5_hostcall`); `call_guest(function, args6)` does the same explicitly.

## Decision 2: guest memory and page watches on 16 KiB pages

The runtime's VM layer keeps owning guest memory (AstroHybrid moved direct/flexible memory into Kyty's kernel; here the recompiled code, the
runtime's HLE and AnyPS5's libkernel all live on the runtime's mappings). The library is told about the GPU-visible ranges (direct, flexible and
mmap memory at or above 0x2_0000_0000, not stacks, not the AGC driver's own ranges) and gets, per range, an **alias**: a second mapping of the same
pages (`mach_vm_remap`, always read-write, never watched). It reads and writes guest memory behind its own watches through the alias (Kyty's
"backing" calls: `TryReadBacking`, `WriteBacking`, ...).

Kyty's PageManager tracks 4 KiB pages and asserts a 4 KiB host page (`pageManager.cpp`). Plan, implemented on the host side:

- Kyty keeps its 4 KiB tracker pages and its counters. It never calls mprotect: `Memory::ProtectGuestHostMemory` becomes the host callback
  `protect_guest(guest, size, watch)` with watch = none / write / access.
- `nps5_kyty_watch.c` stores 2 bits per tracker page (one byte per 16 KiB host page, a lazily committed array: 64 KiB per GiB watched) and the
  guest's own protection per host page, and sets each host page to the strictest of its four tracker pages and the guest's protection.
- A fault on a host page is offered to the library (`handle_fault(guest page, is_write)`) for every armed tracker page of that host page that traps
  the access, the faulting one first. So the access can always be retried. The price of the 16 KiB page: up to three neighbours are reported
  written (or read back) although they were not. `NPS5_KYTY_STATS=1` prints how often (`collateral`).
- The guest's own `mprotect` goes through the same table (`Nps5HostAccessHooks.protect`), system-call buffers through `write_begin` /
  `read_prepare` (they open what would give EFAULT). A fault that raced with a disarm whose `mprotect` was still in flight is retried, not reported.
- `unregister_memory` is called before the pages go away; the library disarms its watches there; whatever is left is dropped.

One owner per range: with the switch on AnyPS5's tracker is not merely off for the ranges Kyty owns, it is not installed.

Measured (`TestKytyWatch`, M4 Pro): nothing armed: 2 ns per prepared buffer; arm+disarm with a host change 0.5-0.8 us; a tracker page whose host
page stays armed by a neighbour 6 ns; arm + store + fault + disarm 4-5 us.

Not handled: two guest ranges backed by the same direct memory (a watch on one does not see stores through the other; logged once per pair).
An alias is made when a range is registered and remade when it is cut or re-registered; if the runtime replaces pages inside a registered
range without a map/protect/unmap hook, the alias would keep the old pages (not seen in the VM layer's paths, not proven absent). A range with
a hole cannot be aliased: the library then gets a null alias and must use the guest view.
`query_memory` takes the lock the memory hooks hold while they call into the library: do not call it from work `unregister_memory` waits for.

### Fault cost in a title, and false sharing on the 16 KiB page (after M4)

First title numbers (ASTRO BOT world map, run ks_f1, `NPS5_KYTY_STATS=10`): about 390 faults per second. Stores to write-watched pages cost
8-12 us inside the library (the faulting page or a neighbour): not a problem. All of the time was in two other places:

1. **Loads from a GPU-owned (access-watched) page**: about 25 per second on ONE page (guest 0x555f41000, a value the game's render threads read
   every frame), each waiting 29-39 ms for the GPU: 700 ms per second inside `handle_fault`. Kyty has a switch for exactly this,
   `KYTY_READ_FAULT_PUBLISHED=1`: the load is completed in the handler from the page's published copy. It needs a decoder of the faulting guest
   instruction (x86 on Windows). ABI 1.7 gives the library the host's: `fault_load(&address, &size)` / `fault_load_apply(bytes, size)`, valid
   inside `handle_fault` of the faulting page of a load; the page stays armed and the thread continues behind the instruction.
2. **False sharing**: a CPU access to an unwatched tracker page that shares its host page with a GPU-owned one. The first seam reported the
   GPU-owned neighbour as accessed, which made Kyty drain the GPU (3-150 ms) and hand the page back to the CPU, every frame. Now
   (`NPS5_KYTY_EMULATE=1`, default) such an access is carried out by `nps5_kyty_emu.c` on the alias: the instruction that trapped is decoded
   (LDR/STR families incl. pairs, SIMD and `DC ZVA`), executed against the alias bytes, and the thread continues behind it; the library is not
   called and its page stays armed. About 3 us per access (one signal). Exclusives and LSE atomics are not emulated: those fall back
   to the report. The Advanced SIMD structure forms are (branch `kyty-fault`): LD1-4/ST1-4 of multiple structures, of one lane, LD1R-4R,
   with and without post-index (the recompiler lowers x86 broadcast loads to `LD1R`: `0x4d40ca00` = `ld1r {v0.4s}, [x16]`). Write-watched neighbours of a store are still reported (cheap, and it opens the host page for good); `NPS5_KYTY_EMULATE=2`
   emulates those too, `0` is the first seam.

3. **Accesses that straddle two tracker pages** (branch `kyty-fault`; hoover_beach: `ldr d31, [x28, x16]` of 8 bytes at `...ffc`, a GPU
   simulation's output the game's tbb / FluidCalculationThread workers sample). When the page that faulted is unwatched but the access reaches
   into an armed one, the emulator must decline (those bytes are the GPU's). The armed page the access touches is then offered to the library
   as the faulting page, with `fault_load` valid, instead of reporting every watched neighbour of the host page (a GPU drain, 5-6 ms each):
   the library serves the load (Kyty: `KYTY_READ_FAULT_PUBLISHED`, loads across two pages) or opens that one page, and the retry is emulated.
   Counted as "straddling into a watched page" in the stats line.

The faulting page is now reported with the faulting byte (not its page base), so Kyty's own 4 KiB false-sharing logic sees the real address.
Cost of what remains is visible per interval: `NPS5_KYTY_STATS=<seconds>`.

## Decision 3: window, input, exit

- **Window/present: Kyty's own SDL3 window.** Cheapest that can work: AnyPS5's presentation lives in its VideoOut driver and Vulkan device, which do
  not exist with the switch on. macOS wants the window and the event pump on the main thread; the runtime's main thread serves the main dispatch
  queue, so `run_on_main_thread(fn, arg, wait)` puts Kyty's `WindowRun` there (wait = 0; SDL's pump keeps serving the queue).
  Risk to check at M2: AnyPS5's pad library links SDL2 (sdl2-compat over SDL3 from Homebrew); a second SDL3 inside kyty_gpu.dylib means two event
  pumps. With the scripted pad AnyPS5's SDL window code is never started.
- **Input:** `NPS5_VPAD_SCRIPT` normally plays inside AnyPS5's window loop, which is gone. `Nps5KytyHostStartPadScript` (AnyPS5 `KytyHost.cpp`) plays
  the same script from a thread straight into `PadPublishInputForPort` (ms events on its own clock, `f` events on delivered flips, taps held at
  least 100 ms); the host then sets `KYTY_GPU_HOST_SCRIPTED_PAD`. Without a script the library's `publish_pad` relay (AstroHybrid 0022) works as on
  Windows. Trigger effects / motion (0024/0025) are not ported: `fetch_pad_output` and `publish_pad_motion` are null.
- **Exit (0023):** `request_exit(code)` -> `LibcRequestExit`. `shutdown()` runs from `atexit`.
- **Events:** `add_gpu_event` / `delete_gpu_event` / `deliver_event` are a port of libAstroHybridHost onto the game's AnyPS5 event queues.

## Kyty side: what kbuild has to do

Kyty branch `nps5-kyty-kseam` (work/kyty/wt-kseam, 762e357b, local only) has the header, `Hosted::` and the pageManager page-size relaxation.
Still to do there (none of it compiled by this work):

1. Bridge `init`: call `Hosted::Init(&g_host)`; skip `donate_guest_range` / `AdoptHostPlaceholders` and `InstallFirstAccessHandler` when hosted.
2. `g_api`: `pointer_mode`, `nid_entry` (look the NID up in the LIB_FUNC tables and return the function; the naked thunks, the trap resolver and the
   `.def` are x86/Windows only), `handle_fault` -> `Memory::HandleGpuFault(is_write ? Write : Read, guest)`, `protect_memory`.
3. `Memory::ProtectGuestHostMemory` -> `Hosted::Watch`; `register_memory` / `unregister_memory` -> Kyty's GPU-range table (`MapGpuRange`-style
   callbacks) with the alias as backing; the direct/flexible/munmap/mprotect kernel exports are not needed (not routed).
4. Every dereference of a guest address: `Hosted::Ptr`. Places known so far: SrtWalker (`read_memory`), command-buffer parsing, T#/V#/index/label
   reads and writes, DMA/copy/write-data, shader headers.
5. Presentation: `WindowRun` through `run_on_main_thread`; pad relay optional (see above).

`tests/kyty_seam/kyty_stub.c` is a working minimal library side (records every call) and can be loaded into a real title run to see the calls arrive.

## How to load

```
# 1. AnyPS5 libs with the host helpers (branch nps5-kyty-seam): wt/anyps5-kseam/build-arm64
# 2. binary: the title's objects relinked with this runtime (a few minutes, no translation). DONE: boot/native_kyty
#    (same 11 gnm stubs and 3 skipped files as native_ab2; the worktree needs 3rdparty/minimp3 and 3rdparty/libatrac9 sources)
wt/kseam-rt/tools/kyty_relink.sh work/ppsa21564/boot/out work/ppsa21564/boot/out_kyty work/ppsa21564/boot/native_kyty
# 3. run (kbuild): canonical launcher, three overrides
ASTRO_LIBS=$NPS/wt/anyps5-kseam/build-arm64 BIN=$B/native_kyty \
  work/ppsa21564/run_astro.sh <tag> 90 "NPS5_GPU=kyty NPS5_KYTY_GPU_LIB=<abs path>/kyty_gpu.dylib NPS5_KYTY_TRACE=1 NPS5_KYTY_STATS=1"
```

The same binary and libs without `NPS5_GPU` are the canonical AnyPS5 path.

## Tests

`go test ./pkg/runtime -run TestKyty` (arm64 macOS, clang):

- `TestKytyWatch` (`tests/kyty_seam/watch_test.c`): 11 groups over real pages and real faults: neighbour write trap, access + write watch in one
  host page, composition with the guest's protection, ranges across host pages and lock spans (one host call per run), system-call buffers (a
  real `read()` EFAULT, then opened), forget, an owner that will not disarm, the raced disarm, a 6000-step fuzz against a model (kernel
  protection of 64 pages checked after every step), 4 writer threads against an armer (write-watch invariant, 0 violations), micro-benchmarks.
- `TestKytySeam` (`route_test.c` + `kyty_stub.c` + generated fakes of the AnyPS5 libraries): off in three spellings; on with guest and host
  pointers: all 84 exports reach the library and none reaches AnyPS5, a non-routed export of the same libraries still reaches AnyPS5, 12-argument,
  float and pointer-result calls, memory hand-off with aliases, watches through real faults, callbacks; a missing NID aborts only when called and
  names the export; an ABI 1.5 library and an unloadable one are refused.
- `TestKytySeamVM` (`vm_seam_test.c`): the seam on the real VM layer (nps5_vm.c, nps5_mem.c, ps4_direct_mem.c) with ASTRO BOT's shape (2 GiB at
  guest 0x300000000, 3.2 GB direct at guest 0x1000000000, bias 2^40): guest memset over registered read-write memory before, under and after
  watches (256 tracker pages = 64 host faults per armed MiB), across guest mprotect, unmap and fixed re-map, GPU-only protections; a store to an
  address nothing maps is declined at once (title crash, SIGBUS on macOS since the reservation is PROT_NONE, crash report class "unmapped
  guest address"), not a watch fault.
- `TestKytyEmu` (`emu_test.c`): the access emulator against the CPU, 130 instruction snippets (registers, writeback, memory identical), 5
  that must be declined, plus the whole Advanced SIMD load/store structure class enumerated from generated code (8192 encodings: the 1890
  defined ones must match the CPU in registers v0-v3, writeback and memory, the 6302 undefined ones must be rejected by the decoder).
  `TestKytyWatch` 10c: loads and stores that straddle an unwatched and a watched tracker page (same host page, across host pages, served /
  opened by the owner), LD1R next to and on an access-watched page. `TestKytyWatch` 10b: four threads on the neighbours of an access-watched page, 160,000 emulated accesses, exact
  totals, owner never called; an LDXR/STXR increment falls back.
- `TestKytyRouteTable`: exactly the 84 NIDs, 73 linked + 11 dlsym, generated file in sync.

AnyPS5 side: `core/libs/nps5/tests/kyty_host_smoke.c` (manual build, see its header): end-of-pipe and flip events on real libkernel queues
(counting, udata, delete), the scripted pad start. cap4 replay md5 of the `nps5-kyty-seam` build: 952535c0... / b6dbbcde... (unchanged).

In the relinked ASTRO BOT binary the routed call site went from `bl island -> stub -> GOT` to `adrp/ldr/blr`, and dyld binds the pointer to
`libSceAgc.prx/_sceAgcDcbDrawIndexAuto` (`dyld_info -fixups`). Not verified by a title run (runs belong to kbuild): that the default path of
`native_kyty` behaves like `native_ab2`.
