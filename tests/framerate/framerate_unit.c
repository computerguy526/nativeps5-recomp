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
// Guest flip intervals (ms) from the flight recordings of BEST runs rebest_a1/a2 (bunched: short/long pairs) and the rebased
// lib's rebest_b1/b2 (even), work/ppsa21564/boot/logs/rebest_*.0.sigterm.flightrec, Sky Garden, 2026-11-20.
static const double TR_A1[] = {15.5, 54.1, 82.5, 44.6, 33.7, 15.6, 87.0, 38.9, 33.6, 16.8,  105.1,
                               42.3, 32.6, 15.7, 71.8, 74.7, 42.8, 35.9, 54.8, 32.3, 14.9,  104.0,
                               42.1, 34.5, 57.8, 32.8, 15.7, 95.2, 36.6, 34.7, 16.1, 100.9, 39.1};
static const double TR_A2[] = {34.3, 35.9, 20.4, 65.3, 24.5, 37.0, 30.3, 33.3, 61.5, 25.5, 36.2, 32.2, 61.5,
                               18.9, 36.9, 31.1, 66.5, 11.9, 40.4, 36.9, 40.9, 63.5, 22.6, 62.1, 18.0, 38.4,
                               70.5, 24.9, 52.6, 11.8, 37.8, 62.7, 8.8,  73.0, 14.7, 40.8, 59.8, 14.0, 39.8,
                               59.4, 11.0, 42.9, 64.3, 15.5, 63.5, 11.6, 39.1, 63.4, 12.6};
static const double TR_B1[] = {49.4, 43.7, 162.5, 44.7, 51.6, 47.4, 51.6, 47.0, 52.5, 46.3, 47.5, 50.9, 47.5,
                               49.1, 50.9, 61.3,  55.8, 58.1, 54.8, 48.9, 53.5, 47.1, 51.5, 43.9, 47.3, 44.6,
                               48.4, 47.2, 49.1,  43.7, 47.0, 45.6, 51.7, 54.0, 49.8, 54.1, 54.1};
static const double TR_B2[] = {36.9, 37.3, 34.6, 34.7, 34.1, 39.0, 41.3, 75.7, 36.0, 36.2, 34.4, 35.2,
                               34.9, 36.6, 37.8, 34.5, 32.9, 31.6, 31.0, 31.5, 33.8, 34.4, 35.7, 36.3,
                               34.5, 34.2, 36.0, 35.2, 34.1, 37.7, 33.8, 35.1, 37.2, 37.5, 35.2, 37.3,
                               35.4, 35.1, 37.4, 34.4, 34.1, 36.2, 34.5, 36.0, 36.0};
// upwin_b_nim1 (rebased lib + KYTY_NATIVE_INDIRECT_MESH=1), last 2 s of the run: even again, 17 fps (below the clamp).
static const double TR_N1[] = {50.1, 54.6, 53.5, 56.2, 66.6, 46.7, 58.0, 55.8, 56.8, 58.3, 115.3,
                               59.6, 56.6, 56.7, 59.9, 56.6, 57.8, 55.3, 57.6, 57.2, 54.8, 55.4,
                               56.8, 55.5, 54.4, 54.9, 53.7, 56.1, 54.7, 53.5, 55.3, 55.5, 53.5};
#define TRN(a) (int)(sizeof a / sizeof *a)
// Replays a trace (cycled) for `secs` seconds of wall time from a block of 60, unfrozen; returns the real flip rate. The block
// (f->cur) is what the game steps by: game speed = real / block.
static double run_trace(Nps5Fr *f, const double *tr, int n, double secs) {
    double w, sum = 0;
    int k = 0;
    for (double t = 0; t < secs * 1000; k++) {
        const double d = tr[k % n];
        T += (uint64_t)(d * 1e6);
        t += d;
        sum += d;
        nps5_fr_step(f, T, 0, &w);
    }
    return 1000.0 * k / sum;
}
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static void trace_case(const char *name, const double *tr, int n) {
    Nps5Fr f;
    init(&f);
    const double real = run_trace(&f, tr, n, 60), want = clampd(real, 20, 60);
    printf("trace %s: real %.1f fps, block %.1f, game speed %.0f %% (ema %.1f, %u rewrites)\n", name, real, f.cur,
           100 * real / f.cur, f.ema, f.rewrites);
    // the block must sit within the 5 % band (+1 fps of EMA wobble) of the real rate, clamped to 20..60
    CHECK(fabs(f.cur - want) <= 0.05 * want + 1.0,
          "trace %s: block %.1f, real %.1f fps (clamped %.1f): game speed %.0f %%", name, f.cur, real, want,
          100 * real / f.cur);
}

