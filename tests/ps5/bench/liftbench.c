/* Microbenchmarks for the m3-liftmisc lifter changes (tools/liftbench.sh; docs/m3/liftmisc.md).
 *   fp_*    UCOMISS/UCOMISD + Jcc/SETcc/CMOVcc loops (clamp, min/max with NaN handling, count-if)
 *   pp_*    functions that save/restore 6 callee-saved registers around a call (push/pop runs)
 *   tail_*  `return f(x)` through a pointer and through a vtable slot: jmp reg / jmp [mem]
 * Times come from sceKernelGetProcessTime (usec). The checks are the same with every flag. */
#include "../src/ps5test.h"
#include <stdlib.h>
extern uint64_t sceKernelGetProcessTime(void);
#define NI __attribute__((noinline))
static volatile uint64_t g_sink;

/* ---- FP compares ---- */
static float g_f[256];
static double g_d[256];
static NI uint64_t fp_count(const float *a, uint64_t n, float lo, float hi) {
  uint64_t c = 0;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) if (a[i] > lo && a[i] <= hi) c++;
  return c;
}
static NI double fp_clamp(const double *a, uint64_t n, double lo, double hi) {
  double s = 0;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) { double v = a[i]; if (v < lo) v = lo; else if (v > hi) v = hi; s += v; }
  return s;
}
static volatile uint64_t g_rare;
static NI void rare(uint64_t i) { g_rare += i; }
/* two predictable compare+branch pairs per element, no select: the data stay inside [lo, hi] */
static NI uint64_t fp_branch(const float *a, uint64_t n, float lo, float hi) {
  uint64_t c = 0;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) { float v = a[i]; if (v > hi) rare(i); if (v < lo) rare(i + 1); c += i; }
  return c;
}
static NI uint64_t fp_eq(const float *a, const float *b, uint64_t n) {
  uint64_t c = 0;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) if (a[i] == b[i]) c++;
  return c;
}
static NI uint64_t fp_ne(const float *a, const float *b, uint64_t n) {
  uint64_t c = 0;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) if (a[i] != b[i]) c += 2;
  return c;
}
static NI uint32_t fp_sel(const float *a, uint64_t n) {
  uint32_t m = 7;
  _Pragma("clang loop vectorize(disable) interleave(disable)")
  for (uint64_t i = 0; i < n; i++) m = a[i] > 0.5f ? m + 3 : m ^ 0x55;
  return m;
}

/* ---- push/pop runs ---- */
static volatile uint64_t g_k = 3;
static NI uint64_t leafcall(uint64_t a) { return a * 3 + g_k; }
static NI uint64_t pp6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
  uint64_t r = leafcall(a) + a * b;      /* a..f live across the call */
  return r + b * c + d * e + f + a + b + c + d + e + f;
}
static NI uint64_t pp4(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  uint64_t r = leafcall(a) + a * b;
  return r + b * c + d + a + b + c + d;
}

/* ---- tail calls through pointers ---- */
typedef uint64_t (*Fn)(uint64_t);
static NI uint64_t t0(uint64_t a) { return a + 1; }
static NI uint64_t t1(uint64_t a) { return a * 3; }
static NI uint64_t t2(uint64_t a) { return a ^ 0x55; }
static NI uint64_t t3(uint64_t a) { return a - 7; }
static Fn g_tab[4];
static NI uint64_t tail_ptr(Fn f, uint64_t a) { return f(a); }
typedef struct Obj Obj;
typedef struct { uint64_t (*pad)(Obj *, uint64_t); uint64_t (*f)(Obj *, uint64_t); } VT;
struct Obj { const VT *vt; uint64_t k; };
static NI uint64_t vA(Obj *o, uint64_t a) { return a + o->k; }
static NI uint64_t vB(Obj *o, uint64_t a) { return a ^ o->k; }
static const VT g_vtA = {0, vA}, g_vtB = {0, vB};
static NI uint64_t tail_virt(Obj *o, uint64_t a) { return o->vt->f(o, a); }
static Obj g_o[2];

