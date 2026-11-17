// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE: <stdio.h> (printf/scanf engines read SysV varargs and guest va_lists), FILE table.
#include "nps5_libc.h"
#include "ps4_vfs.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>
#include "nps5_kio.h"

#define SH(name) void shim_##name(GuestContext *ctx)
#define S0 ((char *)NPS5_G2H(ctx, ctx->rdi))
#define S1 ((char *)NPS5_G2H(ctx, ctx->rsi))

// ------------------------------------------------------------------ varargs sources
void lc_args_from_regs(LcArgs *a, GuestContext *ctx, int first_gp) {
  memset(a, 0, sizeof *a);
  a->ctx = ctx;
  a->gp[0] = ctx->rdi; a->gp[1] = ctx->rsi; a->gp[2] = ctx->rdx;
  a->gp[3] = ctx->rcx; a->gp[4] = ctx->r8;  a->gp[5] = ctx->r9;
  for (int i = 0; i < 8; i++) a->fp[i] = ctx->xmm[i].f64[0];
  a->gp_next = first_gp;
  a->stack = ctx->rsp + 8;  // [rsp] is the return address at shim entry
}
void lc_args_from_va(LcArgs *a, GuestContext *ctx, uint64_t va) {
  memset(a, 0, sizeof *a);
  a->ctx = ctx;
  a->va_addr = va;
}
// va_list layout (SysV AMD64 ABI 3.5.7)
typedef struct { uint32_t gp_offset, fp_offset; uint64_t overflow, reg_save; } GuestVaList;

static uint64_t arg_gp(LcArgs *a) {
  GuestContext *ctx = a->ctx;
  if (a->va_addr) {
    GuestVaList *v = (GuestVaList *)NPS5_G2H(ctx, a->va_addr);
    uint64_t r;
    if (v->gp_offset < 48) { r = *(uint64_t *)NPS5_G2H(ctx, v->reg_save + v->gp_offset); v->gp_offset += 8; }
    else { r = *(uint64_t *)NPS5_G2H(ctx, v->overflow); v->overflow += 8; }
    return r;
  }
  if (a->gp_next < 6) return a->gp[a->gp_next++];
  uint64_t r = *(uint64_t *)NPS5_G2H(ctx, a->stack);
  a->stack += 8;
  return r;
}
static double arg_fp(LcArgs *a) {
  GuestContext *ctx = a->ctx;
  if (a->va_addr) {
    GuestVaList *v = (GuestVaList *)NPS5_G2H(ctx, a->va_addr);
    double r;
    if (v->fp_offset < 176) { memcpy(&r, NPS5_G2H(ctx, v->reg_save + v->fp_offset), 8); v->fp_offset += 16; }
    else { memcpy(&r, NPS5_G2H(ctx, v->overflow), 8); v->overflow += 8; }
    return r;
  }
  if (a->fp_next < 8) return a->fp[a->fp_next++];
  double r;
  memcpy(&r, NPS5_G2H(ctx, a->stack), 8);
  a->stack += 8;
  return r;
}
// long double (x87 80-bit) always travels in the overflow area, 16-byte aligned.
static long double arg_ld(LcArgs *a) {
  GuestContext *ctx = a->ctx;
  uint64_t *pp;
  GuestVaList *v = NULL;
  if (a->va_addr) { v = (GuestVaList *)NPS5_G2H(ctx, a->va_addr); pp = &v->overflow; } else pp = &a->stack;
  *pp = (*pp + 15) & ~15ULL;
  const uint8_t *p = (const uint8_t *)NPS5_G2H(ctx, *pp);
  *pp += 16;
  uint64_t mant; uint16_t se;
  memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
  int e = se & 0x7fff, sign = se >> 15;
  long double r;
  if (e == 0 && mant == 0) r = 0.0L;
  else if (e == 0x7fff) r = (mant << 1) ? (long double)NAN : (long double)INFINITY;
  else r = ldexpl((long double)mant, e - 16383 - 63);
  return sign ? -r : r;
}

// ------------------------------------------------------------------ printf engine
typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2 + 64;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

