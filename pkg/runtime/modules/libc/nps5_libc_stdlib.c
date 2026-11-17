// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE: <stdlib.h>, <inttypes.h>, <time.h>, environment, atexit.
#include "nps5_libc.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SH(name) void shim_##name(GuestContext *ctx)
#define S0 ((char *)NPS5_G2H(ctx, ctx->rdi))
#define S1 ((char *)NPS5_G2H(ctx, ctx->rsi))

// ------------------------------------------------------------------ div family (SysV struct returns)
// div_t {int quot, rem} is 8 bytes of INTEGER class: returned packed in RAX (quot = low 32 bits).
// ldiv_t / lldiv_t / imaxdiv_t {long quot, rem} are 16 bytes of two INTEGER eightbytes: RAX:RDX.
static void fpe(GuestContext *ctx, const char *fn) {
  nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc %s: division by zero or overflow (x86 would raise #DE) rip=0x%llx", fn,
           (unsigned long long)ctx->rip);
  raise(SIGFPE);
}
SH(div) {
  int32_t n = (int32_t)ctx->rdi, d = (int32_t)ctx->rsi;
  if (d == 0 || (n == INT32_MIN && d == -1)) { fpe(ctx, "div"); LC_RET(0); }
  uint32_t q = (uint32_t)(n / d), r = (uint32_t)(n % d);
  LC_RET((uint64_t)q | ((uint64_t)r << 32));
}
static void div64(GuestContext *ctx, const char *fn) {
  int64_t n = (int64_t)ctx->rdi, d = (int64_t)ctx->rsi;
  if (d == 0 || (n == INT64_MIN && d == -1)) { fpe(ctx, fn); ctx->rax = ctx->rdx = 0; return; }
  ctx->rax = (uint64_t)(n / d);
  ctx->rdx = (uint64_t)(n % d);
}
SH(ldiv) { div64(ctx, "ldiv"); SHIM_RETURN(); }
SH(lldiv) { div64(ctx, "lldiv"); SHIM_RETURN(); }
SH(imaxdiv) { div64(ctx, "imaxdiv"); SHIM_RETURN(); }
SH(abs) { int32_t v = (int32_t)ctx->rdi; LC_RET_INT(v < 0 ? (int32_t)(0u - (uint32_t)v) : v); }
SH(labs) { int64_t v = (int64_t)ctx->rdi; LC_RET(v < 0 ? 0 - (uint64_t)v : (uint64_t)v); }
SH(llabs) { int64_t v = (int64_t)ctx->rdi; LC_RET(v < 0 ? 0 - (uint64_t)v : (uint64_t)v); }
SH(imaxabs) { int64_t v = (int64_t)ctx->rdi; LC_RET(v < 0 ? 0 - (uint64_t)v : (uint64_t)v); }

