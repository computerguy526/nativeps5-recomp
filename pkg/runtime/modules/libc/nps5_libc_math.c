// SPDX-License-Identifier: GPL-2.0-or-later
// libc HLE: <math.h>. SysV: double/float args in xmm0..; result in xmm0 (low lane); integer
// results in rax. The host (macOS) libm is correctly rounded to within 1 ulp of the x86 libm for
// these functions; exact-operation functions (sqrt, floor, fmod, ldexp, ...) are bit-exact.
#include "nps5_libc.h"
#include <math.h>

#define SH(name) void shim_##name(GuestContext *ctx)
#define X0 (ctx->xmm[0].f64[0])
#define X1 (ctx->xmm[1].f64[0])
#define X2 (ctx->xmm[2].f64[0])
#define F0 (ctx->xmm[0].f32[0])
#define F1 (ctx->xmm[1].f32[0])
#define F2 (ctx->xmm[2].f32[0])
#define D1(n) SH(n) { X0 = n(X0); SHIM_RETURN(); } SH(n##f) { F0 = n##f(F0); SHIM_RETURN(); }
#define D2(n) SH(n) { X0 = n(X0, X1); SHIM_RETURN(); } SH(n##f) { F0 = n##f(F0, F1); SHIM_RETURN(); }

D1(sin)
D1(cos)
D1(tan)
D1(asin)
D1(acos)
D1(atan)
D1(sinh)
D1(cosh)
D1(tanh)
D1(asinh)
D1(acosh)
D1(atanh)
D1(exp)
D1(exp2)
D1(expm1)
D1(log)
D1(log2)
D1(log10)
D1(log1p)
D1(sqrt)
D1(cbrt)
D1(floor)
D1(ceil)
D1(trunc)
D1(round)
D1(rint)
D1(nearbyint)
D1(fabs)
D1(erf)
D1(erfc)
D1(tgamma)
D1(lgamma)
D1(logb)
D2(atan2)
D2(pow)
D2(hypot)
D2(fmod)
D2(fmin)
D2(fmax)
D2(fdim)
D2(copysign)
D2(remainder)
D2(nextafter)
SH(fma) { X0 = fma(X0, X1, X2); SHIM_RETURN(); }
SH(fmaf) { F0 = fmaf(F0, F1, F2); SHIM_RETURN(); }
SH(ldexp) { X0 = ldexp(X0, (int)ctx->rdi); SHIM_RETURN(); }
SH(ldexpf) { F0 = ldexpf(F0, (int)ctx->rdi); SHIM_RETURN(); }
SH(scalbn) { X0 = scalbn(X0, (int)ctx->rdi); SHIM_RETURN(); }
SH(scalbnf) { F0 = scalbnf(F0, (int)ctx->rdi); SHIM_RETURN(); }
SH(frexp) {
  int e; X0 = frexp(X0, &e);
  *(int32_t *)NPS5_G2H(ctx, ctx->rdi) = e;
  SHIM_RETURN();
}
SH(frexpf) {
  int e; F0 = frexpf(F0, &e);
  *(int32_t *)NPS5_G2H(ctx, ctx->rdi) = e;
  SHIM_RETURN();
}
SH(modf) { X0 = modf(X0, (double *)NPS5_G2H(ctx, ctx->rdi)); SHIM_RETURN(); }
SH(modff) { F0 = modff(F0, (float *)NPS5_G2H(ctx, ctx->rdi)); SHIM_RETURN(); }
SH(remquo) {
  int q; X0 = remquo(X0, X1, &q);
  *(int32_t *)NPS5_G2H(ctx, ctx->rdi) = q;
  SHIM_RETURN();
}
SH(sincos) {
  double s = sin(X0), c = cos(X0);
  *(double *)NPS5_G2H(ctx, ctx->rdi) = s; *(double *)NPS5_G2H(ctx, ctx->rsi) = c;
  SHIM_RETURN();
}
SH(ilogb) { LC_RET_INT(ilogb(X0)); }
SH(ilogbf) { LC_RET_INT(ilogbf(F0)); }
SH(lround) { LC_RET((int64_t)lround(X0)); }
SH(lroundf) { LC_RET((int64_t)lroundf(F0)); }
SH(llround) { LC_RET((int64_t)llround(X0)); }
SH(llroundf) { LC_RET((int64_t)llroundf(F0)); }
SH(lrint) { LC_RET((int64_t)lrint(X0)); }
SH(lrintf) { LC_RET((int64_t)lrintf(F0)); }
SH(llrint) { LC_RET((int64_t)llrint(X0)); }
SH(llrintf) { LC_RET((int64_t)llrintf(F0)); }
SH(isnan) { LC_RET_INT(isnan(X0)); }
SH(isinf) { LC_RET_INT(isinf(X0)); }
SH(finite) { LC_RET_INT(isfinite(X0)); }
SH(__isnanf) { LC_RET_INT(isnan(F0)); }
SH(__isinff) { LC_RET_INT(isinf(F0)); }
SH(__fpclassifyd) { LC_RET_INT(fpclassify(X0)); }
SH(__fpclassifyf) { LC_RET_INT(fpclassify(F0)); }
