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
#include <string.h>

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

// Per-level and live clamp (apesoft-2026-11-26, Apes on the Loose soft-lock at the 20 fps floor).
//   NPS5_FRAMERATE_MIN_LEVELS=<scene>=<fps>[,<scene>=<fps>...]  floor for a level while its scene is current ([scene-open]
//            names); unlisted scenes keep the configured floor. nps5_fr_level_floor returns that floor, def when the scene
//            is not listed (or tab is NULL/empty), and -2 when the table is malformed (the caller ignores it, logs once).
//   NPS5_FRAMERATE_FILE=<path>  live override, polled twice a second: "<min>" (floor) or "<min> <max>" / "<min>-<max>"
//            (e.g. "20 20" pins the laptop's 50 ms step, "60 60" = fixed 1/60). Missing / empty / unparsable = no override.
static inline int nps5_fr_name_ok(const char *p, size_t n) {
  if (!n) return 0;
  for (size_t i = 0; i < n; i++) {
    const char c = p[i];
    if (!(c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
  }
  return 1;
}
static inline double nps5_fr_level_floor(const char *tab, const char *scene, double def) {
  if (!tab || !*tab) return def;
  double hit = -1;
  const char *p = tab;
  for (;;) {
    const char *eq = strchr(p, '=');
    if (!eq || !nps5_fr_name_ok(p, (size_t)(eq - p))) return -2;
    char *end = NULL;
    const double v = strtod(eq + 1, &end);
    if (end == eq + 1 || (*end && *end != ',') || !(v >= 1.0 && v <= 240.0)) return -2;
    const size_t nl = (size_t)(eq - p);
    if (hit < 0 && scene && strlen(scene) == nl && !strncmp(scene, p, nl)) hit = v;
    if (!*end) break;
    p = end + 1;
  }
  return hit >= 0 ? hit : def;
}
// Live file text -> clamp. Returns 1 and sets *lo (and *hi when a max is given, else leaves it) when valid.
static inline int nps5_fr_live_parse(const char *t, double *lo, double *hi) {
  if (!t) return 0;
  while (*t == ' ' || *t == '\t') t++;
  char *end = NULL;
  const double l = strtod(t, &end);
  if (end == t || !(l >= 1.0 && l <= 240.0)) return 0;
  const char *q = end;
  while (*q == ' ' || *q == '\t') q++;
  int dash = 0;
  if (*q == '-') { q++; dash = 1; }
  while (*q == ' ' || *q == '\t') q++;
  double h = -1;
  if (dash && (!*q || *q == '\n' || *q == '\r')) return 0; // "30-": a max is missing
  if (*q && *q != '\n' && *q != '\r') {
    h = strtod(q, &end);
    if (end == q || !(h >= l && h <= 240.0)) return 0;
    q = end;
    while (*q == ' ' || *q == '\t') q++;
  }
  if (*q && *q != '\n' && *q != '\r') return 0;
  *lo = l;
  if (h > 0) *hi = h;
  return 1;
}
// The clamp that applies: a valid live file wins (file_lo > 0; file_hi > 0 also sets the max), else the level table
// floor for the scene (base_lo when unlisted or the table is malformed), with the configured max; lo never above hi.
static inline void nps5_fr_effective(const char *tab, const char *scene, double base_lo, double base_hi, double file_lo,
                                     double file_hi, double *lo, double *hi) {
  double l = file_lo > 0 ? file_lo : nps5_fr_level_floor(tab, scene, base_lo);
  const double h = file_lo > 0 && file_hi > 0 ? file_hi : base_hi;
  if (l < 0) l = base_lo;
  if (l > h) l = h;
  *lo = l; *hi = h;
}
// Keyboard hotkey (NPS5_FRAMERATE_HOTKEY=1 or a floor in fps 20..240; Cmd+Shift+F in the Metal window). Cycles
//   0 = configured (level table / live file / base clamp), 1 = floor <fps> (default 30), 2 = off (the base clamp only:
//   no level floor, no live file) -> 0. The hotkey beats the live file and the level table while it is not 0.
// nps5_fr_hotkey_cfg: env text -> floor fps, 0 = hotkey off (unset, "0", junk, or outside 20..240; "1" means 30).
static inline double nps5_fr_hotkey_cfg(const char *t) {
  if (!t || !*t) return 0;
  char *end = NULL;
  const double v = strtod(t, &end);
  if (end == t || *end) return 0;
  if (v == 1.0) return 30.0;
  return (v >= 20.0 && v <= 240.0) ? v : 0;
}
static inline int nps5_fr_hotkey_next(int s) { return (s < 0 || s >= 2) ? 0 : s + 1; }
// Two key handlers (the Metal window's and Kyty's SDL window) may both see one key press: a call within
// NPS5_FR_HOTKEY_DEBOUNCE_NS of the last accepted one is dropped. *last_ns is updated when the call is accepted.
#define NPS5_FR_HOTKEY_DEBOUNCE_NS 250000000ull
static inline int nps5_fr_hotkey_accept(unsigned long long *last_ns, unsigned long long now_ns) {
  if (*last_ns && now_ns >= *last_ns && now_ns - *last_ns < NPS5_FR_HOTKEY_DEBOUNCE_NS) return 0;
  *last_ns = now_ns ? now_ns : 1;
  return 1;
}
static inline void nps5_fr_effective_hk(const char *tab, const char *scene, double base_lo, double base_hi, double file_lo,
                                        double file_hi, int hk, double hk_fps, double *lo, double *hi) {
  if (hk == 1 && hk_fps > 0) { *hi = base_hi; *lo = hk_fps > base_hi ? base_hi : hk_fps; return; }
  if (hk == 2) { *lo = base_lo > base_hi ? base_hi : base_lo; *hi = base_hi; return; }
  nps5_fr_effective(tab, scene, base_lo, base_hi, file_lo, file_hi, lo, hi);
}
// Change the clamp at run time (level change or live file). lo is limited to hi. When the block value is outside the
// new range it must follow at once (entering a floor-30 level must not wait for the hysteresis): returns 1 with *want_out.
// Inside the range nothing is rewritten; the normal step moves it later (e.g. back down after leaving the level).
static inline int nps5_fr_set_clamp(Nps5Fr *f, double lo, double hi, double *want_out) {
  if (lo > hi) lo = hi;
  f->lo = lo; f->hi = hi; f->off_since_ns = 0;
  double want = f->cur;
  if (want < lo) want = lo;
  if (want > hi) want = hi;
  if (want == f->cur) return 0;
  f->cur = want; f->rewrites++; *want_out = want;
  return 1;
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
