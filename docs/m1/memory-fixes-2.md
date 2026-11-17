# Memory model: fixes for round 2 (adversarial suite)

Input: the adversarial suite of branch `m1-memtests` (`docs/m1/memtests.md`, 209 cases, written against 26f68b5) merged into `m1-memory` (3196016), review 1's deferred items (m7, m8, n1, n5) and the AnyPS5 GPU integration notes. Commits are on `m1-memory`.

**Status before this round (merged suite, `tools/memtests.sh --strict`, after fixing the harness link so it exercises the real runtime): 26 strict failures of 138 results reported (the runtime harnesses did not link `core/nps5_vm.c`, so their cases ran against zero-returning stubs; the arena harness did not compile; the guest programs showed 12 KNOWN-FAIL).** After adapting the harness (below) the real status was the list in the table. **After: zero FAIL, zero KNOWN-FAIL** (counts in the last section).

## Suite adaptation (not weakened assertions)

| Change | Why |
|---|---|
| `tools/memtests_hostcc.sh` links `core/nps5_vm.c` | round 1 moved the VM manager there; the missing symbols were auto-stubbed to 0 and every mapping "succeeded" at address 0 |
| `arena_adv.c` calls `nps5_mem_map_add(..., kind)` | the API gained the extent kind in round 1 |
| `in_child` (fork) replaced by a helper thread with an 8 s watchdog (`vmh.h`) | the arena is `VM_INHERIT_NONE`: a forked child has no guest memory and died in the harness, not in the code under test |
| `conc_adv.c` frees unhinted mappings with the 64 KiB-rounded length | an unhinted mapping is placed in 64 KiB granules (round 1, console behaviour); a shorter `munmap` frees only those pages |
| `thunk_driver.c`: `mem_size` set, `nps5_guest_safe_read` test double, unreadable-iovec case | thunk changes below |
| `mt_syscalls.c`: `mprotect_bookkeeping_only` became `mprotect_roundtrip_and_errors` | the case wrote to a `PROT_READ` page and expected to survive; that now (correctly) kills the guest, and `mt_faults mprotect_write` asserts it |
| `cases.list`: `beyond_area` identity expects `above the guest arena` | the crash report names the more precise class than the test author's guess |
| `known=` / `MT_KNOWN` / Go `knownFail` | all fixed: C `MT_KNOWN` became `MT_EXPECT` (40 sites), Go `knownFail` now always fails. The KNOWN-FAIL machinery stays in `mt.h` for future use; nothing is deferred in the suite |
| `TestMemScanDataAndRelocations`, `TestScanAndSelect` | a data word now counts only when code loads it (item 16); the tests give the word a RIP-relative load |

## Defect -> commit -> test

Numbers are the defect list of `memtests.md`.

