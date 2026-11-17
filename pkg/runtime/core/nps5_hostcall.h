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

// Calls of guest functions made this way (diagnostics).
uint64_t nps5_hostcall_count(void);

#endif
