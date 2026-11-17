/* Call/return microbenchmark for the call models (tools/callbench.sh; docs/m1/flatstack.md).
 * Each case is call-saturated: the loop bodies do almost nothing but CALL and RET, so the numbers
 * are an upper bound on what the call model costs. Times come from sceKernelGetProcessTime (usec). */
#include "../src/ps5test.h"
#include <stdlib.h>
extern uint64_t sceKernelGetProcessTime(void);

#define NI __attribute__((noinline))
static volatile uint64_t g_sink;

static NI uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static NI uint64_t leaf(uint64_t a) { return a * 3 + 1; }
static NI uint64_t mix_d(uint64_t a) { return a ^ (a >> 3); }
static NI uint64_t mix_c(uint64_t a) { return mix_d(a + 1) + 7; }
static NI uint64_t mix_b(uint64_t a) { return mix_d(a) * 5; }
static NI uint64_t mix_a(uint64_t a) { return mix_b(a) + mix_c(a ^ 9); }
typedef uint64_t (*Fn)(uint64_t);
static Fn g_tab[4];
static NI uint64_t ind0(uint64_t a) { return a + 1; }
static NI uint64_t ind1(uint64_t a) { return a * 2; }
static NI uint64_t ind2(uint64_t a) { return a ^ 5; }
static NI uint64_t ind3(uint64_t a) { return a - 3; }
/* Not reducible to a loop: every activation reads a volatile after its recursive call returns. */
static volatile uint64_t g_probe = 3;
static NI uint64_t deep(uint32_t n) {
  if (n == 0) return g_probe;
  uint64_t r = deep(n - 1);
  return r * g_probe + n;
}
/* Virtual calls: o->vt->f(o, a) through objects the compiler cannot see through. */
typedef struct Obj Obj;
typedef struct { uint64_t (*pad)(Obj *, uint64_t); uint64_t (*f)(Obj *, uint64_t); } VT;
struct Obj { const VT *vt; uint64_t k; };
static NI uint64_t vA(Obj *o, uint64_t a) { return a + o->k; }
static NI uint64_t vB(Obj *o, uint64_t a) { return a ^ o->k; }
static const VT g_vtA = { 0, vA }, g_vtB = { 0, vB };
static Obj g_objs[2];
static Obj *volatile g_op[2];
static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

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

int main(void) {
  uint64_t t, s;
  /* recursion: fib(38) = 126,491,971 calls */
  t = sceKernelGetProcessTime(); s = fib(38); report("fib38", t, 126491971ULL, s);
  /* leaf calls in a loop */
  t = sceKernelGetProcessTime(); s = 1; for (uint32_t i = 0; i < 200000000u; i++) s = leaf(s); report("leaf", t, 200000000ULL, s);
  /* 4 call sites, 2 deep: 4 calls per iteration */
  t = sceKernelGetProcessTime(); s = 1; for (uint32_t i = 0; i < 50000000u; i++) s = mix_a(s); report("mix", t, 200000000ULL, s);
  /* indirect calls through a table */
  g_tab[0] = ind0; g_tab[1] = ind1; g_tab[2] = ind2; g_tab[3] = ind3;
  t = sceKernelGetProcessTime(); s = 1; for (uint32_t i = 0; i < 100000000u; i++) s = g_tab[i & 3](s); report("indirect", t, 100000000ULL, s);
  /* virtual call, one site, one target (monomorphic) */
  g_objs[0].vt = &g_vtA; g_objs[0].k = 3; g_objs[1].vt = &g_vtB; g_objs[1].k = 5;
  g_op[0] = &g_objs[0]; g_op[1] = &g_objs[0];
  t = sceKernelGetProcessTime(); s = 1; for (uint32_t i = 0; i < 100000000u; i++) { Obj *o = g_op[i & 1]; s = o->vt->f(o, s); } report("vcall_mono", t, 100000000ULL, s);
  /* virtual call, one site, two targets alternating (polymorphic) */
  g_op[1] = &g_objs[1];
  t = sceKernelGetProcessTime(); s = 1; for (uint32_t i = 0; i < 100000000u; i++) { Obj *o = g_op[i & 1]; s = o->vt->f(o, s); } report("vcall_poly2", t, 100000000ULL, s);
  /* deep recursion inside the shadow stack: 20000 x 5000 */
  t = sceKernelGetProcessTime(); s = 0; for (uint32_t i = 0; i < 5000; i++) s += deep(20000); report("deep20k", t, 100000000ULL, s);
  /* deeper than the shadow stack (32768 entries): the overflow and slow-return path */
  t = sceKernelGetProcessTime(); s = 0; for (uint32_t i = 0; i < 100; i++) s += deep(100000); report("deep100k", t, 10000000ULL, s);
  /* HLE call: a shim that does next to nothing (guest -> host -> guest) */
  { int (*volatile absp)(int) = abs; /* through the pointer: the libc import, not a builtin */
    t = sceKernelGetProcessTime(); s = 0; for (uint32_t i = 0; i < 20000000u; i++) s += (uint64_t)absp((int)i - 7); report("shim_abs", t, 20000000ULL, s); }
  /* HLE -> guest callbacks: qsort of 200000 elements, 20 times (calls = an estimate, n log2 n) */
  static uint32_t v[200000];
  uint64_t seed = 7, cb = 0;
  t = sceKernelGetProcessTime();
  for (int r = 0; r < 20; r++) {
    for (int i = 0; i < 200000; i++) v[i] = (uint32_t)t_lcg(&seed);
    qsort(v, 200000, 4, cmp_u32);
    cb += v[100000];
  }
  report("qsort_cb", t, 20ULL * 200000 * 17, cb);
  g_sink = s;
  return 0;
}
