// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure decision logic of the real-time-speed block ([framerate], nps5_anyps5.c), kept free of guest memory and clocks so
// tests/framerate/framerate_unit.c can drive it. One call per game frame (AGC flip).
//   freeze   while a video (sceAvPlayer) plays or the title is loading: no measurement, no rewrite; the block keeps its
//            last value. After the freeze ends a warm-up of warm_frames frames runs (EMA restarts from the block value).
//   smooth   EMA of the clamped instantaneous rate (alpha ema_alpha), then
//   hysteresis  a rewrite happens only when the smoothed rate differs from the block by more than hyst_frac for longer
//            than hyst_ns, continuously. (Near the limit the rate snaps to hi, as before.)
//   legacy   NPS5_FRAMERATE_LEGACY=1: the old behaviour (fast EMA, rewrite at >= 0.75 fps change, no freeze).
#pragma once
#include <math.h>
#include <stdint.h>

typedef struct {
  int legacy;
  double lo, hi, ema_alpha, hyst_frac;
  uint64_t hyst_ns;
  uint32_t warm_frames;
  // state
  uint64_t last_ns, off_since_ns;
  double ema, cur;
  uint32_t frames, warm, rewrites, frozen_frames;
  int was_frozen;
} Nps5Fr;

static inline void nps5_fr_defaults(Nps5Fr *f, double lo, double hi) {
  f->legacy = 0; f->lo = lo; f->hi = hi; f->ema_alpha = 0.02; f->hyst_frac = 0.05; f->hyst_ns = 1000000000ull; f->warm_frames = 60;
  f->last_ns = f->off_since_ns = 0; f->ema = f->cur = hi; f->frames = f->warm = f->rewrites = f->frozen_frames = 0; f->was_frozen = 0;
}

// Returns 1 when the block must be rewritten to *want.
static inline int nps5_fr_step(Nps5Fr *f, uint64_t now_ns, int frozen, double *want_out) {
  const uint64_t prev = f->last_ns;
  f->last_ns = now_ns;
  f->frames++;
  if (!f->legacy && frozen) {
    f->was_frozen = 1; f->frozen_frames++; f->off_since_ns = 0;
    return 0;
  }
  if (!f->legacy && f->was_frozen) { // first frame after a freeze: dt is meaningless; restart from the block value
    f->was_frozen = 0; f->warm = f->warm_frames; f->ema = f->cur; f->off_since_ns = 0;
    return 0;
  }
  if (!prev || now_ns <= prev) return 0;
  double inst = 1.0 / ((double)(now_ns - prev) * 1e-9);
  if (inst < f->lo) inst = f->lo;
  if (inst > f->hi) inst = f->hi;
  if (f->legacy) f->ema += (inst - f->ema) * (f->frames < 60 ? 0.2 : 0.05);
  else f->ema += (inst - f->ema) * (f->frames < 60 ? 0.2 : f->ema_alpha);
  if (!f->legacy && f->warm) { f->warm--; return 0; }
  double want = f->ema;
  if (want >= f->hi * 0.94) want = f->hi;
  if (want < f->lo) want = f->lo;
  if (f->legacy) {
    if (fabs(want - f->cur) >= 0.75 || (want == f->hi && f->cur != f->hi)) { *want_out = want; f->cur = want; f->rewrites++; return 1; }
    return 0;
  }
  if (fabs(want - f->cur) > f->hyst_frac * f->cur) {
    if (!f->off_since_ns) f->off_since_ns = now_ns;
    if (now_ns - f->off_since_ns > f->hyst_ns) { *want_out = want; f->cur = want; f->rewrites++; f->off_since_ns = 0; return 1; }
  } else f->off_since_ns = 0;
  return 0;
}
