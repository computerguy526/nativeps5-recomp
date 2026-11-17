// Controller haptics from the game's vibration audio port (sceAudioOut2 type 6): the pure logic, host-testable
// (tests/haptics/haptics_unit.c, tools/haptics_unit.sh). No SDL, no Objective-C: the sinks are function pointers.
//
// Flow: the audio thread calls nps5_hap_push() (wait-free: it copies the grain into a single-producer ring and
// returns). A pump thread (or a test) calls nps5_hap_pump(): it first offers the audio to the HD sink (DualSense USB
// 4-channel device or Bluetooth HID audio, in the host's Kyty library: KytyGpuQueuePadAudio); a sink that returns 0
// ("no DualSense took it") puts the stream in rumble mode: the envelope of each channel becomes the large/small motor
// amplitude (a rate-limited stream of motor updates that any gamepad accepts, Switch Pro and Xbox included).
// The HD probe is repeated every NPS5_HAP_REPROBE_NS while in rumble mode, so a hotplug changes the mode.
#ifndef NPS5_HAPTICS_LOGIC_H
#define NPS5_HAPTICS_LOGIC_H
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define NPS5_HAP_RATE 48000u
#define NPS5_HAP_RING 8192u               // stereo frames (170 ms)
#define NPS5_HAP_MAX_LAG 3840u            // frames; older queued audio is skipped (80 ms)
#define NPS5_HAP_MIN_INTERVAL_NS 5000000ull   // at most 200 motor updates per second
#define NPS5_HAP_REFRESH_NS 40000000ull   // while active the amplitude is re-sent (the host holds it 80 ms)
#define NPS5_HAP_REPROBE_NS 1000000000ull
#define NPS5_HAP_SILENCE_NS 500000000ull  // after this long without signal the HD sink is no longer fed
#define NPS5_HAP_HOLD_NS 60000000ull      // rumble keeps its amplitude this long when no new audio arrives
#define NPS5_HAP_STEP 3                   // smallest amplitude change (of 255) worth an update

typedef enum { NPS5_HAP_OFF = 0, NPS5_HAP_DROPPED, NPS5_HAP_HD, NPS5_HAP_RUMBLE } Nps5HapMode;

static inline const char *nps5_hap_mode_name(Nps5HapMode m) {
  return m == NPS5_HAP_HD ? "hd" : m == NPS5_HAP_RUMBLE ? "rumble" : m == NPS5_HAP_DROPPED ? "dropped" : "off";
}

// Sinks. hd: returns the microseconds queued, 0 = no DualSense took the audio. rumble: 0..255 motors.
typedef struct {
  uint64_t (*hd)(void *user, const float *lr, uint32_t frames, uint32_t rate);
  void (*rumble)(void *user, uint8_t large, uint8_t small);
  int (*rumble_available)(void *user); // 0 = no pad accepts rumble (mode "dropped"); NULL = assume yes
  void *user;
} Nps5HapSinks;

// ---- routing and downmix -------------------------------------------------------------------------------------------
// Source channels that drive the two actuators: a 2-channel haptic stream is left/right; a 4-channel stream (the
// DualSense layout: 1-2 speaker, 3-4 actuators) uses channels 3 and 4; 6/8 use the first pair after the speaker pair;
// mono feeds both.
static inline void nps5_hap_route(uint32_t channels, uint32_t *l, uint32_t *r) {
  if (channels <= 1) { *l = *r = 0; }
  else if (channels == 2) { *l = 0; *r = 1; }
  else { *l = 2; *r = 3; if (channels < 4) *r = 2; }
}

static inline float nps5_hap_finite(float v) { return isfinite(v) ? (v > 1.f ? 1.f : v < -1.f ? -1.f : v) : 0.f; }

// Adds one port's frames into the stereo accumulation lr (frames*2 floats), with per-channel volumes (NULL = 1).
static inline void nps5_hap_accumulate(const void *pcm, uint32_t frames, uint32_t channels, int is_int16, const float *vol, float *lr) {
  if (!pcm || !channels) return;
  uint32_t cl, cr;
  nps5_hap_route(channels, &cl, &cr);
  for (uint32_t f = 0; f < frames; f++) {
    float s[2];
    const uint32_t c[2] = {cl, cr};
    for (int k = 0; k < 2; k++) {
      size_t i = (size_t)f * channels + c[k];
      float v = is_int16 ? ((const int16_t *)pcm)[i] / 32768.0f : ((const float *)pcm)[i];
      s[k] = nps5_hap_finite(v) * (vol ? vol[c[k]] : 1.0f);
    }
    lr[f * 2] += s[0];
    lr[f * 2 + 1] += s[1];
  }
}

