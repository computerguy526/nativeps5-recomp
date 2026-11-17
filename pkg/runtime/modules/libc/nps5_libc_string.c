// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE: <string.h>, <strings.h>, <ctype.h>, wide-string basics (C locale).
#include "nps5_libc.h"
#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

#define SH(name) void shim_##name(GuestContext *ctx)
#define S0 ((char *)NPS5_G2H(ctx, ctx->rdi))
#define S1 ((char *)NPS5_G2H(ctx, ctx->rsi))

SH(memchr) { LC_RET_PTR(memchr(S0, (int)ctx->rsi, ctx->rdx)); }
SH(memrchr) {
  const uint8_t *s = (const uint8_t *)S0;
  for (size_t n = ctx->rdx; n; n--)
    if (s[n - 1] == (uint8_t)ctx->rsi) LC_RET_PTR(s + n - 1);
  LC_RET(0);
}
SH(memcmp) { LC_RET_INT(memcmp(S0, S1, ctx->rdx)); }
SH(bcmp) { LC_RET_INT(memcmp(S0, S1, ctx->rdx) != 0); }
SH(bcopy) { memmove(S1, S0, ctx->rdx); SHIM_RETURN(); }
SH(bzero) { memset(S0, 0, ctx->rsi); SHIM_RETURN(); }
SH(memccpy) {
  void *r = memccpy(S0, S1, (int)ctx->rdx, ctx->rcx);
  LC_RET_PTR(r);
}
SH(mempcpy) { memcpy(S0, S1, ctx->rdx); LC_RET(ctx->rdi + ctx->rdx); }
SH(strchr) { LC_RET_PTR(strchr(S0, (int)ctx->rsi)); }
SH(strrchr) { LC_RET_PTR(strrchr(S0, (int)ctx->rsi)); }
SH(strchrnul) { char *s = S0; while (*s && *s != (char)ctx->rsi) s++; LC_RET_PTR(s); }
SH(strstr) { LC_RET_PTR(strstr(S0, S1)); }
SH(strcasestr) { LC_RET_PTR(strcasestr(S0, S1)); }
SH(strncmp) { LC_RET_INT(strncmp(S0, S1, ctx->rdx)); }
SH(strcasecmp) { LC_RET_INT(strcasecmp(S0, S1)); }
SH(strncasecmp) { LC_RET_INT(strncasecmp(S0, S1, ctx->rdx)); }
SH(strcoll) { LC_RET_INT(strcmp(S0, S1)); }  // C locale
SH(strxfrm) {
  size_t n = strlen(S1);
  if (ctx->rdx) strlcpy(S0, S1, ctx->rdx);
  LC_RET(n);
}
SH(strcat) { strcat(S0, S1); LC_RET(ctx->rdi); }
SH(strncat) { strncat(S0, S1, ctx->rdx); LC_RET(ctx->rdi); }
SH(strlcpy) { LC_RET(strlcpy(S0, S1, ctx->rdx)); }
SH(strlcat) { LC_RET(strlcat(S0, S1, ctx->rdx)); }
SH(stpcpy) { LC_RET_PTR(stpcpy(S0, S1)); }
SH(stpncpy) { LC_RET_PTR(stpncpy(S0, S1, ctx->rdx)); }
SH(strnlen) { LC_RET(strnlen(S0, ctx->rsi)); }
SH(strspn) { LC_RET(strspn(S0, S1)); }
SH(strcspn) { LC_RET(strcspn(S0, S1)); }
SH(strpbrk) { LC_RET_PTR(strpbrk(S0, S1)); }
SH(strdup) {
  const char *s = S0;
  size_t n = strlen(s) + 1;
  uint64_t p = nps5_libc_malloc(ctx, n);
  if (p) memcpy(NPS5_G2H(ctx, p), s, n);
  LC_RET(p);
}
SH(strndup) {
  const char *s = S0;
  size_t n = strnlen(s, ctx->rsi);
  uint64_t p = nps5_libc_malloc(ctx, n + 1);
  if (p) { memcpy(NPS5_G2H(ctx, p), s, n); ((char *)NPS5_G2H(ctx, p))[n] = 0; }
  LC_RET(p);
}
// strtok_r(char *s, const char *delim, char **save): the save slot lives in guest memory.
SH(strtok_r) {
  uint64_t save_addr = ctx->rdx;
  uint64_t *save = (uint64_t *)NPS5_G2H(ctx, save_addr);
  char *s = S0 ? S0 : (char *)NPS5_G2H(ctx, *save);
  const char *delim = S1;
  if (!s) LC_RET(0);
  s += strspn(s, delim);
  if (!*s) { *save = NPS5_H2G(ctx, s); LC_RET(0); }
  char *tok = s;
  s += strcspn(s, delim);
  if (*s) *s++ = 0;
  *save = NPS5_H2G(ctx, s);
  LC_RET_PTR(tok);
}
SH(strtok) {  // process-global save pointer, as in libc
  static _Thread_local uint64_t save;
  char *s = S0 ? S0 : (char *)NPS5_G2H(ctx, save);
  const char *delim = S1;
  if (!s) LC_RET(0);
  s += strspn(s, delim);
  if (!*s) { save = NPS5_H2G(ctx, s); LC_RET(0); }
  char *tok = s;
  s += strcspn(s, delim);
  if (*s) *s++ = 0;
  save = NPS5_H2G(ctx, s);
  LC_RET_PTR(tok);
}
SH(strsep) {
  uint64_t *sp = (uint64_t *)NPS5_G2H(ctx, ctx->rdi);
  char *s = (char *)NPS5_G2H(ctx, *sp);
  if (!s) LC_RET(0);
  char *tok = s;
  s += strcspn(s, S1);
  if (*s) { *s++ = 0; *sp = NPS5_H2G(ctx, s); } else *sp = 0;
  LC_RET_PTR(tok);
}

