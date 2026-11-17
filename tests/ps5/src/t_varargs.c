/* Varargs (SysV: al = number of vector regs, register save area, overflow area),
 * va_list forwarding, va_copy, and libc formatted output into a buffer. */
#include "ps5test.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static T_NOINLINE uint64_t sum_ints(int n, ...) {
  va_list ap; va_start(ap, n); uint64_t s = 0;
  for (int i = 0; i < n; i++) s = s * 131 + (uint64_t)va_arg(ap, long);
  va_end(ap); return s;
}
static T_NOINLINE double sum_dbls(int n, ...) {
  va_list ap; va_start(ap, n); double s = 0;
  for (int i = 0; i < n; i++) s = s * 1.5 + va_arg(ap, double);
  va_end(ap); return s;
}
/* format string: i=int l=long d=double s=string c=char P=struct{long;double} */
typedef struct { long a; double b; } Pair;
static T_NOINLINE uint64_t mixed(const char *fmt, ...) {
  va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
  uint64_t h = T_FNV0;
  for (const char *f = fmt; *f; f++) {
    switch (*f) {
    case 'i': { int v = va_arg(ap, int); h = t_fnv(h, &v, 4); break; }
    case 'l': { long v = va_arg(ap, long); h = t_fnv(h, &v, 8); break; }
    case 'd': { double v = va_arg(ap, double); h = t_fnv(h, &v, 8); break; }
    case 's': { const char *v = va_arg(ap, const char *); h = t_fnv(h, v, strlen(v)); break; }
    case 'c': { int v = va_arg(ap, int); h = t_fnv(h, &v, 4); break; }
    case 'P': { Pair v = va_arg(ap, Pair); h = t_fnv(h, &v, sizeof v); break; }
    }
  }
  int n = 0; for (const char *f = fmt; *f; f++) n++;
  uint64_t h2 = T_FNV0;  /* second pass over the copy, first arg only */
  if (n) { long v = va_arg(cp, long); h2 = t_fnv(h2, &v, 8); }
  va_end(cp); va_end(ap);
  return h ^ (h2 << 1);
}
static T_NOINLINE int fmt_wrap(char *buf, size_t n, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int main(void) {
  t_hex("sum_ints_3", sum_ints(3, 1L, 2L, 3L));
  t_hex("sum_ints_12", sum_ints(12, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L, 11L, 12L));
  double d = sum_dbls(11, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0);
  uint64_t bits; memcpy(&bits, &d, 8); t_hex("sum_dbls_11", bits);
  Pair p = {-5, 2.75};
  t_hex("mixed", mixed("iidlsdcPidldi", 1, -2, 3.5, 4L, "str", 6.25, 'x', p, 9, 10.0, 11L, 12.5, 13));
  static char buf[256];
  int n = fmt_wrap(buf, sizeof buf, "%d|%5d|%-5d|%05d|%x|%X|%o|%c|%s|%10.3s|%ld|%lld|%u", -42, 42, 42, 42, 0xbeef, 0xbeef, 8, 'z', "hello", "truncate", -1L, 1LL << 40, 4000000000u);
  t_dec("snprintf_len1", n); t_str(buf); t_str("\n");
  n = fmt_wrap(buf, sizeof buf, "%f|%.2f|%10.4f|%e|%g|%g|%g|%.0f|%5.1f%%", 3.14159265, 2.675, -1.5, 12345.6789, 0.0001, 1e20, 100.0, 0.5, 99.95);
  t_dec("snprintf_len2", n); t_str(buf); t_str("\n");
  n = fmt_wrap(buf, 8, "%s", "overflowing-buffer");
  t_dec("snprintf_trunc", n); t_str(buf); t_str("\n");
  n = sprintf(buf, "%s-%d-%c-%.1f", "x", 7, 'q', 8.25); t_dec("sprintf_len", n); t_str(buf); t_str("\n");
  return 0;
}
