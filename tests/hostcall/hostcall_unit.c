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
#define GUEST_FN14 0x42c0e40ULL // the n-argument guest function (nps5_hostcall_run_n)
#define GUEST_RAM 0x200000000ULL    // guest address of the test's 1 MiB of guest memory

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// What nps5_hostcall.c and the inline dispatcher need from the runtime.
recomp_fn_t **g_dispatch_l1[1 << DISPATCH_LEVEL_BITS];
uint64_t g_recomp_call_model;
bool (*g_nps5_hostcall_fault_hook)(void *ucontext, uint64_t fault_addr);
uintptr_t recomp_dispatch_miss(GuestContext *ctx, uint64_t rip, uint32_t reason) { (void)ctx; (void)rip; (void)reason; return 0; }
bool recomp_pc_in_guest_code(uint64_t pc) { (void)pc; return false; }
_Thread_local GuestContext *g_current_ctx;

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

// The n-argument guest function: records the six register arguments, the stack arguments at [rsp + 8...] and the
// current context, returns the sum of all fourteen.
static uint64_t seen14[14], seen14_rsp;
static GuestContext *seen14_cur;
static void guest_fn14(GuestContext *ctx) {
    seen14[0] = ctx->rdi;
    seen14[1] = ctx->rsi;
    seen14[2] = ctx->rdx;
    seen14[3] = ctx->rcx;
    seen14[4] = ctx->r8;
    seen14[5] = ctx->r9;
    for (int i = 0; i < 8; i++)
        seen14[6 + i] = *(uint64_t *)(ctx->mem_base + ctx->rsp + 8 + 8 * i);
    seen14_rsp = ctx->rsp;
    seen14_cur = g_current_ctx;
    uint64_t sum = 0;
    for (int i = 0; i < 14; i++)
        sum += seen14[i];
    ctx->rax = sum;
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

  l3[GUEST_FN14 & DISPATCH_LEVEL_MASK] = guest_fn14;

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

  // 4. nps5_hostcall_run_n: a host-owned context, no bracket; 14 arguments (6 in registers, 8 on the stack as after a CALL).
  {
      static GuestContext H;
      H.mem_base = arena;
      H.mem_size = C.mem_size;
      H.rsp = GUEST_RAM + 0xC0000;
      H.rdi = 0x5555;
      H.rax = 0x6666;
      const uint64_t a[14] = {GUEST_RAM + 0x10, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
      const uint64_t before = nps5_hostcall_count();
      g_current_ctx = &C;
      const uint64_t r = nps5_hostcall_run_n(&H, GUEST_FN14, a, 14);
      uint64_t want = 0;
      for (int i = 0; i < 14; i++) {
          want += a[i];
          CHECK(seen14[i] == a[i]);
      }
      CHECK(r == want);
      CHECK((seen14_rsp & 15) == 8 && seen14_rsp < GUEST_RAM + 0xC0000 - 128 - 64);
      CHECK(seen14_cur == &H && g_current_ctx == &C); // current context swapped for the call only
      CHECK(H.rsp == GUEST_RAM + 0xC0000 && H.rdi == 0x5555 && H.rax == 0x6666); // context state restored
      CHECK(nps5_hostcall_count() == before + 1);
      CHECK(nps5_hostcall_ctx() == NULL); // no bracket opened
      // fewer arguments: unused registers are zero, nothing on the stack is read as an argument
      const uint64_t b[3] = {1, 2, 3};
      memset(seen14, 0xff, sizeof seen14);
      CHECK(nps5_hostcall_run_n(&H, GUEST_FN14, b, 3) ==
            6 + seen14[6] + seen14[7] + seen14[8] + seen14[9] + seen14[10] + seen14[11] + seen14[12] + seen14[13]);
      CHECK(seen14[0] == 1 && seen14[2] == 3 && seen14[3] == 0 && seen14[5] == 0);
      CHECK(nps5_hostcall_run_n(NULL, GUEST_FN14, a, 14) == ~0ull);
      CHECK(nps5_hostcall_run_n(&H, GUEST_FN14, a, 17) == ~0ull);
      CHECK(nps5_hostcall_run_n(&H, GUEST_FN14, NULL, 2) == ~0ull);
  }

  printf(fails ? "hostcall_unit: %d FAILURES\n" : "hostcall_unit: all passed\n", fails);
  return fails ? 1 : 0;
}
