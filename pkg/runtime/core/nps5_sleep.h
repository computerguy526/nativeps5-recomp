// Precise sleeps for guest threads (docs/m3/perf.md).
//
// nanosleep/usleep on macOS are subject to timer coalescing: the wake-up is late by 15-20 % of the interval
// (measured: 21 ms requested, 24 ms slept on average, at any QoS). A title that paces its frames with a sleep
// (Syobon: one nanosleep of the rest of its 33 ms frame) then runs below its own target rate. A kqueue timer with
// NOTE_CRITICAL and no leeway is not coalesced (0.05-0.25 ms late on average). Header-only so that the shim sources
// keep linking alone in the unit tests.
#ifndef NPS5_SLEEP_H
#define NPS5_SLEEP_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

static pthread_key_t nps5_sleep_key;
static pthread_once_t nps5_sleep_once = PTHREAD_ONCE_INIT;
static void nps5_sleep_close(void *p) { close((int)(intptr_t)p - 1); }
static void nps5_sleep_init(void) { pthread_key_create(&nps5_sleep_key, nps5_sleep_close); }

// Sleeps for ns nanoseconds. Returns 0, or -1 with errno = EINTR and *remaining_ns set when a signal handler ran.
static inline int nps5_sleep_ns(uint64_t ns, uint64_t *remaining_ns) {
  if (remaining_ns) *remaining_ns = 0;
  if (ns == 0) return 0;
  pthread_once(&nps5_sleep_once, nps5_sleep_init);
  int kq = (int)(intptr_t)pthread_getspecific(nps5_sleep_key) - 1; // stored + 1: 0 means none yet
  if (kq < 0) {
    kq = kqueue();
    if (kq >= 0) pthread_setspecific(nps5_sleep_key, (void *)(intptr_t)(kq + 1));
  }
  const uint64_t wake = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + ns;
  if (kq < 0) { // out of descriptors: the coalesced sleep
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}, rem = {0, 0};
    int r = nanosleep(&ts, &rem);
    if (r != 0 && remaining_ns) *remaining_ns = (uint64_t)rem.tv_sec * 1000000000ull + (uint64_t)rem.tv_nsec;
    return r;
  }
  for (;;) {
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now >= wake) return 0;
    struct kevent64_s ch = {0}, ev = {0};
    ch.ident = 1;
    ch.filter = EVFILT_TIMER;
    ch.flags = EV_ADD | EV_ONESHOT;
    ch.fflags = NOTE_NSECONDS | NOTE_CRITICAL | NOTE_LEEWAY; // ext[1] = 0: no leeway
    ch.data = (int64_t)(wake - now);
    int c = kevent64(kq, &ch, 1, &ev, 1, 0, NULL);
    if (c < 0 && errno == EINTR) {
      const uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      if (t >= wake) return 0;
      if (remaining_ns) *remaining_ns = wake - t;
      return -1;
    }
    if (c < 0 || (c > 0 && (ev.flags & EV_ERROR))) { // the timer could not be armed
      struct timespec ts = {(time_t)((wake - now) / 1000000000ull), (long)((wake - now) % 1000000000ull)};
      nanosleep(&ts, NULL);
      return 0;
    }
  }
}

#endif
