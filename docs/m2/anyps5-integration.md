# AnyPS5 graphics, video and input in the nativeps5 runtime (M2)

Branch `m2-anyps5int` (ps5recomp) and `nps5-int` (AnyPS5, worktree `wt/anyps5-int`, from `nps5-link`). Status: **Calculator3D, a retail-style
PS5 homebrew title (eboot.elf from `work/s3/rosetta/extract`), renders natively**: x86-64 eboot -> arm64 recompiled -> AnyPS5 AGC driver and shader
recompiler -> Vulkan on KosmicKrisp -> window. Frame 0 is **100 % pixel-identical** (1920x1080, max channel difference 0) to frame 0 of the
Rosetta x86 reference run (`work/s3/rosetta/frames_x86/frame_000.bmp`), about 60 frames per second with a warm cache. Frames:
the frame pictures are not kept in this tree (no frame captures are committed; the full-size BMPs the driver wrote were
`tests/ps5/build/m2/...`/`scratchpad`, not kept: 8 MB each).

AGC Gears (`testbed/ps5-agc-gears`) was **not built**: its shaders need `amdllpc` with GFX1013 support and the build clones
`mpereiraesaa/ps5-native-app-boilerplate` (network, an LLVM-based LLPC build); neither exists here. Calculator3D was the fallback named in the task.

## What the build needs

* AnyPS5 `build-arm64` from `wt/anyps5-int` (`cmake -S . -B build-arm64 -G Ninja -DAPS5_HOST_ARM64=ON -DCMAKE_BUILD_TYPE=Release`, then
  `ninja libSceAgc libSceAgcDriver libkernel libc nps5_gpu`; about 25 minutes cold on an M4 Pro, one build directory, ~600 MB).
  Outputs `core/libs/libs/unpatched/{libSceAgc,libSceAgcDriver,libkernel,libc}.prx` and `libnps5_gpu.dylib`. The "unpatched" copies keep the real
  export names; the NID patched ones rename exports to NIDs and are not used.
* KosmicKrisp arm64 ICD (`work/s3/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`, `VK_DRIVER_FILES`); `SDL_VULKAN_LIBRARY` defaults to
  `/opt/homebrew/lib/libvulkan.dylib` (set by the runtime when unset). brew: sdl2(-compat), glslang, spirv-tools, zstd.
* Disk: nothing is copied; the translated title links the dylibs by absolute path and an rpath.

## Using it

```
export NPS5_ANYPS5_BUILD=<wt/anyps5-int>/build-arm64
ps4-recomp -compile=false -gpu anyps5 -o out eboot.elf
tools/m1_native_build.sh out out/native                  # or without -compile=false: `ps4-recomp -gpu anyps5 -o out eboot.elf`
(cd out && NPS5_WORKDIR=... NPS5_EXTRA_ENV="VK_DRIVER_FILES=<icd.json> NPS5_ANYPS5_TRACE=1" tools/run_sandboxed.sh 30 ./native)
```

`tools/m2_anyps5_run.sh <eboot.elf> [secs] [frames]` does all of it and leaves `frame_NNN.bmp` in the output directory. `-gpu off` (default) is
unchanged: no AnyPS5 define, no thunks compiled (`nps5_anyps5_*.c` are empty without `NPS5_HAVE_ANYPS5_AGC`), no link flags, and
`tools/m1_run.sh` is not affected (see Verification).

Environment knobs: `NPS5_ANYPS5_BUILD` (required for `-gpu anyps5`), `NPS5_ANYPS5_TRACE=1|2` (log every routed call with arguments and result; 2 also at entry),
`APS5_DUMP_FRAMES=<n>` + `APS5_DUMP_SCALE=1` (AnyPS5: the first n presented frames as `frame_NNN.bmp` in the working directory = the application root),
`ANYPS5_SHADER_CACHE_DIR` (default `$PS4RECOMP_CACHE_DIR/anyps5`).

## Design

