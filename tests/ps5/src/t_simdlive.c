/* Vector state at the points where the lifter's lane liveness (docs/m3/simdregs.md) changes what
 * is kept in the host registers: calls in the middle of scalar and packed sequences, more live
 * values than argument registers, __m128/__m256 arguments and results (returned through a
 * wrapper, by tail call, zero-extended from 128 bits), scalar intrinsics whose upper lanes are
 * part of the result, varargs, callbacks from libc (qsort), indirect calls, setjmp/longjmp.
 * Built twice: with AVX2/FMA (VEX encodings, YMM) and without (legacy SSE2 encodings).  The
 * x86 code and the portable model compute the same bits; the host build prints the model. */
#include "ps5test.h"
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef __x86_64__
#include <immintrin.h>
#endif

static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static volatile int g_sink;

/* ---- 1. scalar double kernel (LWC style) with calls in the middle ---- */
typedef struct { double m[4][4]; } M4;

static T_NOINLINE double det2(double a, double b, double c, double d) { return a * d - b * c; }
static T_NOINLINE double mix10(double a, double b, double c, double d, double e, double f, double g, double h,
                               double i, double j) { /* 8 in xmm0-7, 2 on the stack */
  return a - b * 2 + c * 3 - d * 4 + e * 5 - f * 6 + g * 7 - h * 8 + i * 9 - j * 10;
}

static T_NOINLINE double inverse(const M4 *in, M4 *out) {
  const double (*m)[4] = in->m;
  double s0 = det2(m[0][0], m[0][1], m[1][0], m[1][1]);
  double s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
  double s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
  double s3 = det2(m[0][1], m[0][2], m[1][1], m[1][2]);
  double s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
  double s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
  double c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
  double c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
  double c3 = det2(m[2][1], m[2][2], m[3][1], m[3][2]);
  double c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
  double c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
  double c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
  double det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
  double id = 1.0 / det;
  double (*o)[4] = out->m;
  o[0][0] = (m[1][1] * c5 - m[1][2] * c4 + m[1][3] * c3) * id;
  o[0][1] = (-m[0][1] * c5 + m[0][2] * c4 - m[0][3] * c3) * id;
  o[0][2] = (m[3][1] * s5 - m[3][2] * s4 + m[3][3] * s3) * id;
  o[0][3] = (-m[2][1] * s5 + m[2][2] * s4 - m[2][3] * s3) * id;
  o[1][0] = (-m[1][0] * c5 + m[1][2] * c2 - m[1][3] * c1) * id;
  o[1][1] = (m[0][0] * c5 - m[0][2] * c2 + m[0][3] * c1) * id;
  o[1][2] = (-m[3][0] * s5 + m[3][2] * s2 - m[3][3] * s1) * id;
  o[1][3] = (m[2][0] * s5 - m[2][2] * s2 + m[2][3] * s1) * id;
  o[2][0] = (m[1][0] * c4 - m[1][1] * c2 + m[1][3] * c0) * id;
  o[2][1] = (-m[0][0] * c4 + m[0][1] * c2 - m[0][3] * c0) * id;
  o[2][2] = (m[3][0] * s4 - m[3][1] * s2 + m[3][3] * s0) * id;
  o[2][3] = (-m[2][0] * s4 + m[2][1] * s2 - m[2][3] * s0) * id;
  o[3][0] = (-m[1][0] * c3 + m[1][1] * c1 - m[1][2] * c0) * id;
  o[3][1] = (m[0][0] * c3 - m[0][1] * c1 + m[0][2] * c0) * id;
  o[3][2] = (-m[3][0] * s3 + m[3][1] * s1 - m[3][2] * s0) * id;
  o[3][3] = (m[2][0] * s3 - m[2][1] * s1 + m[2][2] * s0) * id;
  /* all twelve minors are still needed after this call */
  return mix10(s0, s1, s2, s3, s4, s5, c0, c1, c2, c3) + c4 * c5 + det;
}

