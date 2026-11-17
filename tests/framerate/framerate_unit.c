// Host test of the [framerate] decision logic (pkg/runtime/modules/libSceAnyPS5/nps5_framerate_logic.h). Build/run: tools/framerate_unit.sh
#include "../../pkg/runtime/modules/libSceAnyPS5/nps5_framerate_logic.h"
#include <stdio.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint64_t T;
// run n frames of dt_ms each; frozen flag fixed; returns rewrites that happened
static int run(Nps5Fr *f, int n, double dt_ms, int frozen) {
  int rw = 0; double w;
  for (int i = 0; i < n; i++) { T += (uint64_t)(dt_ms * 1e6); rw += nps5_fr_step(f, T, frozen, &w); }
  return rw;
}
static void init(Nps5Fr *f) { nps5_fr_defaults(f, 20, 60); f->cur = f->ema = 60; T = 1000000000ull; f->last_ns = T; }
int main(void) {
  Nps5Fr f;
  // steady 60 fps: no rewrites
  init(&f); CHECK(run(&f, 600, 1000.0 / 60, 0) == 0, "steady 60 fps rewrote");
  // freeze during video: wild frame times, zero rewrites, block untouched, ema untouched
  init(&f); run(&f, 100, 1000.0 / 60, 0);
  double c0 = f.cur;
  int rw = 0; for (int i = 0; i < 20; i++) { rw += run(&f, 30, 40.8, 1); rw += run(&f, 30, 16.7, 1); }
  CHECK(rw == 0 && f.cur == c0 && f.rewrites == 0, "rewrote during freeze (%d)", rw);
  // resume: first frames after freeze (warm-up) do not rewrite even at a different rate
  rw = run(&f, 59, 33.3, 0);
  CHECK(rw == 0, "rewrote during warm-up (%d)", rw);
  // sustained 30 fps afterwards: eventually follows, and only after >1 s outside the 5 % band
  rw = run(&f, 600, 33.3, 0);
  CHECK(rw >= 1 && f.cur < 40, "did not follow sustained slowdown (cur %.1f rw %d)", f.cur, rw);
  CHECK(rw <= 40, "too many rewrites (%d)", rw);
  // hysteresis: a 4 % dip never rewrites; a 1 s-short excursion (>5 %) does not either
  init(&f); CHECK(run(&f, 1200, 1000.0 / 57.6, 0) == 0 && f.rewrites == 0, "4%% dip rewrote (cur %.2f)", f.cur);
  init(&f); run(&f, 100, 1000.0 / 60, 0);
  rw = run(&f, 20, 1000.0 / 45, 0) ; rw += run(&f, 300, 1000.0 / 60, 0);
  CHECK(rw == 0, "short excursion rewrote (%d)", rw);
  // jittery video-like pattern (24.5/60/39.6 alternating bursts) without freeze stays quiet (smoothing)
  init(&f); rw = 0; for (int i = 0; i < 20; i++) { rw += run(&f, 15, 1000.0 / 24.5, 0); rw += run(&f, 15, 1000.0 / 60, 0); rw += run(&f, 15, 1000.0 / 39.6, 0); }
  CHECK(f.rewrites <= 12, "oscillating input rewrote %u times", f.rewrites);
  // legacy: rewrites readily, no freeze
  init(&f); f.legacy = 1; rw = 0; for (int i = 0; i < 20; i++) { rw += run(&f, 15, 1000.0 / 24.5, 1); rw += run(&f, 15, 1000.0 / 60, 1); }
  CHECK(rw > 5, "legacy did not rewrite (%d)", rw);
  // tuning texts: valid values are taken, anything else keeps the default (never a negative band, a wrapped time or
  // frame count, or an EMA weight outside (0, 1])
  init(&f);
  CHECK(nps5_fr_tune(&f, NULL, NULL, NULL, NULL) == 0 && f.ema_alpha == 0.02 && f.hyst_frac == 0.05 && f.hyst_ns == 1000000000ull && f.warm_frames == 60, "defaults changed");
  CHECK(nps5_fr_tune(&f, "0.1", "10", "250", "30") == 0 && f.ema_alpha == 0.1 && fabs(f.hyst_frac - 0.10) < 1e-12 && f.hyst_ns == 250000000ull && f.warm_frames == 30, "valid tuning not taken");
  CHECK(nps5_fr_tune(&f, "1", "0", "0", "0") == 0 && f.ema_alpha == 1.0 && f.hyst_frac == 0.0 && f.hyst_ns == 0 && f.warm_frames == 0, "range ends not taken");
  static const char *bad[] = {"-1", "nan", "inf", "-inf", "abc", "", "1e400", "5x", "-0.5", "1e9"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    init(&f);
    CHECK(nps5_fr_tune(&f, bad[i], bad[i], bad[i], bad[i]) == 4, "bad text '%s' not counted", bad[i]);
    CHECK(f.ema_alpha == 0.02 && f.hyst_frac == 0.05 && f.hyst_ns == 1000000000ull && f.warm_frames == 60, "bad text '%s' changed the tuning", bad[i]);
  }
  init(&f); CHECK(nps5_fr_tune(&f, "0", "51", "60001", "36001") == 4 && f.ema_alpha == 0.02 && f.warm_frames == 60, "out-of-range values taken");
  // with the tuning that garbage used to produce the block must still follow a sustained slowdown
  init(&f); nps5_fr_tune(&f, "7", "-5", "-1000", "-1"); run(&f, 100, 1000.0 / 60, 0);
  rw = run(&f, 600, 33.3, 0);
  CHECK(rw >= 1 && rw <= 40 && f.cur < 40, "garbage tuning: cur %.1f after a sustained 30 fps, %d rewrites", f.cur, rw);
  printf(fails ? "framerate_unit: %d FAILED\n" : "framerate_unit: all passed\n", fails);
  return fails != 0;
}