// ---- envelope ------------------------------------------------------------------------------------------------------
// RMS of one channel -> motor amplitude: gain 2 (a full-scale sine, RMS 0.707, saturates at 255), 0.01 gate, same
// curve as AnyPS5's AudioOut2RumbleFromPadFrames. peak_hold: a short burst is not averaged away by a long grain, the
// larger of RMS*2 and 0.8*peak is used.
static inline uint8_t nps5_hap_amp(float rms, float peak) {
  float a = rms * 2.0f;
  float p = peak * 0.8f;
  if (p > a) a = p;
  if (rms < 0.01f && peak < 0.02f) return 0;
  if (a > 1.0f) a = 1.0f;
  return (uint8_t)lrintf(a * 255.0f);
}

static inline void nps5_hap_envelope(const float *lr, uint32_t frames, uint8_t out[2]) {
  for (int k = 0; k < 2; k++) {
    double e = 0;
    float pk = 0;
    for (uint32_t f = 0; f < frames; f++) {
      float v = nps5_hap_finite(lr[f * 2 + k]);
      e += (double)v * v;
      float a = fabsf(v);
      if (a > pk) pk = a;
    }
    out[k] = frames ? nps5_hap_amp((float)sqrt(e / frames), pk) : 0;
  }
}

// ---- rate limiter --------------------------------------------------------------------------------------------------
typedef struct {
  uint8_t last[2];
  uint64_t last_ns;
  int valid;
  uint64_t sent;
} Nps5HapLimiter;

// Decides whether (a0,a1) is sent at now_ns. Always at most one update per MIN_INTERVAL; a change to or from silence
// is never deferred past the interval; an unchanged active amplitude is refreshed every REFRESH; small changes
// (< STEP) are coalesced (the newest value is sent when the change grows or the refresh comes).
static inline int nps5_hap_limit(Nps5HapLimiter *l, uint64_t now_ns, const uint8_t a[2]) {
  int active = a[0] || a[1];
  int was_active = l->valid && (l->last[0] || l->last[1]);
  if (!l->valid) {
    if (!active) return 0; // nothing to say yet
  } else {
    if (now_ns - l->last_ns < NPS5_HAP_MIN_INTERVAL_NS) return 0;
    int d0 = abs((int)a[0] - (int)l->last[0]), d1 = abs((int)a[1] - (int)l->last[1]);
    int edge = active != was_active;
    int big = d0 >= NPS5_HAP_STEP || d1 >= NPS5_HAP_STEP;
    int refresh = active && now_ns - l->last_ns >= NPS5_HAP_REFRESH_NS;
    if (!edge && !big && !refresh) return 0;
  }
  l->last[0] = a[0]; l->last[1] = a[1]; l->last_ns = now_ns; l->valid = 1; l->sent++;
  return 1;
}

// ---- the stream ----------------------------------------------------------------------------------------------------
typedef struct {
  int enabled;
  Nps5HapSinks sinks;
  float ring[NPS5_HAP_RING * 2];
  _Atomic uint64_t head, tail;     // frames; the producer owns tail, the consumer head
  _Atomic uint64_t dropped;        // frames the producer could not store
  Nps5HapMode mode;                // consumer side
  uint64_t last_signal_ns, last_probe_ns;
  Nps5HapLimiter lim;
  uint8_t amp[2];                  // last envelope, for the log
  uint64_t hd_calls, rumble_calls, pumps;
} Nps5Hap;

static inline void nps5_hap_init(Nps5Hap *h, int enabled, const Nps5HapSinks *s) {
  memset(h, 0, sizeof *h);
  h->enabled = enabled;
  h->sinks = *s;
  h->mode = enabled ? NPS5_HAP_DROPPED : NPS5_HAP_OFF; // decided by the first pump
}

