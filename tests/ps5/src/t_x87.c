/* x87: extended precision, fcomip flag semantics (incl. unordered -> PF), control
 * word rounding, fild/fistp, fscale, fxch.  Values chosen so the host double model is
 * exact, except cases that need 64-bit mantissa where the model hardcodes the x86
 * result (noted inline). */
#include "ps5test.h"
#include <string.h>

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* returns CF|PF<<1|ZF<<2 from fcomip a vs b */
static T_NOINLINE unsigned fcomip_flags(double a, double b) {
#ifdef __x86_64__
  unsigned char c, p, z;
  __asm__ volatile("fldl %[b]\n fldl %[a]\n fcomip %%st(1),%%st\n fstp %%st(0)\n setb %[c]\n setp %[p]\n sete %[z]"
                   : [c]"=q"(c), [p]"=q"(p), [z]"=q"(z) : [a]"m"(a), [b]"m"(b) : "cc", "st", "st(1)");
  return c | p << 1 | z << 2;
#else
  if (a != a || b != b) return 7;
  return a < b ? 1 : a == b ? 4 : 0;
#endif
}
static T_NOINLINE unsigned fcomip_jp(double a, double b) { /* jp consumer of x87 compare */
#ifdef __x86_64__
  unsigned r;
  __asm__ volatile("fldl %[b]\n fldl %[a]\n fcomip %%st(1),%%st\n fstp %%st(0)\n jp 1f\n movl $0,%[r]\n jmp 2f\n1: movl $1,%[r]\n2:"
                   : [r]"=&r"(r) : [a]"m"(a), [b]"m"(b) : "cc", "st", "st(1)");
  return r;
#else
  return (a != a || b != b);
#endif
}
static T_NOINLINE int64_t fist_mode(double v, int mode) { /* mode: 0 nearest 1 down 2 up 3 trunc */
#ifdef __x86_64__
  uint16_t cw, nw; int64_t r;
  __asm__ volatile("fnstcw %[cw]" : [cw]"=m"(cw));
  nw = (cw & ~0x0c00) | (mode << 10);
  __asm__ volatile("fldcw %[nw]\n fldl %[v]\n fistpll %[r]\n fldcw %[cw]" : [r]"=m"(r) : [nw]"m"(nw), [v]"m"(v), [cw]"m"(cw) : "st");
  return r;
#else
  double f = __builtin_floor(v);
  switch (mode) {
  case 0: { double d = v - f; return (int64_t)(d < 0.5 ? f : d > 0.5 ? f + 1 : (((int64_t)f & 1) ? f + 1 : f)); }
  case 1: return (int64_t)f;
  case 2: return (int64_t)__builtin_ceil(v);
  default: return (int64_t)v;
  }
#endif
}
static T_NOINLINE double fscale_d(double v, int n) {
#ifdef __x86_64__
  double r; int16_t k = n;
  __asm__ volatile("filds %[k]\n fldl %[v]\n fscale\n fstpl %[r]\n fstp %%st(0)" : [r]"=m"(r) : [k]"m"(k), [v]"m"(v) : "st", "st(1)");
  return r;
#else
  return __builtin_ldexp(v, n);
#endif
}
static T_NOINLINE double ext_sqrt2(void) {
#ifdef __x86_64__
  double r;
  __asm__ volatile("fld1\n fld1\n faddp\n fsqrt\n fstpl %[r]" : [r]"=m"(r) : : "st");
  return r;
#else
  return __builtin_sqrt(2.0);
#endif
}
static T_NOINLINE double ext_pi(void) {
#ifdef __x86_64__
  double r; __asm__ volatile("fldpi\n fstpl %[r]" : [r]"=m"(r) : : "st"); return r;
#else
  return 3.14159265358979323846;
#endif
}
/* extended-precision identity: (x+1)-x with x=1e16: 1.0 in 64-bit mantissa, 0.0 in double. */
static T_NOINLINE double ext_add_sub(void) {
#ifdef __x86_64__
  volatile long double x = 1e16L; volatile long double one = 1.0L;
  long double y = (x + one) - x;
  return (double)y;
#else
  return 1.0; /* x86 reference value */
#endif
}
static T_NOINLINE uint64_t ext_mant_bits(void) { /* mantissa of 1/3 in extended, via fstpt */
#ifdef __x86_64__
  volatile long double t = 1.0L, three = 3.0L;
  long double r = t / three;
  uint8_t b[16] = {0}; memcpy(b, &r, 10);
  uint64_t m; memcpy(&m, b, 8);
  return m;
#else
  return 0xaaaaaaaaaaaaaaabULL; /* 1/3 = 1.0101..b * 2^-2, rounded to nearest in 64 bits */
#endif
}
static T_NOINLINE double mixed_x87(double a, double b) {
#ifdef __x86_64__
  double r;
  __asm__ volatile("fldl %[a]\n fldl %[b]\n fxch %%st(1)\n fmul %%st(1),%%st\n fchs\n fabs\n faddp\n fstpl %[r]" : [r]"=m"(r) : [a]"m"(a), [b]"m"(b) : "st", "st(1)");
  return r;
#else
  /* st0=b,st1=a after loads; fxch -> st0=a, st1=b; fmul st(1): st0=a*b; fchs; fabs -> |a*b|; faddp: st1 = b + |a*b| */
  double p = a * b; if (p < 0) p = -p;
  return b + p;
#endif
}

int main(void) {
  static const double fv[] = {0.0, -0.0, 1.0, -1.0, 2.5, -2.5, 3.5, 0.5, 1e300, -1e300, 1.0/0.0, -1.0/0.0, 0.0/0.0};
  unsigned n = sizeof fv / sizeof fv[0];
  uint64_t h = T_FNV0;
  for (unsigned i = 0; i < n; i++) for (unsigned j = 0; j < n; j++) {
    unsigned f = fcomip_flags(fv[i], fv[j]) | fcomip_jp(fv[i], fv[j]) << 3;
    h = t_fnv(h, &f, 4);
  }
  t_hex("fcomip_fnv", h);
  t_hex("fcomip_nan_1", fcomip_flags(0.0/0.0, 1.0));
  t_hex("fcomip_lt", fcomip_flags(1.0, 2.0));
  t_hex("fcomip_eq", fcomip_flags(2.0, 2.0));
  t_hex("fcomip_gt", fcomip_flags(3.0, 2.0));
  static const double iv[] = {0.0, 0.4, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 123456789.5, -7.9, 7.9};
  for (int m = 0; m < 4; m++) {
    uint64_t r = T_FNV0;
    for (unsigned i = 0; i < sizeof iv / sizeof iv[0]; i++) { int64_t x = fist_mode(iv[i], m); r = t_fnv(r, &x, 8); }
    t_dec("fist_mode", m); t_hex("fist_fnv", r);
  }
  t_hex("fist_nearest_2.5", (uint64_t)fist_mode(2.5, 0));
  t_hex("fist_trunc_-7.9", (uint64_t)fist_mode(-7.9, 3));
  t_hex("fscale_3_5", bits(fscale_d(3.0, 5)));
  t_hex("fscale_1_-1074", bits(fscale_d(1.0, -1074)));
  t_hex("sqrt2", bits(ext_sqrt2()));
  t_hex("pi", bits(ext_pi()));
  t_hex("ext_add_sub", bits(ext_add_sub()));
  t_hex("ext_mant_third", ext_mant_bits());
  t_hex("mixed", bits(mixed_x87(-3.0, 7.0)));
  return 0;
}
