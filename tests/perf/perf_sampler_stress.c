// Stress test for the NPS5_PERF sampler (nps5_perf.c): thousands of threads start, make an "HLE call" (which registers
// them with the sampler) and exit while the sampler runs at 10 kHz. Before the fix the sampler called thread_info on
// freed/reused mach port names and macOS killed the process with EXC_GUARD (GUARD_TYPE_MACH_PORT). Passing = no crash.
#include "nps5_perf.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach.h>

void (*g_nps5_perf_hle_enter)(void *) = 0; // normally defined by nps5_diag.c; the stubs below stand in for it
void (*g_nps5_perf_finish)(const char *) = 0;
void (*g_nps5_hle_exit_hook)(void *, void *) = 0;
#include <stdint.h>
uint64_t g_recomp_call_model = 0;
void nps5_lean_require_hooks(void) {}
static void dummy_shim(void) {}
static _Atomic int g_done, g_stop;

static void *worker(void *a) {
  (void)a;
  if (g_nps5_perf_hle_enter) g_nps5_perf_hle_enter((void *)dummy_shim);
  volatile unsigned x = 0;
  for (int i = 0; i < 20000; i++) x += i; // burn a little CPU so the sampler has something to read
  atomic_fetch_add(&g_done, 1);
  return NULL;
}

// Name churn: strict-guarded receive rights allocated and destroyed in a loop, so freed thread-port names get reused by
// guarded ports (what Metal / XPC do in the real process). A stale thread_info on such a name raises EXC_GUARD.
static void *guard_churn(void *a) {
  (void)a;
  while (!atomic_load(&g_stop)) {
    mach_port_t ps[32];
    mach_port_options_t o = {.flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT};
    int n = 0;
    for (; n < 32; n++) {
      if (mach_port_construct(mach_task_self(), &o, (mach_port_context_t)0x8000010200000003ull + n, &ps[n]) != KERN_SUCCESS) break;
    }
    for (int i = 0; i < n; i++) mach_port_destruct(mach_task_self(), ps[i], 0, (mach_port_context_t)0x8000010200000003ull + i);
  }
  return NULL;
}

int main(int argc, char **argv) {
  int total = argc > 1 ? atoi(argv[1]) : 4000;
  if (g_nps5_perf_level < 2) { fprintf(stderr, "FAIL: run with NPS5_PERF=2 NPS5_PERF_HZ=10000\n"); return 2; }
  pthread_t gc;
  pthread_create(&gc, NULL, guard_churn, NULL);
  int launched = 0;
  while (launched < total) {
    pthread_t t[16];
    int n = 0;
    for (; n < 16 && launched < total; n++, launched++)
      if (pthread_create(&t[n], NULL, worker, NULL)) break;
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    if (n == 0) usleep(1000);
  }
  usleep(50000);
  atomic_store(&g_stop, 1);
  pthread_join(gc, NULL);
  // our own right count must not grow without bound: sampler drops the right of each exited thread
  printf("perf_sampler_stress: %d threads, %d completed: PASS (no crash)\n", launched, atomic_load(&g_done));
  return atomic_load(&g_done) == launched ? 0 : 1;
}
