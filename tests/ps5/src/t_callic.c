/* Call-site inline caches (docs/m3/callic.md): adversarial patterns for indirect/virtual call
 * sites. Every result is a hash of values that do not depend on timing, so the output is the same
 * with caches on or off and on the host build (-DNPS5_HOST). */
#include "ps5test.h"
#include <pthread.h>
#include <setjmp.h>

typedef uint64_t (*F)(uint64_t);
#define DEF(n, expr) static T_NOINLINE uint64_t f##n(uint64_t x) { return (expr); }
DEF(0, x + 1) DEF(1, x * 3) DEF(2, x ^ 0x55) DEF(3, x - 7) DEF(4, (x << 1) | 1) DEF(5, x + (x >> 3))
DEF(6, x * x + 1) DEF(7, ~x)
static F g_f[8] = {f0, f1, f2, f3, f4, f5, f6, f7};

/* One call site, one function: the target is the argument. */
static T_NOINLINE uint64_t site(F f, uint64_t x) { return f(x); }
/* A second site, called through a volatile slot (the vtable shape: load pointer, call). */
static F volatile g_slot;
static T_NOINLINE uint64_t vsite(uint64_t x) { return g_slot(x); }

static uint64_t drive(const char *name, unsigned ntargets, uint64_t n, int mode) {
  uint64_t h = T_FNV0, s = 12345, x = 1;
  for (uint64_t i = 0; i < n; i++) {
    unsigned k;
    switch (mode) {
    case 0: k = 0; break;                         /* monomorphic */
    case 1: k = (unsigned)(i * ntargets / n); break; /* phases: the target changes and stays changed */
    case 2: k = (unsigned)(i % ntargets); break;  /* round robin */
    default: k = (unsigned)(t_lcg(&s) % ntargets); /* random */
    }
    x = site(g_f[k], x ^ i);
    h = (h ^ x) * 0x100000001b3ULL;
  }
  t_hex(name, h);
  return h;
}

/* ---- recursion through a pointer, deeper than the shadow stack (32768 entries) ---- */
static F g_rec;
static T_NOINLINE uint64_t rec(uint64_t n) {
  volatile uint64_t keep = n * 7;
  if (n == 0) return 1;
  return g_rec(n - 1) * 3 + keep;
}
static F g_ping, g_pong;
static T_NOINLINE uint64_t ping(uint64_t n) { return n == 0 ? 5 : g_pong(n - 1) + n; }
static T_NOINLINE uint64_t pong(uint64_t n) { return n == 0 ? 9 : g_ping(n - 1) ^ n; }

/* ---- longjmp across frames that went through cached sites ---- */
static jmp_buf g_jb;
static F g_dive;
static T_NOINLINE uint64_t dive_to(uint64_t n);
static uint64_t g_jump_at;
static T_NOINLINE uint64_t dive(uint64_t n) {
  volatile uint64_t keep = n * 11;
  if (n == g_jump_at) longjmp(g_jb, (int)(n & 0x7fff) + 1);
  return dive_to(n + 1) + keep;
}
static T_NOINLINE uint64_t dive_to(uint64_t n) { return g_dive(n); }
static T_NOINLINE uint64_t jump_round(uint64_t depth) {
  g_jump_at = depth;
  int r = setjmp(g_jb);
  if (r == 0) return dive_to(0);
  return (uint64_t)r + depth;
}

/* ---- threads hammering one site ---- */
static volatile int g_go, g_stop;
static F volatile g_shared;       /* flipped by the writer thread while workers call through it */
static T_NOINLINE uint64_t shared_site(uint64_t x) { return g_shared(x); }
static uint64_t g_wres[4]; static uint64_t g_bad[4];
static void *worker(void *arg) {
  uintptr_t id = (uintptr_t)arg;
  while (!g_go) {}
  uint64_t h = T_FNV0, bad = 0, s = id * 77 + 1;
  for (uint32_t i = 0; i < 300000; i++) {
    unsigned k = (id + i / 5000) % 8;          /* private target, same site as the other threads */
    uint64_t x = site(g_f[k], i);
    h = (h ^ x) * 0x100000001b3ULL;
    uint64_t y = shared_site(i + (s & 0xff));  /* the target flips under us: either is legal */
    if (y != f0(i + (s & 0xff)) && y != f1(i + (s & 0xff)) && y != f2(i + (s & 0xff))) bad++;
    if ((i & 1023) == 0) s = t_lcg(&s);
  }
  g_wres[id] = h; g_bad[id] = bad;
  return 0;
}
static void *flipper(void *arg) {
  (void)arg;
  unsigned i = 0;
  while (!g_stop) { g_shared = g_f[i++ % 3]; for (volatile int d = 0; d < 20; d++) {} }
  return 0;
}

int main(void) {
  drive("mono", 1, 200000, 0);
  drive("phases_2", 2, 200000, 1);
  drive("phases_8", 8, 200000, 1);
  drive("rr_2", 2, 200000, 2);
  drive("rr_3", 3, 200000, 2);
  drive("rr_8", 8, 300000, 2);
  drive("random_2", 2, 200000, 3);
  drive("random_8", 8, 300000, 3);
  /* the vtable-shaped site: change the slot between bursts */
  { uint64_t h = T_FNV0, x = 1;
    for (int b = 0; b < 4000; b++) { g_slot = g_f[(b * 5 + b / 7) & 7]; for (int j = 0; j < 20; j++) { x = vsite(x + (uint64_t)j); h = (h ^ x) * 0x100000001b3ULL; } }
    t_hex("vslot_bursts", h); }
  g_rec = rec;
  t_hex("rec_20000", rec(20000));
  t_hex("rec_100000", rec(100000));   /* past the shadow stack: overflow + slow returns, all through one site */
  t_hex("rec_100000_again", rec(100000));
  g_ping = ping; g_pong = pong;
  t_hex("pingpong_50000", ping(50000));
  g_dive = dive;
  { uint64_t h = T_FNV0;
    for (uint64_t d = 0; d < 400; d++) { h = (h ^ jump_round(d * 37 % 5000)) * 0x100000001b3ULL; }
    t_hex("longjmp_cached", h);
    t_hex("rec_after_longjmp", rec(3000)); }
  g_shared = f0;
  pthread_t th[4], fl;
  pthread_create(&fl, 0, flipper, 0);
  for (uintptr_t i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, (void *)i);
  g_go = 1;
  for (int i = 0; i < 4; i++) pthread_join(th[i], 0);
  g_stop = 1; pthread_join(fl, 0);
  uint64_t tot = 0, bad = 0;
  for (int i = 0; i < 4; i++) { t_hex("thread", g_wres[i]); tot ^= g_wres[i]; bad += g_bad[i]; }
  t_hex("threads_xor", tot);
  t_dec("threads_bad", (long long)bad);
  t_str("t_callic: done\n");
  return 0;
}