// ------------------------------------------------------------------ qsort / bsearch
// qsort is FreeBSD's (Bentley-McIlroy) so equal-key ordering matches the console libc. The
// comparator is a guest function, called through nps5_libc_call_guest.
typedef struct { GuestContext *ctx; uint64_t cmp; uint64_t es; } QS;
static int qcmp(QS *q, uint64_t a, uint64_t b) { return (int32_t)nps5_libc_call_guest(q->ctx, q->cmp, a, b, 0, 0); }
static void qswap(QS *q, uint64_t a, uint64_t b) {
  uint8_t *x = (uint8_t *)NPS5_G2H(q->ctx, a), *y = (uint8_t *)NPS5_G2H(q->ctx, b);
  for (uint64_t i = 0; i < q->es; i++) { uint8_t t = x[i]; x[i] = y[i]; y[i] = t; }
}
static void qvecswap(QS *q, uint64_t a, uint64_t b, uint64_t n) {
  for (uint64_t i = 0; i < n; i += q->es) qswap(q, a + i, b + i);
}
static uint64_t qmed3(QS *q, uint64_t a, uint64_t b, uint64_t c) {
  return qcmp(q, a, b) < 0 ? (qcmp(q, b, c) < 0 ? b : (qcmp(q, a, c) < 0 ? c : a))
                           : (qcmp(q, b, c) > 0 ? b : (qcmp(q, a, c) < 0 ? a : c));
}
static void qsort_g(QS *q, uint64_t a, uint64_t n) {
  uint64_t es = q->es, pa, pb, pc, pd, pl, pm, pn, d, r;
  int cr, swap_cnt;
loop:
  swap_cnt = 0;
  if (n < 7) {
  insertion:
    for (pm = a + es; pm < a + n * es; pm += es)
      for (pl = pm; pl > a && qcmp(q, pl - es, pl) > 0; pl -= es) qswap(q, pl, pl - es);
    return;
  }
  pm = a + (n / 2) * es;
  if (n > 7) {
    pl = a;
    pn = a + (n - 1) * es;
    if (n > 40) {
      d = (n / 8) * es;
      pl = qmed3(q, pl, pl + d, pl + 2 * d);
      pm = qmed3(q, pm - d, pm, pm + d);
      pn = qmed3(q, pn - 2 * d, pn - d, pn);
    }
    pm = qmed3(q, pl, pm, pn);
  }
  qswap(q, a, pm);
  pa = pb = a + es;
  pc = pd = a + (n - 1) * es;
  for (;;) {
    while (pb <= pc && (cr = qcmp(q, pb, a)) <= 0) {
      if (cr == 0) { swap_cnt = 1; qswap(q, pa, pb); pa += es; }
      pb += es;
    }
    while (pb <= pc && (cr = qcmp(q, pc, a)) >= 0) {
      if (cr == 0) { swap_cnt = 1; qswap(q, pc, pd); pd -= es; }
      pc -= es;
    }
    if (pb > pc) break;
    qswap(q, pb, pc);
    swap_cnt = 1;
    pb += es;
    pc -= es;
  }
  if (swap_cnt == 0) goto insertion;
  pn = a + n * es;
  r = (pa - a) < (pb - pa) ? (pa - a) : (pb - pa);
  qvecswap(q, a, pb - r, r);
  r = (pd - pc) < (pn - pd - es) ? (pd - pc) : (pn - pd - es);
  qvecswap(q, pb, pn - r, r);
  if ((r = pb - pa) > es) qsort_g(q, a, r / es);
  if ((r = pd - pc) > es) { a = pn - r; n = r / es; goto loop; }
}
SH(qsort) {  // (base, nmemb, size, compar)
  QS q = {ctx, ctx->rcx, ctx->rdx};
  if (ctx->rsi > 1 && ctx->rdx > 0) qsort_g(&q, ctx->rdi, ctx->rsi);
  SHIM_RETURN();
}
SH(bsearch) {  // (key, base, nmemb, size, compar)
  uint64_t key = ctx->rdi, base = ctx->rsi, size = ctx->rcx, cmp = ctx->r8;
  uint64_t lim = ctx->rdx;
  GuestContext *c = ctx;
  for (; lim != 0; lim >>= 1) {
    uint64_t p = base + (lim >> 1) * size;
    int r = (int32_t)nps5_libc_call_guest(c, cmp, key, p, 0, 0);
    if (r == 0) LC_RET(p);
    if (r > 0) { base = p + size; lim--; }
  }
  LC_RET(0);
}

// ------------------------------------------------------------------ string -> number
#define END_OUT(endp_reg, host_end, s) \
  do { if (endp_reg) *(uint64_t *)NPS5_G2H(ctx, endp_reg) = NPS5_H2G(ctx, (host_end)); (void)(s); } while (0)
static void conv_errno(GuestContext *ctx) { if (errno == ERANGE || errno == EINVAL) lc_errno(ctx, errno); }
#define STRTOI(name, type, fn)                                               \
  SH(name) {                                                                 \
    char *end; errno = 0;                                                    \
    type v = fn(S0, &end, (int)ctx->rdx);                                    \
    END_OUT(ctx->rsi, end, S0); conv_errno(ctx); LC_RET(v);                  \
  }
