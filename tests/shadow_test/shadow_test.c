// Host unit test of the flat call model runtime (pkg/runtime/core/nps5_shadow.c): shadow stack
// mapping, overflow compaction through the real fault path, trimming, the RET slow path, the
// continuation table and invalidation. Built and run by pkg/runtime TestShadowStack.
// The fatal paths (no continuation, boundary crossing) are covered by tools/m1_run.sh.
#include "recomp_runtime.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t g_recomp_call_model = 1; // flat

// ---- what nps5_shadow.c needs from the rest of the runtime
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int subsys, int level, const char *fmt, ...) { (void)subsys; (void)level; (void)fmt; }
void nps5_diag_set_note(const char *fmt, ...) { (void)fmt; }
int nps5_diag_host_pc_to_guest(uint64_t host_pc, uint64_t *guest_pc, uint64_t *fn_entry, const char **fn_name) {
  (void)host_pc; (void)guest_pc; (void)fn_entry; (void)fn_name;
  return 0;
}
bool nps5_guest_safe_read(uint64_t g, void *dst, size_t len) { (void)g; memset(dst, 0, len); return true; }
bool recomp_pc_in_guest_code(uint64_t pc) { (void)pc; return false; }

static int g_fail;
#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; }  \
  } while (0)

// The push of a call entry, with the same instruction the fault handler recognises.
__attribute__((noinline)) static RecompShadowEntry *push(RecompShadowEntry *sp, uint64_t ra, uint64_t rsp, uint64_t cont) {
  __asm__ volatile("stp %1, %2, [%0, #-32]!\n"
                   "str %3, [%0, #16]\n"
                   : "+r"(sp)
                   : "r"(ra), "r"(rsp), "r"(cont)
                   : "memory");
  return sp;
}

static void on_fault(int sig, siginfo_t *si, void *uc) {
  if (recomp_shadow_handle_fault(uc, (uint64_t)(uintptr_t)si->si_addr)) return;
  static const char m[] = "FAIL: fault not handled by the shadow stack\n";
  (void)!write(1, m, sizeof m - 1);
  _exit(3);
  (void)sig;
}

// Non-shadow faults (NPS5_SHADOW_FAULT_FAST test): record what the handler answered, then open
// the page so the access retries.
static volatile int g_rc = -1;
static void on_fault_probe(int sig, siginfo_t *si, void *uc) {
    g_rc = recomp_shadow_handle_fault(uc, (uint64_t)(uintptr_t)si->si_addr);
    size_t pg = (size_t)getpagesize();
    mprotect((void *)((uintptr_t)si->si_addr & ~(uintptr_t)(pg - 1)), pg, PROT_READ | PROT_WRITE);
    (void)sig;
}

#define SP(ctx) ((RecompShadowEntry *)(uintptr_t)(ctx)->shadow_sp)

// Call-site inline cache records (docs/m3/callic.md): 2 sites x {slot0, slot1, counter}.
__attribute__((used, aligned(16), section("__DATA,__nps5_ic"))) static uint64_t g_ic_sites[2][6] = {
    {0x1000, 0x500000, ~0ull, 0, 3, 0}, {~0ull, 0, 0x2000, 0x600010, 0, 0}};

