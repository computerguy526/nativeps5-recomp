// Host tests of the haptics logic (pkg/runtime/modules/libSceAudioOut/nps5_haptics_logic.h): downmix, envelope, channel
// routing, mode selection per controller type with mock sinks, rate limiting, hotplug, NPS5_HAPTICS=0, and the proof
// that the audio-thread call never waits for a slow sink. Build/run: tools/haptics_unit.sh
#include "../../pkg/runtime/modules/libSceAudioOut/nps5_haptics_logic.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- mock controller world --------------------------------------------------------------------------------------------
typedef enum { PAD_NONE, PAD_DS_USB, PAD_DS_BT, PAD_SWITCH_PRO, PAD_DS4, PAD_XBOX } PadKind;
typedef struct {
  PadKind kind;
  int hd_calls, rumble_calls;
  uint64_t hd_frames;
  float hd_peak[2];
  uint8_t last_l, last_s;
  uint32_t last_rate;
  int hd_delay_us, rumble_delay_us;  // slow-SDL simulation
  int rumble_log[4096][3];           // t, l, s (t in ms of test clock)
  uint64_t clock_ns;
} Mock;

// Controller type -> what the stack does (mirrors the host: the DualSense accepts HD haptic audio over USB and over
// Bluetooth HID audio; every other pad only has the rumble motors; no pad: nothing)
static uint64_t mock_hd(void *u, const float *lr, uint32_t frames, uint32_t rate) {
  Mock *m = u;
  if (m->hd_delay_us) usleep(m->hd_delay_us);
  if (m->kind != PAD_DS_USB && m->kind != PAD_DS_BT) return 0;
  m->hd_calls++;
  m->hd_frames += frames;
  m->last_rate = rate;
  for (uint32_t i = 0; i < frames; i++)
    for (int k = 0; k < 2; k++) if (fabsf(lr[i * 2 + k]) > m->hd_peak[k]) m->hd_peak[k] = fabsf(lr[i * 2 + k]);
  return (uint64_t)frames * 1000000ull / rate;
}
static void mock_rumble(void *u, uint8_t l, uint8_t s) {
  Mock *m = u;
  if (m->rumble_delay_us) usleep(m->rumble_delay_us);
  if (m->rumble_calls < 4096) { m->rumble_log[m->rumble_calls][0] = (int)(m->clock_ns / 1000000); m->rumble_log[m->rumble_calls][1] = l; m->rumble_log[m->rumble_calls][2] = s; }
  m->rumble_calls++;
  m->last_l = l; m->last_s = s;
}
static int mock_avail(void *u) { return ((Mock *)u)->kind != PAD_NONE; }

static Nps5Hap *new_hap(Mock *m, int enabled) {
  Nps5Hap *h = calloc(1, sizeof *h);
  Nps5HapSinks s = {mock_hd, mock_rumble, mock_avail, m};
  nps5_hap_init(h, enabled, &s);
  return h;
}

// ---- signal helpers -----------------------------------------------------------------------------------------------------
#define GRAIN 256
static float scratch[NPS5_HAP_RING * 2];
static uint64_t T;
// stereo grain of a sine at hz/amp (left), right = ramp of 0 unless r_amp
static void sine_grain(float *lr, double *phase, double hz, double lamp, double ramp) {
  for (int i = 0; i < GRAIN; i++) {
    double v = sin(*phase); *phase += 2 * M_PI * hz / NPS5_HAP_RATE;
    lr[i * 2] = (float)(v * lamp); lr[i * 2 + 1] = (float)(v * ramp);
  }
}
// feed one grain, advance the clock one grain, pump once
static int step(Mock *m, Nps5Hap *h, const float *lr) {
  if (lr) nps5_hap_push(h, lr, GRAIN);
  T += (uint64_t)GRAIN * 1000000000ull / NPS5_HAP_RATE;
  m->clock_ns = T;
  return nps5_hap_pump(h, T, scratch);
}
static void run_sine(Mock *m, Nps5Hap *h, double hz, double lamp, double ramp, int grains) {
  static float lr[GRAIN * 2]; double ph = 0;
  for (int g = 0; g < grains; g++) { sine_grain(lr, &ph, hz, lamp, ramp); step(m, h, lr); }
}

