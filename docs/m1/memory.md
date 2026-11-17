# M1 guest memory model (ADR)

Branch `m1-memory`. Status: decided and implemented for M1. Supersedes the "identity only" decision of SPEC §3.6 as amended by R-ADDR / S3b.

Scope: `pkg/elfloader` (sparse image, placement), `pkg/guestmem` (layout, mode selection, manifest), `pkg/emitter` + `pkg/cli` (image file, config, build flag), `pkg/runtime/core/nps5_mem.[ch]`, `nps5_thunk.h`, the VM extent manager in `recomp_runtime.c`, `pkg/thunkgen` and the libkernel subset behind generated thunks.

## 1. Decision

| Question | Decision |
|---|---|
| Default mode | **Base** (`host = guest + bias`). It is the only mode that can serve every title: the guest sees the console's address space, including ranges macOS occupies itself. |
| Identity mode | Kept as a per-title optimisation (`-addr-mode identity` or `auto`). Runs end to end (hello_k), but is not the default until it has passed the M1 corpus and its gain is measured. |
| Per-title selection | Translate time, from a scan of the title (§4). `identity` is refused, with the evidence, when the scan rules it out. |
| Host placement, base | One reservation of 2^40 bytes at a multiple of 2^40, 2^40 preferred: `host = guest \| 1<<40`. All 40 guest address bits are backed by memory the runtime owns. |
| Host placement, identity | One reservation, exactly `0x70_0000_0000`–`0xFC_0000_0000` (S1). No alternative exists. |
| Single or split reservation | Single, in both modes. Base mode needs no split because the guest ranges `0x2_0000_0000..` and `0x70_0000_0000..` are both inside the one 1 TiB span. |
| Guest layout | The console's (§3): executable at `0x40_0000`, unhinted mappings from `0x2_0000_0000`, application area to `0xFC_0000_0000`. Identity mode uses the same layout shifted by `0x70_0000_0000`. |
| Pages | 16 KiB guest = 16 KiB host. The allocator's granule is the page; no sub-page emulation. |
| Unmapped memory | `PROT_NONE`. Pages are committed when the layout or a guest mapping creates them and decommitted on unmap. |
| Guards | `[0, 4 MiB)` null guard; one guard page under the main stack and under every thread stack. |
| Reservation safety | Non-destructive fixed mapping only. An occupied range is an error that lists the host regions in the way. `MAP_FIXED` is used only inside the runtime's own reservation. |
| HLE pointers | Translated in generated thunks (`±bias`, NULL preserved), with descriptors for nested pointers (§7). Compiles to nothing in identity mode. |
| GPU-visible addresses | Stay guest addresses; the AGC driver translates at every dereference (§8). No identity-mapped GPU window. |

### Why base is the default

