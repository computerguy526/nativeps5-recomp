/* TLS: __thread initialised/zero data, arrays, per-thread values across two pthreads,
 * address-taken TLS, and (C++) thread_local with constructor is covered in m1_main-style C++ tests. */
#include "ps5test.h"
#include <pthread.h>

static __thread uint64_t tl_counter = 100;
static __thread uint64_t tl_zero;
static __thread uint8_t tl_buf[256];
static __thread const char *tl_name = "init";
static uint64_t result[3];

static T_NOINLINE uint64_t *tl_addr(void) { return &tl_counter; }
static T_NOINLINE uint64_t work(int id) {
  for (int i = 0; i < 1000; i++) { tl_counter += id + i; tl_zero ^= tl_counter; tl_buf[i & 255] += (uint8_t)(tl_counter >> 3); }
  *tl_addr() += 5;
  uint64_t h = T_FNV0;
  h = t_fnv(h, &tl_counter, 8); h = t_fnv(h, &tl_zero, 8); h = t_fnv(h, tl_buf, sizeof tl_buf);
  return h;
}
static void *th(void *a) { int id = (int)(intptr_t)a; tl_name = "thread"; result[id] = work(id); return (void *)(intptr_t)(tl_name[0]); }

int main(void) {
  pthread_t t1, t2; void *r1, *r2;
  pthread_create(&t1, 0, th, (void *)1); pthread_create(&t2, 0, th, (void *)2);
  pthread_join(t1, &r1); pthread_join(t2, &r2);
  result[0] = work(0);
  t_hex("main_tls", result[0]); t_hex("thread1_tls", result[1]); t_hex("thread2_tls", result[2]);
  t_dec("thread_name_char", (long long)(intptr_t)r1 + (long long)(intptr_t)r2);
  t_str("main_name="); t_str(tl_name); t_str("\n");
  t_hex("main_counter_after", tl_counter);
  return 0;
}
