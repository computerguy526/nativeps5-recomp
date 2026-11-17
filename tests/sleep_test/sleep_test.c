// nps5_sleep_ns (core/nps5_sleep.h): never returns early, is not coalesced (a 20 ms nanosleep is about 3 ms late on
// macOS; the critical kqueue timer about 0.2 ms), reports an interrupted sleep with the time left.
#include "nps5_sleep.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void on_alarm(int s) { (void)s; }

int main(void) {
  if (nps5_sleep_ns(0, NULL) != 0) return 1;
  uint64_t late_sum = 0, late_min = ~0ull;
  const int n = 20;
  for (int i = 0; i < n; i++) {
    uint64_t t = now();
    if (nps5_sleep_ns(20000000ull, NULL) != 0) { printf("FAIL: sleep failed\n"); return 1; }
    uint64_t d = now() - t;
    if (d < 20000000ull) { printf("FAIL: woke %llu ns early\n", (unsigned long long)(20000000ull - d)); return 1; }
    late_sum += d - 20000000ull;
    if (d - 20000000ull < late_min) late_min = d - 20000000ull;
  }
  // The best case is free of scheduling noise: a coalesced timer is never less than ~1.5 ms late on a 20 ms sleep.
  printf("late: avg %.3f ms, min %.3f ms\n", late_sum / 1e6 / n, late_min / 1e6);
  if (late_min > 1000000ull) { printf("FAIL: the timer is coalesced\n"); return 1; }
  // EINTR with the remaining time.
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_alarm; // no SA_RESTART
  sigaction(SIGALRM, &sa, NULL);
  struct itimerval it = {{0, 0}, {0, 50000}};
  setitimer(ITIMER_REAL, &it, NULL);
  uint64_t left = 0, t = now();
  int r = nps5_sleep_ns(400000000ull, &left);
  uint64_t d = now() - t;
  if (r != -1 || errno != EINTR || left == 0 || left > 400000000ull || d > 300000000ull) {
    printf("FAIL: interrupted sleep: r=%d errno=%d left=%llu slept=%llu\n", r, errno, (unsigned long long)left, (unsigned long long)d);
    return 1;
  }
  printf("SLEEP OK\n");
  return 0;
}