/* ---- 2. many values live across calls (more than the 16 registers hold) ---- */
static T_NOINLINE float fstep(float x, float y) { return x * 0.5f + y; }
static T_NOINLINE double many_live(const double *p, const float *q) {
  double a = p[0] * p[1], b = p[2] + p[3], c = p[4] - p[5], d = p[6] * p[7];
  double e = p[8] / p[9], f = p[10] * p[11], g = p[12] + p[13], h = p[14] - p[15];
  float u = q[0] * q[1], v = q[2] + q[3], w = q[4] - q[5], x = q[6] * q[7];
  double r1 = det2(a, b, c, d);
  float t1 = fstep(u, v);
  double r2 = det2(e, f, g, h);
  float t2 = fstep(w, x);
  double r3 = mix10(a, b, c, d, e, f, g, h, r1, r2);
  return r1 * a + r2 * b + r3 * c + d * e + f * g + h * (double)t1 + (double)t2 * (double)u + (double)(v * w) + (double)x;
}

/* ---- 3. packed float loops with a call per row ---- */
static T_NOINLINE float rowsum(const float *v, int n) {
  float s = 0;
  for (int i = 0; i < n; i++) s += v[i];
  return s;
}
static T_NOINLINE void transform(float *dst, const float *src, const float *mat, int rows) {
  for (int r = 0; r < rows; r++) {
    float k = rowsum(mat + 8 * (r & 3), 8); /* call between the packed loops */
    for (int i = 0; i < 16; i++) dst[16 * r + i] = src[16 * r + i] * mat[i] + k;
    for (int i = 0; i < 16; i++) dst[16 * r + i] = dst[16 * r + i] * dst[16 * r + i] - src[16 * r + (i ^ 1)];
  }
}

/* ---- 4. vector arguments and results (x86: intrinsics; host: the same lanes by hand) ---- */
typedef struct { float f[8]; } F8;
typedef struct { double d[4]; } D4;
#if defined(__x86_64__) && defined(__AVX2__)
static T_NOINLINE __m256 scale256(__m256 v, float s) { return _mm256_mul_ps(v, _mm256_set1_ps(s)); }
static T_NOINLINE __m256 pass256(__m256 v, float s) { return scale256(v, s); } /* tail call with a YMM argument */
static T_NOINLINE __m256 call256(__m256 v, float s) { /* YMM result of a call returned untouched */
  g_sink++;
  __m256 r = scale256(v, s);
  g_sink++;
  return r;
}
static T_NOINLINE __m256 zext256(__m128 v) { return _mm256_zextps128_ps256(v); } /* bits 128.. must be 0 */
static T_NOINLINE __m128 low128(__m256 v, __m256 w) { return _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(w, 1)); }
static T_NOINLINE __m256d fma256(__m256d a, __m256d b, __m256d c) { return _mm256_fmadd_pd(a, b, c); }
/* reads bits 128..255 of its arguments: the caller's zero extension must have happened */
static T_NOINLINE __m128 high128(__m256 v, __m256 w) { return _mm_sub_ps(_mm256_extractf128_ps(v, 1), _mm256_extractf128_ps(w, 1)); }
static T_NOINLINE __m128 zext_arg(__m256 dirty, __m128 a, __m128 b) {
  /* ymm0/ymm1 hold "dirty" upper halves; the arguments of high128 are rebuilt by 128-bit adds */
  __m256 keep = scale256(dirty, 5.0f);
  __m128 r = high128(_mm256_zextps128_ps256(_mm_add_ps(a, b)), _mm256_zextps128_ps256(_mm_sub_ps(a, b)));
  return _mm_add_ps(r, _mm256_castps256_ps128(keep));
}
static void vec256(const F8 *in, const D4 *din, uint64_t *h) {
  __m256 v = _mm256_loadu_ps(in->f);
  F8 o;
  _mm256_storeu_ps(o.f, pass256(v, 3.0f));
  *h = t_fnv(*h, &o, sizeof o);
  _mm256_storeu_ps(o.f, call256(v, -0.25f));
  *h = t_fnv(*h, &o, sizeof o);
  _mm256_storeu_ps(o.f, zext256(_mm_loadu_ps(in->f + 2)));
  *h = t_fnv(*h, &o, sizeof o);
  memset(&o, 0, sizeof o);
  _mm_storeu_ps(o.f, low128(v, scale256(v, 2.0f)));
  *h = t_fnv(*h, &o, sizeof o);
  memset(&o, 0, sizeof o);
  _mm_storeu_ps(o.f, zext_arg(v, _mm_loadu_ps(in->f), _mm_loadu_ps(in->f + 4)));
  *h = t_fnv(*h, &o, sizeof o);
  D4 d;
  __m256d x = _mm256_loadu_pd(din->d);
  _mm256_storeu_pd(d.d, fma256(x, x, _mm256_set1_pd(0.5)));
  *h = t_fnv(*h, &d, sizeof d);
}
#else
static void vec256(const F8 *in, const D4 *din, uint64_t *h) {
  F8 o;
  for (int i = 0; i < 8; i++) o.f[i] = in->f[i] * 3.0f;
  *h = t_fnv(*h, &o, sizeof o);
  for (int i = 0; i < 8; i++) o.f[i] = in->f[i] * -0.25f;
  *h = t_fnv(*h, &o, sizeof o);
  memset(&o, 0, sizeof o);
  for (int i = 0; i < 4; i++) o.f[i] = in->f[2 + i];
  *h = t_fnv(*h, &o, sizeof o);
  memset(&o, 0, sizeof o);
  for (int i = 0; i < 4; i++) o.f[i] = in->f[i] + in->f[4 + i] * 2.0f;
  *h = t_fnv(*h, &o, sizeof o);
  memset(&o, 0, sizeof o);
  for (int i = 0; i < 4; i++) o.f[i] = (0.0f - 0.0f) + in->f[i] * 5.0f;
  *h = t_fnv(*h, &o, sizeof o);
  D4 d;
  for (int i = 0; i < 4; i++) d.d[i] = fma(din->d[i], din->d[i], 0.5);
  *h = t_fnv(*h, &d, sizeof d);
}
#endif