int nps5_libc_format(GuestContext *ctx, const char *fmt, LcArgs *a, char **out) {
  Buf b = {0};
  buf_add(&b, "", 0);
  for (const char *f = fmt; *f;) {
    if (*f != '%') {
      const char *e = strchr(f, '%');
      size_t n = e ? (size_t)(e - f) : strlen(f);
      buf_add(&b, f, n);
      f += n;
      continue;
    }
    const char *start = f++;
    char spec[64]; int sl = 0;
    spec[sl++] = '%';
    // flags
    while (*f && strchr("-+ #0'", *f)) { if (sl < 40) spec[sl++] = *f; f++; }
    int width_star = 0, prec_star = 0, have_prec = 0;
    long width = -1, prec = -1;
    if (*f == '*') { width_star = 1; f++; }
    else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0'); }
    if (*f == '.') {
      f++; have_prec = 1;
      if (*f == '*') { prec_star = 1; f++; }
      else { prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
    }
    if (*f == '$') {
      nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc printf: positional argument '%%n$' unsupported in \"%s\"", fmt);
      break;
    }
    if (width_star) { int w = (int)arg_gp(a); if (w < 0) { spec[sl++] = '-'; w = -w; } sl += snprintf(spec + sl, 12, "%d", w); }
    else if (width >= 0) sl += snprintf(spec + sl, 12, "%ld", width);
    if (have_prec) {
      if (prec_star) { int p = (int)arg_gp(a); if (p >= 0) sl += snprintf(spec + sl, 14, ".%d", p); }
      else sl += snprintf(spec + sl, 14, ".%ld", prec);
    }
    // length
    int lmod = 0;  // 'H'=hh 'h' 'l' 'q'=ll 'j' 'z' 't' 'L'
    if (*f == 'h') { f++; lmod = 'h'; if (*f == 'h') { f++; lmod = 'H'; } }
    else if (*f == 'l') { f++; lmod = 'l'; if (*f == 'l') { f++; lmod = 'q'; } }
    else if (*f == 'j' || *f == 'z' || *f == 't' || *f == 'L' || *f == 'q') { lmod = (*f == 'q') ? 'q' : *f; f++; }
    char conv = *f ? *f++ : 0;
    char tmp[512], *t = tmp;
    int tn;
    char *heap = NULL;
    spec[sl] = 0;
    switch (conv) {
    case 'd': case 'i': {
      long long v = (long long)arg_gp(a);
      if (lmod == 0) v = (int32_t)v; else if (lmod == 'h') v = (int16_t)v; else if (lmod == 'H') v = (int8_t)v;
      strcat(spec, "lld");
      tn = snprintf(NULL, 0, spec, v);
      if (tn >= (int)sizeof tmp) t = heap = malloc(tn + 1);
      snprintf(t, tn + 1, spec, v);
      break;
    }
    case 'u': case 'x': case 'X': case 'o': {
      unsigned long long v = arg_gp(a);
      if (lmod == 0) v = (uint32_t)v; else if (lmod == 'h') v = (uint16_t)v; else if (lmod == 'H') v = (uint8_t)v;
      sl += snprintf(spec + sl, 4, "ll%c", conv);
      tn = snprintf(NULL, 0, spec, v);
      if (tn >= (int)sizeof tmp) t = heap = malloc(tn + 1);
      snprintf(t, tn + 1, spec, v);
      break;
    }
    case 'c': {
      int v = (int)arg_gp(a);
      strcat(spec, "c");
      tn = snprintf(tmp, sizeof tmp, spec, v);
      if (tn >= (int)sizeof tmp) tn = sizeof tmp - 1;
      // NUL char: snprintf's count includes it, buf_add copies tn bytes
      break;
    }
    case 's': {
      uint64_t g = arg_gp(a);
      const char *s = g ? (const char *)NPS5_G2H(ctx, g) : "(null)";
      if (lmod == 'l') {  // %ls: narrow ASCII
        const int32_t *w = (const int32_t *)s;
        size_t n = 0; while (g && w[n]) n++;
        char *nb = malloc(n + 1);
        for (size_t i = 0; i < n; i++) nb[i] = (w[i] > 0 && w[i] < 128) ? (char)w[i] : '?';
        nb[n] = 0; s = nb; heap = nb;
      }
      strcat(spec, "s");
      tn = snprintf(NULL, 0, spec, s);
      char *dst = tmp;
      if (tn >= (int)sizeof tmp) dst = malloc(tn + 1);
      snprintf(dst, tn + 1, spec, s);
      free(heap);
      t = heap = (dst == tmp) ? NULL : dst;
      if (!heap) t = tmp;
      break;
    }
    case 'p': {
      uint64_t v = arg_gp(a);
      strcat(spec, "p");
      tn = snprintf(tmp, sizeof tmp, spec, (void *)(uintptr_t)v);
      break;
    }
    case 'n': {
      uint64_t g = arg_gp(a);
      void *p = NPS5_G2H(ctx, g);
      if (p) {
        if (lmod == 'H') *(int8_t *)p = (int8_t)b.n; else if (lmod == 'h') *(int16_t *)p = (int16_t)b.n;
        else if (lmod == 0) *(int32_t *)p = (int32_t)b.n; else *(int64_t *)p = (int64_t)b.n;
      }
      continue;
    }
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
      if (lmod == 'L') {
        long double v = arg_ld(a);
        sl += snprintf(spec + sl, 4, "L%c", conv);
        tn = snprintf(NULL, 0, spec, v);
        if (tn >= (int)sizeof tmp) t = heap = malloc(tn + 1);
        snprintf(t, tn + 1, spec, v);
      } else {
        double v = arg_fp(a);
        sl += snprintf(spec + sl, 4, "%c", conv);
        tn = snprintf(NULL, 0, spec, v);
        if (tn >= (int)sizeof tmp) t = heap = malloc(tn + 1);
        snprintf(t, tn + 1, spec, v);
      }
      break;
    }
    case '%': tmp[0] = '%'; tn = 1; break;
    default:
      nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc printf: unsupported conversion '%c' in \"%s\"", conv ? conv : '?', fmt);
      buf_add(&b, start, (size_t)(f - start));
      continue;
    }
    if (tn < 0) tn = 0;
    buf_add(&b, t, (size_t)tn);
    free(heap);
  }
  *out = b.p;
  return (int)b.n;
}

