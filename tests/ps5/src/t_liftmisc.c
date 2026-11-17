/* m3-liftmisc (docs/m3/liftmisc.md): FP compares consumed straight from the FCMP flags (NaN, +-0,
 * inf), push/pop runs (callee-saved registers around calls, deep recursion), and cached indirect
 * jumps (tail calls through a pointer and through a vtable slot, targets changing under the site).
 * Every result is a hash of values that do not depend on timing; the output is the same with
 * every feature on or off and on the host build (-DNPS5_HOST). */
#include "ps5test.h"
#include <math.h>

#define N T_NOINLINE
static volatile float vf[12];
static volatile double vd[12];
static const float fv[] = {-1.5f, -0.0f, 0.0f, 0.5f, 2.0f, 1e30f, __builtin_inff(), -__builtin_inff(), __builtin_nanf(""), 3.0f, -3.0f, 0.5f};

/* Each function returns a bit set with every comparison operator, so ordered/unordered cases hit
 * ja/jae/jb/jbe/je/jne/jp/setcc/cmovcc of both widths. */
static N unsigned cmp_f(float a, float b) {
  return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 | (a == b) << 4 | (a != b) << 5 | (isnan(a) | isnan(b)) << 6 | (isunordered(a, b)) << 7;
}
static N unsigned cmp_d(double a, double b) {
  return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 | (a == b) << 4 | (a != b) << 5 | (isnan(a) | isnan(b)) << 6;
}
static N int sel_f(float a, float b, int x, int y) { return a < b ? x : y; }
static N int sel_g(float a, float b, int x, int y) { return a == b ? x : y; }
static N int sel_h(double a, double b, int x, int y) { return a != b ? x : y; }
static N double clampd(double v, double lo, double hi) { if (v < lo) v = lo; else if (v > hi) v = hi; return v; }
static N float minf(float a, float b) { return a < b ? a : b; }
static N float maxf(float a, float b) { return a > b ? a : b; }

static uint64_t fp_all(void) {
  uint64_t h = T_FNV0;
  int n = sizeof fv / sizeof fv[0];
  for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
    float a = fv[i], b = fv[j];
    double da = a, db = b;
    uint64_t v = cmp_f(a, b) | (uint64_t)cmp_d(da, db) << 8 | (uint64_t)(unsigned)sel_f(a, b, 11, 22) << 16 |
                 (uint64_t)(unsigned)sel_g(a, b, 33, 44) << 24 | (uint64_t)(unsigned)sel_h(da, db, 55, 66) << 32;
    float m = minf(a, b), M = maxf(a, b);
    uint32_t mb, Mb; __builtin_memcpy(&mb, &m, 4); __builtin_memcpy(&Mb, &M, 4);
    double c = clampd(da, -1.0, 1.0); uint64_t cb; __builtin_memcpy(&cb, &c, 8);
    h = t_fnv(h, &v, 8); h = t_fnv(h, &mb, 4); h = t_fnv(h, &Mb, 4); h = t_fnv(h, &cb, 8);
  }
  return h;
}

/* ---- push/pop: six live values across a call, and recursion through them ---- */
static volatile uint64_t g_k = 3;
static N uint64_t leaf(uint64_t a) { return a * 3 + g_k; }
static N uint64_t six(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
  uint64_t r = leaf(a) + a * b;
  return r + b * c + d * e + f + a + b + c + d + e + f;
}
static N uint64_t four(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return leaf(a) + a * b + b * c + d + a + b + c + d; }
static N uint64_t rec(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
  if (n == 0) return a + b + c + d + e;
  return rec(n - 1, b + 1, c ^ n, d + a, e * 3, a) + leaf(n) + a * b + c * d + e;
}
static uint64_t pp_all(void) {
  uint64_t h = T_FNV0, x = 1;
  for (uint64_t i = 0; i < 2000; i++) { x = six(x, i, 3, 4, 5, 6) & 0xffffffffff; x = four(x, i, 7, 8) & 0xffffffffff; h = t_fnv(h, &x, 8); }
  for (int n = 0; n < 40; n++) { uint64_t r = rec(n, 1, 2, 3, 4, 5); h = t_fnv(h, &r, 8); }
  uint64_t d = rec(3000, 1, 2, 3, 4, 5); h = t_fnv(h, &d, 8);
  return h;
}

/* ---- cached indirect jumps: `return f(x)` is jmp reg, `return o->vt->f(o, x)` is jmp [mem] ---- */
typedef uint64_t (*Fn)(uint64_t);
#define DEF(n, e) static N uint64_t f##n(uint64_t x) { return (e); }
DEF(0, x + 1) DEF(1, x * 3) DEF(2, x ^ 0x55) DEF(3, x - 7) DEF(4, (x << 1) | 1) DEF(5, x + (x >> 3)) DEF(6, x * x + 1) DEF(7, ~x)
static Fn tab[8] = {f0, f1, f2, f3, f4, f5, f6, f7};
static N uint64_t tail(Fn f, uint64_t x) { return f(x); }
typedef struct O O;
typedef struct { uint64_t (*pad)(O *, uint64_t); uint64_t (*f)(O *, uint64_t); } VT;
struct O { const VT *vt; uint64_t k; };
static N uint64_t va(O *o, uint64_t x) { return x + o->k; }
static N uint64_t vb(O *o, uint64_t x) { return x ^ o->k; }
static N uint64_t vc(O *o, uint64_t x) { return x * 5 + o->k; }
static const VT vta = {0, va}, vtb = {0, vb}, vtc = {0, vc};
static N uint64_t vtail(O *o, uint64_t x) { return o->vt->f(o, x); }
static uint64_t jmp_all(void) {
  uint64_t h = T_FNV0, x = 7, s = 99;
  for (int phase = 1; phase <= 8; phase *= 2)            /* 1, 2, 4, 8 targets, each phase changes the set */
    for (uint64_t i = 0; i < 4000; i++) { x = tail(tab[(i * phase / 4000 + (i % phase)) & 7], x ^ i); h = t_fnv(h, &x, 8); }
  for (uint64_t i = 0; i < 4000; i++) { x = tail(tab[t_lcg(&s) & 7], x); h = t_fnv(h, &x, 8); }
  O o[3] = {{&vta, 3}, {&vtb, 5}, {&vtc, 9}};
  for (int burst = 0; burst < 6; burst++) {              /* the object's vtable changes between bursts */
    o[0].vt = burst & 1 ? &vtb : (burst & 2 ? &vtc : &vta);
    for (uint64_t i = 0; i < 1000; i++) { x = vtail(&o[i % 3], x + i); h = t_fnv(h, &x, 8); }
  }
  return h;
}

int main(void) {
  t_hex("fp", fp_all());
  t_hex("pushpop", pp_all());
  t_hex("jmp", jmp_all());
  return 0;
}