STRTOI(strtol, long, strtol)
STRTOI(strtoll, long long, strtoll)
STRTOI(strtoul, unsigned long, strtoul)
STRTOI(strtoull, unsigned long long, strtoull)
STRTOI(strtoimax, intmax_t, strtoimax)
STRTOI(strtoumax, uintmax_t, strtoumax)
SH(atoi) { LC_RET_INT(atoi(S0)); }
SH(atol) { LC_RET((uint64_t)atol(S0)); }
SH(atoll) { LC_RET((uint64_t)atoll(S0)); }
SH(strtod) {
  char *end; errno = 0;
  double v = strtod(S0, &end);
  END_OUT(ctx->rsi, end, S0); conv_errno(ctx);
  ctx->xmm[0].f64[0] = v;
  SHIM_RETURN();
}
SH(atof) { ctx->xmm[0].f64[0] = strtod(S0, NULL); SHIM_RETURN(); }
SH(strtof) {
  char *end; errno = 0;
  float v = strtof(S0, &end);
  END_OUT(ctx->rsi, end, S0); conv_errno(ctx);
  ctx->xmm[0].u128 = 0; ctx->xmm[0].f32[0] = v;
  SHIM_RETURN();
}
SH(strtold) {  // long double returns in st(0); the guest FPU stack holds doubles
  char *end; errno = 0;
  long double v = strtold(S0, &end);
  END_OUT(ctx->rsi, end, S0); conv_errno(ctx);
  fpu_push(ctx, (double)v);
  SHIM_RETURN();
}
// wide variants: narrow to ASCII, convert, map the end pointer back
static size_t narrow(GuestContext *ctx, uint64_t waddr, char *out, size_t cap) {
  const int32_t *w = (const int32_t *)NPS5_G2H(ctx, waddr);
  size_t i = 0;
  for (; w[i] && i + 1 < cap; i++) out[i] = (w[i] > 0 && w[i] < 128) ? (char)w[i] : '\x01';
  out[i] = 0;
  return i;
}
#define WSTRTO(name, decl, call, ret)                                                   \
  SH(name) {                                                                            \
    char buf[1024]; narrow(ctx, ctx->rdi, buf, sizeof buf);                             \
    char *end; errno = 0; decl = call;                                                  \
    if (ctx->rsi) *(uint64_t *)NPS5_G2H(ctx, ctx->rsi) = ctx->rdi + 4ULL * (uint64_t)(end - buf); \
    conv_errno(ctx); ret; SHIM_RETURN();                                                \
  }
WSTRTO(wcstod, double v, strtod(buf, &end), ctx->xmm[0].f64[0] = v)
WSTRTO(wcstof, float v, strtof(buf, &end), (ctx->xmm[0].u128 = 0, ctx->xmm[0].f32[0] = v))
WSTRTO(wcstold, long double v, strtold(buf, &end), fpu_push(ctx, (double)v))
WSTRTO(wcstol, long v, strtol(buf, &end, (int)ctx->rdx), ctx->rax = (uint64_t)v)
WSTRTO(wcstoll, long long v, strtoll(buf, &end, (int)ctx->rdx), ctx->rax = (uint64_t)v)
WSTRTO(wcstoul, unsigned long v, strtoul(buf, &end, (int)ctx->rdx), ctx->rax = (uint64_t)v)
WSTRTO(wcstoull, unsigned long long v, strtoull(buf, &end, (int)ctx->rdx), ctx->rax = (uint64_t)v)

// ------------------------------------------------------------------ rand (FreeBSD libc Park-Miller)
static _Atomic unsigned long g_rand_next = 1;
static int do_rand(unsigned long *c) {
  long hi, lo, x;
  if (*c == 0) *c = 123459876;
  hi = (long)(*c / 127773);
  lo = (long)(*c % 127773);
  x = 16807 * lo - 2836 * hi;
  if (x < 0) x += 0x7fffffff;
  return (int)((*c = (unsigned long)x) % ((unsigned long)0x7fffffff + 1));
}
SH(srand) { g_rand_next = (unsigned)ctx->rdi; SHIM_RETURN(); }
SH(rand) { unsigned long n = g_rand_next; int r = do_rand(&n); g_rand_next = n; LC_RET_INT(r); }
SH(rand_r) {
  unsigned *p = (unsigned *)NPS5_G2H(ctx, ctx->rdi);
  unsigned long n = *p; int r = do_rand(&n); *p = (unsigned)n; LC_RET_INT(r);
}