// ------------------------------------------------------------------ scanf engine
int nps5_libc_scan(GuestContext *ctx, const char *str, const char *fmt, LcArgs *a) {
  const char *s = str;
  int assigned = 0;
  for (const char *f = fmt; *f;) {
    if (*f == ' ' || (*f >= 9 && *f <= 13)) { while (*s == ' ' || (*s >= 9 && *s <= 13)) s++; f++; continue; }
    if (*f != '%') { if (*s != *f) return assigned ? assigned : (*s ? 0 : -1); s++; f++; continue; }
    f++;
    int suppress = 0, width = 0, lmod = 0;
    if (*f == '*') { suppress = 1; f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == 'h') { f++; lmod = 'h'; if (*f == 'h') { f++; lmod = 'H'; } }
    else if (*f == 'l') { f++; lmod = 'l'; if (*f == 'l') { f++; lmod = 'q'; } }
    else if (*f == 'j' || *f == 'z' || *f == 't' || *f == 'L') { lmod = *f == 'L' ? 'L' : 'q'; f++; }
    char conv = *f ? *f++ : 0;
    if (conv == '%') { while (*s == ' ' || (*s >= 9 && *s <= 13)) s++; if (*s != '%') return assigned; s++; continue; }
    if (conv == 'n') { if (!suppress) { void *p = NPS5_G2H(ctx, arg_gp(a)); if (p) *(int32_t *)p = (int)(s - str); } continue; }
    if (conv != 'c' && conv != '[') while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (!*s) return assigned ? assigned : -1;
    int w = width ? width : 4096;
    switch (conv) {
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
      char tmp[128]; size_t n = 0;
      while (s[n] && n < (size_t)w && n < sizeof tmp - 1 && (strchr("+-0123456789abcdefABCDEFxX", s[n]))) n++;
      memcpy(tmp, s, n); tmp[n] = 0;
      int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
      char *end; errno = 0;
      long long v = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') ? (long long)strtoull(tmp, &end, base) : strtoll(tmp, &end, base);
      if (end == tmp) return assigned;
      s += end - tmp;
      if (!suppress) {
        void *p = NPS5_G2H(ctx, arg_gp(a));
        if (lmod == 'H') *(int8_t *)p = (int8_t)v; else if (lmod == 'h') *(int16_t *)p = (int16_t)v;
        else if (lmod == 0) *(int32_t *)p = (int32_t)v; else *(int64_t *)p = v;
        assigned++;
      }
      break;
    }
    case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
      char tmp[128]; size_t n = 0;
      while (s[n] && n < (size_t)w && n < sizeof tmp - 1 && (strchr("+-0123456789.eEinfINFaAxXpPnN", s[n]))) n++;
      memcpy(tmp, s, n); tmp[n] = 0;
      char *end; double v = strtod(tmp, &end);
      if (end == tmp) return assigned;
      s += end - tmp;
      if (!suppress) {
        void *p = NPS5_G2H(ctx, arg_gp(a));
        if (lmod == 'l') *(double *)p = v; else if (lmod == 'L') nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc scanf: %%Lf unsupported"); else *(float *)p = (float)v;
        assigned++;
      }
      break;
    }
    case 's': {
      size_t n = 0;
      while (s[n] && !(s[n] == ' ' || (s[n] >= 9 && s[n] <= 13)) && n < (size_t)w) n++;
      if (!suppress) { char *d = (char *)NPS5_G2H(ctx, arg_gp(a)); memcpy(d, s, n); d[n] = 0; assigned++; }
      s += n;
      break;
    }
    case 'c': {
      int n = width ? width : 1;
      for (int i = 0; i < n; i++) if (!s[i]) return assigned;
      if (!suppress) { char *d = (char *)NPS5_G2H(ctx, arg_gp(a)); memcpy(d, s, n); assigned++; }
      s += n;
      break;
    }
    case '[': {
      int neg = 0; const char *set;
      if (*f == '^') { neg = 1; f++; }
      set = f;
      if (*f == ']') f++;
      while (*f && *f != ']') f++;
      size_t setlen = (size_t)(f - set);
      if (*f == ']') f++;
      size_t n = 0;
      while (s[n] && n < (size_t)w) {
        int in = 0;
        for (size_t i = 0; i < setlen; i++) {
          if (i + 2 < setlen && set[i + 1] == '-') { if ((unsigned char)s[n] >= (unsigned char)set[i] && (unsigned char)s[n] <= (unsigned char)set[i + 2]) in = 1; i += 2; }
          else if (set[i] == s[n]) in = 1;
        }
        if (in == neg) break;
        n++;
      }
      if (n == 0) return assigned;
      if (!suppress) { char *d = (char *)NPS5_G2H(ctx, arg_gp(a)); memcpy(d, s, n); d[n] = 0; assigned++; }
      s += n;
      break;
    }
    default:
      nps5_log(NPS5_S_HLE, NPS5_L_ERROR, "libc scanf: unsupported conversion '%c' in \"%s\"", conv, fmt);
      return assigned;
    }
  }
  return assigned;
}

