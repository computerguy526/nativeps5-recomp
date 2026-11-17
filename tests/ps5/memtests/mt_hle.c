/* PS5 guest program: pointer-heavy data crossing the HLE boundary (docs/m1/memory.md section 7).
 * The case is selected by argv[1] (see mtg.h); every case prints RESULT lines (tests/memtests/mt.h).
 * Guest memory holds the pointers; the libc HLE (qsort and bsearch call back into guest code with
 * guest pointers; strtok returns pointers into the guest buffer; strtol and modf write pointers
 * and values through out-parameters; vsnprintf walks a guest va_list) must translate every one. */
#define MT_GUEST 1
#include "mtg.h"
#include "../../memtests/mt.h"

extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void *memalign(size_t, size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
extern char *strtok(char *, const char *);
extern long strtol(const char *, char **, int);
extern double strtod(const char *, char **);
extern double modf(double, double *);
extern double frexp(double, int *);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern char *strcpy(char *, const char *);
extern char *strcat(char *, const char *);
extern char *strdup(const char *);
extern char *strchr(const char *, int);
extern char *strstr(const char *, const char *);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);
extern int sceKernelMunmap(void *addr, size_t len);
extern int strncmp(const char *, const char *, size_t);

#define PAGE 0x4000UL
static const char *MODE;
#define NM(s) (MODE[0] == 'i' ? "hle.identity." s : "hle.base." s)

struct payload { int tag; const char *label; struct payload *self; };
struct rec { const char *name; struct payload *p; int key; struct rec *next; };

static int cmp_rec(const void *a, const void *b) {
  const struct rec *x = (const struct rec *)a, *y = (const struct rec *)b;
  return strcmp(x->name, y->name);
}
static int cmp_pstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

static int vfmt(char *b, size_t n, const char *f, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, f);
  int r = vsnprintf(b, n, f, ap);
  __builtin_va_end(ap);
  return r;
}

static void t_qsort_structs(void) {
  mt_begin(NM("qsort_structs_with_pointers"), "hle");
  enum { N = 200 };
  static struct rec recs[N]; static struct payload pays[N]; static char names[N][16];
  uint32_t s = 12345;
  for (int i = 0; i < N; i++) {
    s = s * 1664525u + 1013904223u;
    int v = (int)(s >> 8) % 100000;
    names[i][0] = 'k'; for (int d = 0; d < 6; d++) { names[i][1 + d] = (char)('0' + (v % 10)); v /= 10; } names[i][7] = 0;
    pays[i].tag = i; pays[i].label = names[i]; pays[i].self = &pays[i];
    recs[i].name = names[i]; recs[i].p = &pays[i]; recs[i].key = i; recs[i].next = &recs[(i + 1) % N];
  }
  qsort(recs, N, sizeof recs[0], cmp_rec);
  int sorted = 1, intact = 1;
  for (int i = 0; i < N; i++) {
    if (i && strcmp(recs[i - 1].name, recs[i].name) > 0) sorted = 0;
    if (recs[i].p != &pays[recs[i].key] || recs[i].p->self != recs[i].p || recs[i].p->label != recs[i].name || recs[i].p->tag != recs[i].key) intact = 0;
  }
  MT_EXPECT(sorted, "not sorted");
  MT_EXPECT(intact, "pointer fields damaged by qsort");
  struct rec probe = {names[17], 0, 0, 0};
  struct rec *hit = (struct rec *)bsearch(&probe, recs, N, sizeof recs[0], cmp_rec);
  MT_EXPECT(hit && strcmp(hit->name, names[17]) == 0 && hit >= recs && hit < recs + N, "bsearch result %p outside the array", (void *)hit);
  struct rec miss = {"zzzz", 0, 0, 0};
  MT_EXPECT(bsearch(&miss, recs, N, sizeof recs[0], cmp_rec) == 0, "bsearch of a missing key returned non-NULL");
  mt_end();

  mt_begin(NM("qsort_pointer_array_and_ints"), "hle");
  const char *w[] = {"pear", "apple", "fig", "banana", "cherry", "date", "elder", "grape"};
  const char *arr[8]; for (int i = 0; i < 8; i++) arr[i] = w[i];
  qsort(arr, 8, sizeof arr[0], cmp_pstr);
  int ok = 1; for (int i = 1; i < 8; i++) if (strcmp(arr[i - 1], arr[i]) > 0) ok = 0;
  MT_EXPECT(ok && strcmp(arr[0], "apple") == 0 && strcmp(arr[7], "pear") == 0, "string pointers not sorted: %s %s", arr[0], arr[7]);
  static int big[20000]; uint32_t t = 99;
  for (int i = 0; i < 20000; i++) { t = t * 1103515245u + 12345u; big[i] = (int)(t >> 4); }
  qsort(big, 20000, sizeof big[0], cmp_int);
  int bsorted = 1; for (int i = 1; i < 20000; i++) if (big[i - 1] > big[i]) bsorted = 0;
  MT_EXPECT(bsorted, "20000 ints not sorted");
  qsort(big, 0, sizeof big[0], cmp_int);  /* n = 0: nothing may be called */
  qsort(big, 1, sizeof big[0], cmp_int);
  mt_end();
}