/* Scalar intrinsics whose untouched lanes belong to the result and are returned in xmm0. */
typedef struct { float f[4]; } F4;
typedef struct { double d[2]; } D2;
#ifdef __x86_64__
static T_NOINLINE __m128d addsd_ret(__m128d a, __m128d b) { return _mm_add_sd(a, b); }
static T_NOINLINE __m128d mulsd_chain(__m128d a, __m128d b) { return _mm_mul_sd(_mm_sub_sd(_mm_add_sd(a, b), b), a); }
static T_NOINLINE __m128 sqrtss_ret(__m128 a) { return _mm_sqrt_ss(a); }
static T_NOINLINE __m128 movess_ret(__m128 a, __m128 b) { return _mm_move_ss(a, b); }
static T_NOINLINE __m128 cvtsi_ret(__m128 a, int k) { return _mm_cvtsi32_ss(a, k); }
static T_NOINLINE __m128d cvtss2sd_ret(__m128d a, __m128 b) { return _mm_cvtss_sd(a, b); }
static T_NOINLINE __m128d loadsd_ret(const double *p) { return _mm_mul_sd(_mm_load_sd(p), _mm_load_sd(p + 1)); } /* upper = 0 */
static T_NOINLINE __m128 minss_ret(__m128 a, __m128 b) { return _mm_min_ss(a, b); }
/* all eight vector argument registers, both lanes of each read by the callee */
static T_NOINLINE __m128d sum8(__m128d a, __m128d b, __m128d c, __m128d d, __m128d e, __m128d f, __m128d g, __m128d h) {
  return _mm_add_pd(_mm_add_pd(_mm_add_pd(a, _mm_mul_pd(b, _mm_set1_pd(2))), _mm_add_pd(_mm_mul_pd(c, _mm_set1_pd(3)), _mm_mul_pd(d, _mm_set1_pd(4)))),
                    _mm_add_pd(_mm_add_pd(_mm_mul_pd(e, _mm_set1_pd(5)), _mm_mul_pd(f, _mm_set1_pd(6))), _mm_add_pd(_mm_mul_pd(g, _mm_set1_pd(7)), _mm_mul_pd(h, _mm_set1_pd(8)))));
}
static T_NOINLINE __m128d args8(__m128d x, __m128d y, const double *p) {
  /* each argument: low lane computed, high lane carried over from x or y by the scalar instruction */
  __m128d r = sum8(_mm_add_sd(x, y), _mm_mul_sd(y, x), _mm_sub_sd(x, _mm_load_sd(p)), _mm_div_sd(y, _mm_load_sd(p + 1)),
                   _mm_add_sd(y, _mm_load_sd(p + 2)), _mm_mul_sd(x, _mm_load_sd(p + 3)), _mm_sqrt_sd(y, _mm_load_sd(p + 4)), _mm_sub_sd(y, x));
  g_sink++; /* a real call, not a tail call: the argument registers are set up for a CALL */
  return r;
}
static void scalar_lanes(const F4 *a, const F4 *b, const D2 *c, const D2 *e, uint64_t *h) {
  __m128 fa = _mm_loadu_ps(a->f), fb = _mm_loadu_ps(b->f);
  __m128d dc = _mm_loadu_pd(c->d), de = _mm_loadu_pd(e->d);
  D2 d; F4 f;
  _mm_storeu_pd(d.d, addsd_ret(dc, de)); *h = t_fnv(*h, &d, sizeof d);
  _mm_storeu_pd(d.d, mulsd_chain(dc, de)); *h = t_fnv(*h, &d, sizeof d);
  _mm_storeu_ps(f.f, sqrtss_ret(fa)); *h = t_fnv(*h, &f, sizeof f);
  _mm_storeu_ps(f.f, movess_ret(fa, fb)); *h = t_fnv(*h, &f, sizeof f);
  _mm_storeu_ps(f.f, cvtsi_ret(fb, 77)); *h = t_fnv(*h, &f, sizeof f);
  _mm_storeu_pd(d.d, cvtss2sd_ret(de, fa)); *h = t_fnv(*h, &d, sizeof d);
  _mm_storeu_pd(d.d, loadsd_ret(c->d)); *h = t_fnv(*h, &d, sizeof d);
  _mm_storeu_ps(f.f, minss_ret(fb, fa)); *h = t_fnv(*h, &f, sizeof f);
  double p[5] = {1.25, 4.0, -0.5, 3.0, 6.25};
  _mm_storeu_pd(d.d, args8(dc, de, p)); *h = t_fnv(*h, &d, sizeof d);
}
#else
static void scalar_lanes(const F4 *a, const F4 *b, const D2 *c, const D2 *e, uint64_t *h) {
  D2 d; F4 f;
  d = *c; d.d[0] = c->d[0] + e->d[0]; *h = t_fnv(*h, &d, sizeof d);
  d = *c; d.d[0] = ((c->d[0] + e->d[0]) - e->d[0]) * c->d[0]; *h = t_fnv(*h, &d, sizeof d);
  f = *a; f.f[0] = sqrtf(a->f[0]); *h = t_fnv(*h, &f, sizeof f);
  f = *a; f.f[0] = b->f[0]; *h = t_fnv(*h, &f, sizeof f);
  f = *b; f.f[0] = 77.0f; *h = t_fnv(*h, &f, sizeof f);
  d = *e; d.d[0] = (double)a->f[0]; *h = t_fnv(*h, &d, sizeof d);
  d.d[0] = c->d[0] * c->d[1]; d.d[1] = 0; *h = t_fnv(*h, &d, sizeof d);
  f = *b; f.f[0] = b->f[0] < a->f[0] ? b->f[0] : a->f[0]; *h = t_fnv(*h, &f, sizeof f);
  double p[5] = {1.25, 4.0, -0.5, 3.0, 6.25};
  double x0 = c->d[0], x1 = c->d[1], y0 = e->d[0], y1 = e->d[1];
  double lo[8] = {x0 + y0, y0 * x0, x0 - p[0], y0 / p[1], y0 + p[2], x0 * p[3], sqrt(p[4]), y0 - x0};
  double hi[8] = {x1, y1, x1, y1, y1, x1, y1, y1};
  d.d[0] = ((lo[0] + lo[1] * 2) + (lo[2] * 3 + lo[3] * 4)) + ((lo[4] * 5 + lo[5] * 6) + (lo[6] * 7 + lo[7] * 8));
  d.d[1] = ((hi[0] + hi[1] * 2) + (hi[2] * 3 + hi[3] * 4)) + ((hi[4] * 5 + hi[5] * 6) + (hi[6] * 7 + hi[7] * 8));
  *h = t_fnv(*h, &d, sizeof d);
}
#endif

