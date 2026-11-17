// SPDX-License-Identifier: GPL-2.0-or-later
// Host unit test of nps5_hostcall.c: host code that calls a guest function pointer (the way AnyPS5's
// Agc calls a CommandBuffer's out-of-space callback) reaches the guest function through the fault
// bridge, with host arena pointers turned back into guest addresses, and gets its return value;
// outside a bracket the same branch stays a fault. Native arm64 only, no guest code: the "guest
// function" is a C function registered in the dispatch table at a guest address.
#include "nps5_diag.h"
#include "nps5_hostcall.h"
#include "recomp_context.h"
#include "recomp_runtime.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define GUEST_FN 0x42c0dd0ULL       // a guest code address: unmapped in the host (below 4 GiB)
#define GUEST_RAM 0x200000000ULL    // guest address of the test's 1 MiB of guest memory

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// What nps5_hostcall.c and the inline dispatcher need from the runtime.
recomp_fn_t **g_dispatch_l1[1 << DISPATCH_LEVEL_BITS];
uint64_t g_recomp_call_model;
bool (*g_nps5_hostcall_fault_hook)(void *ucontext, uint64_t fault_addr);
uintptr_t recomp_dispatch_miss(GuestContext *ctx, uint64_t rip, uint32_t reason) { (void)ctx; (void)rip; (void)reason; return 0; }
bool recomp_pc_in_guest_code(uint64_t pc) { (void)pc; return false; }

static GuestContext C;
static uint64_t seen[6], seen_rsp, nested;
typedef uint64_t (*cb6_t)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// The guest callback: (buffer, count, user_data, ...) -> count + 1. Guest RET pops the return address.
static void guest_callback(GuestContext *ctx) {
  seen[0] = ctx->rdi; seen[1] = ctx->rsi; seen[2] = ctx->rdx; seen[3] = ctx->rcx; seen[4] = ctx->r8; seen[5] = ctx->r9;
  seen_rsp = ctx->rsp;
  if (nested) { // the callback calls a shim of the same library again
    nested = 0;
    nps5_hostcall_enter(ctx);
    nps5_hostcall_leave(ctx);
  }
  *(uint32_t *)(ctx->mem_base + ctx->rdi) = 0xc0ffee; // writes through the guest address it was given
  ctx->rax = ctx->rsi + 1;
  ctx->rsp += 8;
}

static sigjmp_buf jb;
static void on_fault(int sig, siginfo_t *si, void *uc) {
  if (g_nps5_hostcall_fault_hook && g_nps5_hostcall_fault_hook(uc, (uint64_t)(uintptr_t)si->si_addr)) return;
  siglongjmp(jb, sig);
}

int main(void) {
  // An arena as the runtime's: guest address g is at arena + g; only 1 MiB at GUEST_RAM is backed.
  uint8_t *arena = mmap(NULL, GUEST_RAM + (1 << 20), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (arena == MAP_FAILED) { perror("mmap"); return 2; }
  uint8_t *ram = arena + GUEST_RAM;
  if (mprotect(ram, 1 << 20, PROT_READ | PROT_WRITE) != 0) { perror("mprotect"); return 2; }
  C.mem_base = arena;
  C.mem_size = GUEST_RAM + (1 << 20);
  C.rsp = GUEST_RAM + 0x80000 + 8; // as at shim entry: the return address is on top
  C.rax = 0x1111; C.rdi = 0x2222; C.rsi = 0x3333;

  static recomp_fn_t l3[1 << DISPATCH_LEVEL_BITS];
  static recomp_fn_t *l2[1 << DISPATCH_LEVEL_BITS];
  l3[GUEST_FN & DISPATCH_LEVEL_MASK] = guest_callback;
  l2[(GUEST_FN >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK] = l3;
  g_dispatch_l1[(GUEST_FN >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK] = l2;

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  volatile cb6_t cb = (cb6_t)(uintptr_t)GUEST_FN; // what the library read from the guest structure

  // 1. Not inside a bracket: a wild branch, still a fault.
  if (sigsetjmp(jb, 1) == 0) { cb(ram, 1, 2, 3, 4, 5); CHECK(!"unbracketed call returned"); }
  CHECK(nps5_hostcall_count() == 0);

  // 2. Bracketed: the guest function runs, pointer arguments arrive as guest addresses.
  if (sigsetjmp(jb, 1) == 0) {
    nps5_hostcall_enter(&C);
    nested = 1;
    uint64_t r = cb(ram + 0x100, 41, 0x7000, (uint64_t)(uintptr_t)(ram + 0x200), 5, 6);
    uint64_t r2 = cb(ram + 0x100, 1, 0, 0, 0, 0); // and again after a nested bracket inside the callback
    nps5_hostcall_leave(&C);
    CHECK(r == 42 && r2 == 2);
    CHECK(seen[0] == GUEST_RAM + 0x100 && seen[1] == 1 && seen[2] == 0);
    CHECK(*(uint32_t *)(ram + 0x100) == 0xc0ffee);
    CHECK((seen_rsp & 15) == 8 && seen_rsp < GUEST_RAM + 0x80000 - 128 && seen_rsp >= GUEST_RAM);
    CHECK(C.rsp == GUEST_RAM + 0x80000 + 8 && C.rax == 0x1111 && C.rdi == 0x2222 && C.rsi == 0x3333); // caller state restored
    CHECK(nps5_hostcall_count() == 2);
  } else {
    CHECK(!"bracketed call faulted");
  }

  // 3. After the bracket closed: a fault again.
  if (sigsetjmp(jb, 1) == 0) { cb(ram, 1, 2, 3, 4, 5); CHECK(!"call after leave returned"); }

  printf(fails ? "hostcall_unit: %d FAILURES\n" : "hostcall_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
