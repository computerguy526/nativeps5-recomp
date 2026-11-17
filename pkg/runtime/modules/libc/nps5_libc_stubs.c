// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE: signal-set helpers and explicit, logged stubs for C-library calls that have no
// sensible implementation in the recompiled process. A stub logs once per name
// ("libc stub NAME called (not implemented)") and returns the documented value; it never
// silently returns 0. Failing calls set errno (ENOSYS) the way the real call would.
#include "nps5_libc.h"
#include <errno.h>

#define SH(name) void shim_##name(GuestContext *ctx)

// FreeBSD sigset_t = uint32_t __bits[4]; signal n is bit (n-1)
SH(sigemptyset) { memset(NPS5_G2H(ctx, ctx->rdi), 0, 16); LC_RET_INT(0); }
static int sig_ok(GuestContext *ctx, int sig) {
  if (sig < 1 || sig > 128) { lc_errno(ctx, EINVAL); return 0; }
  return 1;
}
SH(sigaddset) {
  int s = (int)ctx->rsi;
  if (!sig_ok(ctx, s)) LC_RET_INT(-1);
  ((uint32_t *)NPS5_G2H(ctx, ctx->rdi))[(s - 1) >> 5] |= 1u << ((s - 1) & 31);
  LC_RET_INT(0);
}
SH(sigdelset) {
  int s = (int)ctx->rsi;
  if (!sig_ok(ctx, s)) LC_RET_INT(-1);
  ((uint32_t *)NPS5_G2H(ctx, ctx->rdi))[(s - 1) >> 5] &= ~(1u << ((s - 1) & 31));
  LC_RET_INT(0);
}
SH(sigismember) {
  int s = (int)ctx->rsi;
  if (!sig_ok(ctx, s)) LC_RET_INT(-1);
  LC_RET_INT((((uint32_t *)NPS5_G2H(ctx, ctx->rdi))[(s - 1) >> 5] >> ((s - 1) & 31)) & 1);
}

// STUB(name, return value, errno or 0)
#define STUB(name, ret, err)                       \
  SH(name) {                                       \
    nps5_libc_stub(ctx, #name, (uint64_t)(ret));   \
    if (err) lc_errno(ctx, err);                   \
    LC_RET(ret);                                   \
  }
STUB(dlopen, 0, 0)               // NULL handle; dlerror() also reports NULL
STUB(dlsym, 0, 0)
STUB(dlclose, -1, 0)
STUB(dlerror, 0, 0)
STUB(fork, -1, ENOSYS)
STUB(execvp, -1, ENOSYS)
STUB(execve, -1, ENOSYS)
STUB(waitpid, -1, ECHILD)
STUB(kill, -1, ENOSYS)
STUB(sysctl, -1, ENOENT)
STUB(sysctlbyname, -1, ENOENT)
STUB(pthread_set_name_np, 0, 0)  // void
STUB(_init_env, 0, 0)            // void; libc-internal environ setup
STUB(swprintf, -1, ENOSYS)
STUB(vswprintf, -1, ENOSYS)
STUB(fscanf, -1, ENOSYS)
STUB(scanf, -1, ENOSYS)
STUB(system, -1, ENOSYS)
STUB(popen, 0, ENOSYS)
STUB(qsort_r, 0, 0)              // void; NOT implemented: arg order differs between FreeBSD versions