/* ---- 5. varargs: doubles through xmm0-7 and the stack ---- */
static T_NOINLINE double vsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; i++) s = s * 1.5 + va_arg(ap, double);
  va_end(ap);
  return s;
}

/* ---- 6. callbacks and indirect calls ---- */
static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
typedef double (*Op)(double, double);
static T_NOINLINE double op_add(double a, double b) { return a + b; }
static T_NOINLINE double op_mul(double a, double b) { return a * b; }
static T_NOINLINE double op_hyp(double a, double b) { return sqrt(a * a + b * b); }
static T_NOINLINE double fold(const double *v, int n, Op volatile *ops) {
  double acc = v[0], k = v[1] * 0.125; /* k lives across the indirect calls (spilled by the compiler) */
  for (int i = 1; i < n; i++) acc = ops[i % 3](acc, v[i]) + k;
  return acc;
}

/* ---- 7. setjmp/longjmp out of the middle of a vector sequence ---- */
static jmp_buf g_jb;
static T_NOINLINE double bail(const double *v, int n, int at) {
  double s = 0, p = 1;
  for (int i = 0; i < n; i++) {
    s += v[i] * p;
    p *= 1.0625;
    if (i == at) longjmp(g_jb, i + 1);
  }
  return s;
}
static T_NOINLINE double with_setjmp(const double *v, int n) {
  volatile double acc = v[0] * v[1];
  volatile int tries = 0;
  int r = setjmp(g_jb);
  if (r) acc = acc * 3 + r;
  tries = tries + 1;
  if (tries < 4) acc = acc + bail(v, n, tries * 3);
  return acc + bail(v, n, -1) * 0.5;
}