int main(void) {
  setenv("NPS5_SHADOW_ENTRIES", "64", 1);
  struct sigaction sa = {0};
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = on_fault;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  GuestContext *ctx = calloc(1, sizeof *ctx);
  recomp_shadow_attach(ctx);
  RecompShadow *s = ctx->shadow;
  CHECK(s && SP(ctx) == s->bottom);
  CHECK(s->bottom->guest_ra == RECOMP_SHADOW_RA_BOTTOM && s->bottom->guest_rsp == ~0ull);
  CHECK((size_t)(s->bottom - s->lo) >= 64);
  const size_t cap = (size_t)(s->bottom - s->lo);

  // ---- chain and trim: frames at descending guest rsp
  RecompShadowEntry *sp = s->bottom;
  for (uint64_t i = 0; i < 5; i++) sp = push(sp, 0x1000 + i, 0x9000 - 0x10 * i, 0x700000 + 0x100 * i);
  ctx->shadow_sp = (uint64_t)(uintptr_t)sp;
  uint64_t conts[16];
  size_t n = recomp_shadow_chain(ctx->shadow_sp, 0, conts, 16);
  CHECK(n == 5 && conts[0] == 0x700400 && conts[4] == 0x700000);
  // the guest popped the two innermost frames without returning through them
  n = recomp_shadow_chain(ctx->shadow_sp, 0x8fe0, conts, 16);
  CHECK(n == 3 && conts[0] == 0x700200);
  CHECK(recomp_shadow_chain(0x1234, 0, conts, 16) == 0); // not a shadow stack address
  RecompShadowEntry *last = recomp_shadow_trim(ctx, 0x8fe0);
  CHECK(last && last->guest_ra == 0x1003 && SP(ctx) == s->bottom - 3);

  // ---- RET slow path: the cache is misaligned (two stale entries on top of the real one)
  sp = SP(ctx);
  sp = push(sp, 0x2222, 0x8f00, 0x711000); // real entry of the call being returned from
  sp = push(sp, 0x3333, 0x8e00, 0x722000); // stale
  sp = push(sp, 0x4444, 0x8d00, 0x733000); // stale
  ctx->shadow_sp = (uint64_t)(uintptr_t)sp;
  ctx->rsp = 0x8f08; // after the RET that popped 0x2222 from slot 0x8f00
  CHECK(recomp_flat_continue(ctx, 0x2222, RECOMP_MISS_RET) == 0x711000);
  CHECK(SP(ctx) == s->bottom - 3);

  // ---- RET slow path: by address, from the continuation table and the dispatch table
  recomp_cont_register(0x5555, 0x744000);
  ctx->rsp = 0x8f10;
  CHECK(recomp_flat_continue(ctx, 0x5555, RECOMP_MISS_RET) == 0x744000);
  CHECK(recomp_cont_lookup(0x5555) == 0x744000 && recomp_cont_table_lookup(0x6666) == 0);
  recomp_cont_register(0x5555, 0x745000); // replaced
  CHECK(recomp_cont_lookup(0x5555) == 0x745000);
  static recomp_fn_t l3[DISPATCH_LEVEL_SIZE];
  static recomp_fn_t *l2[DISPATCH_LEVEL_SIZE];
  l3[0x7777] = (recomp_fn_t)(uintptr_t)0x750000;
  l2[0x40] = l3;
  g_dispatch_l1[0] = l2;
  CHECK(recomp_cont_lookup(0x407777) == 0x750000 + RECOMP_FLAT_JUMP_ENTRY_OFFSET);
  CHECK(recomp_flat_continue(ctx, 0x407777, RECOMP_MISS_JUMP) == 0x750000 + RECOMP_FLAT_JUMP_ENTRY_OFFSET);
  CHECK(recomp_cont_resolve(ctx, 0x408888, RECOMP_MISS_CALL) == 0); // weak recomp_dispatch_miss

  // ---- return to a host boundary, with the shadow stack discarded
  RecompBoundary b = {0};
  b.guest_rsp = 0x8f80;
  b.guest_ra = 0xdeadbeef;
  ctx->host_boundary = (uint64_t)(uintptr_t)&b;
  ctx->shadow_sp = (uint64_t)(uintptr_t)s->bottom;
  ctx->rsp = 0x8f88;
  CHECK(recomp_flat_continue(ctx, 0xdeadbeef, RECOMP_MISS_RET) == recomp_shadow_boundary_cont());
  ctx->host_boundary = 0;

  // ---- overflow: pushes run into the guard page; the newest half survives each compaction
  sp = s->bottom;
  uint64_t epoch0 = atomic_load(&s->epoch);
  // (the capacity is page-granular: 64 requested entries are one 16 KiB page of them)
  const uint64_t N = 20000;
  for (uint64_t i = 1; i <= N; i++) sp = push(sp, 0x10000 + i, 0x1000000 - 8 * i, 0x800000 + i);
  CHECK(atomic_load(&s->epoch) > epoch0 && atomic_load(&s->overflows) >= 10);
  CHECK(sp >= s->lo && sp < s->bottom && (size_t)(s->bottom - sp) <= cap);
  uint64_t want = N;
  for (RecompShadowEntry *e = sp; e < s->bottom; e++, want--) {
    if (e->guest_ra != 0x10000 + want || e->guest_rsp != 0x1000000 - 8 * want || e->host_cont != 0x800000 + want) {
      CHECK(!"entries are not the newest ones in order");
      break;
    }
  }
  CHECK((size_t)(s->bottom - sp) >= cap / 2); // at least half a stack of history is kept
  CHECK(s->bottom->guest_ra == RECOMP_SHADOW_RA_BOTTOM);
  ctx->shadow_sp = (uint64_t)(uintptr_t)sp;

  // ---- invalidation: entries and registered continuations that point into a code range
  uint64_t gen = recomp_code_generation();
  uint64_t top_cont = sp->host_cont;
  recomp_cont_register(0x9999, 0x800000 + N - 10);
  recomp_shadow_invalidate(0x800000 + N - 10, 0x800000 + N - 4);
  CHECK(recomp_code_generation() == gen + 1);
  int poisoned = 0, kept = 0;
  for (RecompShadowEntry *e = sp; e < s->bottom; e++) {
    int in = e->host_cont >= 0x800000 + N - 10 && e->host_cont < 0x800000 + N - 4;
    if (in && e->guest_ra == RECOMP_SHADOW_RA_POISON) poisoned++;
    if (!in && e->guest_ra != RECOMP_SHADOW_RA_POISON) kept++;
  }
  CHECK(poisoned == 6 && kept == (int)(s->bottom - sp) - 6 && sp->host_cont == top_cont);
  CHECK(recomp_cont_table_lookup(0x9999) == 0 && recomp_cont_lookup(0x5555) == 0x745000);
  // a poisoned entry is never used as a continuation: the return goes by address
  RecompShadowEntry *p = sp;
  while (p->guest_ra != RECOMP_SHADOW_RA_POISON) p++;
  recomp_cont_register(0x10000 + (p->host_cont - 0x800000), 0x900000);
  ctx->shadow_sp = (uint64_t)(uintptr_t)p;
  ctx->rsp = p->guest_rsp + 8;
  CHECK(recomp_flat_continue(ctx, 0x10000 + (p->host_cont - 0x800000), RECOMP_MISS_RET) == 0x900000);
  CHECK(SP(ctx) == p + 1);

  RecompShadowStats st;
  recomp_shadow_stats(&st);
  CHECK(st.stacks == 1 && st.slow_rets == 4 && st.slow_hits == 1 && st.slow_boundary == 1 && st.slow_by_address == 2 && st.overflows >= 10);

  // ---- a second context has its own stack; release unmaps
  GuestContext *c2 = calloc(1, sizeof *c2);
  recomp_shadow_attach(c2);
  CHECK(c2->shadow && c2->shadow != s && c2->shadow->bottom != s->bottom);
  uint64_t c2sp = c2->shadow_sp;
  recomp_shadow_release(c2);
  CHECK(c2->shadow == NULL && c2->shadow_sp == 0);
  CHECK(recomp_shadow_chain(c2sp, 0, conts, 16) == 0); // no longer a registered stack
  recomp_shadow_release(ctx);
  recomp_shadow_release(ctx); // idempotent

  // ---- call-site inline caches: range flush, full flush, epoch
  g_recomp_ic[2] = 0xabc000; // what the trampoline holds: _recomp_indirect_call
  uint64_t ep = g_recomp_ic[0];
  recomp_ic_flush_range(0x500000, 0x500008);
  CHECK(g_recomp_ic[0] == ep + 1);
  CHECK(g_ic_sites[0][0] == ~0ull && g_ic_sites[0][1] == g_recomp_ic[2] && g_ic_sites[0][4] == 3);
  CHECK(g_ic_sites[1][2] == 0x2000 && g_ic_sites[1][3] == 0x600010);
  recomp_shadow_invalidate(0x600000, 0x600100); // unloading code clears the caches that name it
  CHECK(g_recomp_ic[0] == ep + 2 && g_ic_sites[1][2] == ~0ull && g_ic_sites[1][3] == g_recomp_ic[2]);
  g_ic_sites[0][2] = 0x3000; g_ic_sites[0][3] = 0x700000;
  recomp_ic_flush_all();
  CHECK(g_recomp_ic[0] == ep + 3 && g_ic_sites[0][2] == ~0ull && g_ic_sites[0][3] == g_recomp_ic[2]);

  // ---- NPS5_SHADOW_FAULT_FAST: same answers in both orders; the scan is skipped for non-pushes
  {
      struct sigaction pa = {0};
      pa.sa_flags = SA_SIGINFO;
      pa.sa_sigaction = on_fault_probe;
      sigaction(SIGSEGV, &pa, NULL);
      sigaction(SIGBUS, &pa, NULL);
      size_t pg = (size_t)getpagesize();
      for (int fast = 0; fast <= 1; fast++) {
          recomp_shadow_test_set_fault_fast(fast);
          GuestContext *c3 = calloc(1, sizeof *c3);
          recomp_shadow_attach(c3);
          // a fault at an unrelated address, instruction is a load: not ours
          uint8_t *bad = mmap(NULL, pg, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
          uint64_t s0 = recomp_shadow_fault_scans();
          g_rc = -1;
          (void)*(volatile uint64_t *)bad;
          CHECK(g_rc == 0);
          CHECK(recomp_shadow_fault_scans() - s0 == (fast ? 0u : 1u)); // fast: no registry scan
          // a load from the shadow stack's own guard page: ours by address, not by instruction
          s0 = recomp_shadow_fault_scans();
          g_rc = -1;
          (void)*(volatile uint64_t *)(uintptr_t)(c3->shadow->map + 8);
          CHECK(g_rc == 0);
          mprotect(c3->shadow->map, pg, PROT_NONE); // the probe opened the guard: close it again
          CHECK(recomp_shadow_fault_scans() - s0 == (fast ? 0u : 1u));
          // the push into the guard page is still compacted (returns 1 through the real handler)
          sigaction(SIGSEGV, &sa, NULL);
          sigaction(SIGBUS, &sa, NULL);
          uint64_t ov0 = atomic_load(&c3->shadow->overflows);
          RecompShadowEntry *q = c3->shadow->bottom;
          for (uint64_t i = 1; i <= 5000; i++)
              q = push(q, 0x20000 + i, 0x2000000 - 8 * i, 0x900000 + i);
          CHECK(atomic_load(&c3->shadow->overflows) > ov0);
          sigaction(SIGSEGV, &pa, NULL);
          sigaction(SIGBUS, &pa, NULL);
          recomp_shadow_release(c3);
          munmap(bad, pg);
      }
      sigaction(SIGSEGV, &sa, NULL);
      sigaction(SIGBUS, &sa, NULL);
  }

  if (g_fail) return 1;
  printf("SHADOW OK\n");
  return 0;
}