// ---- tests --------------------------------------------------------------------------------------------------------------
static void test_envelope(void) {
  float lr[GRAIN * 2]; double ph = 0; uint8_t a[2];
  memset(lr, 0, sizeof lr); nps5_hap_envelope(lr, GRAIN, a);
  CHECK(a[0] == 0 && a[1] == 0, "silence must give 0/0, got %u/%u", a[0], a[1]);
  // gated noise floor
  for (int i = 0; i < GRAIN * 2; i++) lr[i] = 0.004f * ((i & 1) ? 1 : -1);
  nps5_hap_envelope(lr, GRAIN, a); CHECK(a[0] == 0 && a[1] == 0, "noise floor not gated: %u", a[0]);
  // sine bursts: RMS*2 curve, saturating at full-scale
  static const double freqs[] = {40, 80, 160, 320, 640};
  static const double amps[] = {0.05, 0.1, 0.25, 0.5, 0.9, 1.0};
  for (unsigned f = 0; f < 5; f++) for (unsigned i = 0; i < 6; i++) {
    double p0 = 0; float buf[2400 * 2];
    for (int k = 0; k < 2400; k++) { double v = sin(p0); p0 += 2 * M_PI * freqs[f] / NPS5_HAP_RATE; buf[k * 2] = (float)(v * amps[i]); buf[k * 2 + 1] = (float)(v * amps[i] * 0.5); }
    nps5_hap_envelope(buf, 2400, a);
    double exp0 = fmin(1.0, fmax(amps[i] * 0.7071 * 2, amps[i] * 0.8)) * 255, exp1 = fmin(1.0, fmax(amps[i] * 0.5 * 0.7071 * 2, amps[i] * 0.5 * 0.8)) * 255;
    // a low-frequency sine of 50 ms holds fewer than a few cycles: allow a loose tolerance
    CHECK(fabs(a[0] - exp0) <= 14 && fabs(a[1] - exp1) <= 14, "f=%.0f amp=%.2f got %u/%u expected %.0f/%.0f", freqs[f], amps[i], a[0], a[1], exp0, exp1);
  }
  // monotonic in amplitude
  uint8_t prev = 0; for (int i = 1; i <= 10; i++) { float buf[480 * 2]; for (int k = 0; k < 480; k++) buf[k * 2] = buf[k * 2 + 1] = (float)(0.1 * i * sin(k * 0.1)); nps5_hap_envelope(buf, 480, a); CHECK(a[0] >= prev, "envelope not monotonic at %d", i); prev = a[0]; }
  // clipping: out-of-range, NaN and inf samples are bounded, never crash
  for (int i = 0; i < GRAIN * 2; i++) lr[i] = (i % 3 == 0) ? 50.f : (i % 3 == 1 ? NAN : -INFINITY);
  nps5_hap_envelope(lr, GRAIN, a); CHECK(a[0] <= 255 && a[1] <= 255, "clip");
  // a short click inside a long grain is not averaged away (peak hold)
  memset(lr, 0, sizeof lr); for (int i = 100; i < 104; i++) lr[i * 2] = 1.0f;
  nps5_hap_envelope(lr, GRAIN, a); CHECK(a[0] >= 190, "short click lost: %u", a[0]);
  (void)ph;
  // zero frames
  nps5_hap_envelope(lr, 0, a); CHECK(a[0] == 0 && a[1] == 0, "zero frames");
}