static void t_u64(uint64_t u) {
  char b[24]; int i = 24; b[--i] = 0;
  do { b[--i] = '0' + u % 10; u /= 10; } while (u);
  t_str(b + i);
}
static void report(const char *name, uint64_t t0u, uint64_t units, uint64_t check) {
  uint64_t us = sceKernelGetProcessTime() - t0u;
  char b[17]; t_hexs(b, check, 16); b[16] = 0;
  t_str(name); t_str(" check="); t_str(b);
  t_str(" calls="); t_u64(units);
  t_str(" ns_per_call_x100="); t_u64(units ? us * 100000ULL / units : 0);
  t_str("\n");
}

int main(void) {
  uint64_t s = 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < 256; i++) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    g_f[i] = 0.25f + (float)(int32_t)(s >> 40) / 8388608.0f / 64.0f;
    g_d[i] = (double)(int32_t)(s >> 33) / 1073741824.0 / 4.0;
    if ((i % 64) == 5) g_f[i] = __builtin_nanf("");
    if ((i % 64) == 3) g_f[i] = 0.5f;
  }
  float gb[256]; for (int i = 0; i < 256; i++) gb[i] = (i % 64) ? g_f[i] : g_f[(i + 1) & 255];
  const uint64_t R = 40000;
  uint64_t t, h;
  t = sceKernelGetProcessTime(); h = 0;
  for (uint64_t r = 0; r < R; r++) h += fp_count(g_f + (r & 7), 248, -0.25f, 0.5f);
  report("fp_count", t, R * 248, h);
  t = sceKernelGetProcessTime(); double ds = 0;
  for (uint64_t r = 0; r < R; r++) ds += fp_clamp(g_d + (r & 7), 248, -0.5, 0.5);
  report("fp_clamp", t, R * 248, (uint64_t)(int64_t)(ds * 1000));
  t = sceKernelGetProcessTime(); h = 0;
  for (uint64_t r = 0; r < R; r++) h += fp_branch(g_f + (r & 7), 248, -9.0f, 9.0f);
  report("fp_branch", t, R * 248, h + g_rare);
  t = sceKernelGetProcessTime(); h = 0;
  for (uint64_t r = 0; r < R; r++) h += fp_eq(g_f + (r & 7), gb + (r & 7), 248);
  report("fp_eq", t, R * 248, h);
  t = sceKernelGetProcessTime(); h = 0;
  for (uint64_t r = 0; r < R; r++) h += fp_ne(g_f + (r & 7), gb + (r & 7), 248);
  report("fp_ne", t, R * 248, h);
  t = sceKernelGetProcessTime(); h = 0;
  for (uint64_t r = 0; r < R; r++) h += fp_sel(g_f + (r & 7), 248);
  report("fp_sel", t, R * 248, h);

  const uint64_t N = 20000000;
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = pp6(h, i, 3, 4, 5, 6) & 0xffffffff;
  report("pp6", t, N, h);
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = pp4(h, i, 3, 4) & 0xffffffff;
  report("pp4", t, N, h);

  g_tab[0] = t0; g_tab[1] = t1; g_tab[2] = t2; g_tab[3] = t3;
  g_o[0].vt = &g_vtA; g_o[0].k = 5; g_o[1].vt = &g_vtB; g_o[1].k = 9;
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = tail_ptr(g_tab[0], h ^ i);
  report("tail_ptr_mono", t, N, h);
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = tail_ptr(g_tab[i & 1], h ^ i);
  report("tail_ptr_poly2", t, N, h);
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = tail_ptr(g_tab[i & 3], h ^ i);
  report("tail_ptr_rr4", t, N, h);
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = tail_virt(&g_o[0], h ^ i);
  report("tail_virt_mono", t, N, h);
  t = sceKernelGetProcessTime(); h = 1;
  for (uint64_t i = 0; i < N; i++) h = tail_virt(&g_o[i & 1], h ^ i);
  report("tail_virt_poly2", t, N, h);
  return 0;
}
