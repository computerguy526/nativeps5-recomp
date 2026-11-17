/* Stack access microbenchmark for -tso-elide (tools/tsobench.sh; docs/m3/tsoelide.md).
 * Each case is saturated with frame accesses, so the numbers bound what ordering the accesses of
 * a frame costs. "private" cases keep every frame address in registers (plain in both modes);
 * "escaped" cases hand one address to a callee first, which makes the accesses of the whole
 * function ordered under -tso-elide stack. Times come from sceKernelGetProcessTime (usec). */
#include "../src/ps5test.h"
extern uint64_t sceKernelGetProcessTime(void);

#define NI __attribute__((noinline))
static volatile uint64_t g_sink;
static NI void touch(volatile uint64_t *p) { g_sink += *p; }
static NI uint64_t sum8(const uint64_t *p) { uint64_t s = 0; for (int i = 0; i < 8; i++) s += p[i]; return s; }

/* Spilled scalars: 4 loads and 3 stores of frame slots per iteration. */
static NI uint64_t spill_private(uint64_t n) {
  volatile uint64_t a = 1, b = 2, c = 3;
  for (uint64_t i = 0; i < n; i++) { a = a + b; b = b ^ c; c = a + i; }
  return a + b + c;
}
static NI uint64_t spill_escaped(uint64_t n) {
  volatile uint64_t a = 1, b = 2, c = 3;
  touch(&c);
  for (uint64_t i = 0; i < n; i++) { a = a + b; b = b ^ c; c = a + i; }
  return a + b + c;
}

/* A local array, indexed: insertion sort of 32 elements, n times. */
static inline uint64_t sort32(uint64_t *v, uint64_t seed) {
  for (int i = 0; i < 32; i++) v[i] = t_lcg(&seed) & 0xffff;
  for (int i = 1; i < 32; i++) {
    uint64_t x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return v[0] + v[31] * 3 + v[16];
}
static NI uint64_t array_private(uint64_t n) {
  uint64_t v[32], h = 0;
  for (uint64_t i = 0; i < n; i++) h += sort32(v, i);
  return h;
}
static NI uint64_t array_escaped(uint64_t n) {
  uint64_t v[32], h = 0;
  for (uint64_t i = 0; i < n; i++) h += sort32(v, i);
  return h + sum8(v);
}

/* An object on the stack with out-of-line methods, C++ style: two calls, the fields read back
 * from the frame after each. */
struct vec { uint64_t x, y, z, w; };
static NI void vec_step(struct vec *v, uint64_t k) { v->x += k; v->y ^= v->x; v->z += v->y >> 3; v->w++; }
static NI uint64_t object_escaped(uint64_t n) {
  struct vec v = {1, 2, 3, 4};
  uint64_t h = 0;
  for (uint64_t i = 0; i < n; i++) {
    vec_step(&v, i);
    h += v.x + v.z;
    vec_step(&v, h);
    h ^= v.y + v.w;
  }
  return h;
}

/* Calls: saved registers, return addresses, a frame per activation; no frame address escapes. */
static NI uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* Ten arguments: four are read from the caller's frame (never private for the callee). */
static NI uint64_t args10(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                          uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
  return (a ^ b) + (c ^ d) + (e ^ f) + (g ^ h) + (i ^ j);
}
static NI uint64_t stack_args(uint64_t n) {
  uint64_t s = 0;
  for (uint64_t k = 0; k < n; k++) s += args10(k, s, 3, 4, 5, 6, k + 7, s + 8, 9, k ^ 10);
  return s;
}

static void t_u64(uint64_t u) {
  char b[24];
  int i = 24;
  b[--i] = 0;
  do { b[--i] = '0' + u % 10; u /= 10; } while (u);
  t_str(b + i);
}
static void report(const char *name, uint64_t t0, uint64_t iters, uint64_t check) {
  uint64_t us = sceKernelGetProcessTime() - t0;
  char b[17];
  t_hexs(b, check, 16);
  b[16] = 0;
  t_str(name); t_str(" check="); t_str(b);
  t_str(" iters="); t_u64(iters);
  t_str(" ns_per_iter_x100="); t_u64(iters ? us * 100000ULL / iters : 0);
  t_str("\n");
}

int main(void) {
  uint64_t t0, r;
  t0 = sceKernelGetProcessTime(); r = spill_private(300000000); report("spill_private", t0, 300000000, r);
  t0 = sceKernelGetProcessTime(); r = spill_escaped(300000000); report("spill_escaped", t0, 300000000, r);
  t0 = sceKernelGetProcessTime(); r = array_private(1500000); report("array_private", t0, 1500000, r);
  t0 = sceKernelGetProcessTime(); r = array_escaped(1500000); report("array_escaped", t0, 1500000, r);
  t0 = sceKernelGetProcessTime(); r = object_escaped(100000000); report("object_escaped", t0, 100000000, r);
  t0 = sceKernelGetProcessTime(); r = fib(36); report("fib36", t0, 48315633, r);
  t0 = sceKernelGetProcessTime(); r = stack_args(150000000); report("stack_args", t0, 150000000, r);
  return 0;
}