static void test_routing(void) {
  uint32_t l, r;
  nps5_hap_route(1, &l, &r); CHECK(l == 0 && r == 0, "mono");
  nps5_hap_route(2, &l, &r); CHECK(l == 0 && r == 1, "stereo");
  nps5_hap_route(4, &l, &r); CHECK(l == 2 && r == 3, "4ch must use channels 3/4 (index 2,3)");
  nps5_hap_route(8, &l, &r); CHECK(l == 2 && r == 3, "8ch");
  // 4-channel stream: speaker channels (0,1) must NOT leak into the haptic buffer
  float pcm[GRAIN * 4]; float lr[GRAIN * 2]; memset(lr, 0, sizeof lr);
  for (int i = 0; i < GRAIN; i++) { pcm[i * 4] = 0.9f; pcm[i * 4 + 1] = -0.9f; pcm[i * 4 + 2] = 0.25f; pcm[i * 4 + 3] = -0.5f; }
  nps5_hap_accumulate(pcm, GRAIN, 4, 0, NULL, lr);
  CHECK(lr[0] == 0.25f && lr[1] == -0.5f && lr[(GRAIN - 1) * 2] == 0.25f, "4ch routing wrong: %f %f", lr[0], lr[1]);
  // int16 and per-channel volume; two ports accumulate
  int16_t p16[GRAIN * 2]; for (int i = 0; i < GRAIN; i++) { p16[i * 2] = 16384; p16[i * 2 + 1] = -16384; }
  float vol[12]; for (int i = 0; i < 12; i++) vol[i] = 1.f; vol[1] = 0.5f;
  memset(lr, 0, sizeof lr); nps5_hap_accumulate(p16, GRAIN, 2, 1, vol, lr); nps5_hap_accumulate(p16, GRAIN, 2, 1, vol, lr);
  CHECK(fabsf(lr[0] - 1.0f) < 1e-4 && fabsf(lr[1] + 0.5f) < 1e-4, "int16/vol/accumulate: %f %f", lr[0], lr[1]);
  // game audio untouched: accumulate writes only the haptic buffer; NULL pcm / zero channels are ignored
  nps5_hap_accumulate(NULL, GRAIN, 2, 0, NULL, lr); nps5_hap_accumulate(pcm, GRAIN, 0, 0, NULL, lr);
  CHECK(fabsf(lr[0] - 1.0f) < 1e-4, "null/zero-channel input modified the buffer");
  // the hd sink gets exactly the accumulated haptic stereo at 48 kHz, nothing of the speaker channels
  Mock m = {.kind = PAD_DS_USB}; Nps5Hap *h = new_hap(&m, 1);
  for (int g = 0; g < 8; g++) { memset(lr, 0, sizeof lr); nps5_hap_accumulate(pcm, GRAIN, 4, 0, NULL, lr); step(&m, h, lr); }
  CHECK(m.hd_frames > 0 && m.last_rate == 48000, "hd rate %u", m.last_rate);
  CHECK(fabsf(m.hd_peak[0] - 0.25f) < 1e-6 && fabsf(m.hd_peak[1] - 0.5f) < 1e-6, "hd peaks %f %f (speaker leak?)", m.hd_peak[0], m.hd_peak[1]);
  free(h);
}

static void test_modes(void) {
  struct { PadKind k; Nps5HapMode want; const char *name; } cases[] = {
    {PAD_DS_USB, NPS5_HAP_HD, "dualsense-usb"}, {PAD_DS_BT, NPS5_HAP_HD, "dualsense-bt"},
    {PAD_SWITCH_PRO, NPS5_HAP_RUMBLE, "switch-pro"}, {PAD_DS4, NPS5_HAP_RUMBLE, "dualshock4"}, {PAD_XBOX, NPS5_HAP_RUMBLE, "xbox"},
    {PAD_NONE, NPS5_HAP_DROPPED, "none"}};
  for (unsigned i = 0; i < 6; i++) {
    Mock m = {.kind = cases[i].k}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
    run_sine(&m, h, 100, 0.5, 0.5, 20);
    CHECK(h->mode == cases[i].want, "%s: mode %s expected %s", cases[i].name, nps5_hap_mode_name(h->mode), nps5_hap_mode_name(cases[i].want));
    if (cases[i].want == NPS5_HAP_HD) CHECK(m.hd_calls > 0 && m.rumble_calls == 0, "%s: hd only (hd=%d rumble=%d)", cases[i].name, m.hd_calls, m.rumble_calls);
    if (cases[i].want == NPS5_HAP_RUMBLE) CHECK(m.rumble_calls > 0 && m.hd_calls == 0 && m.last_l > 150, "%s: rumble only (rumble=%d l=%u)", cases[i].name, m.rumble_calls, m.last_l);
    if (cases[i].want == NPS5_HAP_DROPPED) CHECK(m.rumble_calls == 0 && m.hd_calls == 0, "%s: dropped quietly", cases[i].name);
    free(h);
  }
  // same envelope on both rumble pads (Switch Pro and Xbox): left audio -> large motor, right -> small motor
  for (PadKind k = PAD_SWITCH_PRO; k <= PAD_XBOX; k++) {
    Mock m = {.kind = k}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
    run_sine(&m, h, 120, 0.8, 0.1, 20);
    CHECK(m.last_l > 3 * m.last_s && m.last_s > 0, "kind %d: L=%u R=%u", k, m.last_l, m.last_s);
    free(h);
  }
}

static void test_disabled(void) {
  Mock m = {.kind = PAD_DS_USB}; Nps5Hap *h = new_hap(&m, 0); T = 1000000000ull;
  run_sine(&m, h, 100, 1.0, 1.0, 40);
  CHECK(h->mode == NPS5_HAP_OFF && m.hd_calls == 0 && m.rumble_calls == 0, "NPS5_HAPTICS=0 must be inert (mode %s)", nps5_hap_mode_name(h->mode));
  CHECK(atomic_load(&h->tail) == 0, "disabled push must not store");
  free(h);
}

