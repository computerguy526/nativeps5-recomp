/* HLE call cost microbenchmark (tools/shimbench.sh; docs/m3/leanshim.md).
 * Every case calls a libc function through the import (a function pointer the compiler cannot see
 * through), so each call is a guest -> host shim call. The *_direct cases do the same work in guest code
 * (a one-size-class free list, an unrolled copy): what a guest-side replacement of the shim would cost,
 * without any bridge. Times come from sceKernelGetProcessTime (usec). */
#include "../src/ps5test.h"
#include <stdlib.h>
#include <string.h>
extern uint64_t sceKernelGetProcessTime(void);
extern uint64_t sceKernelGetProcessTimeCounter(void);
extern uint64_t sceKernelReadTsc(void);
extern int clock_gettime(int, void *);
extern int gettimeofday(void *, void *);

#define NI __attribute__((noinline))
static volatile uint64_t g_sink;

static void t_u64(uint64_t u) {
  char b[24];
  int i = 24;
  b[--i] = 0;
  do { b[--i] = '0' + u % 10; u /= 10; } while (u);
  t_str(b + i);
}
static void report(const char *name, uint64_t t0, uint64_t calls, uint64_t check) {
  uint64_t us = sceKernelGetProcessTime() - t0;
  char b[17];
  t_hexs(b, check, 16);
  b[16] = 0;
  t_str(name); t_str(" check="); t_str(b);
  t_str(" calls="); t_u64(calls);
  t_str(" ns_per_call_x100="); t_u64(calls ? us * 100000ULL / calls : 0);
  t_str("\n");
}

/* guest-side free list of 64-byte blocks */
static uint64_t g_pool[1 << 14][8];
static uint64_t *g_free_head;
static uint32_t g_pool_next;
static NI void *direct_malloc(void) {
  uint64_t *p = g_free_head;
  if (p) { g_free_head = (uint64_t *)p[0]; return p; }
  return g_pool[g_pool_next++ & ((1 << 14) - 1)];
}
static NI void direct_free(void *q) {
  uint64_t *p = q;
  p[0] = (uint64_t)g_free_head;
  g_free_head = p;
}
static NI void direct_copy64(void *d, const void *s) {
  uint64_t *dd = d; const uint64_t *ss = s;
  for (int i = 0; i < 8; i++) dd[i] = ss[i];
}

int main(void) {
  uint64_t t, s;
  void *(*volatile mallocp)(size_t) = malloc;
  void (*volatile freep)(void *) = free;
  void *(*volatile memcpyp)(void *, const void *, size_t) = memcpy;
  void *(*volatile memsetp)(void *, int, size_t) = memset;
  size_t (*volatile strlenp)(const char *) = strlen;
  int (*volatile absp)(int) = abs;
  static char a[256], b[256];
  for (int i = 0; i < 255; i++) a[i] = 'a' + i % 26;
  const uint32_t N = 5000000;

  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { char *p = mallocp(64); p[0] = (char)i; s += (uint64_t)p[0]; freep(p); }
  report("malloc_free", t, 2ULL * N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { char *p = direct_malloc(); p[0] = (char)i; s += (uint64_t)p[0]; direct_free(p); }
  report("malloc_free_direct", t, 2ULL * N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { a[0] = (char)i; memcpyp(b, a, 64); s += (uint64_t)b[0]; }
  report("memcpy64", t, N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { a[0] = (char)i; direct_copy64(b, a); s += (uint64_t)b[0]; }
  report("memcpy64_direct", t, N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { memsetp(b, (int)i, 64); s += (uint64_t)b[63]; }
  report("memset64", t, N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { a[i & 31] = 'x'; s += strlenp(a + (i & 7)); }
  report("strlen", t, N, s);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) s += (uint64_t)absp((int)i - 7);
  report("abs_full_only", t, N, s);
  /* clocks (docs/m3/clock.md) */
  uint64_t (*volatile ptcp)(void) = sceKernelGetProcessTimeCounter;
  uint64_t (*volatile gptp)(void) = sceKernelGetProcessTime;
  uint64_t (*volatile tscp)(void) = sceKernelReadTsc;
  int (*volatile cgtp)(int, void *) = clock_gettime;
  int (*volatile gtodp)(void *, void *) = gettimeofday;
  uint64_t ts[2];
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) s += ptcp() & 1;
  report("proc_time_counter", t, N, 0);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) s += gptp() & 1;
  report("proc_time_us", t, N, 0);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) s += tscp() & 1;
  report("read_tsc_call", t, N, 0);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); s += lo & 1; }
  report("rdtsc_insn", t, N, 0);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { cgtp(4, ts); s += ts[1] & 1; }
  report("clock_gettime_mono", t, N, 0);
  t = sceKernelGetProcessTime(); s = 0;
  for (uint32_t i = 0; i < N; i++) { gtodp(ts, 0); s += ts[1] & 1; }
  report("gettimeofday", t, N, 0);
  g_sink = s;
  return 0;
}