| # | Defect | Status | Commit | Test |
|---|---|---|---|---|
| 1,2,3 | MAP_FIXED at 0, 64 KiB rounding of fixed maps, MapDirect deadlock | fixed in round 1 | 4c5d855 | `vm_adv` (now run against the real VM code, helper thread for the deadlock case) |
| 4 | `recomp_vm_free` length wrap | fixed | ebb8100 | `vm_adv unmap_*` |
| 5 | concurrent overlapping munmap vs MAP_FIXED lose mappings | fixed: `recomp_vm_free` marks the extents it frees `busy` before dropping the lock and phase 3 frees exactly those extents; others skip or refuse busy extents | ebb8100 | `conc_adv overlapping_window_and_snapshot_readers` |
| 6a | seqlock reader torn reads | fixed in round 1 (acquire fence) | 4c5d855 | `conc_adv` |
| 6b | stale tables after `nps5_mem_release` | fixed | 1e5b385 | `arena teardown_resets_classification_tables` |
| 6c | >64 note ranges dropped | fixed (4096, truncation reported by the map dump) | 1e5b385 | `arena fixed_range_table_capacity` |
| 6d | >2048 mappings reported UNMAPPED | fixed: growable snapshot buffer (256 .. 2^20 entries), `Nps5FaultInfo.truncated` only on OOM | 1e5b385 | `arena mapping_snapshot_capacity` |
| 6e | adjacent thread stacks merged | fixed in round 1 (kind), test adapted to the 4-argument API | 4c5d855 | `arena snapshot_merges_adjacent_thread_stacks`, `vm_adv thread_stack_guards` |
| 7a | base slot search capped at 16 | fixed (walks to 2^47) | 1e5b385 | `arena slots_beyond_16` |
| 7b | commit/decommit/protect_inner wrapping lengths | fixed in round 1 | 4c5d855 | `arena commit_decommit_wrap_and_zero` |
| 8 | `NPS5_G2H` out of range | decided: translation maps a guest value at or above the window end to NULL (one compare), `NPS5_CHECKED_PTR` aborts naming it; hand-written shims and identity mode not covered (documented) | d407926 | `xlate_adv out_of_range_guest_pointer_fails_loudly`, `TestCheckedPointerTranslation` |
| 9 | `sceKernelMprotect` retags the whole extent; returns 0 for unmapped/wrapping/past-`guest_hi` | fixed: `recomp_vm_protect` (split, `EINVAL`), shared with Mtypeprotect | ebb8100, 552d46f | `vm_adv mprotect_granularity_and_errors`, `mt_syscalls mprotect_roundtrip_and_errors` |
| 9' | guest protections not enforced | fixed for explicit `mprotect` (host pages); **not at map time** (deferred, below) | ebb8100 | `vm_adv mprotect_enforcement`, `mt_faults mprotect_write`, `mprotect_none_read` |
| 10a | AllocateDirectMemory length/alignment validation | fixed (`EINVAL`) | 552d46f | `vm_adv direct_alloc_invalid_arguments`, `mt_syscalls direct_memory` |
| 10b | ReleaseDirectMemory leaves mappings accessible | fixed: the aliases are unmapped under the direct-memory lock | 552d46f | `vm_adv direct_memory_alloc_map_release` |
| 11 | huge thread stack size gives no stack | fixed (`NULL`/`ENOMEM`) | ebb8100 | `vm_adv thread_stack_guards` |
| 12 | `shim_writev` host crash on an unmapped iovec array | fixed: generated `StructArray` thunks read the array with `nps5_guest_safe_read`; unreadable -> `EFAULT` | d407926 | `vm_adv hle_boundary_bad_guest_pointers`, thunk driver `pointer/iov` case |
| 13 | stack overflow hangs (main and thread) | fixed: the crash handlers loaded the guest stack words at `rsp` (in the guard page) with plain loads; a fault inside a handler with the signal blocked spins forever on macOS. They use `nps5_guest_safe_read` now. Both reports classify `guest stack guard page` | 1e5b385, ebb8100 (legacy handler in `recomp_runtime.c`) | `mt_faults stack_overflow`, `thread_overflow` (base and identity) |
| 14 | stores into guest code do not fault | fixed: the emitter adds read-execute entries for executable non-writable sections of RWE segments (payload SDK), whole pages unless a writable section shares them; `mprotect(RW)` on image pages lifts it | 563f721 | `mt_faults write_code`, `write_code_fn`, `TestCodeProtectionSegmentsForRWESegment` |
| 15 | loader: overlapping PT_LOAD, out-of-segment relocations, relocation offset near 2^64 panic, `ApplyBias` wrap, `filesz > memsz == 0` | fixed | 9f36862 | `TestMemOverlappingSegments`, `TestMemRelocationTargetsAtSegmentEnd`, `TestMemRelocationsFollowBias/overflowing_bias`, `TestMemMalformedSizes` |
| 16 | `ScanTitle` misses A0-A3 moffs64; `ExeBias` accepts a fixed exe >= `guest_hi`; unaligned `image_end`; `NeedsBase` panic | fixed | 07782aa | `TestMemScanAbsoluteMoffsForms`, `TestMemExeBiasEdges`, `TestMemManifestConfigIsAcceptableToRuntime`, `TestMemSelectMatrix` |
| 16' | identity false positive on a data word (mt_faults, mt_hle) | fixed: a data word counts only when code loads it RIP-relatively; the three SDK test programs build in identity mode | 07782aa | `TestMemScanDataAndRelocations`, `e2e.identity.mt_faults.*`, `e2e.identity.mt_hle.*` |
| 17 | thunkgen accepts float/struct/variadic/callback | float/aggregate/variadic fixed in round 1; `__int128` and function-pointer `Ptr` now rejected | 53110ca, 4ea1681 | `TestRejectsUnclassifiableSignatures` |
| 18 | libc `calloc` overflow, `malloc(2^62)` | already failing with ENOMEM in this tree (overflow check and 2^40 cap); `memalign`'s `size + align` could still wrap: fixed | be7157b | `mt_hle` (calloc/malloc cases), memalign by reading |

