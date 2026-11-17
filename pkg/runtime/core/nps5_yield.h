// Adaptive back-off for guest spin-yield loops (scePthreadYield / sched_yield).
//
// Astro Bot guest threads wait with `while (!cond) { scePthreadYield(); sleep(0); }` and call yield 60-95k times per
// frame, burning P cores that the GPU driver's commit / queue workers need. A yield call that follows the previous one
// by less than `gap_ns` belongs to a spin run. The first `after` yields of a run stay plain sched_yield (fast
// hand-off); from then on each further yield of the run waits `cur` ns, with cur doubling from `min_ns` up to `cap_ns`.
// A gap longer than gap_ns (the thread did real work or slept) ends the run and the next yield is plain again, so the
// worst added hand-off latency is cap_ns. Pure logic: no clocks, no syscalls (unit-tested in tests/threading).
#ifndef NPS5_YIELD_H
#define NPS5_YIELD_H
#include <stdint.h>

typedef struct {
  uint64_t cap_ns;  // 0 = disabled (plain sched_yield always)
  uint64_t min_ns;  // first back-off wait
  uint64_t gap_ns;  // yields closer than this are one spin run
  uint32_t after;   // plain yields at the start of a run
} nps5_yield_cfg;

typedef struct {
  uint64_t last_ns; // time the previous yield call finished
  uint64_t cur_ns;  // next wait of this run (0 before the back-off starts)
  uint32_t run;     // yields in the current run
} nps5_yield_state;

// Returns the number of ns to wait instead of sched_yield (0 = plain sched_yield). `now` = time of this call.
static inline uint64_t nps5_yield_step(const nps5_yield_cfg *c, nps5_yield_state *s, uint64_t now) {
  if (c->cap_ns == 0) return 0;
  if (now - s->last_ns < c->gap_ns) {
    if (s->run < 0xffffffffu) s->run++;
  } else {
    s->run = 0;
    s->cur_ns = 0;
  }
  if (s->run < c->after) return 0;
  if (s->cur_ns == 0) s->cur_ns = c->min_ns ? c->min_ns : 1000;
  else if (s->cur_ns < c->cap_ns) { s->cur_ns *= 2; }
  if (s->cur_ns > c->cap_ns) s->cur_ns = c->cap_ns;
  return s->cur_ns;
}
#endif