// strerror: guest-resident string table, filled lazily from the host's messages (FreeBSD and
// macOS errno numbers agree for the common range, text differs slightly).
static const char *err_text(int e) {
  static char buf[64];
  if (e == 0) return "No error: 0";
  if (e < 0 || e > 106) { snprintf(buf, sizeof buf, "Unknown error: %d", e); return buf; }
  return strerror(e);
}
SH(strerror) {
  static _Thread_local uint64_t cell;
  const char *t = err_text((int)ctx->rdi);
  if (!cell) cell = nps5_libc_malloc(ctx, 96);
  strlcpy((char *)NPS5_G2H(ctx, cell), t, 96);
  LC_RET(cell);
}
SH(strerror_r) {
  const char *t = err_text((int)ctx->rdi);
  if (ctx->rdx) strlcpy(S1, t, ctx->rdx);
  LC_RET_INT(0);
}

// ------------------------------------------------------------------ ctype (C locale, ASCII)
#define CT(name, expr) SH(name) { int c = (int)ctx->rdi; LC_RET_INT((c >= 0 && c < 128) ? !!(expr) : 0); }
CT(isalpha, (c | 32) >= 'a' && (c | 32) <= 'z')
CT(isdigit, c >= '0' && c <= '9')
CT(isalnum, ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9'))
CT(isupper, c >= 'A' && c <= 'Z')
CT(islower, c >= 'a' && c <= 'z')
CT(isspace, c == ' ' || (c >= 9 && c <= 13))
CT(isblank, c == ' ' || c == '\t')
CT(iscntrl, c < 32 || c == 127)
CT(isprint, c >= 32 && c < 127)
CT(isgraph, c > 32 && c < 127)
CT(ispunct, c > 32 && c < 127 && !(((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9')))
CT(isxdigit, (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'))
CT(isascii, 1)
SH(toupper) { int c = (int)ctx->rdi; LC_RET_INT((c >= 'a' && c <= 'z') ? c - 32 : c); }
SH(tolower) { int c = (int)ctx->rdi; LC_RET_INT((c >= 'A' && c <= 'Z') ? c + 32 : c); }
SH(toascii) { LC_RET_INT((int)ctx->rdi & 0x7f); }

// ------------------------------------------------------------------ wchar (wchar_t = 32-bit)
#define W0 ((wchar_t *)NPS5_G2H(ctx, ctx->rdi))
#define W1 ((wchar_t *)NPS5_G2H(ctx, ctx->rsi))
SH(wcslen) { LC_RET(wcslen(W0)); }
SH(wcscmp) { LC_RET_INT(wcscmp(W0, W1)); }
SH(wcsncmp) { LC_RET_INT(wcsncmp(W0, W1, ctx->rdx)); }
SH(wcscoll) { LC_RET_INT(wcscmp(W0, W1)); }
SH(wcsstr) { LC_RET_PTR(wcsstr(W0, W1)); }
SH(wmemcmp) { LC_RET_INT(wmemcmp(W0, W1, ctx->rdx)); }
SH(wmemchr) { LC_RET_PTR(wmemchr(W0, (wchar_t)ctx->rsi, ctx->rdx)); }
SH(wcsxfrm) {
  size_t n = wcslen(W1);
  if (ctx->rdx) {
    wchar_t *d = W0; const wchar_t *s = W1; size_t k = 0;
    while (k + 1 < ctx->rdx && s[k]) { d[k] = s[k]; k++; }
    d[k] = 0;
  }
  LC_RET(n);
}
// Single-byte C locale: byte <-> wchar_t 1:1 (FreeBSD's "C" encoding); >0xFF is EILSEQ.
SH(mbrtowc) {  // (wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
  const uint8_t *s = (const uint8_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!s) LC_RET(0);
  if (ctx->rdx == 0) LC_RET((uint64_t)-2);
  if (ctx->rdi) *(int32_t *)NPS5_G2H(ctx, ctx->rdi) = *s;
  LC_RET(*s ? 1 : 0);
}
SH(mbtowc) {
  const uint8_t *s = (const uint8_t *)NPS5_G2H(ctx, ctx->rsi);
  if (!s) LC_RET_INT(0);
  if (ctx->rdx == 0) LC_RET_INT(-1);
  if (ctx->rdi) *(int32_t *)NPS5_G2H(ctx, ctx->rdi) = *s;
  LC_RET_INT(*s ? 1 : 0);
}
SH(mbrlen) {
  const uint8_t *s = (const uint8_t *)NPS5_G2H(ctx, ctx->rdi);
  if (!s) LC_RET(0);
  if (ctx->rsi == 0) LC_RET((uint64_t)-2);
  LC_RET(*s ? 1 : 0);
}
SH(wcrtomb) {
  uint32_t wc = (uint32_t)ctx->rsi;
  if (!ctx->rdi) LC_RET(1);
  if (wc > 0xFF) { lc_errno(ctx, EILSEQ); LC_RET((uint64_t)-1); }
  *(uint8_t *)NPS5_G2H(ctx, ctx->rdi) = (uint8_t)wc;
  LC_RET(1);
}
SH(mbsrtowcs) {  // (wchar_t *dst, const char **src, size_t len, mbstate_t *)
  uint64_t *srcp = (uint64_t *)NPS5_G2H(ctx, ctx->rsi);
  const uint8_t *s = (const uint8_t *)NPS5_G2H(ctx, *srcp);
  int32_t *d = (int32_t *)NPS5_G2H(ctx, ctx->rdi);
  size_t n = 0;
  if (!d) { LC_RET(strlen((const char *)s)); }
  while (n < ctx->rdx) {
    d[n] = *s;
    if (!*s) { *srcp = 0; LC_RET(n); }
    s++; n++;
  }
  *srcp = NPS5_H2G(ctx, s);
  LC_RET(n);
}
SH(wcstombs) {  // (char *dst, const wchar_t *src, size_t n)
  char *d = S0;
  const int32_t *s = (const int32_t *)NPS5_G2H(ctx, ctx->rsi);
  size_t n = 0;
  for (;; n++) {
    if (d && n >= ctx->rdx) break;
    if ((uint32_t)s[n] > 0xFF) { lc_errno(ctx, EILSEQ); LC_RET((uint64_t)-1); }
    if (d) d[n] = (char)s[n];
    if (!s[n]) LC_RET(n);
  }
  LC_RET(n);
}

// ------------------------------------------------------------------ lean variants (docs/m3/leanshim.md)
#include "nps5_lean.h"
#define L0 ((char *)NPS5_G2H(ctx, a0))
#define L1 ((char *)NPS5_G2H(ctx, a1))
#define LI(v) ((uint64_t)(uint32_t)(int32_t)(v))
#define LP(p) NPS5_H2G(ctx, (p))
NPS5_LEAN_FN(memchr) { return LP(memchr(L0, (int)a1, a2)); }
NPS5_LEAN_FN(memcmp) { return LI(memcmp(L0, L1, a2)); }
NPS5_LEAN_FN(bcmp) { return LI(memcmp(L0, L1, a2) != 0); }
NPS5_LEAN_FN(bzero) { memset(L0, 0, a1); return ctx->rax; }
NPS5_LEAN_FN(strchr) { return LP(strchr(L0, (int)a1)); }
NPS5_LEAN_FN(strrchr) { return LP(strrchr(L0, (int)a1)); }
NPS5_LEAN_FN(strncmp) { return LI(strncmp(L0, L1, a2)); }
NPS5_LEAN_FN(strcasecmp) { return LI(strcasecmp(L0, L1)); }
NPS5_LEAN_FN(strncasecmp) { return LI(strncasecmp(L0, L1, a2)); }
NPS5_LEAN_FN(strcat) { strcat(L0, L1); return a0; }
NPS5_LEAN_FN(strncat) { strncat(L0, L1, a2); return a0; }
NPS5_LEAN_FN(strnlen) { return strnlen(L0, a1); }
NPS5_LEAN_FN(toupper) { int c = (int)a0; return LI((c >= 'a' && c <= 'z') ? c - 32 : c); }
NPS5_LEAN_FN(tolower) { int c = (int)a0; return LI((c >= 'A' && c <= 'Z') ? c + 32 : c); }