// ------------------------------------------------------------------ FILE table
// A guest FILE * points at a 32-byte guest block {u32 magic, u32 index, ...}; the host FILE * lives in
// g_files[index]. fileno()/feof() on the *guest* struct (FreeBSD macros) are not supported.
#define FILE_MAGIC 0x454c4946u  // "FILE"
#define MAXFILES 256
static FILE *g_files[MAXFILES];
static uint64_t g_gaddr[MAXFILES];
static int g_is_std[MAXFILES];  // 1 = unbuffered write(fd) path
static pthread_mutex_t g_files_mu = PTHREAD_MUTEX_INITIALIZER;

static int file_new(GuestContext *ctx, FILE *hf, int std, uint64_t *gaddr) {
  uint64_t g = nps5_libc_calloc(ctx, 1, 32);
  if (!g) return -1;
  pthread_mutex_lock(&g_files_mu);
  int i = 3;
  if (std) i = std - 1;
  else while (i < MAXFILES && g_files[i]) i++;
  if (i >= MAXFILES) { pthread_mutex_unlock(&g_files_mu); nps5_libc_free(ctx, g); return -1; }
  g_files[i] = hf; g_gaddr[i] = g; g_is_std[i] = std ? 1 : 0;
  pthread_mutex_unlock(&g_files_mu);
  uint32_t *w = (uint32_t *)NPS5_G2H(ctx, g);
  w[0] = FILE_MAGIC; w[1] = (uint32_t)i;
  *gaddr = g;
  return i;
}
uint64_t nps5_libc_std_file(GuestContext *ctx, int which) {
  uint64_t g = 0;
  pthread_mutex_lock(&g_files_mu);
  g = g_gaddr[which];
  pthread_mutex_unlock(&g_files_mu);
  if (g) return g;
  FILE *hf = which == 0 ? stdin : which == 1 ? stdout : stderr;
  file_new(ctx, hf, which + 1, &g);
  return g;
}
static int file_idx(GuestContext *ctx, uint64_t g) {
  if (!g || g + 8 > ctx->mem_size) return -1;
  uint32_t *w = (uint32_t *)NPS5_G2H(ctx, g);
  if (w[0] != FILE_MAGIC || w[1] >= MAXFILES || g_gaddr[w[1]] != g) return -1;
  return (int)w[1];
}
static FILE *file_of(GuestContext *ctx, uint64_t g, int *idx) {
  int i = file_idx(ctx, g);
  if (idx) *idx = i;
  if (i < 0) { lc_errno(ctx, EBADF); return NULL; }
  return g_files[i];
}
static size_t file_write(GuestContext *ctx, uint64_t g, const void *buf, size_t n) {
  int i; FILE *f = file_of(ctx, g, &i);
  if (!f) return 0;
  if (g_is_std[i]) {
    int fd = i == 1 ? 1 : 2;
    if (i == 0) return 0;
    size_t done = 0;
    while (done < n) { ssize_t w = nps5_kwrite(fd, (const char *)buf + done, n - done); if (w <= 0) break; done += (size_t)w; }
    return done;
  }
  return nps5_kfwrite(buf, 1, n, f);
}

