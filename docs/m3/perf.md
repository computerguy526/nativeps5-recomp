# Runtime performance (branch m3-syoperf, AnyPS5 branch nps5-perf)

Trigger: Syobon Action (a 2D SDL homebrew) was seen at ~18 fps. Result: the title limits itself to 30 fps
(33 ms frames, one `nanosleep` per frame; `sceVideoOutSetFlipRate(0)` = 60 Hz, so the cap is the game's own loop),
and it now holds that cap with the machine loaded. 60 fps is not reachable without patching the game.

## 1. The performance trace (`pkg/runtime/core/nps5_perf.c`)

| switch | what |
|---|---|
| always | `perf.txt` in the run bundle: frames, fps, frame-time avg/p50/p95/p99/max. A frame = two flip submissions (`sceVideoOutSubmitFlip` of either backend; `nps5_perf_register_flip_fn`). Cost: one clock read per flip |
| `NPS5_PERF=1` | attribution, about 1 % overhead (below) |
| `NPS5_PERF=2` | also `perf_frames.csv`: wall, flip-thread CPU, process CPU, HLE wall per frame |
| `NPS5_PERF_HUD=1` | one line per second on stderr (fps, frame p50/p95/max, flip-thread CPU, process CPU), and the report on stderr at exit. On screen: AnyPS5's window title carries the FPS |
| `NPS5_PERF_HZ` | sampling rate of threads running guest code (default 1000) |

`NPS5_PERF=1` adds to `perf.txt`:
* per frame: flip-thread CPU, flip-thread wall time inside HLE calls, not-running time (blocked or preempted), process CPU;
* dispatch slow paths per frame (shadow-stack slow returns, continuation-table / tier-1 dispatches);
* threads by name with CPU ms per frame;
* HLE by shim: calls/frame, wall ms/frame (entry hook + a new exit hook in `_recomp_call_shim`), sampled CPU ms/frame;
* sampled CPU by category: guest code, HLE (by image), runtime, and every host image (AGC driver, KosmicKrisp, Metal, IOKit,
  libsystem_kernel ...); top guest functions; top guest instructions; top host symbols;
* `perf_hot.txt`: the hottest guest functions instruction by instruction (guest pc, ms/frame).

The sampler is a thread (`nps5-perf`), not a signal: it reads each thread's pc with `thread_get_state` and weights the sample
with the CPU time that thread consumed since its previous sample, so the totals are CPU time at any machine load and a
parked thread adds nothing. Caveats: a sample taken while a thread is blocked credits the CPU it used just before to the
blocking call (the ~1 ms "CPU" shown inside `sceKernelWaitEqueue` is guest code before the wait); samples skid to the
instruction after a slow one; shim names need a link that keeps local symbols (`NPS5_KEEP_SYMS=1 tools/m1_native_build.sh`,
the CLI links with `-Wl,-x` and reports `shim@+offset`). GPU time is not read from Metal: the presenter's wait on the present
fence (KosmicKrisp `kk_timeline_wait`) is the proxy, see section 3.

Also fixed on the way: `timeout(1)` delivers SIGTERM twice; the second delivery killed the process while the first handler
was writing `summary.txt` (it came out empty). `NPS5_ANYPS5_TRACE=0` now means off.

## 2. Where Syobon's frame went (before)

Measured on the unmodified build (main 41f2376), machine quiet (load 5), per frame:

| | ms |
|---|---|
| guest thread CPU | 28.6 |
| of which one guest function, `fn_0x61d9e0` = SDL's `SDL_Blit_Slow` (per-pixel generic blit of the 480x420 game surface to the 1920x1080 window surface: three `switch` dispatches, ~45 loads of format fields and lookup tables, ~180 x86 instructions per pixel) | ~27 (96 % of guest code) |
| HLE calls on the game thread (CPU) | < 0.2 |
| dispatch slow paths (shadow slow returns, tier 1, indirect misses) | 0 |
| flip wait (`sceKernelWaitEqueue` on the flip event) | 3.6 wall |
| host threads (main thread event pump + window title, presenter, KosmicKrisp, audio) | 3.9 CPU |
| AGC driver / KosmicKrisp CPU | < 0.1 |
| GPU (present fence wait) | ~1.0 wall |
| title's own sleep | 0-4 |

So: no dispatch problem, no HLE hot path, no driver or GPU problem. One translated loop used 86 % of the process's CPU and left
14 % of the title's 33 ms budget. The 18 fps were the machine: load average 45-58 (other translations and builds) on threads
that had no QoS class. Reproduced at load 46 with the same binary: 16.5 fps, p95 148 ms.

Why the loop is slow natively:
* x86 ordering. With `-tso=false` the same frame costs 12.9 ms. An ordered load costs about one cycle of *throughput* on an
  M4 whatever surrounds it. Microbenchmark, 8 loads per iteration: plain 0.13 ns/load, `ldapur` 0.30, `add`+`ldapr` 0.29,
  `ldar` 0.29, `ldr`+`dmb ishld` 0.37 (one `dmb ishld` after 8 plain loads is free, but that does not order the 8);
  `stlr` is nearly free (0.26 vs 0.15 ns). 45 ordered loads per pixel = the 14 ms.
* Code shape: 723 host instructions on the hot path for 181 guest instructions (4.0x): address copies
  (`mov x16, x23; add x16, x16, #36; add x7, x28, x16; ldaprb`), the 16-byte-boundary check of every 2/4/8-byte access,
  an NZCV save/range check/restore and a 4-instruction compare per case in the jump-table dispatch.

## 3. Changes

Figures are per frame; "loaded" = load average 20-50 from other agents, "quiet" = a quiet window (load ~5). CPU time is far
less sensitive to load than fps but not immune (efficiency cores, shared caches): compare within a column.

| change | Syobon guest-thread CPU (ms) | frame time / fps | why |
|---|---|---|---|
| baseline (main 41f2376) | 28.6 quiet, 33-45 loaded | 33.6 ms p50 quiet (29.7 fps); 16-24 fps loaded | |
| lifter peephole: copy propagation + destination renaming (`arm64_lifter_peephole.go`) | | | 723 -> 543 host instructions on the hot path |
| folded acquire/release (`LDAPUR`/`STLUR`, FEAT_LRCPC2): `add x7, x28, xB; ldapurb w, [x7, #d]`; 2/4/8-byte accesses test the base alignment with `and`+`cbz` and keep the exact crossing check behind it; no check at all for constant addresses (`[rip+d]`) | | | 2 instructions per byte load instead of 4, 4 instead of 7-9 for an aligned word |
| flag-free jump-table dispatch (`sub` offset, `cbz` per case) | | | was `mrs`/2 `cmp`/`msr` + 4 instructions per case |
| SHLX/SHRX/SARX without the count mask | 27.1 quiet (all lifter changes together) | 33.8 ms p50 | |
| guest threads get a QoS class (main: user-interactive, others: user-initiated) | 28.7 at load 50 | load 46-50: 16.5 fps avg / p95 148 ms -> 26.8 fps avg / p95 46 ms | the scheduler treated the game like a batch job |
| sleeps through a critical kqueue timer (`nps5_sleep.h`) | | 33.0 ms p50 (30.3 fps), p95 34.3 at load 33 | `nanosleep` is coalesced: 21 ms asked, 24 ms slept; with a cheap frame the title ran at 36.9 ms |
| AnyPS5 presenter: no main-thread hop before the flip notification, window title twice a second | (host threads 2.85 -> 1.8) | flip wait 3.6 -> 2.8 ms wall | 5 synchronous hops per frame, `SDL_SetWindowTitle` per frame |
| opt-in: `NPS5_TSO_RELAX_FUNCS=0x61d9e0` (per-function plain accesses) | 13.5 | 33.0 ms p50, sleeps 17 ms | section 4 |

### Final measurement (quiet window 20:11:28-20:14:41, load average 8.7 at the start, 21 at the end; 28 s runs, pad timeline)

| build | guest-thread CPU / frame | process CPU / frame | frame p50 / p95 / p99 (ms) | fps at p50 | title's sleep / frame |
|---|---|---|---|---|---|
| main 41f2376 (old AnyPS5 build) | 29.7 ms | ~32 | 33.4 (419 frames in 14 s) | 29.9 | 0-3 ms |
| this branch, strict TSO (default) | 28.9 ms (26.9-28.9 over six runs) | 31.7 | 33.08 / 35.1 / 36.0 | 30.2 | 2.4 ms |
| this branch + `NPS5_TSO_RELAX_FUNCS=0x61d9e0` | 14.6 ms | 17.5 | 33.07 / 34.7 / 35.7 | 30.2 | 16.7 ms |
| this branch, `-tso=false` (reference) | 13.6 ms | 16.2 | 33.07 / 34.5 / 35.6 | 30.2 | 17.8 ms |

First quiet window (19:27:20-19:30:40, load 11 -> 5): main 28.6 ms and 29.7 fps; lifter changes only 27.1 ms, 33.8 ms p50;
`-tso=false` 12.9 ms but 36.9 ms p50 (the coalesced sleep). At load 46-50: main-equivalent scheduling (`NPS5_GUEST_QOS=default`)
16.5 fps avg, this branch 26.8 fps avg with p50 33.9 ms.

Honest reading: the lifter changes are worth 3-6 % of this loop's CPU (they remove a quarter of its instructions, but the
ordered loads set its pace); what made Syobon hold its cap is the QoS class and the uncoalesced sleep. Headroom to the
title's 33 ms frame is ~10 % with strict ordering and ~55 % with the one relaxed function.