static void t_strings(void) {
  mt_begin(NM("strtok_pointers_into_guest_buffer"), "hle");
  char buf[64]; const char *src = "alpha,beta;;gamma delta";
  for (int i = 0; ; i++) { buf[i] = src[i]; if (!src[i]) break; }
  char *tok[8]; int n = 0;
  for (char *p = strtok(buf, ",; "); p && n < 8; p = strtok(0, ",; ")) tok[n++] = p;
  MT_EXPECT(n == 4, "got %d tokens", n);
  const char *want[] = {"alpha", "beta", "gamma", "delta"};
  for (int i = 0; i < n && i < 4; i++) {
    MT_EXPECT(strcmp(tok[i], want[i]) == 0, "token %d = '%s'", i, tok[i]);
    MT_EXPECT(tok[i] >= buf && tok[i] < buf + 64, "token %d pointer %p is outside the guest buffer", i, (void *)tok[i]);
  }
  MT_EXPECT(strtok(0, ",") == 0, "strtok after the last token returned non-NULL");
  char e[4] = {0}; MT_EXPECT(strtok(e, ",") == 0, "strtok on an empty string");
  mt_end();

  mt_begin(NM("strtol_strtod_out_pointers"), "hle");
  const char *volatile in = "  12345abc";
  char *end = 0; long v = strtol((const char *)in, &end, 10);
  MT_EXPECT(v == 12345 && end == (const char *)in + 7, "strtol %ld end=%p in=%p", v, (void *)end, (const void *)in);
  const char *volatile in2 = "3.5e2xyz";
  double d = strtod((const char *)in2, &end);
  MT_EXPECT(d == 350.0 && end == (const char *)in2 + 5, "strtod %f end offset %ld", d, (long)(end - (const char *)in2));
  const char *volatile in3 = "zzz";
  v = strtol((const char *)in3, &end, 10);
  MT_EXPECT(v == 0 && end == (const char *)in3, "no digits: end must be the start");
  double ip = -1; double fr = modf(3.75, &ip); int ex = 0; double m = frexp(48.0, &ex);
  MT_EXPECT(ip == 3.0 && fr == 0.75 && m == 0.75 && ex == 6, "modf/frexp out-params %f %f %f %d", ip, fr, m, ex);
  mt_end();

  mt_begin(NM("string_functions_at_page_ends"), "hle");
  void *pg = 0; MT_EXPECT(sceKernelMmap(0, 2 * PAGE, 3, 0x1002, -1, 0, &pg) == 0, "mmap");
  sceKernelMunmap((char *)pg + PAGE, PAGE);  /* second page unmapped: any overrun faults */
  char *tail = (char *)pg + PAGE - 12;      /* 12 bytes before the end of the mapping */
  strcpy(tail, "0123456789a");              /* 11 chars + NUL = exactly 12 */
  MT_EXPECT(strlen(tail) == 11 && tail[11] == 0, "strcpy to the last 12 bytes");
  char *dup = strdup(tail); MT_EXPECT(dup && strcmp(dup, tail) == 0 && dup != tail, "strdup"); free(dup);
  MT_EXPECT(strchr(tail, '9') == tail + 9 && strchr(tail, 'z') == 0 && strstr(tail, "89a") == tail + 8, "strchr/strstr pointers");
  memmove(tail + 2, tail, 9); MT_EXPECT(memcmp(tail + 2, "012345678", 9) == 0, "memmove overlap forwards");
  memmove(tail, tail + 2, 9); MT_EXPECT(memcmp(tail, "012345678", 9) == 0, "memmove overlap backwards");
  memset(tail, 0x7f, 12); MT_EXPECT((unsigned char)tail[11] == 0x7f && (unsigned char)tail[0] == 0x7f, "memset to the very end");
  char *low = (char *)pg;  memcpy(low, tail, 12); MT_EXPECT((unsigned char)low[11] == 0x7f, "memcpy");
  sceKernelMunmap(pg, PAGE);
  mt_end();
}