SH(fopen) {
  // write modes go through the VFS write gate (EROFS under /app0); there is no fallback to the guest path as a host path
  FILE *f = ps4_vfs_fopen(S0, S1);
  if (!f) { lc_errno(ctx, errno); LC_RET(0); }
  uint64_t g;
  if (file_new(ctx, f, 0, &g) < 0) { fclose(f); lc_errno(ctx, EMFILE); LC_RET(0); }
  LC_RET(g);
}
SH(fdopen) {
  FILE *f = fdopen((int)ctx->rdi, S1);
  if (!f) { lc_errno(ctx, errno); LC_RET(0); }
  uint64_t g;
  if (file_new(ctx, f, 0, &g) < 0) { fclose(f); lc_errno(ctx, EMFILE); LC_RET(0); }
  LC_RET(g);
}
SH(fclose) {
  int i; FILE *f = file_of(ctx, ctx->rdi, &i);
  if (!f) LC_RET_INT(-1);
  int r = 0;
  if (!g_is_std[i]) r = fclose(f);
  else fflush(f);
  if (!g_is_std[i]) {
    pthread_mutex_lock(&g_files_mu);
    g_files[i] = NULL; g_gaddr[i] = 0;
    pthread_mutex_unlock(&g_files_mu);
    ((uint32_t *)NPS5_G2H(ctx, ctx->rdi))[0] = 0;
    nps5_libc_free(ctx, ctx->rdi);
  }
  LC_RET_INT(r);
}
SH(fflush) {
  if (!ctx->rdi) { fflush(NULL); LC_RET_INT(0); }
  FILE *f = file_of(ctx, ctx->rdi, NULL);
  LC_RET_INT(f ? fflush(f) : -1);
}
SH(fread) {  // (ptr, size, nmemb, fp)
  FILE *f = file_of(ctx, ctx->rcx, NULL);
  LC_RET(f ? nps5_kfread(S0, ctx->rsi, ctx->rdx, f) : 0);
}
SH(fwrite) {  // (ptr, size, nmemb, fp)
  uint64_t total;
  if (__builtin_mul_overflow(ctx->rsi, ctx->rdx, &total)) LC_RET(0);
  size_t w = file_write(ctx, ctx->rcx, S0, total);
  LC_RET(ctx->rsi ? w / ctx->rsi : 0);
}
SH(fputs) { size_t n = strlen(S0); LC_RET_INT(file_write(ctx, ctx->rsi, S0, n) == n ? 0 : -1); }
SH(fputc) { char c = (char)ctx->rdi; LC_RET_INT(file_write(ctx, ctx->rsi, &c, 1) == 1 ? (uint8_t)c : -1); }
SH(putc) { shim_fputc(ctx); }
SH(puts) {
  size_t n = strlen(S0);
  uint64_t g = nps5_libc_std_file(ctx, 1);
  file_write(ctx, g, S0, n); file_write(ctx, g, "\n", 1);
  LC_RET_INT(0);
}
SH(putchar) { char c = (char)ctx->rdi; file_write(ctx, nps5_libc_std_file(ctx, 1), &c, 1); LC_RET_INT((uint8_t)c); }
SH(fgetc) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? fgetc(f) : -1); }
SH(getc) { shim_fgetc(ctx); }
SH(getchar) { LC_RET_INT(fgetc(stdin)); }
SH(ungetc) { FILE *f = file_of(ctx, ctx->rsi, NULL); LC_RET_INT(f ? ungetc((int)ctx->rdi, f) : -1); }
SH(fgets) {
  FILE *f = file_of(ctx, ctx->rdx, NULL);
  char *r = f ? fgets(S0, (int)ctx->rsi, f) : NULL;
  LC_RET(r ? ctx->rdi : 0);
}
SH(fseek) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? fseek(f, (long)ctx->rsi, (int)ctx->rdx) : -1); }
SH(fseeko) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? fseeko(f, (off_t)ctx->rsi, (int)ctx->rdx) : -1); }
SH(ftell) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET(f ? (uint64_t)ftell(f) : (uint64_t)-1); }
SH(ftello) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET(f ? (uint64_t)ftello(f) : (uint64_t)-1); }
SH(rewind) { FILE *f = file_of(ctx, ctx->rdi, NULL); if (f) rewind(f); SHIM_RETURN(); }
SH(ferror) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? ferror(f) : 1); }
SH(feof) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? feof(f) : 1); }
SH(clearerr) { FILE *f = file_of(ctx, ctx->rdi, NULL); if (f) clearerr(f); SHIM_RETURN(); }
SH(fileno) { int i; FILE *f = file_of(ctx, ctx->rdi, &i); LC_RET_INT(f ? fileno(f) : -1); }
SH(setvbuf) { LC_RET_INT(0); }
SH(setbuf) { SHIM_RETURN(); }
SH(remove) {
  int r = ps4_vfs_remove(S0);
  if (r) lc_errno(ctx, errno);
  LC_RET_INT(r);
}
SH(perror) {
  const char *m = S0;
  char buf[256];
  snprintf(buf, sizeof buf, "%s%s%s\n", m && *m ? m : "", m && *m ? ": " : "", "error");
  file_write(ctx, nps5_libc_std_file(ctx, 2), buf, strlen(buf));
  SHIM_RETURN();
}