// Audio thread. Wait-free, no allocation, no calls out. lr: frames stereo floats.
static inline void nps5_hap_push(Nps5Hap *h, const float *lr, uint32_t frames) {
  if (!h->enabled || !lr) return;
  uint64_t t = atomic_load_explicit(&h->tail, memory_order_relaxed);
  uint64_t hd = atomic_load_explicit(&h->head, memory_order_acquire);
  uint64_t room = NPS5_HAP_RING - (t - hd);
  uint32_t n = frames;
  if (n > room) { atomic_fetch_add(&h->dropped, n - room); n = (uint32_t)room; }
  for (uint32_t i = 0; i < n; i++) {
    size_t s = (size_t)((t + i) % NPS5_HAP_RING) * 2;
    h->ring[s] = lr[i * 2];
    h->ring[s + 1] = lr[i * 2 + 1];
  }
  atomic_store_explicit(&h->tail, t + n, memory_order_release);
}

// One consumer step. Returns the number of motor updates issued. scratch: NPS5_HAP_RING*2 floats of caller storage.
static inline int nps5_hap_pump(Nps5Hap *h, uint64_t now_ns, float *scratch) {
  if (!h->enabled) return 0;
  h->pumps++;
  uint64_t hd = atomic_load_explicit(&h->head, memory_order_relaxed);
  uint64_t t = atomic_load_explicit(&h->tail, memory_order_acquire);
  uint64_t avail = t - hd;
  if (avail > NPS5_HAP_MAX_LAG) { hd = t - NPS5_HAP_MAX_LAG; avail = NPS5_HAP_MAX_LAG; } // stale: skip, stay current
  for (uint64_t i = 0; i < avail; i++) {
    size_t s = (size_t)((hd + i) % NPS5_HAP_RING) * 2;
    scratch[i * 2] = h->ring[s];
    scratch[i * 2 + 1] = h->ring[s + 1];
  }
  atomic_store_explicit(&h->head, hd + avail, memory_order_release);
  uint8_t a[2] = {0, 0};
  if (avail) nps5_hap_envelope(scratch, (uint32_t)avail, a);
  if (avail && (a[0] || a[1])) h->last_signal_ns = now_ns ? now_ns : 1;
  h->amp[0] = a[0]; h->amp[1] = a[1];
  int signal_recent = h->last_signal_ns && now_ns - h->last_signal_ns < NPS5_HAP_SILENCE_NS;

  int updates = 0;
  int probe = h->mode != NPS5_HAP_HD && (h->last_probe_ns == 0 || now_ns - h->last_probe_ns >= NPS5_HAP_REPROBE_NS);
  if (h->mode == NPS5_HAP_HD || probe) {
    if (avail && (signal_recent || probe) && h->sinks.hd) {
      uint64_t q = h->sinks.hd(h->sinks.user, scratch, (uint32_t)avail, NPS5_HAP_RATE);
      h->hd_calls++;
      if (probe) h->last_probe_ns = now_ns ? now_ns : 1;
      if (q > 0) {
        if (h->mode == NPS5_HAP_RUMBLE && h->sinks.rumble) h->sinks.rumble(h->sinks.user, 0, 0); // leave rumble quiet
        h->mode = NPS5_HAP_HD;
        return 0;
      }
      if (h->mode == NPS5_HAP_HD) h->mode = NPS5_HAP_DROPPED; // the pad went away: re-decide below
    } else if (h->mode == NPS5_HAP_HD) {
      return 0; // silent grain while in HD: nothing to feed
    } else if (probe && !avail) {
      // no audio yet: probe on the next grain
    }
  }
  if (h->mode == NPS5_HAP_HD) return 0;
  int rumble_ok = h->sinks.rumble && (!h->sinks.rumble_available || h->sinks.rumble_available(h->sinks.user));
  if (!rumble_ok) { h->mode = NPS5_HAP_DROPPED; return 0; }
  h->mode = NPS5_HAP_RUMBLE;
  if (!avail && !(h->lim.valid && (h->lim.last[0] || h->lim.last[1]))) return 0;
  if (!avail) { a[0] = h->amp[0]; a[1] = h->amp[1]; } // no new audio: let the limiter's refresh/idle logic decide
  if (!avail && !(h->last_signal_ns && now_ns - h->last_signal_ns < NPS5_HAP_HOLD_NS)) { a[0] = a[1] = 0; }
  if (nps5_hap_limit(&h->lim, now_ns, a)) {
    h->sinks.rumble(h->sinks.user, a[0], a[1]);
    h->rumble_calls++;
    updates = 1;
  }
  return updates;
}
#endif