1. Identity cannot serve our own M2 test. Calculator3D needs guest `[0x2_0000_0000, 0x3_0000_0000)`; natively that is the dyld shared cache. 007 First Light hardcodes `0x30_0000_0000`, inside the 384 GiB GPU carveout. `t_fixedmap` reproduces both and passes only in base mode.
2. Base mode gives deterministic guest addresses equal to the console layout. Reference traces from the Rosetta build and crash reports compare without pointer normalisation, and the null page faults instead of reading the executable's first bytes (the dense-from-0 image put the ELF header at guest 0).
3. The HLE we actually have (PS4Recomp's runtime, 1,163 `mem_base +` sites) is already written for a bias. The spec's argument for identity, AnyPS5's raw-pointer HLE, is answered by the thunk layer in §7; its cost is one add and one select per pointer argument.
4. Cost of base mode in translated code: register-offset addressing `[x28, xN]` and x28 pinned. The lifter measured 2.8–2.9 % more assembly text than identity (Syobon, hello_cxx). Run time, `tools/mem_bench.sh` (best of 5, wall clock of the whole process including about 50 ms of startup, M4 Pro, same guest binary in both modes):

   | Loop (`tests/ps5/memory/bench/b_mem.c`) | base | identity |
   |---|---|---|
   | stream: sum+store over 32 MiB of u64, 24 passes | 224 ms | 220 ms |
   | chase: 400 M dependent loads in a 4 MiB table | 3,642 ms | 3,820 ms |
   | stack: `fib(40)` | 545 ms | 535 ms |

   No difference outside run-to-run noise (the chase result favours base by 5 %, which no mechanism explains). Three micro-loops are not a title, but nothing here argues for making identity the default.

Identity stays because it frees x28, shortens memory operands, and is the only mode in which AnyPS5 libraries could be linked without thunk translation. It is one build flag and one layout origin away, and is tested.

## 2. Host placement

Measured on the M4 Pro (macOS 26.6.2), in addition to S1:

- `mach_vm_map(VM_FLAGS_FIXED, cur_prot NONE)` of 1 TiB at `0x100_0000_0000` succeeds; so do 2 TiB there and 1 TiB at `0x1000_0000_0000`. hello_k ran 30/30 in base mode (always `0x100_0000_0000`–`0x200_0000_0000`) and 30/30 in identity mode.
- Not yet checked: the same reservation with Metal/KosmicKrisp initialised first (S1 did that for the identity range only).

Base mode: the bias is a run-time value (x28 is loaded from the context), so nothing the translator emits depends on it. `nps5_mem_reserve` tries the preferred slot and then the following 1 TiB slots up to 2^47. `memory_manifest.json` records the preferred bias; the bias in use is in the run log (`arena ... bias=`) and in every crash report (`host_bias`).

Identity mode: the range is fixed. Failure is fatal and says so:

```
cannot reserve the identity arena host 0x7000000000-0xfc00000000 (kern_return 3). Identity mode has no
alternative placement; retranslate with -addr-mode base. Host regions in the way:
    host 0x8000000000-0x8000004000 (0 MiB) prot rw- tag 0
```

The reservation has maximum protection read+write: guest memory is never executable on the host. vmmap shows it as memory tag 240.

`recomp_init_runtime(size != 0)` and `PS4_RECOMP_MEM` select a **flat arena** instead: guest `[0, size)` committed read-write at the same bias, nothing ever made inaccessible. It exists for the host unit tests written against PS4Recomp's all-RW memory (boundary harness, gnm tests) and as a diagnostic escape hatch; titles do not use it.

## 3. Guest layout

Relative to the layout origin (0 in base mode, `0x70_0000_0000` in identity mode):

| Guest range | Content |
|---|---|
| `0` – `0x40_0000` | Null guard. Never mapped; a fixed mapping there is refused. |
| `0x40_0000` … | Executable (a relocatable image linked at 0 is moved here, as the console does), then each bundled module at the next 64 KiB boundary. Placed at translate time; code segments without write permission are mapped read-only (to the host they are data). |
| `image_end` … +192 KiB | System block: main thread static TLS (64 KiB), TCB (64 KiB), process arguments (64 KiB). |
| … `0x2_0000_0000` | System zone: thread stacks with TLS and TCB, guest-visible objects the runtime owns (sync object table, HLE buffers). First fit, lowest address. |
| `0x2_0000_0000` … | User zone: every mapping the guest requests without an address (`mmap(NULL)`, flexible and direct memory, virtual range reservations). First fit, lowest address. This is where AnyPS5's arena starts and where Calculator3D's captures have their memory. |
| `0x7_EAFF_8000` | Guard page, then the 64 MiB main stack up to `0x7_EEFF_C000` (AnyPS5's `GUEST_MAIN_STACK_TOP`). |
| `0x10_0000_0000` … | Memory pools (extent kind pool, `sceKernelMemoryPoolReserve`) search from here first. |
| … `0xFC_0000_0000` | End of the application area. Base mode reserves up to 2^40 so a stray 40-bit pointer still lands in owned `PROT_NONE` memory. |

Fixed mappings may be anywhere in `[0x40_0000, 0xFC_0000_0000)` that is not an image, the system block, or the main stack. A fixed-address executable (no relocations, e.g. a non-PIE PS4 eboot at `0x40_0000`) keeps its addresses in base mode and is refused in identity mode.

Sources for the constants: AnyPS5 (`--linux-fixed-base`: "virtual address 0 is placed at the address, as the console places an executable", default `0x400000`; `GUEST_MAIN_STACK_TOP = 0x7eeffc000`; `PS5_PAGE_SIZE = 0x4000`; arena base `0x2_0000_0000`, 1000 GiB, per the S3b captures), `limits.md` (application area `0x2_0000_0000`–`0xFC_0000_0000`). The exact console placement of bundled PRX modules is **not established**; they are packed after the executable as PS4Recomp did.

### Mapping calls

`recomp_vm_map_ex` (with `recomp_vm_map` as the kind-less wrapper) is the one entry point for `mmap`, `sceKernelMmap`, `SYS_mmap`, `sceKernelMapDirectMemory`, `sceKernelMap[Named]FlexibleMemory` and `sceKernelReserveVirtualRange`:

| Request | Result |
|---|---|
| no address | user zone, lowest free address |
| address without `MAP_FIXED` | a hint: that address if it is free space, else as above. A reservation or a pool is a mapping, so a hint never splits one. |
| `MAP_FIXED` | exactly that address; the guest's own mappings in the range are replaced (FreeBSD semantics) in one critical section, so no other thread can take the range meanwhile. Fails (`EINVAL`) if the address is 0 or not 16 KiB aligned, or (`ENOMEM`) if the range touches an image, the system block or the main stack. |
| `MAP_FIXED` + `MAP_EXCL` / `NO_OVERWRITE` | exactly that address, only if free |

A fixed request that cannot be honoured **fails**; it is never moved (the previous code fell back to another address, which is what breaks a title that hardcodes its arena). The refusal is logged with what occupies the address. A fixed length is rounded to pages, not to the 64 KiB granule used for placing unhinted mappings, so it cannot clobber a neighbour; the rounding is done once, in `recomp_vm_map_ex`, for every entry point.

What an extent *is* (free, reservation, pool, flexible, direct, main stack, stack guard, thread stack) is its immutable `kind` (`NPS5_EXT_*`, nps5_mem.h), set when it is created. The name is for display and `sceKernelSetVirtualRangeName` only: renaming a reservation, a thread stack or the main stack changes nothing about how it behaves. `munmap` of the main stack or its guard is refused with `EINVAL`. The extent manager is `core/nps5_vm.c`; its lock order is `g_direct_mutex` -> `vm_mutex`, and `vm_mutex` is never held across the direct-memory unmap hook (the map calls hold the direct-memory lock themselves, and the hook recognises its own thread). See `memory-fixes-1.md`.

`munmap` decommits. A virtual range reservation holds address space without pages until something is mapped into it.

### Protection

`sceKernelMprotect` and `sceKernelMtypeprotect` share `recomp_vm_protect` (core/nps5_vm.c):

- The range is widened to 16 KiB pages; a length that wraps, a range outside the guest window, or a range with nothing mapped in it is `EINVAL`. A range wholly below the extent list (images, system block) is accepted: for the pages of loaded images it reaches the host (below), anything else there has no protection to change.
- Extents are split at the range edges; the rest of a mapping keeps its protection and memory type.
- The CPU view of the new protection is applied to the host pages **while `vm_mutex` is held** (so a concurrent unmap cannot have its decommit undone): read for `PROT_READ|PROT_EXEC`, write for `PROT_WRITE`. A page the GPU may touch (`GPU_READ`/`GPU_WRITE`, bits `0x10`/`0x20`) stays readable (writable for `GPU_WRITE`) because the in-process renderer reads guest memory through the same host pointers. `PROT_NONE` is a real `PROT_NONE`: a store to a `PROT_READ` page and any access to a `PROT_NONE` one is a classified fault (`mt_faults mprotect_write`, `mprotect_none_read`).
- Reservations (address space only), the main stack and its guard, and thread stacks keep their host state.
- **Not enforced at map time.** `mmap`/`sceKernelMapFlexibleMemory`/... commit the pages read-write whatever `prot` says (the runtime fills file-backed mappings and HLE writes into fresh ones afterwards); the protection is recorded and applies from the first `mprotect`. A guard page made with `mmap(PROT_NONE)` therefore does not fault until it is `mprotect`ed. This is the remaining part of review 1's m7 (deferred, §10).

Guest code: every executable, non-writable segment is host read-only after load. The payload SDK links text and headers into one RWE segment; for those the emitter adds a read-execute entry per executable non-writable *section* (`LoadedELF.CodeProtectionSegments`), widened to whole pages unless a writable section shares a page, so their code is read-only too (`mt_faults write_code`, `write_code_fn`). A title that patches its own code calls `mprotect(RW)` first; that lifts the host protection of the image pages (`nps5_mem_protect_image`).

Host-side maximum protection stays read-write after an unmap (`mach_vm_map` with `VM_FLAGS_OVERWRITE`, not `mmap(MAP_FIXED)`), so guest memory is never potentially executable on the host.

## 4. Mode selection

`-addr-mode base` (default) | `identity` | `auto`. `pkg/guestmem.ScanTitle` runs on the executable and every companion module before anything is placed:

- **Relocatable**: the executable's lowest segment is linked in page 0. A fixed-address executable needs base mode.
- **Carveout constants**: page-aligned 64-bit values in `[0x1_8000_0000, 0x70_0000_0000)`, the range no identity-mapped process can serve (shared cache, the two randomised 12 GiB windows, the GPU carveout). Looked for as `movabs r64, imm64` and `REX.W A0-A3 moffs64` (`mov rax,[abs64]` and friends) in executable segments (byte scan, so only false positives are possible) and as 8-byte words in data segments that carry no relocation **and that code loads**: some executable bytes must hold a RIP-relative operand addressing exactly that word. A word nothing refers to is a size, a hash or a table cell far more often than an address the title maps memory at (it made every SDK-built test program refuse identity mode); an address table reached through a base pointer is not seen and is left to the run-time refusal below. Constants below 6 GiB are ignored: they are indistinguishable from sizes. `ExeBias` refuses a fixed-address executable at or beyond the end of the guest address space; the manifest rounds `image_end` up to a page; `NeedsBase` copes with a scan record that counts constants but lists none.
- **Fixed-map imports** (`mmap`, `sceKernelMapDirectMemory`, …) are recorded as context; importing one is not evidence on its own.

`auto` picks identity when nothing rules it out; `identity` fails the translation otherwise:

```
Error: title needs base addressing: 8 page-aligned constant(s) inside 0x180000000-0x7000000000, which identity
mapping cannot serve (first: 0x3000200000 in code at 0x8e4c) (use -addr-mode base or auto)
```

The scan cannot see an address computed at run time. For that case identity mode still fails cleanly at run time: the fixed request is outside the guest window and is refused with a log line, never silently relocated.

The evidence and the decision are in `memory_manifest.json` (`scan`, `mode_reason`).

## 5. Loader and image

- `elfloader.Image` replaces the dense slice: sorted regions, one per module, addressed by final guest address (`Has`, `Bytes`, `From`, `U64`, `PutU64`, `Region`, `Merge`, `Shift`). A module is parsed in its own dense buffer at linked addresses (`.eh_frame` parsing stays as it was) and published as a region; `ApplyBias` moves the region without copying.
- The loader rejects what would otherwise be silently wrong: overlapping `PT_LOAD` segments, a segment with file data but `memsz` 0 (or `filesz > memsz`), a relocation whose 8-byte slot is not wholly inside one loaded segment (no `offset + 8` arithmetic that can wrap), and an `ApplyBias` displacement that wraps the image past 2^64.
- `ApplyBias` can now place the main executable: it also moves LSDA bounds, `PT_SCE_PROCPARAM`, the address-valued dynamic tags and section header addresses, and `RELATIVE` relocations use the module's cumulative `Bias`.
- Placement happens once, in `cli.planGuestMemory`, right after load. CFG discovery, the lifter and the emitter only ever see final guest addresses, so the lifter needs no bias of its own for absolute constants.
- `guest_image.bin` (magic `NPS5IMG\x01`): region table `{guest addr, size, file offset}`, segment table `{guest addr, memsz, PF flags}`, bytes. The runtime refuses the old dense format and any region outside the title's image area.
- `main.c` carries the title's `Nps5MemConfig`; the runtime checks its ABI version and that its mode matches the mode the runtime was compiled for.

## 6. Fault classification (§3.10)

The arena is the single source of truth: `guest = guest_org + (host − host_lo)`, i.e. `host − bias` in base mode and `host` in identity mode. `nps5_mem_classify` (async-signal-safe: atomically published tables, no locks, no allocation) returns one of:

| Class | Meaning |
|---|---|
| guest null guard | null or near-null guest pointer |
| guest image | inside a loaded image (a write to read-only guest code lands here) |
| guest system block / main stack / stack guard page | fixed layout ranges; a thread stack's guard is the first page of its `thread_stack` mapping |
| guest mapping | inside a live mapping, with its name and bounds |
| unmapped guest address | valid guest address with nothing mapped (use after unmap, wild pointer) |
| beyond the guest address space | inside the reservation, past `guest_hi` |
| not guest memory | host null page, host GPU carveout ("a guest address used as a host pointer?"), host stack, above the arena |

`crash.txt` gains `addr_mode`, `host_bias`, `fault_guest_addr`, `fault_range_lo/hi`, `fault_range_name`. For a host fault at an address below `guest_hi` in base mode it also prints the host address the same value would have had as a guest address, which is the signature of a missing translation. `NPS5_DUMP_MAP=1` prints the arena map at exit.

Mappings are published by the VM extent manager under a sequence lock after every change into a buffer that only grows (256 entries, doubling, at most 2^20; the old buffers are never freed because the handler may be reading one, so the leak is bounded by the final size). Nothing is dropped below that bound; if memory runs out the snapshot is marked truncated and an address nothing claims is reported `unmapped` with `Nps5FaultInfo.truncated` set. Image and layout ranges: 4,096 (more are reported by `NPS5_DUMP_MAP`). `nps5_mem_release` resets both tables. In identity mode a fault below 4 GiB is the guest's null (guest == host), classified `guest null guard`; in base mode it is `host null page`.

**The handler must not touch guest memory with plain loads.** A stack overflow leaves `rsp` in the guard page; a load there faults inside the handler with the signal blocked and macOS re-executes the instruction forever (the "stack overflow hangs" defect, `memory-fixes-2.md`). The crash reports read guest words with `nps5_guest_safe_read` (`mach_vm_read_overwrite`), which returns false instead of faulting; HLE code that must not crash on a bad guest array uses the same call.

## 7. HLE pointer translation

Guest code calls an import; the import stub enters `_recomp_call_shim`, which saves the guest context and calls a C function `void shim_X(GuestContext *)`. Until now every such function was hand-written and translated pointers itself. For the libkernel subset M1 uses, `shim_X` is now a **generated thunk** and the implementation is a native function with host pointers (`hle_X`).

`pkg/thunkgen` generates thunks from descriptors (`pkg/thunkgen/libkernel.go`):

| Argument kind | Thunk behaviour (base mode) |
|---|---|
| `Scalar` | C conversion from the 64-bit register to the declared type |
| `Ptr` | `guest ? bias + guest : NULL` |
| `GuestAddr` | "guest address as value": passed unchanged whatever the C type (`uint64_t`, `const void *`, a function pointer): guest code addresses (thread entry, callbacks), opaque guest words, and every address an implementation keys by guest address (AnyPS5's AGC driver, below). `RetGuestAddr` is the same for a result. |
| `PtrSlot` (`void **`) | the callee gets the address of a host temporary holding the translated pointer; afterwards the guest slot is rewritten with the result translated back, **only if the callee changed it** (an untouched slot keeps its guest value, also an out-of-window one) |
| `StructArray` | shadow copy of `count` elements on the host stack, read with `nps5_guest_safe_read`, with the listed pointer fields translated (`struct iovec`); bounded, `NULL` when the count is out of range, `NPS5_BAD_GUEST_ARRAY` (the callee fails with `EFAULT`) when the guest array is not readable. Unlisted fields stay guest values. |
| `Ctx` | the calling thread's `GuestContext`; takes no guest argument slot |

Results: sign-extended integer, pointer translated back (`RetPtr`), the same with `MAP_FAILED` passed through (`RetPtrOrFail`), or no return. Integer arguments beyond six are read from the guest stack (`sceKernelMmap`'s `res`). `thunkgen` rejects a descriptor it cannot marshal (misplaced context, unsized structure, pointer field outside the structure, mismatched return kind, a floating-point, vector, 128-bit, aggregate-by-value or variadic scalar/result type, and a function-pointer type passed as `Ptr`/`PtrSlot`: a callback is guest code and must be `GuestAddr`).

Out-of-window pointers (review 1 m5, suite item 8): the default base-mode translation is `g && g < ctx->mem_size ? bias + g : NULL` (one add, one compare, one select per pointer). A guest value at or above `guest_hi` cannot be a guest pointer; adding the bias would give a host address outside the reservation, so the HLE would read or write whatever lives there. It becomes `NULL`, which the HLE functions already refuse, instead. Building with `NPS5_CHECKED_PTR=1` (`-DNPS5_CHECKED_PTR=1` through `nps5 build`) turns the same condition into a diagnosed stop naming the pointer, for debugging. Near-null pointers (`[0, guest_lo)`) translate normally and fault in the null guard, which is attributable. The hand-written shims (`ctx->mem_base + addr`) are not covered; identity mode translates by cast and does not check.

Identity mode: `NPS5_G2H`/`NPS5_H2G` are casts and the `PtrSlot`/`StructArray` paths pass the guest object itself (`#if NPS5_ADDR_MODE_IDENTITY`). `shim_sceKernelWrite` in an identity build is three loads, the call and the result store; the base build adds `cbz / ldr bias / add`.

Behind thunks now: `write`, `writev`, `sceKernelWrite`, `sceKernelWritev`, `exit`, `mmap`, `munmap`, `sceKernelMmap`, `sceKernelMapFlexibleMemory`, `sceKernelMapNamedFlexibleMemory`, `sceKernelMapDirectMemory`, `sceKernelReserveVirtualRange`, `sceKernelMprotect`, `pthread_mutex_{init,lock,trylock,unlock,destroy}` and the `scePthreadMutex*` forms, `pthread_create`/`join`, `scePthreadCreate`/`Join`, `__tls_get_addr` (28 functions, four generated files next to the code they call). The mapping functions receive host pointers and convert to guest addresses at their edge, because the VM extent manager works in guest addresses.

Not done, by design of this first step:

- The remaining hand-written shims keep translating by hand (`ctx->mem_base + addr`); they are correct in both modes because `mem_base` is the bias (NULL in identity). `mem_base` is therefore no longer usable as an "initialised" test: 254 such tests became `RECOMP_MEM_OK` (`mem_size != 0`).
- No floating-point or aggregate-by-value arguments, variadics or guest callbacks: none of the 28 functions has them. §4.2's full classifier is still to be written; `thunkgen` is where it goes.
- Descriptors are keyed by thunk symbol; the NID-keyed binding of §4.3 maps NIDs to these symbols in `emitter/shims.go` as before.
- Pointers stored *inside* guest memory by HLE (e.g. the host `PthreadAttr *` kept in the guest's `pthread_attr_t` word) are HLE-owned opaque values and are not translated.

## 8. GPU address path (AGC driver) and S3b replay

Base mode does not stop at the libkernel boundary. A title writes **guest** addresses into PM4 command streams, register blocks, `Shader` structures and resource descriptors (T#/V#), and the AGC driver dereferences them on the CPU. AnyPS5's driver assumes guest address == host pointer throughout, so in base mode it reads the wrong memory unless it translates.

**Decision: addresses in GPU-visible guest data stay guest addresses; the AGC driver translates every one it dereferences through a single guest→host function, `host = guest + bias`, with the bias taken from the arena at run time (`g_nps5_arena.bias`). No identity-mapped GPU window is kept.**

Why not an identity window: the addresses at stake are the ones identity cannot map. Calculator3D's GPU memory is at `0x2_0000_0000`–`0x2_4268_0000` (dyld shared cache natively) and the title itself checks that the high half of its arena addresses is 2 (`Renderer.cpp:51`), so the window cannot be moved either. A window would cover only titles that already qualify for identity mode, and those need no window.

Why not rewrite addresses in the command stream: PM4 and descriptors are written incrementally by guest code into memory the driver watches; a rewriter would have to find every embedded address, and the guest reads its own data back.

What the GPU itself sees is unaffected: Vulkan/Metal buffers are host objects, and the driver already keeps the guest base of an imported range separate from its `VkDeviceAddress` (`HostImport`, `GuestBufferMemory.cpp`). Addresses the driver hands back to shaders or writes into guest memory must be guest values (reverse translation).

State of AnyPS5 against this (read-only survey of `repos/AnyPS5` and the arm64 link work in `anyps5-arm64`, branch `nps5-link`; counts are grep counts, not a port):

- There is no choke point. `GuestArena::GuestAddressOffset` is not a translation: it *relocates* the arena, the driver page and the main stack so that the guest sees shifted addresses (non-zero only under ASan; 10 references in 6 files). That is identity mode with a moved arena, which is exactly what a title with hardcoded addresses, or an existing capture, cannot use. The compile-time macOS offset tried in S3b (patch 0002) failed for that reason.
- In the driver (about 120k lines, tests excluded): 337 `reinterpret_cast<T*>(` sites, 132 pointer-to-integer casts, 48 hi/lo address joins from PM4 words, guest structures with native pointer fields dereferenced without any cast (`Shader`: `user_data`, `cx_registers`, `sh_registers`, `specials`; `Packet.addr`), and 137 calls into `GuestArena`/`GuestAllocations`/`GuestWriteWatch`, all keyed by host address. `GuestMemory::Read/Write/Accessible/CheckRange` funnels part of it (63/27/90/46 uses) and is itself identity. Estimate: 150–250 sites need the translation, 300–600 changed lines in 25–40 files.
- The hazard is silent: host `0x2_xxxx_xxxx` is the shared cache and is *readable*, so a missed translation returns garbage instead of faulting. The port should therefore change guest pointer fields to an integer-typed guest pointer so the compiler finds every use, and assert in `GuestMemory::CheckRange` that an address is inside the arena.
- AnyPS5's own allocator (`DirectMemory.cpp`, `GuestArena.cpp`: arena at `0x2_0000_0000`, `MAP_FIXED`, 1000 GiB) and `Thread.cpp` link on arm64 but are non-functional there. They are the counterpart of this design: under nativeps5 the arena is `nps5_mem`, guest mappings come from the VM extent manager (§3), and `GuestArena`/`GuestAllocations`/`GuestWriteWatch` are to be backed by it with host addresses `guest + bias`. Write-watch (`mprotect` + SIGSEGV/SIGBUS, already with an aarch64 branch) works on host addresses inside our reservation and needs our crash handler to chain to it.

Sequencing for M2: (1) typed guest pointers + `ToHost`/`ToGuest` in the driver, tested by frame replay; (2) `GuestArena` on `nps5_mem`; (3) the live title. Until (1) exists, AGC titles run only in identity mode, and Calculator3D cannot run natively at all.

### S3b native replay: assessment

Question: can replay-only avoid the "136 undefined x86-specific symbols" by mapping the capture at an offset and translating in the replay path?

- The 136 symbols are not x86-specific and are not needed by replay. They come from libkernel objects pulled in to satisfy the driver (`File::*` 26, `GuestSockets::*` 16, `log_file::*` 14, DirectMemory/`DoMap*` about 22, `TimedWait` 7, kqueue/ksem 6, `sceKernel*` 9, SDL 8, CoreFoundation 5, AppArchive 4, misc). A replay-only link drops those objects and stubs about 20 symbols (equeue hooks, `DirectMemoryAliased`, app metadata getters, `HostCpuModel`, timer/wake hooks, `Aps5RunOnMainThread`, `NotImplementedAt`). The `anyps5-arm64` work has since made `agc_frame_replay` link natively with no undefined symbols, so this half of the blocker is gone.
- The address half is not replay-local. `FrameReplay.cpp` maps every chunk at `reinterpret_cast<void*>(chunk.address)` (about 20 sites) and refuses a capture whose `arena_base` differs from the built arena; that part is a small patch. But the recorded calls pass raw guest pointers into the driver (`const Shader*`, `Packet{...}`), and everything behind them is the untranslated driver described above. Mapping at an offset inside the tool alone cannot work.
- Captures themselves are fine for our layout: 8/18/18 ranges, all 64 KiB aligned, `0x2_0000_0000`–`0x2_4268_0000` plus the 2 MiB driver page at `0xE_E000_0000` (the "0x2408000000" in the S3b report is a typo).

Verdict: **no replay-only patch; replay needs step (1) of the M2 sequence and is its natural first test.** No patch was written: a partial one would produce wrong images silently. Two further unknowns stand in front of a first native image: replay has never completed on macOS on any architecture (S3b patch 0003 stops at "overlapping guest allocation" on the driver page), and `VK_EXT_external_memory_host` on arm64 KosmicKrisp is unverified (`APS5_NO_HOST_IMPORT` is the fallback). Findings are also in `work/s3/rosetta/patches/0004-native-replay-offset-ASSESSMENT.md`.

## 9. What runs

`tools/mem_prove.sh` (translate, link, run sandboxed, compare with `tests/ps5/memory/expected/`):

| Test | Mode | Result |
|---|---|---|
| PS4Recomp `hello_world.elf` | base | PASS. Image `0x400000`–`0x518000`, 725,248 bytes of code read-only, first `mmap` at `0x2_0000_0000`. |
| PS5 `hello_k` | base | PASS. Image at `0x400000`, bias `0x100_0000_0000`. |
| PS5 `hello_k` | identity | PASS. Image at `0x70_0040_0000`, bias 0. First end-to-end identity run. |
| PS5 `hello_k` | auto | PASS (selects identity). |
| `t_fixedmap` | base | PASS. Fixed 2 MiB maps at `0x2_0000_0000`, `0x30_0020_0000`, `0x70_0000_0000`, `0xFB_FFE0_0000`, written and read through hardcoded addresses; `MAP_EXCL` refused over a live mapping; plain `MAP_FIXED` replaces exactly one page; unaligned and over-the-image requests refused; flexible memory mapped, unmapped and remapped zero-filled at `0x24_0000_0000`. |
| `t_fixedmap` | identity | Refused at translate time with the message in §4, as intended. |

Unit tests: `TestGuestArena` (both modes: fallback slot, conflict listing, nothing clobbered, commit/decommit, classification), `pkg/guestmem`, `pkg/thunkgen`, `TestImageSparseAccess`, `TestParseLSDAPlacedImage`, `TestResolveImportsByKind`; the boundary harness runs on the flat arena with thread create/join through generated thunks. M1 corpus (`tools/m1_run.sh`): `t_switch`, `t_recurse` and the `broken_*` set behave as before; `broken_null` now reports `guest null guard`.

## 10. Open items

1. Base-vs-identity run-time cost is measured on three micro-loops only (§1); revisit with a real title.
2. The 1 TiB base reservation is not yet verified with Metal/KosmicKrisp initialised in the same process.
3. Guest protections are enforced on the host only from an explicit `mprotect` on (§3 "Protection"); a protection given at map time stays bookkeeping. Write-watch integration is §3.10 work.
4. Identity mode has run one program. The hand-written shims contain "is this already a host pointer" heuristics (`gaddr >= mem_base && gaddr < mem_base + mem_size`) that are vacuous in identity mode; they need review before identity is used for a title with those modules.
5. Console placement of bundled modules and of thread stacks is not modelled beyond "below `0x2_0000_0000`".
6. The mapping snapshot for the crash handler is republished in full (O(mappings)) under `vm_mutex` on every map/unmap (review 1 n5). It has no 2,048 cap any more. Deferred: it only matters for titles with tens of thousands of live mappings that churn mmap; the fix is a lazy or incremental publish.
7. The payload SDK's own `mmap()` is a raw `SYSCALL` wrapper, which the lifter rejects; `t_fixedmap` therefore uses `sceKernelMmap`.
8. The Bloodborne patch table is link-address based and now follows `LoadedELF.Bias`; not re-verified against the game.
9. The PS4 Gnm renderer wraps 64 MiB windows of guest memory as no-copy Metal buffers (`ps4_gnm_guest_window.h`) and was written for an arena that is readable and writable everywhere. Those modules do not build in this environment (missing third-party submodules) and were not run against the committed-on-demand arena; a PS4 graphics title needs either window clipping to mapped extents or the flat arena (`PS4_RECOMP_MEM`). AnyPS5's AGC driver, the M2 path, is unaffected by this code.
10. Follow-ups from review 1, state after round 2 (`memory-fixes-2.md`):
    - m7 (mprotect): split at the range edges, `EINVAL` for unmapped/wrapping/out-of-window ranges, host enforcement of the CPU view on `mprotect`, image code read-only also in RWE segments and liftable by `mprotect(RW)`: done. Remaining: a protection given at map time is bookkeeping (the runtime writes into fresh and file-backed mappings after creating them; enforcing it needs those writers to go through a permission-aware path), and `sceKernelMprotect` of a range that straddles an image and an extent is treated as the extent part only.
    - m8 (host readers): the primitive exists (`nps5_mem_safe_read`, `nps5_guest_safe_read`) and the `writev` thunk and the crash handlers use it. The Gnm renderer (`.mm`, not buildable here) and the hand-written shims that bound only by `mem_size` still read guest memory with plain loads.
    - n1 (max protection): done for decommit and the direct-memory unmap hook; `dm_phys_init` and the alias mapping still come from `mmap`/`mach_vm_remap` of the physical region.
    - n5: see 6.
11. Not defects, kept as documented behaviour: a `fork`ed child has no guest memory (the arena is `VM_INHERIT_NONE`; a guest never forks), the memtests therefore run their "child" cases on a helper thread; an unhinted `mmap` is placed in 64 KiB granules, so `munmap` must name the rounded length to free all of it; a concurrent `MAP_FIXED` over an extent that another `munmap` is still releasing fails with `ENOMEM` instead of waiting (waiting would invert the lock order `g_direct_mutex` -> `vm_mutex` for `MapDirectMemory`).

## 11. AnyPS5 integration (AGC driver, M2)

AnyPS5's AGC driver now translates every GPU-visible guest address through one choke point (`AnyPS5/docs/nps5-gpu-addr.md`, `AgcGuestMemory.h`). What the runtime side does and must keep doing:

1. **Configuration at startup.** A runtime built with `-DNPS5_HAVE_ANYPS5_AGC=1` (the build that links the AnyPS5 AGC libraries adds it; a weak reference would still fail the link of every other build on macOS, so it is a compile-time switch) has `recomp_init_runtime` call, once and before any guest code, `AgcSetGuestBias(int64_t)` with the arena bias (`g_nps5_arena.bias`; 0 in identity mode), `AgcSetGuestArena(guest_lo, guest_hi)`, and in base mode `AgcSetGuestScratch(0xF0_0000_0000, 256 MiB)`. The scratch window is address space the driver maps pages into itself: the runtime reserves it as an extent named `agc_scratch` (decommitted, so inaccessible, and taken, so the guest is never handed it). Identity mode has bias 0 and the driver's scratch memory is ordinary heap memory, so no window is reserved. Without the define nothing is referenced (`xlate_adv agc_driver_gets_bias_arena_and_scratch` compiles with it and tests the calls with doubles, both modes).
2. **Registrations are guest-valued.** `GuestAllocations` is keyed by guest address, so whoever registers a range (`AgcGuestAllocationRegister/Unregister/Protect`, from the mapping and `mprotect` paths when the driver is linked) must pass the **guest** address the VM manager returned, never `guest + bias`. `AgcGuestAllocationUnregister` must be called before the host memory is released: in `recomp_vm_free` that is before `ps4_direct_mem_on_unmap`/decommit, and `AgcGuestAllocationProtect` before the host `mprotect` in `recomp_vm_protect`. The hooks themselves are not wired yet (the driver is not linked into the runtime in this tree); the places are marked here so they are one-line additions.
3. **Thunk descriptors of AGC entry points must not translate GPU-visible values.** `Packet::addr`, `const Shader *` and the pointers inside `Shader`/`ShaderUserData` are guest values. Use `GuestAddr` for such arguments (`CType` may be `uint64_t` or a pointer type; it is never translated), list only the genuinely host-side pointers of a `StructArray` in `PtrFields` (unlisted fields are copied as guest values), and `RetGuestAddr` for results that are guest values. `const Packet *` itself is a host pointer (translated by the thunk). `TestGuestAddrAsValueIsNeverTranslated` pins this.
4. **No second translation.** The driver does `host = guest + AgcGetGuestBias()`; the runtime's `NPS5_G2H` does the same with `ctx->mem_base`. Both read the same bias, set once, so they cannot disagree.