### Libraries: dylibs, not static archives

The task asked for AnyPS5's "static libs". There are none for the AGC driver: AnyPS5 builds `libSceAgc`, `libSceAgcDriver`, `libkernel` and `libc` as
shared libraries (`.prx`), and the driver depends on both of the others. Linking them statically drags in the 136-symbol libkernel cascade of
`docs/m1/memory.md` section 8. Linking the dylibs resolves that inside AnyPS5. `libSceVideoOut`, `libScePad` and `libSceUserService` only exist as static
`nps5_*` targets (their `.prx` needs libSceSystemService); `nps5-int` adds one target, `nps5_gpu` (`core/libs/prx/libSceVideoOut/CMakeLists.txt`), a dylib
holding all of them with `-force_load` (+ Mouse/Keyboard/Ime, the PNG decoder, SDL2, and a stub of `sceSystemServiceParamGetInt`).

Why a dylib for those three and not a link: the runtime itself defines C functions with the same plain names (PS4Recomp's HLE of `sceVideoOut*`,
`scePad*`, `sceUserService*`: 53 duplicate symbols at link time, and with the objects first the thunks would have called the wrong implementation).
Two-level namespaces keep the two apart. The thunks of those three libraries therefore call **function pointers** filled by `dlsym` on the `libnps5_gpu`
image (`Desc.Indirect`, `nps5_anyps5_bind_exports`, run once from `nps5_anyps5_startup`). The AGC driver and libSceAgc have no namesake in the runtime and are called directly.

### Startup (recomp_init_runtime)

`6783e77` (m1-memory) already calls `AgcSetGuestBias(bias)`, `AgcSetGuestArena(lo, hi)` and reserves `guest 0xF0_0000_0000` (+256 MiB, decommitted, extent
`agc_scratch`) for `AgcSetGuestScratch`, behind `NPS5_HAVE_ANYPS5_AGC`. `nps5_anyps5_startup` (`pkg/runtime/modules/libSceAnyPS5/nps5_anyps5.c`, one call added to that function) adds:

* **The driver page.** The driver maps a 2 MiB page of its own at guest `0xE_E000_0000` (fetch-shader table, static buffers) with `MAP_FIXED` inside our reservation.
  That range is claimed in the extent list first (`recomp_vm_alloc_fixed`, extent `agc_driver_page`, decommitted) so the guest allocator can never hand it out.
* `Aps5HostServesMainQueue_nid_no_patch()` (new in AnyPS5): libkernel's `Aps5RunOnMainThread` ran functions on the calling thread unless it was itself running the
  main loop. The runtime's main thread serves the main queue (`ps4_host_run_main_loop`), the guest runs on another thread, and SDL (video init, window, events) must be on the
  main thread: without this SDL failed with "No available video device".