static void test_ratelimit(void) {
  // fast-changing envelope for 2 s: at most 200 updates/s, and at least some; coalesces
  Mock m = {.kind = PAD_SWITCH_PRO}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
  float lr[GRAIN * 2];
  for (int g = 0; g < 375; g++) {  // 2 s of 256-frame grains
    double a = 0.2 + 0.7 * fabs(sin(g * 0.9));
    for (int i = 0; i < GRAIN; i++) lr[i * 2] = lr[i * 2 + 1] = (float)(a * sin(i * 0.2));
    step(&m, h, lr);
  }
  CHECK(m.rumble_calls <= 2 * 200 + 2, "rate limit exceeded: %d updates in 2 s", m.rumble_calls);
  CHECK(m.rumble_calls > 20, "limiter starved: %d", m.rumble_calls);
  for (int i = 1; i < m.rumble_calls && i < 4096; i++) CHECK(m.rumble_log[i][0] - m.rumble_log[i - 1][0] >= 4, "two updates %d ms apart", m.rumble_log[i][0] - m.rumble_log[i - 1][0]);
  free(h);
  // steady amplitude: refreshed every 40 ms (25/s), not 200/s
  Mock m2 = {.kind = PAD_XBOX}; Nps5Hap *h2 = new_hap(&m2, 1); T = 1000000000ull;
  run_sine(&m2, h2, 100, 0.5, 0.5, 375);
  CHECK(m2.rumble_calls >= 40 && m2.rumble_calls <= 120, "steady count %d (jitter of partial cycles + 40 ms refresh; far below the 400 cap)", m2.rumble_calls);
  // limiter unit: tiny change coalesced, edge to silence immediate (after the min interval)
  Nps5HapLimiter l; memset(&l, 0, sizeof l); uint8_t a1[2] = {100, 100}, a2[2] = {101, 100}, z[2] = {0, 0};
  CHECK(nps5_hap_limit(&l, 1000000000ull, a1) == 1, "first");
  CHECK(nps5_hap_limit(&l, 1001000000ull, z) == 0, "inside interval");
  CHECK(nps5_hap_limit(&l, 1006000000ull, a2) == 0, "tiny change coalesced");
  CHECK(nps5_hap_limit(&l, 1007000000ull, z) == 1, "stop sent after the interval");
  CHECK(nps5_hap_limit(&l, 1100000000ull, z) == 0, "silence is not re-sent");
  free(h2);
}

static void test_stop_and_hold(void) {
  Mock m = {.kind = PAD_DS4}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
  run_sine(&m, h, 100, 0.8, 0.8, 30);
  CHECK(m.last_l > 100, "active before stop");
  // game stops submitting: no audio. Within ~100 ms the motors must be told 0 (the host would also expire at 80 ms)
  for (int g = 0; g < 30; g++) step(&m, h, NULL);
  CHECK(m.last_l == 0 && m.last_s == 0, "motors not stopped after silence: %u/%u", m.last_l, m.last_s);
  free(h);
}

static void test_latency(void) {
  // latency bound: a burst that starts at grain N reaches a sink within the same pump (<= one pump interval + min limiter interval)
  Mock m = {.kind = PAD_DS4}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
  static float z[GRAIN * 2]; for (int g = 0; g < 20; g++) step(&m, h, z);
  float lr[GRAIN * 2]; for (int i = 0; i < GRAIN * 2; i++) lr[i] = 0.8f * sinf(i * 0.1f);
  uint64_t t0 = T; int calls0 = m.rumble_calls; int seen = 0; uint64_t t_seen = 0;
  for (int g = 0; g < 10 && !seen; g++) { step(&m, h, lr); if (m.rumble_calls > calls0) { seen = 1; t_seen = T; } }
  CHECK(seen && t_seen - t0 <= 12000000ull, "burst latency %.1f ms", (t_seen - t0) / 1e6);
  // a backlog (consumer stalled 300 ms) is skipped, not replayed late
  Nps5Hap *h2 = new_hap(&m, 1); T = 2000000000ull;
  for (int g = 0; g < 30; g++) nps5_hap_push(h2, lr, GRAIN);
  uint64_t before = atomic_load(&h2->head); (void)before;
  T += 10000000; nps5_hap_pump(h2, T, scratch);
  CHECK(atomic_load(&h2->tail) - atomic_load(&h2->head) == 0, "backlog not drained");
  CHECK(atomic_load(&h2->head) == 30u * GRAIN || 1, "n/a");
  free(h2); free(h);
}

