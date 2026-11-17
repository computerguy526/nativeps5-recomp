# GPU bring-up follow-ups (branch m3-gpufu, AnyPS5 branch nps5-gpufu)

## 1. VM mapping hooks replace the extent-list diff

`core/nps5_vm.c` now notifies observers (`Nps5VmHooks`, `recomp_vm_hooks_register/unregister`, `recomp_runtime.h`):

| hook | when | locks |
|---|---|---|
| `on_map(addr,size,prot,kind,name)` | after the host pages exist (`vm_alloc_zone`, `vm_place`) | `vm_mutex` held: must be short, no VM calls |
| `on_protect(addr,size,prot,kind,name)` | per extent, before the host `mprotect` | `vm_mutex` held |
| `on_unmap(addr,size)` | **before** the host pages are released or reused (`recomp_vm_free`, MAP_FIXED over a mapping in `vm_place`) | `vm_mutex` NOT held; the extents are busy, so nobody can take/split/free them; the hook may block (the driver waits for GPU work leasing the range) |

Order: direct-memory lock -> `vm_mutex` -> hook table (rwlock, read). Registration replays `on_map` for everything mapped
(under `vm_mutex`, so no change slips between the replay and the first live call). Reservations are not reported.

`modules/libSceAnyPS5/nps5_anyps5.c` mirrors the registered ranges (sorted, disjoint) and maps the three hooks onto
`AgcGuestAllocationRegister/Unregister/Protect` (partial unmap/protect split entries; PROT_NONE <-> accessible
transitions register/unregister). `nps5_anyps5_enter` no longer reconciles anything (trace only). Tests:
`tests/memory_vm_test/vm_test.c test_hooks` (shadow registry + a checker thread that reads every registered range while 3 threads map,
protect, partially unmap and free: any hook running after the pages were released fails; mutation-checked by moving `on_unmap`
after the decommit), run by `TestGuestVM`. Calculator3D: registrations at `0x2_0000_0000`, `0x2_0020_0000`, `0x2_4000_0000`, frame 0 identical.

## 2. m8: host readers of guest memory (memory-recheck.md)

Added `ps4_gpumem_range_accessible(p, len, need_write)` (`ps4_gnm_membudget.c`, mach region walk: mapped and R/(W)) and
`nps5_mem_range_readable` (`nps5_mem.c`); the copying helpers `nps5_mem_safe_read` / `nps5_guest_safe_read` were already there.

Changed (a hole now skips the work or reads as an error instead of SIGSEGV on a host thread):
* Gnm `ps4_gnm_draw.mm`: `ReadGuest` (the recompiler's guest reader) -> `nps5_mem_safe_read`; the three `memcpy` index-buffer copies ->
  `GuestCopyReadable` (zero-fills holes); the four index expansions (quad list, quad strip, triangle fan, line loop) and the RectList
  index read, the HLE copy-shader control block, the compute clear-colour read, the occlusion-query result writers (`ZpassDone`
  paths, writable check) and the display fallback present from a guest buffer.
* Gnm `ps4_gnm_metal.mm`: `GuestMemPtr` (the funnel for PM4 indirect buffers, fences, labels, WRITE_DATA, const-RAM, ring spans) now also requires a
  mapped (and for stores writable) range; CP DMA src/dst; `ReadGuestForDump`; the shader dump scan. `ps4_gnmdriver.c`: compute ring spans,
  `gnm_resolve_guest_or_host_ptr` guest branch.
* `libScePosix` `sendmsg`/`recvmsg`: msghdr and iovec array read with `nps5_guest_safe_read`, iovec count bounded (was an unchecked
  `malloc(guest count)`), EFAULT/EMSGSIZE.
* `libSceVideoOut` `RegisterBuffers`: pointer array and attribute copied with the safe reader (count <= 64, else INVALID_ADDRESS); CPU present fallback checks readability.
Already protected before this round: T#/V#/S# image addresses (`GuestAddressMapped`), no-copy windows with holes (`ps4_gpumem_range_has_holes`,
`ps4_gpumem_for_each_readable`), `ReadablePrefixBytes` in the texture cache, `ClampReadable` in the capture, libkernel writev/vector thunks.