| title (after the rebase onto main 521c553, new AnyPS5 build, load ~20) | fps | frame p50 / p95 / p99 (ms) | flip-thread CPU / frame | process CPU / frame |
|---|---|---|---|---|
| Syobon (cap 30) | 30.2 | 33.1 / 36.5 / 45.0 (load 34) | 27.6 ms | 30.6 ms |
| Greenhollow (in-game, pad timeline, 45 s) | 59.9 | 16.70 / 17.13 / 18.2 | 0.55 ms | 4.9 ms |
| Calculator3D (25 s) | 59.9 | 16.70 / 17.10 / 18.3 | 0.56 ms | 3.3 ms |

### TSO sequences now

```
movzx eax, byte [rsi+0x24]      add x7, x28, x23 ; ldapurb w19, [x7, #36]
mov r9d, [rsi+0x14]             add x7, x28, x23 ; and x16, x7, #3 ; cbz x16, aligned
                                  (unaligned base: exact check, ldapr or ldr + dmb ishld)
                                aligned: ldapur w9, [x7, #20]
mov rax, [rdi+rcx*8]            add x16, x24, x21, lsl #3 ; add x7, x16, #7 ; eor x7, x7, x16 ; tbz x7, #4, ok ; ... ; ok: add x7, x28, x16 ; ldapr x19, [x7]
mov eax, [rip+d]                movz/movk x16 ; add x7, x28, x16 ; ldapr w19, [x7]        (alignment known at translate time)
```
`NPS5_NO_PEEPHOLE=1` (or `Arm64Lifter.SetPeephole(false)`) turns the pass and the folded forms off for A/B runs.

