/* SSE/AVX state microbenchmark (tools/simdbench.sh; docs/m3/simdregs.md): math kernels of the
 * shape Unreal Engine code has on PS5 (AVX2 + FMA, VEX encodings, double "large world" scalars,
 * float and double vectors).  The same source is built for the PS5 (translated, with vector lane
 * liveness off and on) and natively for the arm64 host (-DNPS5_HOST), so the ratio to native ARM
 * code is measured directly.  Kernels are noinline and read their inputs through pointers, so
 * each iteration really runs the kernel. */
#include "../src/ps5test.h"
#include <math.h>
#include <string.h>
#ifdef NPS5_HOST
#include <time.h>
static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}
#else
extern uint64_t sceKernelGetProcessTime(void);
static uint64_t now_us(void) { return sceKernelGetProcessTime(); }
#endif

#define NI __attribute__((noinline))
typedef struct { double m[4][4]; } M4d;
typedef struct { float m[4][4]; } M4f;
typedef struct { double x, y, z; } V3d;
typedef struct { float x, y, z, w; } Q4f;

/* 1. 4x4 double inverse (scalar LWC code: FMatrix44d::Inverse) */
static NI double inverse4d(M4d *restrict out, const M4d *restrict in) {
  const double (*m)[4] = in->m;
  double s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1], s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
  double s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3], s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
  double s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3], s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
  double c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3], c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
  double c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2], c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
  double c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2], c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
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
  return det;
}

/* 2. 4x4 double multiply (packed doubles, YMM with AVX) */
static NI void mul4d(M4d *restrict out, const M4d *restrict a, const M4d *restrict b) {
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      out->m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
}

/* 3. transform positions by a double matrix, results narrowed to float (LWC to render space) */
static NI void transform_pos(float *restrict out, const V3d *restrict p, const M4d *restrict m, int n) {
  for (int i = 0; i < n; i++) {
    double x = p[i].x, y = p[i].y, z = p[i].z;
    out[3 * i + 0] = (float)(x * m->m[0][0] + y * m->m[1][0] + z * m->m[2][0] + m->m[3][0]);
    out[3 * i + 1] = (float)(x * m->m[0][1] + y * m->m[1][1] + z * m->m[2][1] + m->m[3][1]);
    out[3 * i + 2] = (float)(x * m->m[0][2] + y * m->m[1][2] + z * m->m[2][2] + m->m[3][2]);
  }
}

/* 4. quaternion multiply + normalise (scalar floats, sqrt, divide) */
static NI Q4f quat_mul_norm(const Q4f *restrict a, const Q4f *restrict b) {
  Q4f r;
  r.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
  r.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
  r.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
  r.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
  float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  float il = 1.0f / l;
  r.x *= il; r.y *= il; r.z *= il; r.w *= il;
  return r;
}

/* 5. float blend loop (auto-vectorised: 8 lanes with AVX, 4 with NEON) */
static NI void blend(float *restrict out, const float *restrict a, const float *restrict b, float w, int n) {
  for (int i = 0; i < n; i++) out[i] = a[i] * w + b[i] * (1.0f - w);
}

/* 6. scalar reduction returned in xmm0 (bounds/length computations) */
static NI float len_sum(const float *restrict v, int n) {
  float s = 0;
  for (int i = 0; i + 2 < n; i += 3) s += sqrtf(v[i] * v[i] + v[i + 1] * v[i + 1] + v[i + 2] * v[i + 2]);
  return s;
}

/* 7. a scalar helper called from a scalar loop (calls in the middle of FP code) */
static NI double lerp_d(double a, double b, double t) { return a + (b - a) * t; }
static NI double lerp_chain(const double *restrict v, int n) {
  double acc = v[0];
  for (int i = 1; i < n; i++) acc = lerp_d(acc, v[i], 0.375) * 1.0009765625;
  return acc;
}

