/* Tiny result recorder shared by the memtests C programs (host harnesses and PS5 guest programs).
 *
 * One test case = one mt_begin()/mt_end() pair. Inside a case:
 *   MT_EXPECT(cond, fmt, ...)         a violated expectation: the case FAILs
 *   MT_KNOWN(cond, reason, fmt, ...)  an expectation that the current implementation is known (and
 *                                     recorded in docs/m1/memtests.md) to violate: the case is
 *                                     KNOWN-FAIL while it is violated, PASS once the bug is fixed.
 * Output, one line per case:   RESULT|<name>|<area>|<PASS|FAIL|KNOWN-FAIL|SKIP>|<detail>
 * With MEMTESTS_STRICT=1 in the environment (host harnesses), KNOWN-FAIL is reported as FAIL, so
 * the fix loop sees the open bugs as red. The exit status is the number of FAILs (capped).
 * Freestanding on purpose: the guest build has no printf/stdio dependency beyond sceKernelWrite. */
#ifndef MT_H
#define MT_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef MT_GUEST
extern long sceKernelWrite(int fd, const void *buf, size_t n);
static void mt_out(const char *s, size_t n) { sceKernelWrite(1, s, n); }
/* freestanding formatter: %s %c %d %u %x %X %p with l/ll/z modifiers and 0/width; enough for the messages here */
static int mt_vsnprintf(char *dst, size_t cap, const char *f, va_list ap) {
  size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) dst[n] = (char)(ch); n++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++;
    int zero = 0, width = 0, lng = 0;
    if (*f == '0') { zero = 1; f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    while (*f == 'l' || *f == 'z' || *f == 'h') { if (*f != 'h') lng++; f++; }
    char tmp[24]; int k = 0;
    if (*f == 's') { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) { PUT(*s); s++; } continue; }
    if (*f == 'c') { PUT(va_arg(ap, int)); continue; }
    if (*f == '%') { PUT('%'); continue; }
    uint64_t v; int neg = 0;
    if (*f == 'd' || *f == 'i') { int64_t sv = lng ? va_arg(ap, long long) : va_arg(ap, int); if (sv < 0) { neg = 1; v = (uint64_t)(-(sv + 1)) + 1; } else v = (uint64_t)sv; }
    else { v = lng ? (uint64_t)va_arg(ap, unsigned long long) : (uint64_t)va_arg(ap, unsigned int); }
    if (*f == 'p') { v = (uint64_t)va_arg(ap, void *); PUT('0'); PUT('x'); }
    unsigned base = (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : 10;
    if (v == 0) tmp[k++] = '0';
    while (v) { unsigned d = (unsigned)(v % base); tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; }
    if (neg) PUT('-');
    for (int pad = k; pad < width; pad++) PUT(zero ? '0' : ' ');
    while (k) PUT(tmp[--k]);
  }
  if (cap) dst[n < cap ? n : cap - 1] = 0;
  return (int)n;
#undef PUT
}
static int mt_snprintf(char *dst, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int r = mt_vsnprintf(dst, cap, f, ap); va_end(ap); return r; }
static size_t mt_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
#define MT_VSNPRINTF mt_vsnprintf
#define MT_SNPRINTF mt_snprintf
#define MT_STRLEN mt_strlen
#else
#define MT_VSNPRINTF vsnprintf
#define MT_SNPRINTF snprintf
#define MT_STRLEN strlen
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int mt_fd = 1;
static void mt_out(const char *s, size_t n) { (void)!write(mt_fd, s, n); }
#endif

static struct {
  const char *name, *area;
  int fail, known, skip, nfail_total;
  char fdetail[400], kdetail[400];
  int strict;
  int counts[4];
} mt_s;

static void mt_init(void) {
#ifndef MT_GUEST
  const char *e = getenv("MEMTESTS_STRICT");
  mt_s.strict = e && e[0] == '1';
  const char *o = getenv("MEMTESTS_OUT");  /* result lines go here when set: the runtime's own logging shares stdout */
  if (o && *o) { int fd = open(o, O_WRONLY | O_CREAT | O_APPEND, 0644); if (fd >= 0) mt_fd = fd; }
#endif
}
static void mt_begin(const char *name, const char *area) {
  mt_s.name = name;
  mt_s.area = area;
  mt_s.fail = mt_s.known = mt_s.skip = 0;
  mt_s.fdetail[0] = mt_s.kdetail[0] = 0;
}
static void mt_note(char *dst, size_t cap, const char *fmt, va_list ap) {
  size_t n = 0;
  while (n < cap && dst[n]) n++;
  if (n + 3 >= cap) return;
  if (n) { dst[n++] = ';'; dst[n++] = ' '; }
  MT_VSNPRINTF(dst + n, cap - n, fmt, ap);
  for (char *p = dst; *p; p++) if (*p == '|' || *p == '\n') *p = '/';
}
static void mt_expect(int ok, const char *fmt, ...) {
  if (ok) return;
  mt_s.fail = 1;
  va_list ap;
  va_start(ap, fmt);
  mt_note(mt_s.fdetail, sizeof mt_s.fdetail, fmt, ap);
  va_end(ap);
}
static void mt_known(int ok, const char *reason, const char *fmt, ...) {
  if (ok) return;
  mt_s.known = 1;
  char tmp[300];
  va_list ap;
  va_start(ap, fmt);
  MT_VSNPRINTF(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  char one[400];
  MT_SNPRINTF(one, sizeof one, "%s [%s]", reason, tmp);
  va_list none;
  (void)none;
  size_t n = MT_STRLEN(mt_s.kdetail);
  if (n + 4 < sizeof mt_s.kdetail) MT_SNPRINTF(mt_s.kdetail + n, sizeof mt_s.kdetail - n, "%s%s", n ? "; " : "", one);
  for (char *p = mt_s.kdetail; *p; p++) if (*p == '|' || *p == '\n') *p = '/';
}
static void mt_skip(const char *fmt, ...) {
  mt_s.skip = 1;
  va_list ap;
  va_start(ap, fmt);
  mt_note(mt_s.kdetail, sizeof mt_s.kdetail, fmt, ap);
  va_end(ap);
}
static void mt_end(void) {
  const char *st = "PASS", *d = "";
  if (mt_s.fail) st = "FAIL", d = mt_s.fdetail;
  else if (mt_s.known) { st = mt_s.strict ? "FAIL" : "KNOWN-FAIL"; d = mt_s.kdetail; }
  else if (mt_s.skip) st = "SKIP", d = mt_s.kdetail;
  if (st[0] == 'F') mt_s.nfail_total++;
  char line[1100];
  int n = MT_SNPRINTF(line, sizeof line, "RESULT|%s|%s|%s|%s\n", mt_s.name, mt_s.area, st, d);
  mt_out(line, (size_t)n);
}
static int mt_status(void) { return mt_s.nfail_total > 100 ? 100 : mt_s.nfail_total; }
#define MT_EXPECT(cond, ...) mt_expect(!!(cond), __VA_ARGS__)
#define MT_KNOWN(cond, reason, ...) mt_known(!!(cond), reason, __VA_ARGS__)
#endif
