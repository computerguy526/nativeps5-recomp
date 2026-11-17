# The periodic freeze of PPSA30552 (branch m3-stall, AnyPS5 nps5-stall)

Symptom: in the level every 4th flip arrives 0.4-1.8 s after the previous one (p95 3.5 s on a loaded machine); the other three
flips follow within 20-40 ms. The process is idle during the gap, and the GPU is idle ~90 % of the time overall.

## What the gap is

Not a GPU, fence or label wait. Evidence (title runs with `APS5_TRACE_FLIP=1`, `NPS5_TRACE_CONDWAIT=<ms>`, `APS5_TRACE_SLOWWAIT=<ms>`,
thread samples taken with `sample` right at the start of a gap):

* All threads are parked in condition-variable / equeue waits; the AGC command-processor thread and the draw-commit thread are idle,
  the presenter waits for the next flip request. The render thread submits no command buffer for the whole gap.
* One guest thread loops `FPlatformProcess::Sleep(float)` (guest fn 0x40eed0 -> sceKernelUsleep) from a frame-pacing function
  (guest 0x8af4d0 / 0xd530a0) with requests of 83-784 ms, pairs summing to ~850 ms. In all 142 gaps > 250 ms of a run exactly
  one of these sleeps ends inside the gap, ~40 ms before the flip (the pool of job threads wakes, the render thread submits).
* The sleep is `max(0, deadline - now)`, `now = sceKernelGetProcessTimeCounter() * SecondsPerCycle`, and the deadline is built from
  a time stamp the title reads from VideoOut (vblank/flip status `processTimeCounter`).

## Root cause

AnyPS5 stamped the flip/vblank status with ITS OWN libkernel clock (`sceKernelGetProcessTimeCounter` = cntvct, 1 GHz on the M4;
`sceKernelReadTsc` raw cntvct), while the title reads the runtime's clocks (docs/m3/clock.md: 2.13 GHz ticks, GPU-stall credit
excluded). Counter / frequency is seconds only for one clock at a time; the title mixed the two, so the pacer's deadline ran at a
different rate and offset from its own clock and it slept for the difference.

## Fix

AnyPS5 libkernel exports `KernelSetGuestClock_nid_postfix(const KernelGuestClock*)` (Time.hpp); while a host clock is installed the
process-time and TSC exports (and so VideoOut's stamps and AMPR timestamps) return it. The runtime installs its own clocks at
AnyPS5 start-up (`nps5_anyps5_install_guest_clock`, `NPS5_ANYPS5_OWN_CLOCK=1` turns that off for A/B). Test:
AnyPS5 core/libs/tests/GuestClockHook.cpp (tools/nps5-clock-hook-test.sh builds and runs it on the arm64 host).

## Tracing added

`APS5_TRACE_FLIP=1`: `[flip]` line per flip (stage times), `[submit]` per SubmitDcb, `[vblank-clock]` stamp scale every 120 vblanks.
`APS5_TRACE_SLOWWAIT=<ms>`: slow equeue/sema/evf/cond waits. `NPS5_TRACE_CONDWAIT=<ms>`: guest cond waits (with the latest signal on
that cond) and sceKernelUsleep sleeps (with the guest caller chain).

## Result (coh title run, level, same machine, 120 s)

| | level flips p50 / p95 / max | flips > 250 ms | avg fps in the level | pacer sleeps >= 50 ms |
|---|---|---|---|---|
| before (c9: stamps on AnyPS5's clock) | 79 / 555 / 1208 ms | 112 of 369 | 5.4 | 314 (112 s of 120) |
| after (c11: stamps on the runtime's credited clock) | 37 / 39 / 41 ms | 0 of 731 | 26.7 | 0 |

(The one 58 s flip interval at the start of both runs is the level load.) The `[vblank-clock]` trace in the fixed run shows
`counter/us = 2130.0` and `freq = 2130000000`: the stamp and the title's own counter are in the same units on the same timeline.
Control leg (`NPS5_ANYPS5_OWN_CLOCK=1` on the same binary) was not run on the title (the title slot went to a user playtest).
