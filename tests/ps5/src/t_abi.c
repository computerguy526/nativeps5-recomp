/* Mixed int/FP and struct-by-value System V ABI, both user<->user (through volatile
 * function pointers so nothing inlines) and user<->libc (div/ldiv/lldiv, qsort callback,
 * strtod, frexp/modf out-pointers, math).  Includes long double (x87 return) and __int128. */
#include "ps5test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y; } DD;                 /* SSE,SSE   -> xmm0:xmm1 */
typedef struct { long a, b; } LL;                   /* INT,INT   -> rax:rdx */
typedef struct { long a; double b; } LD;            /* INT,SSE   -> rax:xmm0 */
typedef struct { double a; long b; } DL;            /* SSE,INT   -> xmm0:rax */
typedef struct { float a, b, c; } F3;               /* SSE,SSE   -> xmm0 (a,b) xmm1 (c) */
typedef struct { float a, b; } F2;                  /* one SSE eightbyte */
typedef struct { long a, b, c, d; } Big;            /* MEMORY */
typedef struct { char c; short s; int i; } Small;   /* one INT eightbyte pair */
typedef struct { double d[3]; } D3;                 /* MEMORY */

static T_NOINLINE DD mk_dd(double a, double b) { return (DD){a + b, a * b}; }
static T_NOINLINE LL mk_ll(long a, long b) { return (LL){a * b, a - b}; }
static T_NOINLINE LD mk_ld(long a, double b) { return (LD){a + 1, b * 2}; }
static T_NOINLINE DL mk_dl(double a, long b) { return (DL){a * 2, b + 3}; }
static T_NOINLINE F3 mk_f3(float a, float b, float c) { return (F3){a + b, b + c, c + a}; }
static T_NOINLINE F2 mk_f2(F2 v) { return (F2){v.a * v.b, v.a - v.b}; }
static T_NOINLINE Big mk_big(long a, Big b) { return (Big){a + b.a, a + b.b, a + b.c, a + b.d}; }
static T_NOINLINE Small mk_small(Small s, int k) { return (Small){(char)(s.c + k), (short)(s.s * k), s.i - k}; }
static T_NOINLINE D3 mk_d3(D3 v, double k) { return (D3){{v.d[0] * k, v.d[1] + k, v.d[2] - k}}; }
static T_NOINLINE long double mk_ldbl(long double a, long double b) { return a * b + 1.0L / 3.0L; }
static T_NOINLINE __int128 mk_i128(__int128 a, long b) { return a * b + 0x0123456789abcdefLL; }
/* many args: 6 int regs + 8 fp regs + stack args */
static T_NOINLINE double many(int a, double b, long c, float d, int e, double f, short g, double h, char i, double j,
                              long k, double l, int m, double n, double o, double p, long q, float r, int s) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13 + n * 14 + o * 15 + p * 16 + q * 17 + r * 18 + s * 19;
}
static T_NOINLINE DD pass_struct_mix(LD a, DD b, F3 c, Small d, double e, int f) {
  return (DD){a.a + a.b + b.x + c.a + d.i + e, b.y * c.b + c.c + d.c + f};
}
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static int cmp_ll(const void *a, const void *b) { long x = *(const long *)a, y = *(const long *)b; return x < y ? -1 : x > y; }