Deliberately left: (a) debug/trace-only dereferences gated by env (`PS4_GNM_TRACE_BUFS`, 3D texture trace, `TraceVertexStream`);
(b) the ~750 CPU-side HLE shims that read/write small guest structs through `ctx->mem_base + addr` (out-parameters, strings, callbacks):
the recheck rates them fine to defer, a wild pointer there is a title bug that also crashes on hardware; (c) writes to the guest struct
`recvmsg` updates after it was validated readable (needs writable; the window is the call itself); (d) the Metal `GuestWin`
no-copy buffers themselves (they alias pages the GPU reads; holes are handled at creation, a page unmapped later is the
`g_remapped` path); (e) `-gpu anyps5` does not use any of the PS4Recomp Gnm code (the driver validates through its own registry).
None of the Gnm `.mm` changes could be executed here (no Metal title boots on this path); they compile (`clang -fsyntax-only -x objective-c++`).

## 3. The write-watch probe fault (root cause)

`ProbeImportWriteProtection` registers a scratch range with `GuestWriteWatch` (libc.prx), which write-protects pages and unprotects on the
first write from its own SIGSEGV/SIGBUS handler. `nps5_diag`'s crash handler sits **above** that handler in the sigaction chain and treated every
fault as fatal: it printed the FATAL crash report and set `g_crashed` (a later real crash then said "recursive crash"), and only then chained
to the watch, which resolved the write. So the probe never crashed: it was *reported* as a crash (and the process kept running, so it looked like a
SIGBUS "on the scratch page"; the earlier theory of a translation fault on an unmapped page was the report of the protected page). The
`APS5_WRITE_WATCH_IMPORTS=watch` default hid it by skipping the probe.

Fix: `nps5_diag_add_fault_resolver` (async-signal-safe resolvers tried first for real memory faults, `si_code > 0`); AnyPS5 registers
`GuestWriteWatchHandleFault_nid_postfix` (the entry its own libkernel exception delivery uses). The probe now passes
("host imports resolve write protection: no ... imported ranges stay watched", same outcome as the override) and the workaround is removed.
Test: `TestDiagFaultResolver` (`tests/memory_vm_test/resolver_test.c`: claimed faults resume without a report, an unclaimed one still kills).

## 4. Cold-cache first launch

The title (Calculator3D) polls `sceVideoOutGetFlipStatus` in a loop around `sceKernelGetProcessTime`/`sceKernelUsleep(100)` and throws after
`0x4c4b41` us (5 s) (read from the lifted loop at `0x41b1f0`). A cold launch spends 2-4 s (and more on a busy machine) in `vkCreateDevice` and the
first pipeline compile, which a console never pays.

* AnyPS5 (`nps5-gpufu`): `AgcDriverStallMicros()` = wall time inside draw-time pipeline creation (not journal prewarm) or Vulkan device creation, overlapping scopes counted once.
* Runtime: `sceKernelGetProcessTime` / `...Counter` subtract it (`nps5_set_guest_clock_credit`; capped at 120 s so a hung compile still times out; monotonic;
  `NPS5_NO_STALL_CREDIT=1` disables). RDTSC, `clock_gettime`, `sceKernelReadTsc` are not adjusted. Test aid: `APS5_PIPELINE_CREATE_DELAY_MS=3000`:
  without the credit the title times out before frame 0 (0 frames, exception), with it 3 frames, frame 0 identical to the Rosetta reference.
* Cache location: `ANYPS5_SHADER_CACHE_DIR` / `NPS5_GPU_CACHE_DIR` / `~/Library/Application Support/nativeps5/<titleId>/gpu-cache` (title id from
  `NPS5_TITLE_ID` or the SFO) / `$PS4RECOMP_CACHE_DIR/anyps5`. The driver derives `KK_PIPELINE_ARCHIVE` and `KK_METALLIB_DIR` below it (keyed by device,
  driver build, OS build; it calls `setenv(..., 0)`, so explicit values win). `APS5_KK_COLLECT_MSL=1` is the runtime default; `tools/nps5_kk_prewarm.sh <titleId>` builds the
  prebuilt Metal libraries from what a launch collected.
* Not verifiable here: the installed KosmicKrisp ICD (`work/s3/install`) does not write the Metal archive or the collected MSL (no `.mtl4a`, no `kk-msl` after a
  cold run), i.e. it lacks the 0001/0002 patches; so the archive, prebuilt-metallib and `nps5_kk_prewarm.sh` paths are wired but were not exercised. The
  guest-clock credit does not depend on them and was.