## 4. TSO: what was tried, what the switch is

Hardware TSO is not available to a native process (docs/m0/S1.md), so every non-stack scalar load is an acquire load, and
no ordered form is cheaper than ~1 cycle of throughput (section 2). Ideas rejected as unsound: one `dmb ishld` per group of
plain loads (loads inside the group can still pass each other: the message-passing litmus outcome), redundant-load
elimination across another load (r1=A; r2=B; r3=A must not reuse r1), hoisting. Sound but small and not done: plain loads
from read-only image segments with a constant base (jump tables; 3-4 of Syobon's ~45 loads per pixel. The blitter's
`SDL_expand_byte` pointer table is in `.data`).

`NPS5_TSO_RELAX_FUNCS=<entry>,<entry>` (translate time; `Arm64Lifter.SetTSORelax`; recorded as `tso_relaxed_functions` in
`lifter_manifest.json`) lifts the named functions with plain accesses and leaves the rest of the title ordered. It is a
per-title profile entry, default empty. Evidence a function needs before it goes on the list:
1. it is a leaf kernel: no calls, no `LOCK`-prefixed instruction or `XCHG`, no branch on a value another thread publishes;
2. its loads read data that is immutable for the duration of the call or that only feeds arithmetic stored back as data
   (pixels, samples, table entries), so the only effect of two loads passing each other is a value a racing writer could
   have produced anyway.