static void t_format(void) {
  mt_begin(NM("vsnprintf_into_guest_buffers"), "hle");
  char b[96]; const char *volatile s1 = "str"; long long big = -9876543210LL; double dv = 2.5;
  int r = vfmt(b, sizeof b, "%s|%d|%5.2f|%x|%c|%lld|%%|%08.3f|%-4d|", (const char *)s1, -42, dv, 255, 'Z', big, 3.14159, 7);
  MT_EXPECT(r == (int)strlen(b) && strcmp(b, "str|-42| 2.50|ff|Z|-9876543210|%|0003.142|7   |") == 0, "got '%s' (r=%d)", b, r);
  char tiny[8]; r = vfmt(tiny, sizeof tiny, "%s", "0123456789");
  MT_EXPECT(r == 10 && strcmp(tiny, "0123456") == 0, "truncation: r=%d '%s'", r, tiny);
  r = vfmt(tiny, 0, "%s", "abc");  /* n == 0: nothing written, length returned */
  MT_EXPECT(r == 3, "n=0 returned %d", r);
  /* exact fit at the end of a mapping with the next page unmapped */
  void *pg = 0; sceKernelMmap(0, 2 * PAGE, 3, 0x1002, -1, 0, &pg); sceKernelMunmap((char *)pg + PAGE, PAGE);
  char *end12 = (char *)pg + PAGE - 12;
  r = vfmt(end12, 12, "%s", "01234567890");
  MT_EXPECT(r == 11 && end12[11] == 0, "exact-fit vsnprintf r=%d", r);
  r = vfmt(end12, 12, "%s%d", "0123456789", 123456);  /* longer than n: must not touch the unmapped page */
  MT_EXPECT(r == 16 && end12[11] == 0, "truncated at the page end r=%d", r);
  sceKernelMunmap(pg, PAGE);
  /* many arguments: more than fit in the registers, strings from several places */
  const char *a = "a", *c = "c";
  r = vfmt(b, sizeof b, "%s%d%s%d%s%d%s%d%s%d", a, 1, "b", 2, c, 3, "d", 4, "e", 5);
  MT_EXPECT(strcmp(b, "a1b2c3d4e5") == 0, "10 args: '%s'", b);
  r = vfmt(b, sizeof b, "%f %f %f %f %f %f %f %f %f %f", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);
  MT_EXPECT(b[0] == '1' && strstr(b, "10.000000") != 0, "10 doubles: '%s'", b);
  mt_end();
}

