// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure decision logic of the real-time-speed block ([framerate], nps5_anyps5.c), kept free of guest memory and clocks so
// tests/framerate/framerate_unit.c can drive it. One call per game frame (AGC flip).
//   freeze   while a video (sceAvPlayer) plays or the title is loading: no measurement, no rewrite; the block keeps its
//            last value. After the freeze ends a warm-up of warm_frames frames runs (EMA restarts from the block value).
//   smooth   EMA (alpha ema_alpha; 0.2 in the first 60 frames and in a warm-up) of the frame PERIOD, rate = 1 / period,
//            clamped to lo..hi. The period is linear in the flip times, so guest flips that come in bunches (16 ms, then
//            90 ms) average to the real rate; the old EMA of the clamped per-frame rate read 9-23 fps too high on them
//            (rebest_a1/a2, work/research/pacefix-2026-11-20.md). One interval counts at most max(3 periods, 2.5 / lo)
//            so a single compile stall does not move the estimate.
//   load     nps5_fr_load_step: a new level phase freezes for 5 s; VFS volume (> 6 MB in a second, +2 s) freezes for
//            at most load_max_ns per run of busy seconds, then counts as streaming (ignored) until 5 quiet seconds.
//            Without the cap, a slow game streams for longer and the freeze held the block for 30-50 s.
//   hysteresis  a rewrite happens only when the smoothed rate differs from the block by more than hyst_frac for longer
//            than hyst_ns, continuously. (Near the limit the rate snaps to hi, as before.)
//   legacy   NPS5_FRAMERATE_LEGACY=1: the old behaviour (fast EMA, rewrite at >= 0.75 fps change, no freeze).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  int legacy;
  double lo, hi, ema_alpha, hyst_frac;
  uint64_t hyst_ns;
  uint32_t warm_frames;
  // state
  uint64_t last_ns, off_since_ns;
  double ema, cur, period; // ema = 1 / period (rate, for logs and legacy); period in s (0 = start from 1 / ema)
  uint32_t frames, warm, rewrites, frozen_frames;
  int was_frozen;
} Nps5Fr;

static inline void nps5_fr_defaults(Nps5Fr *f, double lo, double hi) {
  f->legacy = 0; f->lo = lo; f->hi = hi; f->ema_alpha = 0.02; f->hyst_frac = 0.05; f->hyst_ns = 1000000000ull; f->warm_frames = 60;
  f->last_ns = f->off_since_ns = 0;
  f->ema = f->cur = hi;
  f->period = 0;
  f->frames = f->warm = f->rewrites = f->frozen_frames = 0;
  f->was_frozen = 0;
}

// Tuning from the environment texts (NULL = not set, the default stays): NPS5_FRAMERATE_EMA (0 < v <= 1), _HYST_PCT
// (0..50), _HYST_MS (0..60000), _WARM (0..36000 frames). A text that is not a number in its range is ignored: the
// default stays (a negative band would rewrite every second, a negative time or frame count converts to a huge
// unsigned value and the block would never follow again, an EMA weight above 1 oscillates). Returns how many were ignored.
static inline int nps5_fr_tune_one(const char *text, double lo, double hi, int lo_open, double *out) {
  if (!text) return 0;
  char *end = NULL;
  const double v = strtod(text, &end);
  if (end == text || *end != 0 || !(v >= lo && v <= hi) || (lo_open && v == lo)) return 1;
  *out = v;
  return 0;
}
static inline int nps5_fr_tune(Nps5Fr *f, const char *ema, const char *hyst_pct, const char *hyst_ms, const char *warm) {
  double a = f->ema_alpha, pct = f->hyst_frac * 100.0, ms = (double)f->hyst_ns / 1e6, w = (double)f->warm_frames;
  const int bad = nps5_fr_tune_one(ema, 0.0, 1.0, 1, &a) + nps5_fr_tune_one(hyst_pct, 0.0, 50.0, 0, &pct) +
                  nps5_fr_tune_one(hyst_ms, 0.0, 60000.0, 0, &ms) + nps5_fr_tune_one(warm, 0.0, 36000.0, 0, &w);
  f->ema_alpha = a; f->hyst_frac = pct / 100.0; f->hyst_ns = (uint64_t)(ms * 1e6); f->warm_frames = (uint32_t)w;
  return bad;
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
      f->was_frozen = 0;
      f->warm = f->warm_frames;
      f->ema = f->cur;
      f->period = 1.0 / f->cur;
      f->off_since_ns = 0;
      return 0;
  }
  if (!prev || now_ns <= prev) return 0;
  const double dt = (double)(now_ns - prev) * 1e-9;
  if (f->legacy) {
      double inst = 1.0 / dt;
      if (inst < f->lo) inst = f->lo;
      if (inst > f->hi) inst = f->hi;
      f->ema += (inst - f->ema) * (f->frames < 60 ? 0.2 : 0.05);
  } else {
      if (!(f->period > 0)) f->period = 1.0 / f->ema;
      const double cap = 3.0 * f->period > 2.5 / f->lo ? 3.0 * f->period : 2.5 / f->lo;
      f->period += ((dt < cap ? dt : cap) - f->period) * (f->frames < 60 || f->warm ? 0.2 : f->ema_alpha);
      f->ema = 1.0 / f->period;
  }
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

// Load freeze (pure; the caller samples the VFS phase and byte counter). One call per frame.
typedef struct {
    uint64_t max_ns; // longest VFS-volume freeze per run of busy seconds (0 = unlimited, the old behaviour)
    uint64_t bytes_ns, phase_until_ns, vfs_until_ns, run_since_ns, quiet_since_ns;
    unsigned long long last_bytes;
    int last_phase, streaming;
} Nps5FrLoad;
static inline void nps5_fr_load_init(Nps5FrLoad *s, uint64_t max_ns) {
    s->max_ns = max_ns;
    s->bytes_ns = s->phase_until_ns = s->vfs_until_ns = s->run_since_ns = s->quiet_since_ns = 0;
    s->last_bytes = 0;
    s->last_phase = 0;
    s->streaming = 0;
}
// Returns 0 = not loading, 1 = new level phase, 2 = VFS volume.
static inline int nps5_fr_load_step(Nps5FrLoad *s, uint64_t now, int phase, unsigned long long bytes) {
    if (phase != s->last_phase) {
        s->last_phase = phase;
        s->phase_until_ns = now + 5000000000ull;
    }
    if (now - s->bytes_ns >= 1000000000ull) {
        if (s->bytes_ns) {
            if (bytes - s->last_bytes > 6ull * 1024 * 1024) {
                s->quiet_since_ns = 0;
                if (!s->run_since_ns) s->run_since_ns = now;
                if (s->max_ns && now - s->run_since_ns > s->max_ns) s->streaming = 1;
                if (!s->streaming) s->vfs_until_ns = now + 2000000000ull;
            } else {
                if (!s->quiet_since_ns) s->quiet_since_ns = now;
                if (now - s->quiet_since_ns >= (s->streaming ? 5000000000ull : 2000000000ull)) {
                    s->run_since_ns = 0;
                    s->streaming = 0;
                }
            }
        }
        s->last_bytes = bytes;
        s->bytes_ns = now;
    }
    if (now < s->phase_until_ns) return 1;
    return now < s->vfs_until_ns ? 2 : 0;
}