`0x61d9e0` in Syobon (PPSA47402) meets both: `SDL_Blit_Slow` reads an `SDL_BlitInfo`, two `SDL_PixelFormat`s, the static
expand tables and source pixels, and writes destination pixels. Frames 0 and 100 (title screen) are byte-identical between the main
build, the strict build and the relaxed build, frame 200 between strict and relaxed. The global `-tso=false` stays what it was: everything plain.

## 5. Other titles

* Greenhollow and Calculator3D stopped flipping after three frames on main between cda2f15 (merge of m3-gpufu) and 521c553
  (found and bisected here, fixed by the graphics branch: VM map-hook ordering). On the rebased branch both run at the
  display rate (table above). Calculator3D frame 0 is byte-identical to the Rosetta reference
  (`work/s3/rosetta/frames_x86/frame_000.bmp`). Before the fix, Greenhollow was also run with this branch's `pkg/lifter` on
  the 03ee3ac runtime next to the old build: 60.8 vs 63 fps over 18 s with the call trace on, game thread 2.0 vs 1.9 ms
  per frame, i.e. unchanged.
* Titles that flip from a command buffer never call `sceVideoOutSubmitFlip`; `sceAgcDcbSetFlip` is registered as a frame
  boundary too.
* In both AGC titles almost all sampled CPU is libsystem_kernel on host threads (draw-commit, queue worker, presenter):
  wake-ups and waits, 2-4 ms per frame. The game thread polls `sceVideoOutGetFlipStatus` with `sceKernelUsleep(100)`.
  Not looked at further.

## 6. Verification

* `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...`: green (`TestGuestVM` needs a quiet machine: its 120 s limit).
* New tests: `arm64_peephole_test.go` (shapes; a differential run of the memory corpus plus an alignment corpus - every base
  alignment in a 16-byte block, crossing accesses, loads/stores/sign extensions/RMW - with the pass on and off, TSO on and
  off, executed on the host), `TestTSORelaxPerFunction`, `TestRegressJumpDispatchKeepsFlags` (updated),
  `TestPreciseSleep`.
* `tools/m1_run.sh`: 38/38. `tools/memtests.sh --strict`: PASS 240, FAIL 0. FEX oracle (all 2291 tests): 0 regressions against the committed baseline (290 newly passing, from earlier merges; `tests/fex/results.json` not updated here).
* Syobon: title screen frames 0/100 byte-identical to the main build; gameplay with the pad timeline, 25-30 s runs.

## 7. What is left

* Syobon strict: 26-27 ms of CPU in a 33 ms frame, of which ~3 ms is the flip (present fence ~1 ms). ~10 % headroom; the
  ordered loads are the rest (section 2). The relax entry is the lever.
* Lifter, general (also the UE study, work/ue-arm-study/REPORT.md): the slow path of the checked access is still inline
  (code size, not executed instructions); call + return is ~15 host instructions; 8-bit `TEST`/`CMP` build flags with 3-4
  instructions; BEXTR 14, VZEROUPPER 16; an RBP-based access is treated as stack even when RBP is a general pointer
  (existing policy, it makes those loads plain).
* The CLI links with `-Wl,-x`: reports name shims as `shim@+offset` (cli.go belongs to the iterspeed branch).
* GPU time from Metal timestamps is not wired; AnyPS5's own `PerformanceTrace`/timing log (`APS5_ENABLE_TIMING_LOG`, off in
  the arm64 build) would give it.
* `ninja nps5_gpu` does not relink `libnps5_gpu.dylib` when `libnps5_VideoOut.a` changes (force-load dependency): delete the
  dylib first.