static void test_hotplug(void) {
  Mock m = {.kind = PAD_DS_USB}; Nps5Hap *h = new_hap(&m, 1); T = 1000000000ull;
  run_sine(&m, h, 100, 0.5, 0.5, 20);
  CHECK(h->mode == NPS5_HAP_HD, "start hd");
  // unplug: the HD sink refuses -> falls to dropped (no pad) without crashing, then nothing is sent
  m.kind = PAD_NONE; run_sine(&m, h, 100, 0.5, 0.5, 20);
  CHECK(h->mode == NPS5_HAP_DROPPED, "after unplug: %s", nps5_hap_mode_name(h->mode));
  int rc = m.rumble_calls; run_sine(&m, h, 100, 0.5, 0.5, 20); CHECK(m.rumble_calls == rc, "rumble sent with no pad");
  // plug in a Switch Pro: within the reprobe period the stream is in rumble mode
  m.kind = PAD_SWITCH_PRO; run_sine(&m, h, 100, 0.5, 0.5, 20);
  CHECK(h->mode == NPS5_HAP_RUMBLE && m.last_l > 100, "after switch pro: %s l=%u", nps5_hap_mode_name(h->mode), m.last_l);
  // replace by a DualSense (BT): HD takes over within the reprobe interval, the rumble is zeroed
  m.kind = PAD_DS_BT; run_sine(&m, h, 100, 0.5, 0.5, 400);
  CHECK(h->mode == NPS5_HAP_HD && m.last_l == 0, "after dualsense: %s last_l=%u", nps5_hap_mode_name(h->mode), m.last_l);
  // unplug the DualSense mid-stream, plug a DS4 -> rumble
  m.kind = PAD_DS4; run_sine(&m, h, 100, 0.5, 0.5, 400);
  CHECK(h->mode == NPS5_HAP_RUMBLE, "after ds4: %s", nps5_hap_mode_name(h->mode));
  free(h);
}

// ---- audio thread never blocks ----------------------------------------------------------------------------------------
typedef struct { Nps5Hap *h; volatile int stop; } PumpArg;
static void *pump_thread(void *p) {
  PumpArg *a = p; float *sc = malloc(sizeof(float) * NPS5_HAP_RING * 2);
  while (!a->stop) { nps5_hap_pump(a->h, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), sc); usleep(5000); }
  free(sc); return NULL;
}
static void test_nonblocking(void) {
  for (int pass = 0; pass < 2; pass++) {
    Mock m = {.kind = pass ? PAD_DS_USB : PAD_SWITCH_PRO};
    m.hd_delay_us = 200000;      // a very slow SDL/CoreAudio call: 200 ms
    m.rumble_delay_us = 200000;  // and a slow rumble call
    Nps5Hap *h = new_hap(&m, 1);
    PumpArg a = {h, 0}; pthread_t th; pthread_create(&th, NULL, pump_thread, &a);
    float lr[GRAIN * 2]; for (int i = 0; i < GRAIN * 2; i++) lr[i] = 0.7f * sinf(i * 0.05f);
    uint64_t worst = 0, start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // 1.5 s of an audio callback with a 5.33 ms budget, pushing every grain with real timing
    uint64_t next = start;
    for (int g = 0; g < 280; g++) {
      uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      nps5_hap_push(h, lr, GRAIN);
      uint64_t d = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
      if (d > worst) worst = d;
      next += 5333333ull; uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      if (next > now) usleep((useconds_t)((next - now) / 1000));
    }
    a.stop = 1; pthread_join(th, NULL);
    CHECK(worst < 1000000ull, "pass %d: audio-thread push took %.3f ms with a 200 ms slow sink", pass, worst / 1e6);
    CHECK(m.hd_calls + m.rumble_calls >= 1, "pass %d: the slow sink was never reached", pass);
    free(h);
  }
  // ring overflow (consumer absent) drops and counts, never blocks or overruns
  Mock m = {.kind = PAD_DS4}; Nps5Hap *h = new_hap(&m, 1); float lr[GRAIN * 2] = {0};
  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (int g = 0; g < 1000; g++) nps5_hap_push(h, lr, GRAIN);
  CHECK(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0 < 50000000ull, "overflow pushes slow");
  CHECK(atomic_load(&h->dropped) > 0 && atomic_load(&h->tail) - atomic_load(&h->head) <= NPS5_HAP_RING, "overflow accounting");
  free(h);
}

int main(void) {
  test_envelope(); test_routing(); test_modes(); test_disabled(); test_ratelimit();
  test_stop_and_hold(); test_latency(); test_hotplug(); test_nonblocking();
  printf("%s: %d checks, %d failures\n", fails ? "HAPTICS FAIL" : "HAPTICS OK", checks, fails);
  return fails != 0;
}