// ------------------------------------------------------------------ environment (empty, guest-owned)
typedef struct EnvEnt { char *name; uint64_t val; struct EnvEnt *next; } EnvEnt;
static EnvEnt *g_env;
static pthread_mutex_t g_env_mu = PTHREAD_MUTEX_INITIALIZER;
static EnvEnt *env_find(const char *n) {
  for (EnvEnt *e = g_env; e; e = e->next) if (!strcmp(e->name, n)) return e;
  return NULL;
}
SH(getenv) {
  pthread_mutex_lock(&g_env_mu);
  EnvEnt *e = env_find(S0);
  uint64_t v = e ? e->val : 0;
  pthread_mutex_unlock(&g_env_mu);
  LC_RET(v);
}
SH(setenv) {
  const char *n = S0, *v = S1;
  if (!*n || strchr(n, '=')) { lc_errno(ctx, EINVAL); LC_RET_INT(-1); }
  pthread_mutex_lock(&g_env_mu);
  EnvEnt *e = env_find(n);
  if (e && !(int)ctx->rdx) { pthread_mutex_unlock(&g_env_mu); LC_RET_INT(0); }
  size_t l = strlen(v) + 1;
  uint64_t g = nps5_libc_malloc(ctx, l);
  if (!g) { pthread_mutex_unlock(&g_env_mu); LC_RET_INT(-1); }
  memcpy(NPS5_G2H(ctx, g), v, l);
  if (!e) { e = calloc(1, sizeof *e); e->name = strdup(n); e->next = g_env; g_env = e; }
  else nps5_libc_free(ctx, e->val);
  e->val = g;
  pthread_mutex_unlock(&g_env_mu);
  LC_RET_INT(0);
}
SH(unsetenv) {
  pthread_mutex_lock(&g_env_mu);
  for (EnvEnt **pp = &g_env; *pp; pp = &(*pp)->next)
    if (!strcmp((*pp)->name, S0)) { EnvEnt *e = *pp; *pp = e->next; nps5_libc_free(ctx, e->val); free(e->name); free(e); break; }
  pthread_mutex_unlock(&g_env_mu);
  LC_RET_INT(0);
}

// ------------------------------------------------------------------ atexit / abort / _Exit
typedef struct AtExit { uint64_t fn, arg, dso; int has_arg; struct AtExit *next; } AtExit;
static AtExit *g_atexit;
static pthread_mutex_t g_atexit_mu = PTHREAD_MUTEX_INITIALIZER;
static void atexit_add(uint64_t fn, uint64_t arg, uint64_t dso, int has_arg) {
  AtExit *a = calloc(1, sizeof *a);
  a->fn = fn; a->arg = arg; a->dso = dso; a->has_arg = has_arg;
  pthread_mutex_lock(&g_atexit_mu);
  a->next = g_atexit; g_atexit = a;
  pthread_mutex_unlock(&g_atexit_mu);
}
SH(atexit) { atexit_add(ctx->rdi, 0, 0, 0); LC_RET_INT(0); }
SH(__cxa_atexit) { atexit_add(ctx->rdi, ctx->rsi, ctx->rdx, 1); LC_RET_INT(0); }
SH(__cxa_finalize) {  // (dso) : run and drop handlers of that dso (all if 0)
  uint64_t dso = ctx->rdi;
  for (;;) {
    pthread_mutex_lock(&g_atexit_mu);
    AtExit **pp = &g_atexit;
    while (*pp && dso && (*pp)->dso != dso) pp = &(*pp)->next;
    AtExit *a = *pp;
    if (a) *pp = a->next;
    pthread_mutex_unlock(&g_atexit_mu);
    if (!a) break;
    nps5_libc_call_guest(ctx, a->fn, a->has_arg ? a->arg : 0, 0, 0, 0);
    free(a);
  }
  SHIM_RETURN();
}
// Called by shim_exit (weak) before the host quits.
void nps5_libc_run_atexit(GuestContext *ctx) {
  static _Atomic int running;
  if (atomic_exchange(&running, 1)) return;
  for (;;) {
    pthread_mutex_lock(&g_atexit_mu);
    AtExit *a = g_atexit;
    if (a) g_atexit = a->next;
    pthread_mutex_unlock(&g_atexit_mu);
    if (!a) break;
    nps5_libc_call_guest(ctx, a->fn, a->has_arg ? a->arg : 0, 0, 0, 0);
    free(a);
  }
}
SH(abort) {
  nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "guest called abort() rip=0x%llx caller=0x%llx", (unsigned long long)ctx->rip,
           (unsigned long long)(RECOMP_MEM_OK(ctx) && ctx->rsp < ctx->mem_size ? *(uint64_t *)(ctx->mem_base + ctx->rsp) : 0));
  fflush(NULL);
  abort();
}
SH(_Exit) { fflush(NULL); ps4_host_quit((int)ctx->rdi); }

