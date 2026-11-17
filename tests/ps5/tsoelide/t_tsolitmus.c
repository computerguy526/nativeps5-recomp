/* x86-TSO litmus tests through a stack object that another thread can reach
 * (docs/m3/tsoelide.md, tools/tsoelide_test.sh). Each test counts outcomes x86-TSO forbids; a
 * correct translation prints 0 for all of them. The accesses of the owning thread are
 * RSP-relative, which the lifter's original rule (-tso-elide off) emits without ordering.
 *
 *   owner_reads   MP, the owner is the reader: helper stores data, flag through the pointer;
 *                 owner loads flag, data from its frame. Forbidden: data older than flag.
 *   owner_writes  MP, the owner is the writer: owner stores data, flag into its frame; helper
 *                 loads flag, data through the pointer. Forbidden: data older than flag.
 *   pingpong      request/response with a turn word, all in the owner's frame; a wrong
 *                 response is a lost ordering on either side.
 * The first load of each reader has an address that depends on a division chain, so an
 * out-of-order core has the second load ready long before the first. */
#include "../src/ps5test.h"
#include <pthread.h>

#define APART 136 /* data and flag on different cache lines (128 bytes on Apple cores) */
struct mp {
  volatile uint64_t data;
  char pad0[APART - 8];
  volatile uint64_t flag[2];
  char pad1[APART - 16];
  volatile uint64_t req, resp;
  char pad2[APART - 16];
  volatile uint64_t turn;
};
static struct mp *volatile g_p;
static volatile uint64_t g_stop, g_ready, g_one = 1, g_zero, g_helper_iters, g_helper_viol;

static inline uint64_t late_zero(uint64_t n) { /* 0, after a dependent chain of divisions */
  uint64_t t = (n | 0x100000) / g_one;
  t = t / g_one; t = t / g_one;
  return t & g_zero;
}

static void *writer_helper(void *a) {
  (void)a;
  struct mp *p = g_p;
  __atomic_store_n(&g_ready, 1, __ATOMIC_SEQ_CST);
  uint64_t i = 0;
  while (!g_stop) { i++; p->data = i; p->flag[0] = i; }
  g_helper_iters = i;
  return 0;
}
static T_NOINLINE uint64_t owner_reads(uint64_t iters) {
  struct mp s;
  s.data = 0; s.flag[0] = 0; s.flag[1] = 0;
  g_stop = 0; g_ready = 0; g_p = &s;
  pthread_t t;
  pthread_create(&t, 0, writer_helper, 0);
  while (!__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST)) {}
  uint64_t viol = 0;
  for (uint64_t n = 0; n < iters; n++) {
    uint64_t q = late_zero(n);
    uint64_t f = s.flag[q]; /* first, late */
    uint64_t d = s.data;    /* second, early */
    viol += d < f;
  }
  g_stop = 1;
  pthread_join(t, 0);
  return viol;
}

static void *reader_helper(void *a) {
  (void)a;
  struct mp *p = g_p;
  __atomic_store_n(&g_ready, 1, __ATOMIC_SEQ_CST);
  uint64_t viol = 0, n = 0;
  while (!g_stop) {
    uint64_t q = late_zero(n++);
    uint64_t f = p->flag[q];
    uint64_t d = p->data;
    viol += d < f;
  }
  g_helper_viol = viol; g_helper_iters = n;
  return 0;
}
static T_NOINLINE uint64_t owner_writes(uint64_t iters) {
  struct mp s;
  s.data = 0; s.flag[0] = 0; s.flag[1] = 0;
  g_stop = 0; g_ready = 0; g_p = &s;
  pthread_t t;
  pthread_create(&t, 0, reader_helper, 0);
  while (!__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST)) {}
  for (uint64_t i = 1; i <= iters; i++) { s.data = i; s.flag[0] = i; }
  g_stop = 1;
  pthread_join(t, 0);
  return g_helper_viol;
}

static void *pong_helper(void *a) {
  (void)a;
  struct mp *p = g_p;
  __atomic_store_n(&g_ready, 1, __ATOMIC_SEQ_CST);
  for (;;) {
    while (p->turn != 1) { if (g_stop) return 0; }
    uint64_t r = p->req;
    p->resp = r * 3 + 1;
    p->turn = 0;
  }
}
static T_NOINLINE uint64_t pingpong(uint64_t iters) {
  struct mp s;
  s.req = 0; s.resp = 0; s.turn = 0;
  g_stop = 0; g_ready = 0; g_p = &s;
  pthread_t t;
  pthread_create(&t, 0, pong_helper, 0);
  while (!__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST)) {}
  uint64_t viol = 0;
  for (uint64_t i = 1; i <= iters; i++) {
    s.req = i;
    s.turn = 1;
    while (s.turn != 0) {}
    viol += s.resp != i * 3 + 1;
  }
  g_stop = 1;
  pthread_join(t, 0);
  return viol;
}

int main(void) {
  uint64_t v;
  v = owner_reads(60000000);
  t_dec("owner_reads_violations", (long long)v);
  t_dec("owner_reads_helper_stores", (long long)g_helper_iters);
  v = owner_writes(120000000);
  t_dec("owner_writes_violations", (long long)v);
  t_dec("owner_writes_helper_loads", (long long)g_helper_iters);
  v = pingpong(1500000);
  t_dec("pingpong_violations", (long long)v);
  return 0;
}
