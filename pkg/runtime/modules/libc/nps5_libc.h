// SPDX-License-Identifier: GPL-2.0-or-later
// HLE C library (libc / libSceLibcInternal surface) for PS5 guests. See docs/m1/libc.md.
//
// Conventions:
//  * A shim is `void shim_<name>(GuestContext *ctx)`; arguments are in ctx->rdi.. (SysV), the
//    return value goes in ctx->rax (ctx->xmm[0] for float/double, FPU stack for long double),
//    and every path ends in SHIM_RETURN().
//  * Guest pointers are translated ONLY with NPS5_G2H / NPS5_H2G so the guest memory model can
//    be migrated by changing these two macros.
//  * Names are bound by the emitter (pkg/emitter/shims.go libcShims), by plain name and by NID,
//    and only as a FALLBACK: a module export that satisfies the import wins.
#ifndef NPS5_LIBC_H
#define NPS5_LIBC_H

#include "recomp_runtime.h"
#include "nps5_thunk.h"
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// guest address <-> host pointer (0 <-> NULL): NPS5_G2H / NPS5_H2G and nps5_g2h / nps5_h2g come from
// core/nps5_thunk.h, the one definition for the whole runtime (plain casts in an identity build).

#define LC_RET(v)                    \
  do {                               \
    ctx->rax = (uint64_t)(v);        \
    SHIM_RETURN();                   \
  } while (0)
#define LC_RET_INT(v) LC_RET((uint64_t)(uint32_t)(int32_t)(v))
#define LC_RET_PTR(p) LC_RET(NPS5_H2G(ctx, (p)))

// Arguments i = 0..5 in integer registers.
static inline uint64_t lc_arg(const GuestContext *ctx, int i) {
  switch (i) {
  case 0: return ctx->rdi;
  case 1: return ctx->rsi;
  case 2: return ctx->rdx;
  case 3: return ctx->rcx;
  case 4: return ctx->r8;
  default: return ctx->r9;
  }
}

// Call a guest function (SysV) from inside a shim. Up to 4 integer args; returns RAX.
// Keeps the guest stack 16-byte aligned at the callee entry and restores every caller-saved
// register the shim may still need.
uint64_t nps5_libc_call_guest(GuestContext *ctx, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// Heap (guest memory). All return/accept guest addresses.
uint64_t nps5_libc_malloc(GuestContext *ctx, uint64_t size);
uint64_t nps5_libc_calloc(GuestContext *ctx, uint64_t n, uint64_t size);
uint64_t nps5_libc_realloc(GuestContext *ctx, uint64_t p, uint64_t size);
uint64_t nps5_libc_memalign(GuestContext *ctx, uint64_t align, uint64_t size);
void nps5_libc_free(GuestContext *ctx, uint64_t p);
uint64_t nps5_libc_usable_size(GuestContext *ctx, uint64_t p);

// printf engine. Arguments come from a source; see nps5_libc_stdio.c.
typedef struct LcArgs LcArgs;
struct LcArgs {
  GuestContext *ctx;
  uint64_t gp[6];     // captured integer argument registers (variadic call)
  double fp[8];       // captured xmm0..7 low doubles
  int gp_next, fp_next;
  uint64_t stack;     // overflow area (guest address) for register mode
  uint64_t va_addr;   // guest va_list address, or 0 when reading registers
};
// Variadic call: arguments start at integer register `first_gp`; call at shim entry.
void lc_args_from_regs(LcArgs *a, GuestContext *ctx, int first_gp);
// SysV va_list {u32 gp_offset, u32 fp_offset, u64 overflow_arg_area, u64 reg_save_area} at a guest address.
void lc_args_from_va(LcArgs *a, GuestContext *ctx, uint64_t va_list_addr);

// Format into a malloc'ed host buffer (caller frees). Returns length, or -1.
int nps5_libc_format(GuestContext *ctx, const char *fmt, LcArgs *a, char **out);
int nps5_libc_scan(GuestContext *ctx, const char *str, const char *fmt, LcArgs *a);

// Guest-visible per-thread errno (shared with __error()).
static inline void lc_errno(GuestContext *ctx, int e) { set_guest_errno(ctx, e); }

// Logged stub: reports once per name, returns `retval` (documented per stub).
void nps5_libc_stub(GuestContext *ctx, const char *name, uint64_t retval) __attribute__((noinline));

// Called once after the guest image is loaded: points data imports (__stdoutp ...) at guest
// variables. Returns true when the import was one of ours.
bool nps5_libc_bind_data_import(GuestContext *ctx, uint64_t slot_addr, const char *name);

#ifdef __cplusplus
}
#endif
#endif