// apesoft: per-level floor + live clamp. Synthetic scene names only.
static void test_level_clamp(void) {
  const char *tab = "hero_ape_escape=30,other_level=45";
  CHECK(nps5_fr_level_floor(tab, "hero_ape_escape", 20) == 30, "listed level floor");
  CHECK(nps5_fr_level_floor(tab, "other_level", 20) == 45, "second entry");
  CHECK(nps5_fr_level_floor(tab, "hub_crashsite", 20) == 20, "unlisted level must keep the configured floor");
  CHECK(nps5_fr_level_floor(tab, "hero_ape", 20) == 20 && nps5_fr_level_floor(tab, "hero_ape_escape_2", 20) == 20, "prefix is not a match");
  CHECK(nps5_fr_level_floor(tab, "", 20) == 20 && nps5_fr_level_floor(tab, NULL, 20) == 20, "no scene");
  CHECK(nps5_fr_level_floor(NULL, "hero_ape_escape", 20) == 20 && nps5_fr_level_floor("", "hero_ape_escape", 25) == 25, "no table = other titles unchanged");
  static const char *bad[] = {"x", "=30", "a=", "a=0", "a=241", "a=30x", "a b=30", "a=30,", "a=30,,b=40", "a=-5", "lvl/x=30", "a=nan"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(nps5_fr_level_floor(bad[i], "a", 20) == -2, "malformed table '%s' accepted", bad[i]);
  double lo = -1, hi = -1;
  CHECK(nps5_fr_live_parse("30\n", &lo, &hi) == 1 && lo == 30 && hi == -1, "live floor only");
  lo = hi = -1; CHECK(nps5_fr_live_parse("20 20", &lo, &hi) == 1 && lo == 20 && hi == 20, "live pin");
  lo = hi = -1; CHECK(nps5_fr_live_parse(" 30-60\n", &lo, &hi) == 1 && lo == 30 && hi == 60, "live range");
  static const char *lbad[] = {"", "\n", "abc", "0", "241", "60 30", "30 x", "30 40 50", "30-", "-5"};
  for (unsigned i = 0; i < sizeof lbad / sizeof *lbad; i++) { lo = hi = -7; CHECK(nps5_fr_live_parse(lbad[i], &lo, &hi) == 0 && lo == -7 && hi == -7, "bad live text '%s' accepted", lbad[i]); }
  CHECK(nps5_fr_live_parse(NULL, &lo, &hi) == 0, "NULL live text");
  // effective clamp: file wins, then level table, then the configured one
  nps5_fr_effective(tab, "hero_ape_escape", 20, 60, -1, -1, &lo, &hi); CHECK(lo == 30 && hi == 60, "eff level");
  nps5_fr_effective(tab, "hub_crashsite", 20, 60, -1, -1, &lo, &hi); CHECK(lo == 20 && hi == 60, "eff other level");
  nps5_fr_effective(tab, "hero_ape_escape", 20, 60, 60, 60, &lo, &hi); CHECK(lo == 60 && hi == 60, "eff file pin");
  nps5_fr_effective(tab, "hero_ape_escape", 20, 60, 25, -1, &lo, &hi); CHECK(lo == 25 && hi == 60, "eff file floor below the level floor still wins");
  nps5_fr_effective(tab, "other_level", 20, 40, -1, -1, &lo, &hi); CHECK(lo == 40 && hi == 40, "floor limited to max");
  nps5_fr_effective("garbage", "hero_ape_escape", 20, 60, -1, -1, &lo, &hi); CHECK(lo == 20 && hi == 60, "malformed table = configured");

  // synthetic session: hub at 15 fps (block at the 20 floor) -> level open (floor 30, immediate) -> 15 fps in the level
  // (block stays 30) -> an unlisted level (floor 20 again, block follows down after the hysteresis) -> back.
  Nps5Fr f; double w = 0;
  init(&f);
  run(&f, 300, 1000.0 / 15, 0);
  CHECK(f.cur == 20, "hub: block at the floor (cur %.1f)", f.cur);
  nps5_fr_effective(tab, "hero_ape_escape", 20, 60, -1, -1, &lo, &hi);
  CHECK(nps5_fr_set_clamp(&f, lo, hi, &w) == 1 && w == 30 && f.cur == 30, "level open must raise the block at once (w %.1f)", w);
  CHECK(nps5_fr_set_clamp(&f, lo, hi, &w) == 0, "same clamp again must not rewrite");
  const uint32_t r0 = f.rewrites;
  run(&f, 600, 1000.0 / 15, 0);
  CHECK(f.cur == 30 && f.rewrites == r0, "in level at 15 fps: block must stay at 30 (cur %.1f, %u rewrites)", f.cur, f.rewrites - r0);
  run(&f, 60, 1000.0 / 15, 1);  // a video freeze inside the level keeps 30
  run(&f, 300, 1000.0 / 15, 0);
  CHECK(f.cur == 30, "after a video in the level (cur %.1f)", f.cur);
  nps5_fr_effective(tab, "hub_crashsite", 20, 60, -1, -1, &lo, &hi);
  CHECK(nps5_fr_set_clamp(&f, lo, hi, &w) == 0 && f.lo == 20, "leaving lowers the floor without an immediate rewrite");
  run(&f, 300, 1000.0 / 15, 0);
  CHECK(f.cur == 20, "after leaving: block back at 20 (cur %.1f)", f.cur);
  // fast machine in the level: real 45 fps is above the 30 floor -> the block follows the real rate, not the floor
  init(&f);
  nps5_fr_effective(tab, "hero_ape_escape", 20, 60, -1, -1, &lo, &hi);
  nps5_fr_set_clamp(&f, lo, hi, &w);
  run(&f, 900, 1000.0 / 45, 0);
  CHECK(fabs(f.cur - 45) <= 3, "45 fps in the level: block follows the real rate (cur %.1f)", f.cur);
  // live pin 20..20 at 25 fps (M5 at the laptop's step): immediate, held
  init(&f);
  run(&f, 300, 1000.0 / 25, 0);
  CHECK(nps5_fr_set_clamp(&f, 20, 20, &w) == 1 && f.cur == 20, "pin 20 (cur %.1f)", f.cur);
  run(&f, 300, 1000.0 / 25, 0);
  CHECK(f.cur == 20, "pin holds at 25 fps (cur %.1f)", f.cur);
  CHECK(nps5_fr_set_clamp(&f, 60, 60, &w) == 1 && f.cur == 60, "pin 60");
  run(&f, 300, 1000.0 / 25, 0);
  CHECK(f.cur == 60, "pin 60 holds at 25 fps (cur %.1f)", f.cur);
  CHECK(nps5_fr_set_clamp(&f, 90, 60, &w) == 0 && f.lo == 60, "lo above hi is limited");
}

// apefloor hotkey: cycle + interaction with the level table and the live file. Synthetic scene names only.
static void test_hotkey(void) {
  CHECK(nps5_fr_hotkey_cfg(NULL) == 0 && nps5_fr_hotkey_cfg("") == 0 && nps5_fr_hotkey_cfg("0") == 0, "hotkey off by default");
  CHECK(nps5_fr_hotkey_cfg("1") == 30 && nps5_fr_hotkey_cfg("25") == 25 && nps5_fr_hotkey_cfg("240") == 240, "hotkey cfg");
  CHECK(nps5_fr_hotkey_cfg("19") == 0 && nps5_fr_hotkey_cfg("241") == 0 && nps5_fr_hotkey_cfg("x") == 0 && nps5_fr_hotkey_cfg("30x") == 0 &&
            nps5_fr_hotkey_cfg("-30") == 0, "hotkey cfg junk");
  int s = 0, seq[7]; for (int i = 0; i < 7; i++) { s = nps5_fr_hotkey_next(s); seq[i] = s; }
  CHECK(seq[0] == 1 && seq[1] == 2 && seq[2] == 0 && seq[3] == 1 && seq[5] == 0 && seq[6] == 1, "cycle default -> floor -> off -> default");
  CHECK(nps5_fr_hotkey_next(-1) == 0 && nps5_fr_hotkey_next(9) == 0, "bad state resets");
  const char *tab = "lvl_a=45"; double lo, hi;
  for (int st = 0; st < 3; st++) {
    nps5_fr_effective_hk(NULL, "x", 20, 60, -1, -1, st, 30, &lo, &hi);
    CHECK(lo == (st == 1 ? 30 : 20) && hi == 60, "plain clamp state %d (lo %.0f)", st, lo);
  }
  // state 0 = exactly the old behaviour (table, file)
  nps5_fr_effective_hk(tab, "lvl_a", 20, 60, -1, -1, 0, 30, &lo, &hi); CHECK(lo == 45 && hi == 60, "state 0 keeps the level table");
  nps5_fr_effective_hk(tab, "lvl_a", 20, 60, 25, 25, 0, 30, &lo, &hi); CHECK(lo == 25 && hi == 25, "state 0 keeps the live file");
  // state 1 beats table and file; state 2 ignores both
  nps5_fr_effective_hk(tab, "lvl_a", 20, 60, 25, 25, 1, 30, &lo, &hi); CHECK(lo == 30 && hi == 60, "floor beats file and table");
  nps5_fr_effective_hk(tab, "lvl_a", 20, 60, 25, 25, 2, 30, &lo, &hi); CHECK(lo == 20 && hi == 60, "off = base clamp");
  nps5_fr_effective_hk(tab, "lvl_a", 20, 40, -1, -1, 1, 50, &lo, &hi); CHECK(lo == 40 && hi == 40, "floor limited to the max");
  nps5_fr_effective_hk(tab, "lvl_a", 20, 60, -1, -1, 1, 0, &lo, &hi); CHECK(lo == 45, "no hotkey fps = ignored");
  // synthetic session: 15 fps real; floor on -> block raised at once; off -> block falls back after the hysteresis
  Nps5Fr f; double w = 0;
  init(&f);
  run(&f, 300, 1000.0 / 15, 0); CHECK(f.cur == 20, "start at base floor");
  nps5_fr_effective_hk(NULL, "x", 20, 60, -1, -1, 1, 30, &lo, &hi);
  CHECK(nps5_fr_set_clamp(&f, lo, hi, &w) == 1 && w == 30 && f.cur == 30, "hotkey floor raises the block at once");
  run(&f, 300, 1000.0 / 15, 0); CHECK(f.cur == 30, "block stays at 30 (cur %.1f)", f.cur);
  nps5_fr_effective_hk(NULL, "x", 20, 60, -1, -1, 2, 30, &lo, &hi);
  CHECK(nps5_fr_set_clamp(&f, lo, hi, &w) == 0, "off does not rewrite at once");
  run(&f, 600, 1000.0 / 15, 0); CHECK(f.cur == 20, "block follows back to the base floor (cur %.1f)", f.cur);
}
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
  // bunched guest flips (BEST) and even flips (rebased lib): the block follows the real flip rate in both
  trace_case("A1", TR_A1, TRN(TR_A1));
  trace_case("A2", TR_A2, TRN(TR_A2));
  trace_case("B1", TR_B1, TRN(TR_B1));
  trace_case("B2", TR_B2, TRN(TR_B2));
  trace_case("N1", TR_N1, TRN(TR_N1));
  // a single 1.3 s pipeline-compile stall in a steady 60 or 30 fps does not rewrite the block
  init(&f);
  run(&f, 300, 1000.0 / 60, 0);
  rw = run(&f, 1, 1300, 0) + run(&f, 600, 1000.0 / 60, 0);
  CHECK(rw == 0 && f.cur == 60, "60 fps + one stall rewrote (%d, cur %.1f)", rw, f.cur);
  init(&f);
  run(&f, 600, 1000.0 / 30, 0);
  c0 = f.cur;
  rw = run(&f, 1, 1300, 0) + run(&f, 600, 1000.0 / 30, 0);
  CHECK(rw == 0 && f.cur == c0, "30 fps + one stall rewrote (%d, cur %.1f -> %.1f)", rw, c0, f.cur);
  // sudden drop 60 -> 15 fps (below the clamp): follows down to lo
  init(&f);
  run(&f, 300, 1000.0 / 60, 0);
  run(&f, 300, 1000.0 / 15, 0);
  CHECK(f.cur == 20, "60 -> 15 fps: cur %.1f", f.cur);
  // load freeze: a phase freezes 5 s; VFS volume freezes +2 s after each busy second, at most max_ns per run, then it is
  // streaming (ignored) until 5 quiet seconds; a real load after that gets the freeze again
  {
      Nps5FrLoad L;
      nps5_fr_load_init(&L, 8000000000ull);
      uint64_t t = 1000000000ull;
      unsigned long long by = 0;
      int r = 0, ph = 0, n2 = 0;
      for (int i = 0; i < 60; i++, t += 50000000ull)
          r = nps5_fr_load_step(&L, t, ph, by); // 3 s idle
      CHECK(r == 0, "idle froze (%d)", r);
      ph = 1;
      r = nps5_fr_load_step(&L, t, ph, by);
      CHECK(r == 1, "new phase not frozen (%d)", r);
      for (int i = 0; i < 120; i++, t += 50000000ull)
          r = nps5_fr_load_step(&L, t, ph, by); // +6 s quiet
      CHECK(r == 0, "phase freeze longer than 5 s (%d)", r);
      for (int i = 0; i < 600; i++, t += 50000000ull) {
          by += 1024 * 1024;
          n2 += nps5_fr_load_step(&L, t, ph, by) == 2;
      } // 30 s of 20 MB/s
      CHECK(L.streaming && n2 >= 7 * 20 && n2 <= 11 * 20, "streaming: frozen %d frames of 600 (streaming %d)", n2,
            L.streaming);
      CHECK(nps5_fr_load_step(&L, t, ph, by) == 0, "streaming still frozen");
      for (int i = 0; i < 140; i++, t += 50000000ull)
          r = nps5_fr_load_step(&L, t, ph, by); // 7 s quiet: re-armed
      CHECK(!L.streaming && r == 0, "not re-armed after quiet (%d)", L.streaming);
      for (int i = 0; i < 60; i++, t += 50000000ull) {
          by += 1024 * 1024;
          r = nps5_fr_load_step(&L, t, ph, by);
      }
      CHECK(r == 2, "load after re-arm not frozen (%d)", r);
      nps5_fr_load_init(&L, 0);
      n2 = 0; // max 0 = the old, unlimited freeze
      for (int i = 0; i < 600; i++, t += 50000000ull) {
          by += 1024 * 1024;
          n2 += nps5_fr_load_step(&L, t, ph, by) != 0;
      }
      CHECK(n2 >= 590, "max 0 did not keep the old unlimited freeze (%d)", n2);
  }
  // the trap of rebest_a1: A1's bunched flips while the level streams 20 MB/s for 60 s. Old (unlimited freeze): the block
  // stays 60 (game speed ~35 %). New: freeze for 8 s, then the block follows the real rate.
  for (int cap = 0; cap < 2; cap++) {
      Nps5FrLoad L;
      nps5_fr_load_init(&L, cap ? 8000000000ull : 0);
      init(&f);
      unsigned long long by = 0;
      double w, sum = 0;
      int k = 0;
      for (double tt = 0; tt < 60000; k++) {
          const double d = TR_A1[k % TRN(TR_A1)];
          T += (uint64_t)(d * 1e6);
          tt += d;
          sum += d;
          by += (unsigned long long)(d * 20 * 1024 * 1024 / 1000);
          nps5_fr_step(&f, T, nps5_fr_load_step(&L, T, 1, by), &w);
      }
      const double real = 1000.0 * k / sum;
      printf("trap A1 + streaming, load cap %s: real %.1f fps, block %.1f, game speed %.0f %%\n", cap ? "8 s" : "none",
             real, f.cur, 100 * real / f.cur);
      if (cap)
          CHECK(fabs(f.cur - real) <= 0.05 * real + 1.0, "trap: block %.1f real %.1f", f.cur, real);
      else
          CHECK(f.cur == 60, "old unlimited freeze should hold 60 (cur %.1f)", f.cur);
  }
  test_level_clamp();
  test_hotkey();
  printf(fails ? "framerate_unit: %d FAILED\n" : "framerate_unit: all passed\n", fails);
  return fails != 0;
}