* `AddPathAlias_nid_no_patch("/app0", <application root>)` (AnyPS5's libc): where `param.json`, icon and assets are looked up.
* `dlopen("@rpath/libnps5_gpu.dylib")` and binding of the dlsym'ed exports; defaults for `SDL_VULKAN_LIBRARY`, `ANYPS5_SHADER_CACHE_DIR`, `APS5_WRITE_WATCH_IMPORTS=watch` (below).

### GPU allocations: registration from the extent list (superseded: VM hooks, see docs/m3/gpu-followups.md)

**Superseded by m3-gpufu**: `nps5_vm.c` now has `Nps5VmHooks` (on_map / on_protect / on_unmap before the pages are released) and the text below describes the earlier diff-based design.

`AgcGuestAllocationRegister/Unregister` must mirror the guest mappings the GPU may see. `nps5_vm.c` has no notification interface and was not edited (m1-memory is
changing it). Instead every routed call starts with `nps5_anyps5_enter` (generated thunk prologue): if `recomp_vm_generation()` changed since the last call, it walks the
extent list under `vm_mutex` (kinds `MAPPED`, `FLEXIBLE`, `DIRECT`, committed, `>= 0x2_0000_0000`, not the AGC pages, neighbours merged), builds the sorted set,
and reconciles the registry (register new, unregister gone, re-register changed) outside the lock. Observed: `register 0x200000000 +0x2400000 flags 7`
(Calculator3D's GPU heap, 36 MiB at the first AGC call) and `0x240000000 +0x2000000` at `sceAgcInit`.

Limitation, documented: unregistration happens at the next AGC call, after the host pages were released, not before as `AgcGuestAllocationUnregister` asks (it waits for GPU work
that still leases the range). A title that frees GPU memory still in flight can have the GPU read freed pages for one frame. The proper hook is two callbacks from
`nps5_vm.c`: `void (*on_map)(uint64_t guest, uint64_t bytes, int prot, int kind)` after the host mapping exists and `void (*on_unmap)(uint64_t guest, uint64_t bytes)` before it is released
(and `on_protect` before the host `mprotect`), called outside `vm_mutex` with the direct-memory lock rules of `memory-fixes-1.md`. `nps5_anyps5_enter` then becomes dead code. Not done: m1-memory owns that file.

### Import routing and thunks

`pkg/thunkgen/anyps5_sigs.tsv` holds the C signature of 1,397 exports of libSceAgc, libSceAgcDriver, libSceVideoOut, libScePad and libSceUserService, extracted from AnyPS5's
`APS5_VABI` declarations by `tools/anyps5_sigs.py` (symbols `sceFoo_nid_postfix` are the C symbol of the export `sceFoo`; other `_nid_postfix` and all `_nid_no_patch` names are driver
internals and skipped). `thunkgen.AnyPS5Descs` derives a descriptor per export; `go run ./pkg/thunkgen/gen` writes `modules/libSceAnyPS5/nps5_anyps5_{hle.h,thunks.c}`.
Rules (the libSceAgc convention of `docs/nps5-gpu-addr.md`):

| C type | descriptor |
|---|---|
| any pointer parameter | `Ptr`: a host pointer, translated by the thunk (the library dereferences HLE arguments directly). Guest addresses stored **in** the pointed-to memory (PM4 words, `Packet::addr`, `Shader` fields, out-slots of `sceAgcCreateShader`, `sceVideoOutRegisterBuffers2` address arrays) are not touched: only the argument is translated, never memory it points to |
| `sceAgcDriverSetTFRing.base`, `sceAgcDriverRegisterResource.memory`, `sceAgcDriverAddEqEvent.udata`, UserService callbacks | `GuestAddr` (guest-address-as-value, the kind from m1-memory fix round 2): the driver keys by the guest value |
| `float` / `double` | **new kind `Float`**: the n-th floating argument is `ctx->xmm[n]` independent of the integer index (5 exports) |
| integer, `bool`, `KernelEqueue`, `ShaderRegister` (8-byte struct = one integer register in both ABIs) | `Scalar` |
| returned pointer (`sceAgcDcb*` return the cursor of the packet they wrote) | `RetPtr`: translated back to a guest address |

Also new in thunkgen: `Desc.Pre` / `Desc.Post` statements (trace and the allocation sync), `Desc.Indirect`, `IndirectDefs`. Tests: `pkg/thunkgen/anyps5_test.go` (descriptors, guest-address
parameters, float indexing next to integers, header/thunk generation of all 1,397), `pkg/emitter/anyps5_test.go` (name and NID binding).

`emitter.EnableAnyPS5Shims` (called by `cli.setGPU` **before the loader resolves imports**: the first attempt did it after step 1 and half the imports stayed bound to the abort stub) rewrites
`CanonicalShims` for each export by name, by NID, and by `_`-prefixed NID, so a retail title (NIDs) and a payload-SDK title (plain names) both get `shim_aps5_<name>`. Calculator3D: 34 imports routed (libSceAgc 14, libSceAgcDriver 1, VideoOut 10, Pad 6, UserService 2, ...; the list is in `code_002.s` of the output), none left to the PS4Recomp HLE of those libraries.

### Allocation and addressing fixes inside AnyPS5 (branch nps5-int)

* VideoOut was compiled "unchanged" for arm64 and did not know about the bias: it validated **host** HLE arguments with `AgcDriverCheckGuestMemory` (guest-valued) and kept `bufferLabels`,
  the flip labels the guest and GPU poll, in host heap. New `AgcDriverCheckHostMemory_nid_postfix` for argument pointers; the labels live in `GuestAddr::Scratch` memory (guest address, `sceVideoOutGetBufferLabelAddress` returns it).
  The symptom was `AGC driver: guest memory is not readable at 0xb1e0ff3b0`, a host heap pointer taken for a guest address.
* Remaining host/guest audit items for VideoOut (not reached by Calculator3D): `sceVideoOutAddFlipEvent`/event `udata` and `Packet`-like structures in other exports. The first frames take the paths listed above.

## Task 4: "AGC driver memory was not mapped at its fixed address"

Reproduced: `agc_command_tests` and `agc_static_buffer_tests`, 2 of 25 runs each. Cause: with bias 0 on macOS the driver did `mmap(0xE'E000'0000, ...)` **without** `MAP_FIXED`; that address is only a hint and
ASLR occasionally had something there, after which the driver checked `HostToGuest(mapped) != address` and threw. Under our runtime (bias != 0) the page is mapped `MAP_FIXED` inside our reservation and the extent is
claimed first (above), so the product never hit it. Fix (`5ffdc6611d`): with guest == host the page only needs *an* address that is its own guest address: `mach_vm_allocate(VM_FLAGS_FIXED)` at the preferred address (fails cleanly if
taken, never overwrites), else `VM_FLAGS_ANYWHERE`; `DriverMemoryAddress`/`FsTableAddress` became run-time values; the static-buffer test asks `AgcDriverStaticTable` instead of hardcoding the address. **300/300** runs pass
(150 each), `agc_guest_bias_tests` 5/5. A first attempt (a series of 64 fixed 1 GiB slots) still failed 3 of 80 because the whole window was occupied in those runs.

## Findings

1. **Write-protection probe (fixed in m3-gpufu: nps5_diag fault resolvers, docs/m3/gpu-followups.md; text below is the original analysis).** `ProbeImportWriteProtection` (run once when the Vulkan device is created, `GuestBufferMemory.cpp`) faulted on its own scratch page in our process: SIGBUS,
   `ESR 0x92000047` (translation fault level 3), host `0x104844000`, delivered to nps5_diag's handler as fatal after AnyPS5's `Watch::fault` did not claim it (it chains to the handler installed before it, ours).
   Not root-caused: the faulting page is an ordinary anonymous `mmap` of the probe, so something unmapped or protected it between the allocation and the store, and the same probe passes in `agc_frame_replay`.
   Workaround: the runtime defaults `APS5_WRITE_WATCH_IMPORTS=watch`, which is the probe's documented override and skips it; the write watch itself works (frames render, AnyPS5's handler is installed after ours and chains).
   To do: run the probe under lldb (the sandbox rule forbids it here) or build `agc_frame_replay` with our crash handler linked.
2. **The crash handler order.** `nps5_diag.c` reports first and chains afterwards (`crash_handler`): a SIGSEGV/SIGBUS that belongs to a host library's own handler (write watch) is reported fatal if that library's handler declines and chains to us.
   Acceptable (they only chain on a real crash) but `crash.txt` for such a fault says "no guest context".
3. **PS4Recomp's own graphics modules still compile** (`modules/libSceVideoOut`, `libScePad`, `libSceGnmDriver`, ...) and are unused under `-gpu anyps5`; `m1_native_build.sh` stubs their 10 missing `ps4_gnm_*`/`ps4_ajm_*` symbols as before. Removing them from the link is an emitter change left for later.
4. **Lifter.** Calculator3D needed legacy SSE `MINSS/MAXSS/MINSD/MAXSD`, `ANDNPS/ANDNPD` and `CMPSD` (xmm), unsupported in m1-memory: 31 sites. Implemented through the VEX code without the YMM-upper clear (`pkg/lifter/arm64_lifter_fp.go liftSseViaVex`, test `sse_scalar_test.go`). Check against other lifter branches when merging.
5. **The run needs m2-hlesys** (SaveData `sceSaveDataMount3`, `CreateTransactionResource`, ... imported by Calculator3D). The end-to-end run was made on a scratch merge of `m2-hlesys` and this branch (`wt/try`, removed); this branch alone stops at the first SaveData import.
6. **C++ exceptions**: the title imports `__cxa_*` (unresolved here); never reached on the happy path. An AnyPS5 call returning an error makes the title throw, then abort: a mistake in marshalling shows as `__cxa_allocate_exception` unresolved (it did once, `scePadSetVibrationMode` returned `INVALID_HANDLE` because `scePadOpen` had been left to the old HLE: `_nid_postfix` exports must be included).
7. **Cold caches can exceed the title's own flip timeout (addressed in m3-gpufu: the guest process-time clocks exclude host GPU setup time, docs/m3/gpu-followups.md).** Calculator3D polls `sceVideoOutGetFlipStatus` after its first submit and throws (then aborts, `__cxa_allocate_exception` unresolved) if the first frame does not complete in time. With empty shader/pipeline caches the first frame needs the recompile plus Metal pipeline builds (about 3 s alone, longer while the machine is busy: the `tools/m2_anyps5_run.sh` run made next to a full `m1_run.sh` failed this way; the identical rerun with warm caches rendered frames 0-2). Run it twice the first time.
8. Cold start is ~3 s (one 2.8 s graphics pipeline stall on the empty cache); warm starts ~1 s. The pipeline/metallib caches live under `$PS4RECOMP_CACHE_DIR/anyps5`.

## Verification

* `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...` green (thunkgen, emitter, lifter, cli, runtime, ...).
* `tools/m1_run.sh` with default `-gpu off`: **32/34**, identical to m1-memory (only `t_x87` plt/noplt fail, 80-bit lines, as before).
* AnyPS5: `agc_command_tests`, `agc_static_buffer_tests` 150/150 each; `agc_guest_bias_tests` 5/5; the build of `libSceAgc libSceAgcDriver libkernel libc nps5_gpu nps5_VideoOut* nps5_Pad nps5_UserService` is clean.
* Calculator3D: 30 frames dumped in 0.3 s of the AGC driver's clock; frame 0 equals the x86 reference exactly; the window opens (`video driver cocoa: guest 1920x1080 presented at 1072x603`).

## Files

ps5recomp: `pkg/thunkgen/{thunkgen.go,anyps5.go,anyps5_sigs.tsv,anyps5_test.go,gen/main.go}`, `pkg/emitter/anyps5{,_test}.go`, `pkg/cli/{gpu.go,cli.go}`,
`pkg/runtime/modules/libSceAnyPS5/*`, `pkg/runtime/core/recomp_runtime.c` (one call), `pkg/lifter/{arm64_lifter.go,arm64_lifter_fp.go,sse_scalar_test.go}`, `tools/{anyps5_sigs.py,m2_anyps5_run.sh}`.
AnyPS5 (`nps5-int`): `core/libs/nps5/GpuBridge.cpp`, `libSceVideoOut/{CMakeLists.txt,src/Buffer.cpp,src/VideoOutDriver.cpp,include/VideoOutDriver.hpp,tests/Flip.cpp}`,
`libSceAgcDriver/Execution/{include/GuestMemory.hpp,src/GuestMemory.cpp,src/Driver/Lifecycle.cpp}`, `libSceAgc/tests/StaticBuffer.cpp`, `libkernel/Pthread/src/MainThreadMacOs.cpp`.
