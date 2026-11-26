# Guest clocks (branch m3-clock)

Trigger: the UE title calls `sceKernelGetProcessTimeCounter` about 113,000 times per frame, 17 ms of a frame. This
change makes the guest clocks one counter read plus an exact scale, and puts them on the lean bridge
(docs/m3/leanshim.md).

## What was slow

Per call, before: the full shim bridge (11-12 ns), `init_time()`, a 128-bit division inside `recomp_rdtsc()` (a call to
`__udivti3`, ~6 ns), a read of `cntfrq_el0` (3.2 ns: system-register reads of it are slow on the M4 Pro), and, when the
AnyPS5 backend is loaded (it always installs `nps5_set_guest_clock_credit(AgcDriverStallMicros)`), two more 128-bit
divisions and a call of the credit function, which takes a `std::mutex` and reads `steady_clock`. With 113k calls a frame
from several threads that mutex is contended: ~150 ns per call is what 17 ms / 113k works out to. The credit makes the
process clocks exclude the time the host spent compiling GPU state (a "first frame within 5 s" check in the title must
not fire on a cold cache).

## Time base

| | |
|---|---|
| source | `cntvct_el0`, one system-wide counter: monotonic across cores and threads, never adjusted |
| frequency | `cntfrq_el0`: **1 GHz on the M4 Pro this was measured on**, 24 MHz on M1-M3. Everything below works for any value |
| TSC the guest sees | `floor(cntvct * 2.13e9 / cntfrq)` (RECOMP_TSC_FREQ = 2.13 GHz, what `sceKernelGetTscFrequency` returns) |
| process time counter | the same scale of `cntvct - cntvct at startup`, in TSC ticks; `sceKernelGetProcessTimeCounterFrequency` = 2.13 GHz, so counter / frequency is seconds |
| `sceKernelGetProcessTime` | counter / 2130, exactly `floor(ticks * 1e6 / 2.13e9)` |
| `clock_gettime` MONOTONIC and UPTIME | counter in ns + an offset fixed at startup so the value starts at the host's `CLOCK_MONOTONIC`. Machine sleep is not counted, like the process time. Nothing in the runtime compares guest monotonic times with the host clock: every host absolute wait converts from `CLOCK_REALTIME`, which stays a host call (as does `gettimeofday`) |
| `RDTSC`, `RDTSCP`, `sceKernelReadTsc` | the unshifted TSC above (they must agree: `TestRdtscMatchesRuntimeTsc`) |

### The scale (`recomp_cnt_to_tsc`, recomp_context.h)

* 24 MHz: the ratio is exactly 355/4, so `(v * 355) >> 2` with a 64x64->128 multiply (`mul` + `umulh`) and an
  `extr`. The 128-bit product is shifted before it is cut to 64 bits: no overflow at any uptime (a 64-bit `v*355`
  would wrap at 2^64/355 ticks = 68 years).
* Any other frequency: the exact divmod form `q*F + (r*F)/frq` with `q, r = divmod(v, frq)`. Two hardware `udiv`s (3
  cycles each on Apple cores), products below 2^64 for `frq < 2^33`. Exact, so the lifter, the runtime and the tests
  agree to the tick.
* The 64-bit TSC itself wraps at 270 years of counter time (2^64 / 2.13e9 s); not a concern.
* `cntfrq_el0` is read once per translation unit (`recomp_cntfrq()`), not per call.

### The GPU-stall credit without the call

The credit function is sampled at most every 250 us (counter time) by whichever thread finds the cache expired
(`try-lock`, the others carry on with the old record), into a seqlock-protected record `{ec, floor, deadline}`:

* `ec`, the credit in TSC ticks: never above the elapsed ticks, never decreasing, capped at 120 s as before.
* `floor`, the largest process time any reader can have seen before the last credit increase, plus 1 us of slack.

A reader returns `max(ticks - ec, floor)`: when the credit steps up, the clock stands still until it catches up instead
of moving back, so it stays monotonic across threads **without a compare-and-swap on every read** (the old code CASed a
shared cache line on every call). The reader costs a counter read, three relaxed loads and a validation. A stale credit
lags the true one by at most 250 us.

## Lean bridge

`sceKernelGetProcessTimeCounter`, `...CounterFrequency`, `sceKernelGetProcessTime`, `sceKernelGetTscFrequency`,
`sceKernelReadTsc`, `clock_gettime`, `sceKernelClockGettime`, `gettimeofday`, `sceKernelGettimeofday` are
`leanHandwritten` (pkg/emitter/lean.go) with `NPS5_LEAN_FN` entries in modules/libkernel/ps4_kernel_sys.c. They are
integer leaves that never enter guest code. The full shims remain (`NPS5_LEAN=0`, the weak fallback).

### Why no call-site inlining in the lifter