// ------------------------------------------------------------------ printf family
static void emit_buf(GuestContext *ctx, uint64_t dst, uint64_t cap, const char *s, int len, int terminate) {
  if (!dst) return;
  char *d = (char *)NPS5_G2H(ctx, dst);
  if (!terminate) { memcpy(d, s, (size_t)len); d[len] = 0; return; }
  if (cap == 0) return;
  size_t n = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
  memcpy(d, s, n);
  d[n] = 0;
}
#define FMT(argexpr_fmt, setup)                                                     \
  LcArgs a; setup; char *out = NULL;                                                \
  int n = nps5_libc_format(ctx, (const char *)NPS5_G2H(ctx, argexpr_fmt), &a, &out);

SH(sprintf) { FMT(ctx->rsi, lc_args_from_regs(&a, ctx, 2)); emit_buf(ctx, ctx->rdi, 0, out, n, 0); free(out); LC_RET_INT(n); }
SH(snprintf) { FMT(ctx->rdx, lc_args_from_regs(&a, ctx, 3)); emit_buf(ctx, ctx->rdi, ctx->rsi, out, n, 1); free(out); LC_RET_INT(n); }
SH(vsprintf) { FMT(ctx->rsi, lc_args_from_va(&a, ctx, ctx->rdx)); emit_buf(ctx, ctx->rdi, 0, out, n, 0); free(out); LC_RET_INT(n); }
SH(vsnprintf) { FMT(ctx->rdx, lc_args_from_va(&a, ctx, ctx->rcx)); emit_buf(ctx, ctx->rdi, ctx->rsi, out, n, 1); free(out); LC_RET_INT(n); }
SH(printf) {
  FMT(ctx->rdi, lc_args_from_regs(&a, ctx, 1));
  file_write(ctx, nps5_libc_std_file(ctx, 1), out, (size_t)n); free(out); LC_RET_INT(n);
}
SH(vprintf) {
  FMT(ctx->rdi, lc_args_from_va(&a, ctx, ctx->rsi));
  file_write(ctx, nps5_libc_std_file(ctx, 1), out, (size_t)n); free(out); LC_RET_INT(n);
}
SH(fprintf) { FMT(ctx->rsi, lc_args_from_regs(&a, ctx, 2)); file_write(ctx, ctx->rdi, out, (size_t)n); free(out); LC_RET_INT(n); }
SH(vfprintf) { FMT(ctx->rsi, lc_args_from_va(&a, ctx, ctx->rdx)); file_write(ctx, ctx->rdi, out, (size_t)n); free(out); LC_RET_INT(n); }
SH(dprintf) {
  FMT(ctx->rsi, lc_args_from_regs(&a, ctx, 2));
  ssize_t w = nps5_kwrite((int)ctx->rdi, out, (size_t)n); (void)w; free(out); LC_RET_INT(n);
}
static void asprintf_finish(GuestContext *ctx, char *out, int n) {
  uint64_t g = nps5_libc_malloc(ctx, (uint64_t)n + 1);
  if (!g) { *(uint64_t *)NPS5_G2H(ctx, ctx->rdi) = 0; free(out); LC_RET_INT(-1); }
  memcpy(NPS5_G2H(ctx, g), out, (size_t)n + 1);
  *(uint64_t *)NPS5_G2H(ctx, ctx->rdi) = g;
  free(out);
  LC_RET_INT(n);
}
SH(asprintf) { FMT(ctx->rsi, lc_args_from_regs(&a, ctx, 2)); asprintf_finish(ctx, out, n); }
SH(vasprintf) { FMT(ctx->rsi, lc_args_from_va(&a, ctx, ctx->rdx)); asprintf_finish(ctx, out, n); }

// ------------------------------------------------------------------ scanf family (string sources)
SH(sscanf) { LcArgs a; lc_args_from_regs(&a, ctx, 2); LC_RET_INT(nps5_libc_scan(ctx, S0, S1, &a)); }
SH(vsscanf) { LcArgs a; lc_args_from_va(&a, ctx, ctx->rdx); LC_RET_INT(nps5_libc_scan(ctx, S0, S1, &a)); }

// ------------------------------------------------------------------ wide stdio (C locale byte semantics)
SH(fputwc) { char c = (char)ctx->rdi; LC_RET_INT(file_write(ctx, ctx->rsi, &c, 1) == 1 ? (int)(uint32_t)ctx->rdi : -1); }
SH(fgetwc) { FILE *f = file_of(ctx, ctx->rdi, NULL); LC_RET_INT(f ? fgetc(f) : -1); }
SH(ungetwc) { FILE *f = file_of(ctx, ctx->rsi, NULL); LC_RET_INT(f ? ungetc((int)ctx->rdi, f) : -1); }
