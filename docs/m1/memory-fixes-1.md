# Memory model: fixes for review 1

Input: `work/reviews/memory-review-1.md` (m1-memory at 26f68b5). Commits are on `m1-memory`.

| Commit | Subject |
|---|---|
| `4c5d855` | extent kinds, atomic MAP_FIXED, page-exact fixed maps |
| `ac3dce1` | identity mode: `mem_base == NULL` is not "not set up" |
| `53110ca` | thunkgen rejects unmarshalable types |
| `fce5f0a`, `b309e2b` | ApplyBias at link address 0 (the second commit keeps entry 0 as "none") |
| `456c35a` | checked pointer translation, docs |

## Finding -> commit -> test

| ID | Status | Commit | Test |
|---|---|---|---|
| B1 deadlock, `sceKernelMapDirectMemory(MAP_FIXED)` over a mapping | fixed | 4c5d855 | `tests/memory_vm_test/vm_test.c` `test_b1` (reserve then MapDirect FIXED, then remap FIXED over the direct mapping; 5 s alarm), run by `TestGuestVM` |
| M1 MAP_FIXED replace not atomic | fixed | 4c5d855 | `test_m1`: 100,000 fixed replaces against two threads doing `mmap(NULL)`/`munmap`, 0 failures (was 269/200,000) |
| M2 fixed requests widened to 64 KiB / alignment | fixed | 4c5d855 | `test_m2`: flexible, direct (2 MiB alignment) and reserve with FIXED keep neighbours; misaligned fixed gives EINVAL |
| M3 `MemoryPoolReserve(addr)` fakes success | fixed | 4c5d855 | `test_m3`: EXCL over a mapping gives ENOMEM, a hint relocates and is really mapped, below the extent list fails, free fixed works |
| M4 identity mode `mem_base` as "initialised" | fixed | ac3dce1 | `pkg/runtime/mem_lint_test.go` `TestMemBaseNotBoolean` (fails on the 6 original sites) |
| M5 hint carves a reservation or pool | fixed | 4c5d855 | `test_m6`: hint into a reservation and into a pool moves elsewhere and leaves them whole |
| m1 MAP_FIXED at 0 relocated | fixed | 4c5d855 | `test_minor` (`m1`); `sceKernelBatchMap` passes no MAP_FIXED for `start == 0` |
| m2 munmap of the main stack | fixed | 4c5d855 | `test_minor` (`m2`): EINVAL, extent untouched, MAP_FIXED over it refused |
| m3 adjacent thread-stack guards merged | fixed | 4c5d855 | `test_minor` (`m3`): both guards classified; also after a guest rename |
| m4 commit/decommit length wrap | fixed | 4c5d855 | `test_minor` (`m4`), also `protect_inner` |
| m5 no range check in pointer translation | fixed behind a flag | 456c35a | `TestCheckedPointerTranslation` (`tests/memory_vm_test/ptrcheck.c`): `NPS5_CHECKED_PTR=1` aborts on `1<<41` and `NULL-8`, default build unchanged |
| m6 semantics keyed on names | fixed (M5 above) | 4c5d855 | `test_m6`: renamed reservation stays one, a mapping renamed `reserved` is not one, renamed thread stack keeps its guard |
| m7 mprotect is bookkeeping | deferred | - | documented in memory.md §3/§10 |
| m8 host readers bound by `mem_size` only | deferred | - | documented in memory.md §10 |
| m9 thunkgen accepts float/struct | fixed | 53110ca | `TestUnmarshalableTypesRejected` |
| m10 ApplyBias at address 0 | fixed (sections, symbols, segment 0) | fce5f0a, b309e2b | `TestApplyBiasImageLinkedAtZero` |
| n2 seqlock reader fence | fixed | 4c5d855 | by reading (`atomic_thread_fence` before the re-check) |
| n3 alignment above 2^63 | fixed | 4c5d855 | `test_minor` (`n3`) |
| n4 select.go comment | fixed | 456c35a | comment only |
| n1 decommit leaves max protection RWX | deferred | - | needs `mach_vm_map(..., OVERWRITE)` with max RW in nps5_mem.c and the direct-memory alias path; risk without a test that can observe max protection |
| n5 snapshot republished on every change | deferred | - | performance only, still O(n) under `vm_mutex` at 2,048 entries; memory.md §10.6 |

## Design notes

- **Lock order.** `g_direct_mutex -> vm_mutex`. The allocator (`core/nps5_vm.c`) never holds `vm_mutex` while it calls `ps4_direct_mem_on_unmap`. The map calls hold the direct-memory lock around the allocator; the unmap hook checks a thread-local "I hold it" flag instead of re-taking the mutex.
- **Atomic replace.** `vm_place` validates, removes the old extents and tags the new extent under one `vm_mutex` hold. If old mappings existed, the new extent is marked `busy` while their aliases are detached and host pages released outside the lock; busy extents are not split, replaced or freed. A concurrent `munmap` of exactly that range therefore skips it (it is ordered before the mapping, which is a valid linearization).
- **Kinds.** `NPS5_EXT_*` is set at creation. Only the runtime's own `recomp_vm_alloc_named_aligned` derives a kind from a name (`thread_stack`); guest-supplied names never do.
- **Entry point.** `ApplyBias` leaves an entry point of 0 alone: SCE executables carry none (moving it broke every M1 test).
- **Reviewer repros.** `memory-review-1-repro/build.sh` extracts lines 1103-1574 of `recomp_runtime.c`, which are now `core/nps5_vm.c`; the same scenarios (R1-R6, F1, P1, DM1) are ported to `vm_test.c` with assertions.

## Verification (this branch)

- `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...`: all packages pass.
- `tools/m1_run.sh`: 32/34 (only `t_x87` plt/noplt fail, as before).
- `tools/mem_prove.sh`: 5/5.