int main(void) {
  uint64_t seed = 0x1234567;
  M4 a, inv;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) a.m[i][j] = (double)(int)(t_lcg(&seed) % 19) - 9 + (i == j ? 20 : 0);
  double extra = inverse(&a, &inv);
  t_hex("inverse_fnv", t_fnv(T_FNV0, &inv, sizeof inv));
  t_hex("inverse_ret", dbits(extra));

  double p[16];
  float q[8];
  for (int i = 0; i < 16; i++) p[i] = (double)(int)(t_lcg(&seed) % 41) / 4 + 1;
  for (int i = 0; i < 8; i++) q[i] = (float)(int)(t_lcg(&seed) % 23) / 8 - 1;
  t_hex("many_live", dbits(many_live(p, q)));

  static float src[64], dst[64], mat[32];
  for (int i = 0; i < 64; i++) src[i] = (float)(int)(t_lcg(&seed) % 33) / 16 - 1;
  for (int i = 0; i < 32; i++) mat[i] = (float)(int)(t_lcg(&seed) % 17) / 8 - 1;
  transform(dst, src, mat, 4);
  t_hex("transform_fnv", t_fnv(T_FNV0, dst, sizeof dst));

  F8 f8;
  D4 d4;
  F4 fa, fb;
  D2 dc, de;
  for (int i = 0; i < 8; i++) f8.f[i] = (float)(int)(t_lcg(&seed) % 29) / 4 - 3;
  for (int i = 0; i < 4; i++) d4.d[i] = (double)(int)(t_lcg(&seed) % 31) / 8 - 2;
  for (int i = 0; i < 4; i++) fa.f[i] = (float)(int)(t_lcg(&seed) % 29) / 4 + 1, fb.f[i] = (float)(int)(t_lcg(&seed) % 29) / 4 - 3;
  for (int i = 0; i < 2; i++) dc.d[i] = (double)(int)(t_lcg(&seed) % 31) / 8 + 1, de.d[i] = (double)(int)(t_lcg(&seed) % 31) / 8 - 2;
  uint64_t h = T_FNV0;
  vec256(&f8, &d4, &h);
  t_hex("vec256_fnv", h);
  h = T_FNV0;
  scalar_lanes(&fa, &fb, &dc, &de, &h);
  t_hex("scalar_lanes_fnv", h);

  t_hex("vsum3", dbits(vsum(3, 1.5, -2.25, 4.0)));
  t_hex("vsum11", dbits(vsum(11, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10])));

  double keys[24];
  for (int i = 0; i < 24; i++) keys[i] = (double)(int)(t_lcg(&seed) % 1001) / 8 - 60;
  double before = keys[5] * keys[7];
  qsort(keys, 24, sizeof keys[0], cmp_d);
  t_hex("qsort_fnv", t_fnv(T_FNV0, keys, sizeof keys));
  t_hex("qsort_before", dbits(before));
  double key = keys[13];
  double *hit = (double *)bsearch(&key, keys, 24, sizeof keys[0], cmp_d);
  t_hex("bsearch", hit ? dbits(*hit) : 0);

  Op volatile ops[3] = {op_add, op_mul, op_hyp};
  t_hex("fold", dbits(fold(p, 16, ops)));
  t_hex("setjmp", dbits(with_setjmp(p, 16)));
  t_hex("float_mix", fbits(fstep(q[0], q[1]) * rowsum(q, 8)));
  return 0;
}