static void t_alloc(void) {
  mt_begin(NM("malloc_calloc_realloc_big_and_many"), "hle");
  size_t big = 192UL << 20;
  unsigned char *p = (unsigned char *)malloc(big);
  MT_EXPECT(p != 0, "malloc(192 MiB) failed");
  if (p) {
    p[0] = 1; p[big / 2] = 2; p[big - 1] = 3;
    MT_EXPECT(p[0] == 1 && p[big / 2] == 2 && p[big - 1] == 3, "big block corrupt");
    unsigned char *q = (unsigned char *)realloc(p, big + (64UL << 20));
    MT_EXPECT(q && q[0] == 1 && q[big / 2] == 2 && q[big - 1] == 3, "realloc lost data");
    if (q) { q[big + (64UL << 20) - 1] = 9; free(q); }
  }
  unsigned char *z = (unsigned char *)calloc(1 << 20, 16);
  int zero = z != 0; for (size_t i = 0; z && i < (16UL << 20); i += 4093) if (z[i]) zero = 0;
  MT_EXPECT(zero, "calloc(16 MiB) not zero");
  free(z);
  void *ov = calloc(1UL << 62, 8);
  MT_EXPECT(ov == 0, "libc HLE calloc does not detect n*size overflow (returns a block for a wrapped size)" " [" "calloc(2^62, 8) = %p" "]", ov);
  void *huge = malloc(1UL << 62);
  MT_EXPECT(huge == 0, "libc HLE malloc(2^62) returns a block instead of NULL/ENOMEM" " [" "malloc(2^62) = %p" "]", huge);
  // memalign: size + align + header wraps to a small number, which a missing overflow check turns into a tiny block.
  void *ma1 = memalign(64, (size_t)-1 - 8);
  MT_EXPECT(ma1 == 0, "memalign(64, SIZE_MAX-8) wrapped size+align and returned a block" " [" "%p" "]", ma1);
  void *ma2 = memalign(1UL << 20, (size_t)-1 - (1UL << 20));
  MT_EXPECT(ma2 == 0, "memalign(1 MiB, SIZE_MAX-1 MiB) wrapped and returned a block" " [" "%p" "]", ma2);
  void *ma3 = memalign(1UL << 45, 64);
  MT_EXPECT(ma3 == 0, "memalign with a 32 TiB alignment returned a block" " [" "%p" "]", ma3);
  void *ma4 = memalign(256, 100);
  MT_EXPECT(ma4 && ((uintptr_t)ma4 & 255) == 0, "memalign(256,100) = %p not aligned", ma4);
  free(ma4);
  void *zero0 = malloc(0); free(zero0);
  free(0);
  enum { M = 20000 };
  static unsigned char *ptrs[M];
  int bad = 0;
  for (int i = 0; i < M; i++) { size_t sz = 1 + (size_t)((i * 37) % 300); ptrs[i] = (unsigned char *)malloc(sz); if (!ptrs[i]) { bad++; continue; } for (size_t k = 0; k < sz; k++) ptrs[i][k] = (unsigned char)(i + k); }
  for (int i = 0; i < M; i++) { if (!ptrs[i]) continue; size_t sz = 1 + (size_t)((i * 37) % 300); for (size_t k = 0; k < sz; k++) if (ptrs[i][k] != (unsigned char)(i + k)) { bad++; break; } }
  for (int i = 0; i < M; i += 2) free(ptrs[i]);
  for (int i = 0; i < M; i += 2) { ptrs[i] = (unsigned char *)malloc(64); if (ptrs[i]) memset(ptrs[i], 0xEE, 64); }
  for (int i = 1; i < M; i += 2) { if (!ptrs[i]) continue; size_t sz = 1 + (size_t)((i * 37) % 300); for (size_t k = 0; k < sz; k++) if (ptrs[i][k] != (unsigned char)(i + k)) { bad++; break; } }
  MT_EXPECT(bad == 0, "%d heap blocks corrupted or not allocated", bad);
  for (int i = 0; i < M; i++) free(ptrs[i]);
  mt_end();

  mt_begin(NM("large_copies_between_mappings"), "hle");
  void *a = 0, *b = 0; size_t n = 64UL << 20;
  MT_EXPECT(sceKernelMmap(0, n, 3, 0x1002, -1, 0, &a) == 0 && sceKernelMmap(0, n, 3, 0x1002, -1, 0, &b) == 0, "mmap 2 x 64 MiB");
  for (size_t i = 0; i < n; i += 4096) ((unsigned char *)a)[i] = (unsigned char)(i >> 12);
  memcpy(b, a, n);
  int ok = 1; for (size_t i = 0; i < n; i += 4096) if (((unsigned char *)b)[i] != (unsigned char)(i >> 12)) ok = 0;
  MT_EXPECT(ok, "64 MiB memcpy mismatch");
  MT_EXPECT(memcmp(a, b, n) == 0, "memcmp of equal 64 MiB blocks");
  memset(b, 0, n); MT_EXPECT(((unsigned char *)b)[n - 1] == 0 && ((unsigned char *)b)[n / 2] == 0, "64 MiB memset");
  sceKernelMunmap(a, n); sceKernelMunmap(b, n);
  mt_end();
}

int main(void) {
  mt_init();
  MODE = mtg_org() ? "identity" : "base";
  const char *c = mtg_arg1();
  if (mtg_eq(c, "qsort")) t_qsort_structs();
  else if (mtg_eq(c, "strings")) t_strings();
  else if (mtg_eq(c, "format")) t_format();
  else if (mtg_eq(c, "alloc")) t_alloc();
  else { mt_begin(NM("no_case_selected"), "hle"); MT_EXPECT(0, "case '%s' unknown", c); mt_end(); }
  return mt_status();
}
