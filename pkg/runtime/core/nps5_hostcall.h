// SPDX-License-Identifier: GPL-2.0-or-later
// Host libraries that call guest function pointers (see nps5_hostcall.c).
#ifndef NPS5_HOSTCALL_H
#define NPS5_HOSTCALL_H

#include "recomp_context.h"
#include <stdbool.h>
#include <stdint.h>

// Brackets a call from a shim into a host library that may call back into the guest through a
// function pointer it reads from guest memory. Nestable; ctx is the calling guest thread's context.
void nps5_hostcall_enter(GuestContext *ctx);
void nps5_hostcall_leave(GuestContext *ctx);

// SIGSEGV/SIGBUS hook (async-signal context). True when the fault was the host library branching
// to a guest code address inside such a bracket: the thread resumes in the bridge that runs the
// guest function, and returns to the library with its result.
bool nps5_hostcall_handle_fault(void *ucontext, uint64_t fault_addr);

// The guest context of the calling thread while it is inside such a bracket, else NULL.
GuestContext *nps5_hostcall_ctx(void);
// Runs the guest function `target` on the calling thread's bracketed context with six System V integer arguments and
// returns its RAX (what the fault bridge does for a library that branches to a guest address). Only inside a bracket.
uint64_t nps5_hostcall_run(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t target);

// Runs the guest function `target` on `ctx` (any guest context, e.g. one made by nps5_hostctx_create) from the calling host
// thread, with n <= 16 System V integer arguments: args[0..5] in rdi, rsi, rdx, rcx, r8, r9, args[6..] on the guest stack as
// after a CALL. Arguments are passed as given (guest values). Returns rax, or ~0 when ctx is null or n > 16. The calling thread's
// current-context pointer is ctx for the duration of the call. Not a bracketed call: no t_ctx is needed.
uint64_t nps5_hostcall_run_n(GuestContext *ctx, uint64_t target, const uint64_t *args, unsigned n);

// Calls of guest functions made this way (diagnostics).
uint64_t nps5_hostcall_count(void);

#endif