The lifter does not know which import a `call` reaches (the stub address maps to a shim name only in the emitter), and
the lean call is now ~2.9 ns: bridge ~2 ns + 0.9 ns of work. An inlined `mrs` + scale would save about 2 ns per call,
113k calls = 0.2 ms of a frame, in exchange for coupling the lifter to import names while two other agents rewrite it.
Not done. If it is ever wanted, the clean place is the stub (guest section, emitter): a stub that does the
`mrs`/scale and pops the guest return address without the 16-GPR store (the shape of `writeLeanStub`).

### RDTSC lowering

`liftRdtsc` (arm64_lifter_alu.go) emits the same formula: `mul`/`umulh`/`extr` for 24 MHz, selected by a `cbz` on the
frequency (no flags are touched), the divmod pair otherwise. On the M4 Pro (1 GHz) it takes the divmod path and costs
the same as before, 3.2 ns, because the `mrs x17, cntfrq_el0` it has to read per instruction dominates (the old 128-bit
divide is hidden behind it by out-of-order execution). Avoiding it needs the frequency from memory (a `GuestContext`
field or a translate-time constant verified at startup); not done, since `GuestContext` offsets are shared with the other
lifter work. On 24 MHz parts it saves the division.

## Numbers

M4 Pro, shared machine, `tools/shimbench.sh` (guest -> import call, best/median of 3, flat call model), before = the
base commit (784f742) with the same benchmark; `tools/clock_unit.sh bench` for host-side calls.

| ns per call | before | after |
|---|---|---|
| guest `sceKernelGetProcessTimeCounter` | 16.1 | **2.9** |
| guest `sceKernelGetProcessTime` | 16.5 | **2.9** |
| guest `sceKernelReadTsc` call | 15.5 | **2.7** |
| guest `clock_gettime(CLOCK_MONOTONIC)` | 24.9 | **3.7** |
| guest `gettimeofday` | 21.2 | 13.1 (still the host call) |
| guest `RDTSC` instruction | 3.2 | 3.2 (dominated by the `cntfrq_el0` read) |
| host: process counter, no credit function | 7.6 | 1.1 |
| host: process counter with a mutex-based credit function installed | 14.8 | 3.0 |

The shimbench homebrew has no credit function. The title does, and there the old cost was dominated by the credit
call's mutex under contention (~150 ns per call in the game); the new path touches no lock. The host benchmark with the
credit installed is single-threaded; the contended case is not benchmarked.

## Tests

`tools/clock_unit.sh` (tests/clock/clock_unit.c; native arm64, includes ps4_kernel_sys.c to reach its statics; also
`tools/clock_unit.sh bench`):

* lean entries against the contract of the full shims (NULL pointers, errno, timespec/timeval/timezone layout).
* **monotonic across 8 threads**, 1 s, for the counter, the microsecond clock and CLOCK_MONOTONIC, and 3 s / 1 s with a
  fake credit function (mutex + host clock, 3 ms stall / 2 ms idle) stepping the credit: no thread ever sees its own
  clock go backwards (0 in every run).
* **frequency over 1 s of wall time**: counter / frequency, process microseconds, TSC and CLOCK_MONOTONIC against
  `CLOCK_UPTIME_RAW` within 0.1% (measured: under 0.001%).
* **overflow**: `recomp_cnt_to_tsc` against a 128-bit reference for 742k values (24 MHz and divmod frequencies, around
  2^64/355 and up to 2^63 ticks), monotonic and exact across the 64-bit wrap of `v*355`; the clocks with the counter
  skewed (`NPS5_CLOCK_TEST`) by 25 and 200 years at the real frequency and at 24 MHz.
* the lifter: `TestRdtscScaleExact` runs the lowering with chosen (counter, frequency) pairs, including the
  fallback and overflow-prone values, against the 128-bit reference; `TestRdtscMatchesRuntimeTsc` still holds.

Known limit, measured: a thread that learns a value another thread published (through memory) and then reads the clock
can get a value up to ~2 us (8-thread stress, all threads hammering one cache line) below the published one, because the
`mrs cntvct_el0` is not ordered with the earlier load that was waiting for the cache line. This is the same property
as `RDTSC` and as the old implementation; it never goes backwards within a thread, except while a GPU-stall credit steps: the seqlock loads between two unordered counter reads let a thread step back by up to ~5 us (measured 4.3 us worst, a handful of steps in 2e9 reads under 8-thread stress; the test tolerates 10 us). Building with `-DNPS5_CLOCK_ISB`
makes `cntvct_now` an `isb; mrs`, which removes it (0 in the same stress) at ~14 ns per read instead of ~1 (measured); not worth it for a game clock.

`shimbench` has new cases (`proc_time_counter`, `proc_time_us`, `read_tsc_call`, `rdtsc_insn`, `clock_gettime_mono`,
`gettimeofday`).
