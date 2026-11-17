// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host code that calls a guest function pointer.
//
// A host library linked into the runtime (AnyPS5's libSceAgc is the case in point) implements a
// console API natively. Some of those APIs call the title back through a pointer stored in a
// guest structure: Agc's CommandBuffer carries an out-of-space callback that DCB/ACB packet
// writers invoke when the buffer is full. The library was written to run next to x86 code, so it
// does `buffer->callback(buffer, count, user_data)`: a branch to the guest address. Here that
// address is not code (guest addresses lie in the 4 GiB the host leaves unmapped), the branch
// faults on the instruction fetch, and the title died on its first full command buffer.
//
// The fetch fault is precise and nothing has run yet: pc = the guest address, x0-x7 = the
// arguments, x30 = the return address in the library. The handler moves the pc to a bridge and
// passes the target in x16; the bridge runs the guest function on the guest stack of the calling
// thread's context (which the shim bridge synced before it called the library) and returns rax
// to the library like the function it meant to call. Arguments that are host pointers into the
// guest arena (the library translates the structures it was given) are turned back into guest
// addresses; anything else passes through as a value.
//
// Only inside nps5_hostcall_enter/leave, which the thunks of such libraries place around their
// calls: a wild branch anywhere else is still a crash with a report.

#define _DARWIN_C_SOURCE 1
#include <signal.h>
#include <stdatomic.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "nps5_hostcall.h"
#include "nps5_diag.h"
#include "nps5_thunk.h"
#include "recomp_runtime.h"

static _Thread_local GuestContext *t_ctx;
static _Thread_local int t_depth;
static _Atomic uint64_t g_calls;

void nps5_hostcall_enter(GuestContext *ctx) {
  if (!g_nps5_hostcall_fault_hook) g_nps5_hostcall_fault_hook = nps5_hostcall_handle_fault; // idempotent store
  t_ctx = ctx;
  t_depth++;
}

void nps5_hostcall_leave(GuestContext *ctx) {
  (void)ctx;
  if (t_depth > 0) t_depth--;
}

uint64_t nps5_hostcall_count(void) { return atomic_load(&g_calls); }

GuestContext *nps5_hostcall_ctx(void) { return t_depth > 0 ? t_ctx : NULL; }

static uint64_t to_guest(const GuestContext *ctx, uint64_t v) {
  const uint64_t base = (uint64_t)(uintptr_t)ctx->mem_base;
  return (base && v - base < ctx->mem_size) ? v - base : v; // unsigned: also false for v < base
}

// The six System V integer arguments and the target, from the bridge below.
uint64_t nps5_hostcall_run(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t target) {
  GuestContext *ctx = t_ctx;
  atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed);
  const uint64_t s_rax = ctx->rax, s_rcx = ctx->rcx, s_rdx = ctx->rdx, s_rsi = ctx->rsi, s_rdi = ctx->rdi;
  const uint64_t s_r8 = ctx->r8, s_r9 = ctx->r9, s_r10 = ctx->r10, s_r11 = ctx->r11, s_rsp = ctx->rsp, s_rip = ctx->rip;
  const int s_depth = t_depth;
  ctx->rdi = to_guest(ctx, a0); ctx->rsi = to_guest(ctx, a1); ctx->rdx = to_guest(ctx, a2);
  ctx->rcx = to_guest(ctx, a3); ctx->r8 = to_guest(ctx, a4); ctx->r9 = to_guest(ctx, a5);
  // At shim entry rsp points at the return address. Stay clear of the red zone, align, and push a
  // null return address: the callee sees rsp % 16 == 8 as after a CALL.
  uint64_t sp = ((s_rsp - 128) & ~15ULL) - 8;
  *(uint64_t *)NPS5_G2H(ctx, sp) = 0;
  ctx->rsp = sp;
  recomp_dispatch(ctx, target);
  const uint64_t ret = ctx->rax;
  ctx->rsp = s_rsp; ctx->rip = s_rip;
  ctx->rdi = s_rdi; ctx->rsi = s_rsi; ctx->rdx = s_rdx; ctx->rcx = s_rcx;
  ctx->r8 = s_r8; ctx->r9 = s_r9; ctx->r10 = s_r10; ctx->r11 = s_r11; ctx->rax = s_rax;
  t_ctx = ctx; // the callee may have called shims that bracket again
  t_depth = s_depth;
  return ret;
}

#if defined(__aarch64__) && defined(__APPLE__)
// x0-x5 = arguments, x16 = guest target (from the fault handler), x30 = the library's return address.
__asm__(".text\n.p2align 2\n.globl _nps5_hostcall_bridge\n_nps5_hostcall_bridge:\n    mov x6, x16\n    b _nps5_hostcall_run\n");
extern void nps5_hostcall_bridge(void);

bool nps5_hostcall_handle_fault(void *ucontext, uint64_t fault_addr) {
  GuestContext *ctx = t_ctx;
  if (!ctx || t_depth <= 0 || !ucontext) return false;
  ucontext_t *uc = (ucontext_t *)ucontext;
  arm_thread_state64_t *ss = &uc->uc_mcontext->__ss;
  const uint64_t pc = arm_thread_state64_get_pc(*ss);
  const uint32_t ec = (uint32_t)(uc->uc_mcontext->__es.__esr >> 26) & 0x3f;
  if (ec != 0x20 && ec != 0x21) return false;       // not an instruction abort
  if (pc != fault_addr || pc < 0x10000 || pc >= ctx->mem_size) return false; // not a guest address
  if (recomp_pc_in_guest_code(pc)) return false;    // host pc inside translated code: a real fault
  ss->__x[16] = pc;
  arm_thread_state64_set_pc_fptr(*ss, nps5_hostcall_bridge);
  return true;
}
#else
bool nps5_hostcall_handle_fault(void *ucontext, uint64_t fault_addr) {
  (void)ucontext; (void)fault_addr;
  return false;
}
#endif