static void t_u64(uint64_t u) {
  char b[24];
  int i = 24;
  b[--i] = 0;
  do { b[--i] = '0' + u % 10; u /= 10; } while (u);
  t_str(b + i);
}
static void report(const char *name, uint64_t t0, uint64_t calls, uint64_t check) {
  uint64_t us = now_us() - t0;
  char b[17];
  t_hexs(b, check, 16);
  b[16] = 0;
  t_str(name); t_str(" check="); t_str(b);
  t_str(" calls="); t_u64(calls);
  t_str(" ps_per_call="); t_u64(calls ? us * 1000000ULL / calls : 0);
  t_str("\n");
}
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

#ifndef SCALE
#define SCALE 1
#endif

int main(void) {
  uint64_t seed = 99, t;
  static M4d a, b, c;
  static V3d pos[256];
  static float fo[768], fa[1024], fb[1024], fc[1024];
  static double dv[256];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      a.m[i][j] = (double)(int)(t_lcg(&seed) % 19) / 16 - 0.5 + (i == j ? 2 : 0);
      b.m[i][j] = (double)(int)(t_lcg(&seed) % 19) / 32 - 0.25 + (i == j ? 1 : 0);
    }
  for (int i = 0; i < 256; i++) {
    pos[i] = (V3d){(double)(int)(t_lcg(&seed) % 2001) - 1000, (double)(int)(t_lcg(&seed) % 2001) - 1000, (double)(int)(t_lcg(&seed) % 2001) - 1000};
    dv[i] = (double)(int)(t_lcg(&seed) % 513) / 64;
  }
  for (int i = 0; i < 1024; i++) fa[i] = (float)(int)(t_lcg(&seed) % 257) / 64 - 2, fb[i] = (float)(int)(t_lcg(&seed) % 257) / 64 - 2;

  /* warm up (page faults, tier-1 nothing) */
  inverse4d(&c, &a);

  uint64_t n = 4000000ull * SCALE;
  double acc = 0;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { acc += inverse4d(&c, &a); a.m[0][0] += 1e-9; }
  report("inverse4d", t, n, dbits(acc) ^ t_fnv(T_FNV0, &c, sizeof c));

  n = 6000000ull * SCALE;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { mul4d(&c, &a, &b); b.m[1][1] = c.m[1][1] * 0.5 + 0.5; }
  report("mul4d", t, n, t_fnv(T_FNV0, &c, sizeof c));

  n = 150000ull * SCALE;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { transform_pos(fo, pos, &a, 256); pos[i & 255].x += 0.25; }
  report("transform_pos", t, n * 256, t_fnv(T_FNV0, fo, sizeof fo));

  n = 12000000ull * SCALE;
  Q4f q = {0.1f, 0.2f, 0.3f, 0.9f}, r = {0.05f, -0.1f, 0.02f, 0.99f};
  t = now_us();
  for (uint64_t i = 0; i < n; i++) q = quat_mul_norm(&q, &r);
  report("quat_mul_norm", t, n, t_fnv(T_FNV0, &q, sizeof q));

  n = 1000000ull * SCALE;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { blend(fc, fa, fb, 0.25f + (float)(i & 7) / 16, 1024); fa[i & 1023] = fc[(i + 1) & 1023]; }
  report("blend", t, n * 1024, t_fnv(T_FNV0, fc, sizeof fc));

  n = 300000ull * SCALE;
  float fs = 0;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { fs += len_sum(fb, 1023); fb[i & 1023] += 0.125f; }
  report("len_sum", t, n * 341, t_fnv(T_FNV0, &fs, sizeof fs));

  n = 150000ull * SCALE;
  double ds = 0;
  t = now_us();
  for (uint64_t i = 0; i < n; i++) { ds += lerp_chain(dv, 256); dv[i & 255] += 0.015625; }
  report("lerp_chain", t, n * 255, dbits(ds));
  return 0;
}
