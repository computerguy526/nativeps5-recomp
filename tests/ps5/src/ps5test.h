/* Shared helpers for the M1 PS5 test corpus.
 * Output goes through sceKernelWrite (libkernel) on the PS5 build and through
 * write(2) on the host cross-check build (-DNPS5_HOST).  No printf: tests stay
 * independent of libc formatting so a diff points at the lifter, not at stdio. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef NPS5_HOST
#include <unistd.h>
static inline long t_write(const void *p, size_t n) { return write(1, p, n); }
#else
#ifdef __cplusplus
extern "C" {
#endif
extern long sceKernelWrite(int fd, const void *buf, size_t n);
#ifdef __cplusplus
}
#endif
static inline long t_write(const void *p, size_t n) { return sceKernelWrite(1, p, n); }
#endif

static inline void t_str(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  t_write(s, n);
}
static inline void t_hexs(char *o, uint64_t v, int digits) {
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < digits; i++) o[i] = H[(v >> (4 * (digits - 1 - i))) & 15];
}
/* "label=0x%016x\n" */
static inline void t_hex(const char *label, uint64_t v) {
  char b[19];
  t_str(label);
  t_str("=0x");
  t_hexs(b, v, 16);
  b[16] = '\n';
  t_write(b, 17);
}
static inline void t_dec(const char *label, long long v) {
  char b[24];
  int i = 24;
  unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
  b[--i] = '\n';
  do { b[--i] = '0' + u % 10; u /= 10; } while (u);
  if (v < 0) b[--i] = '-';
  t_str(label);
  t_str("=");
  t_write(b + i, 24 - i);
}
static inline uint64_t t_fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
  return h;
}
#define T_FNV0 0xcbf29ce484222325ULL
static inline uint64_t t_lcg(uint64_t *s) {
  *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
  return *s >> 11 ^ *s << 17;
}
#define T_NOINLINE __attribute__((noinline))
