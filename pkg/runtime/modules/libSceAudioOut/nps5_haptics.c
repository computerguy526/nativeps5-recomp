// Controller haptics glue for the vibration audio port: owns the process-wide Nps5Hap (nps5_haptics_logic.h), the pump
// thread and the host sinks. The audio thread only calls nps5_haptics_push() (wait-free).
//   NPS5_HAPTICS=0        off (the vibration port is dropped as before)
//   NPS5_HAPTICS_LOG=1    '[haptics] mode=... L=.. R=..' once a second
// HD sink: the Kyty library's KytyGpuQueuePadAudio (DualSense 4-channel USB audio device, or Bluetooth HID audio).
// Rumble sink: the host pad library's audio-haptics input (libnps5_gpu.dylib); the pad output reaches the controller
// through the hybrid's pad relay (Kyty -> SDL rumble: DualSense, DualShock 4, Switch Pro, Xbox ...).
#include "nps5_haptics_logic.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "nps5_diag.h"

extern uint64_t nps5_kyty_queue_pad_audio(uint32_t freq, const float *lr, uint32_t frames); // nps5_kyty.c; 0 = not taken

static Nps5Hap g_hap;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_log;
static void (*p_submit)(uint8_t, uint8_t);

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static uint64_t sink_hd(void *u, const float *lr, uint32_t frames, uint32_t rate) {
  (void)u;
  return nps5_kyty_queue_pad_audio(rate, lr, frames);
}
static void sink_rumble(void *u, uint8_t l, uint8_t s) {
  (void)u;
  if (p_submit) p_submit(l, s);
}
static int sink_avail(void *u) { (void)u; return p_submit != NULL; }

static void *pump_thread(void *arg) {
  (void)arg;
  float *scratch = malloc(sizeof(float) * NPS5_HAP_RING * 2);
  uint64_t last_log = 0;
  Nps5HapMode logged = (Nps5HapMode)-1;
  for (;;) {
    struct timespec ts = {0, 5000000};
    nanosleep(&ts, NULL);
    uint64_t t = now_ns();
    nps5_hap_pump(&g_hap, t, scratch);
    if (g_log && (t - last_log >= 1000000000ull || g_hap.mode != logged)) {
      last_log = t;
      logged = g_hap.mode;
      fprintf(stderr, "[haptics] mode=%s L=%u R=%u hd_calls=%llu rumble_updates=%llu dropped_frames=%llu\n",
              nps5_hap_mode_name(g_hap.mode), g_hap.amp[0], g_hap.amp[1], (unsigned long long)g_hap.hd_calls,
              (unsigned long long)g_hap.rumble_calls, (unsigned long long)atomic_load(&g_hap.dropped));
    }
  }
  return NULL;
}

static void init_once(void) {
  const char *e = getenv("NPS5_HAPTICS");
  int on = !(e && e[0] == '0');
  const char *l = getenv("NPS5_HAPTICS_LOG");
  g_log = l && l[0] && l[0] != '0';
  Nps5HapSinks s = {sink_hd, sink_rumble, sink_avail, NULL};
  nps5_hap_init(&g_hap, on, &s);
  if (!on) return;
  void *gpu = dlopen("@rpath/libnps5_gpu.dylib", RTLD_NOW | RTLD_NOLOAD);
  *(void **)&p_submit = gpu ? dlsym(gpu, "PadSubmitAudioHaptics_nid_postfix") : NULL;
  if (!p_submit) *(void **)&p_submit = dlsym(RTLD_DEFAULT, "PadSubmitAudioHaptics_nid_postfix");
  pthread_t th;
  if (pthread_create(&th, NULL, pump_thread, NULL) == 0) pthread_detach(th);
  else g_hap.enabled = 0;
  NPS5_LOG(NPS5_S_AUDIO, NPS5_L_INFO, "haptics: vibration audio -> controller (HD when a DualSense takes it, else rumble); NPS5_HAPTICS=0 disables");
}

int nps5_haptics_open(void) {
  pthread_once(&g_once, init_once);
  return g_hap.enabled;
}

void nps5_haptics_push(const float *lr, uint32_t frames) {
  if (g_hap.enabled) nps5_hap_push(&g_hap, lr, frames);
}