// ------------------------------------------------------------------ time (struct tm layout is the same on FreeBSD and macOS)
SH(time) {
  time_t t = time(NULL);
  if (ctx->rdi) *(int64_t *)NPS5_G2H(ctx, ctx->rdi) = (int64_t)t;
  LC_RET(t);
}
SH(clock) { LC_RET((uint64_t)clock()); }
SH(difftime) { ctx->xmm[0].f64[0] = difftime((time_t)ctx->rdi, (time_t)ctx->rsi); SHIM_RETURN(); }
static uint64_t tm_cell(GuestContext *ctx) {  // static result struct (56 B) + "UTC"
  static uint64_t cell;
  if (!cell) { cell = nps5_libc_malloc(ctx, 64); if (cell) strcpy((char *)NPS5_G2H(ctx, cell) + 56, "UTC"); }
  return cell;
}
static void tm_to_guest(GuestContext *ctx, uint64_t g, const struct tm *t) {
  struct tm *o = (struct tm *)NPS5_G2H(ctx, g);
  *o = *t;
  o->tm_zone = (char *)(uintptr_t)(g + 56);  // guest pointer to "UTC" (cell + 56)
}
static void tm_from_guest(GuestContext *ctx, uint64_t g, struct tm *t) {
  *t = *(struct tm *)NPS5_G2H(ctx, g);
  t->tm_zone = t->tm_zone ? (char *)NPS5_G2H(ctx, (uint64_t)(uintptr_t)t->tm_zone) : (char *)"UTC";
}
SH(gmtime_r) {
  time_t t = *(int64_t *)NPS5_G2H(ctx, ctx->rdi);
  struct tm r;
  if (!gmtime_r(&t, &r)) LC_RET(0);
  tm_to_guest(ctx, ctx->rsi, &r);
  // tm_zone points to a "UTC" string located in the *result* struct's cell only for the static
  // variant; for _r callers give it a stable guest string in the heap.
  static uint64_t utc;
  if (!utc) { utc = nps5_libc_malloc(ctx, 4); if (utc) strcpy((char *)NPS5_G2H(ctx, utc), "UTC"); }
  ((struct tm *)NPS5_G2H(ctx, ctx->rsi))->tm_zone = (char *)(uintptr_t)utc;
  LC_RET(ctx->rsi);
}
SH(gmtime) {
  time_t t = *(int64_t *)NPS5_G2H(ctx, ctx->rdi);
  struct tm r;
  uint64_t cell = tm_cell(ctx);
  if (!cell || !gmtime_r(&t, &r)) LC_RET(0);
  tm_to_guest(ctx, cell, &r);
  LC_RET(cell);
}
// The console's timezone is UTC for the HLE; localtime == gmtime.
SH(localtime) { shim_gmtime(ctx); }
SH(localtime_r) { shim_gmtime_r(ctx); }
SH(timegm) { struct tm t; tm_from_guest(ctx, ctx->rdi, &t); LC_RET((uint64_t)timegm(&t)); }
SH(mktime) { struct tm t; tm_from_guest(ctx, ctx->rdi, &t); LC_RET((uint64_t)timegm(&t)); }
SH(strftime) {  // (char *s, size_t max, const char *fmt, const struct tm *)
  struct tm t; tm_from_guest(ctx, ctx->rcx, &t);
  char *buf = (char *)NPS5_G2H(ctx, ctx->rdi);
  LC_RET(strftime(buf, ctx->rsi, (const char *)NPS5_G2H(ctx, ctx->rdx), &t));
}
SH(sleep) { LC_RET_INT(sleep((unsigned)ctx->rdi)); }

// ------------------------------------------------------------------ locale / cwd
// Only the "C" locale exists. setlocale(cat, NULL|""|"C"|"POSIX") reports "C"; anything else fails
// (NULL), as on a system without that locale, and is logged.
SH(setlocale) {
  static uint64_t c_str;
  const char *l = S1;
  if (l && *l && strcmp(l, "C") && strcmp(l, "POSIX")) {
    nps5_log(NPS5_S_HLE, NPS5_L_WARN, "libc setlocale(%d, \"%s\"): only the C locale is supported", (int)ctx->rdi, l);
    LC_RET(0);
  }
  if (!c_str) { c_str = nps5_libc_malloc(ctx, 4); if (c_str) strcpy((char *)NPS5_G2H(ctx, c_str), "C"); }
  LC_RET(c_str);
}
// getcwd: a console process starts in its application root, /app0.
SH(getcwd) {
  const char *cwd = "/app0";
  if (!ctx->rdi || ctx->rsi < strlen(cwd) + 1) { lc_errno(ctx, ERANGE); LC_RET(0); }
  strcpy(S0, cwd);
  LC_RET(ctx->rdi);
}
