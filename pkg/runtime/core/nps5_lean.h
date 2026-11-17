// SPDX-License-Identifier: GPL-2.0-or-later
// Lean HLE calls (docs/m3/leanshim.md): leaf host functions that the guest reaches through a short
// bridge instead of the full _recomp_call_shim.
//
// A lean function has the host ABI
//     uint64_t lean_<shim>(uint64_t a0, ..., uint64_t a5, GuestContext *ctx)
// a0..a5 are the System V integer argument registers rdi, rsi, rdx, rcx, r8, r9 in that order
// (arguments the function does not use are garbage), ctx is the calling guest thread's context.
// The result is the new RAX. A function may be lean when it
//   * never enters guest code (no callback, no recomp_call_entry, no fiber switch),
//   * does not switch the context the thread runs on (TCB slot) and does not longjmp,
//   * takes only integer / pointer arguments (at most six) and returns RAX only,
//   * does not depend on the guest FPCR (RMode/FZ): the bridge leaves it as the guest set it,
//   * is an ordinary AAPCS64 function (preserves x19-x28, x29, sp, d8-d15 low halves).
// The bridge reads xmm0-7 neither before nor after: the callee must not take or return vectors.
#ifndef NPS5_LEAN_H
#define NPS5_LEAN_H

#include "recomp_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NPS5_LEAN_FN(name)                                                                              \
  uint64_t lean_##name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,     \
                       GuestContext *ctx)

typedef uint64_t (*nps5_lean_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);

// Mode of the lean bridge, read by every lean stub (asm_trampoline_flat.go):
//   0  off: every stub takes the full bridge (NPS5_LEAN=0)
//   1  lean, no hooks (default)
//   2  lean, entry/exit hooks run as in the full bridge (NPS5_PERF, NPS5_TRACE_HLE, NPS5_LEAN_HOOKS=1)
//   3  checked: hooks and the register-preservation checker (NPS5_LEAN_CHECK=1)
extern uint8_t g_nps5_lean_mode;
// Called by the code that installs a bridge hook (perf trace, HLE trace): a lean call must run it.
void nps5_lean_require_hooks(void);

// The checker (mode 3) found that lean function `fn` broke the host ABI. Never returns.
void nps5_lean_violation(GuestContext *ctx, void *fn, const char *what, uint64_t before, uint64_t after)
    __attribute__((noreturn, cold));

void nps5_lean_check_regs(GuestContext *ctx, void *fn, const uint64_t *before, const uint64_t *after,
                          uint64_t sp_expect, uint64_t fp_live, const uint64_t *entry);

#ifdef __cplusplus
}
#endif
#endif
