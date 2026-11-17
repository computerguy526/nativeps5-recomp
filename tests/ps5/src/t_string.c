/* String ops: rep movs/stos/cmps/scas/lods with DF=0 and DF=1 (inline asm on x86,
 * portable model elsewhere), plus libc mem and str routines (the SDK libc.a uses
 * SSE/AVX2 and rep-prefixed implementations) over many lengths and alignments. */
#include "ps5test.h"
#include <string.h>

static uint8_t A[512], B[512];
static void fill(uint8_t *p, int n, uint64_t seed) { uint64_t s = seed; for (int i = 0; i < n; i++) p[i] = (uint8_t)(t_lcg(&s) >> 3); }

static T_NOINLINE void rep_movsb(void *d, const void *s, size_t n) {
#ifdef __x86_64__
  __asm__ volatile("cld\n rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory", "cc");
#else
  for (size_t i = 0; i < n; i++) ((uint8_t *)d)[i] = ((const uint8_t *)s)[i];
#endif
}
static T_NOINLINE void rep_movsb_back(uint8_t *d, const uint8_t *s, size_t n) { /* std: copies high to low */
#ifdef __x86_64__
  void *dp = d + n - 1; const void *sp = s + n - 1;
  __asm__ volatile("std\n rep movsb\n cld" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory", "cc");
#else
  for (size_t i = n; i-- > 0;) d[i] = s[i];
#endif
}
static T_NOINLINE void rep_movsq(void *d, const void *s, size_t n) {
#ifdef __x86_64__
  __asm__ volatile("cld\n rep movsq" : "+D"(d), "+S"(s), "+c"(n) : : "memory", "cc");
#else
  for (size_t i = 0; i < n; i++) ((uint64_t *)d)[i] = ((const uint64_t *)s)[i];
#endif
}
static T_NOINLINE void rep_stosq(void *d, uint64_t v, size_t n) {
#ifdef __x86_64__
  __asm__ volatile("cld\n rep stosq" : "+D"(d), "+c"(n) : "a"(v) : "memory", "cc");
#else
  for (size_t i = 0; i < n; i++) ((uint64_t *)d)[i] = v;
#endif
}
static T_NOINLINE void rep_stosb(void *d, uint8_t v, size_t n) {
#ifdef __x86_64__
  __asm__ volatile("cld\n rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory", "cc");
#else
  for (size_t i = 0; i < n; i++) ((uint8_t *)d)[i] = v;
#endif
}
/* repe cmpsb: returns number of bytes left (rcx) and compares ZF */
static T_NOINLINE size_t repe_cmpsb(const void *a, const void *b, size_t n, int *zf) {
#ifdef __x86_64__
  unsigned char z;
  __asm__ volatile("cld\n xor %%eax,%%eax\n cmp %%eax,%%eax\n repe cmpsb\n sete %[z]" : "+D"(a), "+S"(b), "+c"(n), [z]"=q"(z) : : "memory", "cc", "rax");
  *zf = z; return n;
#else
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b; size_t i = 0;
  *zf = 1; /* ZF stays set when n==0 */
  while (i < n) { int eq = x[i] == y[i]; i++; *zf = eq; if (!eq) break; }
  return n - i;
#endif
}
static T_NOINLINE size_t repne_scasb(const void *a, uint8_t v, size_t n) { /* returns n - consumed */
#ifdef __x86_64__
  __asm__ volatile("cld\n repne scasb" : "+D"(a), "+c"(n) : "a"(v) : "memory", "cc");
  return n;
#else
  const uint8_t *x = (const uint8_t *)a; size_t i = 0;
  while (i < n) { int eq = x[i] == v; i++; if (eq) break; }
  return n - i;
#endif
}
static T_NOINLINE uint32_t lods_sum(const uint8_t *p, size_t n) {
  uint32_t sum = 0;
#ifdef __x86_64__
  while (n--) { uint32_t v; __asm__ volatile("lodsb" : "=a"(v), "+S"(p) : : "memory"); sum += v & 0xff; }
#else
  for (size_t i = 0; i < n; i++) sum += p[i];
#endif
  return sum;
}

int main(void) {
  uint64_t h = T_FNV0;
  for (int n = 0; n < 130; n += 7) for (int o = 0; o < 9; o += 3) {
    fill(A, 512, n * 17 + o); fill(B, 512, 5);
    rep_movsb(B + o, A + 1, n); h = t_fnv(h, B, 300);
    rep_movsb_back(B + 3, A + o, n); h = t_fnv(h, B, 300);
    rep_movsq(B + 8, A + 8, n / 8); h = t_fnv(h, B, 300);
    rep_stosq(B + 16, 0x0123456789abcdefULL + n, n / 8); h = t_fnv(h, B, 300);
    rep_stosb(B + o, (uint8_t)n, n); h = t_fnv(h, B, 300);
  }
  t_hex("rep_copy_fill_fnv", h);
  uint64_t c = T_FNV0;
  fill(A, 256, 1); memcpy(B, A, 256);
  for (int n = 0; n < 40; n += 3) for (int diff = -1; diff < 40; diff += 5) {
    memcpy(B, A, 256); if (diff >= 0) B[diff] ^= 0x40;
    int zf; size_t left = repe_cmpsb(A, B, n, &zf);
    uint64_t r[3] = {left, (uint64_t)zf, repne_scasb(A, A[(n * 3) % 50], n)};
    c = t_fnv(c, r, sizeof r);
  }
  t_hex("cmps_scas_fnv", c);
  fill(A, 300, 77);
  t_hex("lods_sum", lods_sum(A, 300));
  /* libc routines */
  uint64_t l = T_FNV0;
  static char S[600];
  for (int len = 0; len < 150; len += 1) {
    for (int i = 0; i < len; i++) S[i] = 'a' + (i * 7 + len) % 26;
    S[len] = 0;
    size_t sl = strlen(S);
    char *chr = strchr(S, 'q'), *rch = strrchr(S, 'e'), *mc = (char *)memchr(S, 'z', len);
    uint64_t r[6] = {sl, chr ? (uint64_t)(chr - S) : 999, rch ? (uint64_t)(rch - S) : 999, mc ? (uint64_t)(mc - S) : 999,
                     (uint64_t)(strcmp(S, "abcdefghij") > 0), (uint64_t)(memcmp(S, S + (len > 4), len > 8 ? 8 : len) < 0)};
    l = t_fnv(l, r, sizeof r);
    memset(B, len, 200); memcpy(B + 5, S, len < 190 ? len : 190); memmove(B + 1, B + 3, len < 150 ? len : 150);
    l = t_fnv(l, B, 200);
  }
  t_hex("libc_str_fnv", l);
  return 0;
}