int main(void) {
  uint64_t h = T_FNV0;
  DD (*volatile pdd)(double, double) = mk_dd; DD r1 = pdd(1.5, 4.0); h = t_fnv(h, &r1, 16);
  LL (*volatile pll)(long, long) = mk_ll; LL r2 = pll(-7, 9); h = t_fnv(h, &r2, 16);
  LD (*volatile pld)(long, double) = mk_ld; LD r3 = pld(41, 0.125); h = t_fnv(h, &r3, 16);
  DL (*volatile pdl)(double, long) = mk_dl; DL r4 = pdl(8.5, 99); h = t_fnv(h, &r4, 16);
  F3 (*volatile pf3)(float, float, float) = mk_f3; F3 r5 = pf3(1.5f, 2.25f, -3.0f); h = t_fnv(h, &r5, 12);
  F2 r6 = mk_f2((F2){3.0f, 0.5f}); h = t_fnv(h, &r6, 8);
  Big r7 = mk_big(10, (Big){1, 2, 3, 4}); h = t_fnv(h, &r7, 32);
  Small r8 = mk_small((Small){'a', 300, 7}, 3); h = t_fnv(h, &r8.c, 1); h = t_fnv(h, &r8.s, 2); h = t_fnv(h, &r8.i, 4);
  D3 r9 = mk_d3((D3){{1, 2, 3}}, 0.5); h = t_fnv(h, &r9, 24);
  t_hex("struct_ret_fnv", h);
  long double ld = mk_ldbl(1.5L, 3.0L);
  t_hex("long_double_as_double", bits((double)ld));
  __int128 i128 = mk_i128(((__int128)1 << 70) + 5, -3);
  t_hex("i128_lo", (uint64_t)i128); t_hex("i128_hi", (uint64_t)(i128 >> 64));
  double (*volatile pm)(int, double, long, float, int, double, short, double, char, double, long, double, int, double, double, double, long, float, int) = many;
  t_hex("many_args", bits(pm(1, 2.5, 3, 4.5f, 5, 6.5, 7, 8.5, 9, 10.5, 11, 12.5, 13, 14.5, 15.5, 16.5, 17, 18.5f, 19)));
  DD r10 = pass_struct_mix((LD){5, 0.5}, (DD){1.25, 2.0}, (F3){1, 2, 3}, (Small){'b', 1, 100}, 0.75, 4);
  t_hex("struct_mix_x", bits(r10.x)); t_hex("struct_mix_y", bits(r10.y));
  /* libc: struct returns, callbacks, out-pointers */
  div_t dv = div(-47, 5); ldiv_t ldv = ldiv(-1234567890123L, 1000L); lldiv_t lldv = lldiv(9876543210LL, -777LL);
  t_dec("div_quot", dv.quot); t_dec("div_rem", dv.rem); t_dec("ldiv_quot", ldv.quot); t_dec("ldiv_rem", ldv.rem);
  t_dec("lldiv_quot", lldv.quot); t_dec("lldiv_rem", lldv.rem);
  long arr[9] = {5, -2, 99, 0, 17, 17, -50, 3, 8}; qsort(arr, 9, sizeof(long), cmp_ll);
  t_hex("qsort_fnv", t_fnv(T_FNV0, arr, sizeof arr));
  long key = 17; long *f = (long *)bsearch(&key, arr, 9, sizeof(long), cmp_ll); t_dec("bsearch_idx_ok", f != 0 && *f == 17);
  const char *ss = "  -12.5e-1xyz"; char *end; double sd = strtod(ss, &end); t_hex("strtod", bits(sd)); t_dec("strtod_consumed", (long long)(end - ss));
  int ex; double fr = frexp(48.0, &ex); t_hex("frexp_frac", bits(fr)); t_dec("frexp_exp", ex);
  double ip; double fp = modf(-7.625, &ip); t_hex("modf_frac", bits(fp)); t_hex("modf_int", bits(ip));
  t_hex("fmod", bits(fmod(17.5, 3.0))); t_hex("ldexp", bits(ldexp(0.75, 10))); t_hex("sqrt", bits(sqrt(1024.0)));
  t_hex("floor_ceil", bits(floor(-2.5) + ceil(2.1) * 100));
  t_hex("copysign", bits(copysign(3.0, -0.0)));
  t_dec("atoi", atoi("  -31337")); t_dec("strtol_hex", strtol("0xff", 0, 16)); t_dec("labs", labs(-9000000000L));
  /* transcendental: rounded to 6 decimals so a 1-ulp libm difference cannot show */
  t_dec("sin_e6", (long long)lround(sin(1.0) * 1e6)); t_dec("cos_e6", (long long)lround(cos(1.0) * 1e6));
  t_dec("exp_e6", (long long)lround(exp(1.5) * 1e6)); t_dec("log_e6", (long long)lround(log(10.0) * 1e6));
  t_dec("pow_e6", (long long)lround(pow(2.0, 0.5) * 1e6)); t_dec("atan2_e6", (long long)lround(atan2(1.0, -2.0) * 1e6));
  return 0;
}