Further defects found on the way:

- The thunk `PtrSlot` write-back stored `H2G(G2H(x))` into an untouched slot, which with the out-of-window rule would have turned a wild guest value into 0; it now writes back only what the callee changed (`pointer_slot_roundtrip`).
- A fault inside a crash handler (the guard-page read above) also affected the second-stage legacy report in `recomp_runtime.c`; both are fixed.
- identity mode: a fault below 4 GiB is the guest's null (guest == host); it was reported as "host null page" and now as `guest null guard`.

## Review 1 follow-ups

| Item | State |
|---|---|
| m7 mprotect | split, errors, host enforcement of the CPU view on `mprotect`, code read-only and liftable. **Deferred: protection given at map time** (the runtime writes into fresh and file-backed mappings after creating them; enforcing it needs those writers to be permission-aware) |
| m8 host readers | primitive added (`nps5_mem_safe_read`, `nps5_guest_safe_read`), used by the writev thunk and crash handlers. **Deferred: the Gnm renderer (`.mm`, does not build here) and hand-written shims still read with plain loads** |
| n1 max protection | fixed for decommit (`arena decommit_keeps_max_protection_rw`; verified to fail with the old `mmap`) and the direct-memory unmap hook; `dm_phys_init`/`mach_vm_remap` aliases untouched |
| n5 snapshot republished in full | **Deferred**: O(mappings) per change under `vm_mutex`; irrelevant below thousands of live mappings. `recomp_vm_protect` no longer republishes (protection is not in the snapshot) |

## AnyPS5 integration (ps5recomp side)

See `memory.md` section 11. Summary: `-DNPS5_HAVE_ANYPS5_AGC=1` makes `recomp_init_runtime` call `AgcSetGuestBias(bias)`, `AgcSetGuestArena(lo, hi)` and (base mode) `AgcSetGuestScratch(0xF0_0000_0000, 256 MiB)` after reserving that window as an inaccessible extent (`xlate_adv agc_driver_gets_bias_arena_and_scratch`, both modes, with doubles). Thunk descriptors express "guest address as value" with the existing `GuestAddr` kind (any C type, never translated), the new `RetGuestAddr` result kind, and unlisted `StructArray` fields (`TestGuestAddrAsValueIsNeverTranslated`; a function pointer is accepted as `GuestAddr`, refused as `Ptr`). `AgcGuestAllocation*` registration hooks are documented (guest-valued, unregister before release, protect before the host `mprotect`) but not wired: the driver is not linked into this runtime tree.

## Deferred list (reasons)

1. Map-time guest protections (m7 remainder): needs permission-aware HLE writers; documented, no test pins the current behaviour.
2. Host readers in the Gnm renderer and hand-written shims (m8 remainder): `.mm` does not build here.
3. Snapshot republish cost (n5): performance only.
4. `AgcGuestAllocation*` wiring: the AnyPS5 libraries are not linked into this tree.
5. `dm_phys_init` / alias mappings keep max protection RWX (n1 remainder): `mach_vm_remap` inherits it from the physical region.

## Verification

- `tools/memtests.sh --strict`: **PASS 240, FAIL 0, KNOWN-FAIL 0, SKIP 0** (Go with `-race`, host harnesses, guest programs in base and identity). Before: PASS 100, FAIL 26, KNOWN-FAIL 10, SKIP 2 of 138 reported (the first run of the merged suite; the runtime harnesses did not link `nps5_vm.c` or compile, which is why it is not 209). The guest `mt_faults`/`mt_hle` programs build in identity mode now (the two SKIPs were the data-word false positive).
- `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...`: all packages pass.
- `tools/m1_run.sh`: 32/34 (only `t_x87` plt/noplt fail, as before).
- `tools/mem_prove.sh`: 5/5.
