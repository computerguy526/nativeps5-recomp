// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adversarial tests of guest<->host pointer translation (docs/m1/memory.md section 7,
// pkg/runtime/core/nps5_thunk.h): NULL is never biased, ends of the area round-trip, an
// out-of-range guest pointer must fail loudly rather than silently yield a host address outside
// the reservation, and identity mode translates by cast. Out-of-range cases run in a forked child
// so that an abort is an observable outcome.
#include "vmh.h"

#define AREA "xlate"
#define N(s) "xlate." VMH_MODE "." s

typedef struct { int kind; uint64_t in; } Probe;
// g2h of a guest value must (a) return NULL, (b) die, or (c) return a host pointer inside the reservation.
static void check_g2h(uint64_t g, const char *what) {
  Probe p = {0, g};
  int fd[2]; (void)pipe(fd);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) { close(fd[0]); alarm(5); uint64_t v = (uint64_t)(uintptr_t)NPS5_G2H(ctx, p.in); (void)!write(fd[1], &v, 8); _exit(0); }
  close(fd[1]);
  uint64_t v = 0; ssize_t n = read(fd[0], &v, 8); close(fd[0]);
  int st = 0; waitpid(pid, &st, 0);
  if (n != 8) return;  // died before answering: loud
  int inside = v >= g_nps5_arena.host_lo && v - g_nps5_arena.host_lo < g_nps5_arena.host_len;
  MT_EXPECT(v == 0 || inside, "nps5_g2h of an out-of-range guest pointer silently yields a host address outside the reservation (no check, no abort)" " [" "%s: guest 0x%llx -> host 0x%llx" "]", what, (unsigned long long)g, (unsigned long long)v);
}
static void check_h2g(uint64_t h, const char *what) {
  int fd[2]; (void)pipe(fd);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) { close(fd[0]); alarm(5); uint64_t v = NPS5_H2G(ctx, (const void *)(uintptr_t)h); (void)!write(fd[1], &v, 8); _exit(0); }
  close(fd[1]);
  uint64_t v = 0; ssize_t n = read(fd[0], &v, 8); close(fd[0]);
  int st = 0; waitpid(pid, &st, 0);
  if (n != 8) return;
  int ok = v == 0 || v == ~0ULL || v < g_nps5_arena.guest_hi;  // a plausible guest value for a non-guest host pointer is the bug
  MT_EXPECT(!(v < g_nps5_arena.guest_hi && v != 0), "nps5_h2g of a host pointer outside the reservation returns a plausible guest value instead of failing" " [" "%s: host 0x%llx -> guest 0x%llx" "]", what, (unsigned long long)h, (unsigned long long)v);
  (void)ok;
}

// Test doubles of the AnyPS5 AGC driver's configuration entry points (AgcGuestMemory.h). A runtime
// built with -DNPS5_HAVE_ANYPS5_AGC=1 calls them once from recomp_init_runtime.
static struct { int bias_calls, arena_calls, scratch_calls; int64_t bias; uint64_t alo, ahi, slo, sbytes; } agc;
void AgcSetGuestBias(int64_t b) { agc.bias_calls++; agc.bias = b; }
void AgcSetGuestArena(uint64_t lo, uint64_t hi) { agc.arena_calls++; agc.alo = lo; agc.ahi = hi; }
void AgcSetGuestScratch(uint64_t lo, uint64_t n) { agc.scratch_calls++; agc.slo = lo; agc.sbytes = n; }

int main(void) {
  vmh_init();
  mt_begin(N("agc_driver_gets_bias_arena_and_scratch"), AREA);
#if NPS5_ADDR_MODE_IDENTITY
  // identity: bias 0, the driver's scratch memory is ordinary heap memory: no window is reserved
  MT_EXPECT(agc.bias_calls == 1 && agc.arena_calls == 1 && agc.scratch_calls == 0, "calls: bias %d arena %d scratch %d", agc.bias_calls, agc.arena_calls, agc.scratch_calls);
  MT_EXPECT(agc.bias == 0, "identity bias %lld", (long long)agc.bias);
  MT_EXPECT(agc.alo == g_nps5_arena.guest_lo && agc.ahi == g_nps5_arena.guest_hi, "arena [0x%llx, 0x%llx)", (unsigned long long)agc.alo, (unsigned long long)agc.ahi);
  mt_end();
#else
  MT_EXPECT(agc.bias_calls == 1 && agc.arena_calls == 1 && agc.scratch_calls == 1, "calls: bias %d arena %d scratch %d", agc.bias_calls, agc.arena_calls, agc.scratch_calls);
  MT_EXPECT(agc.bias == (int64_t)(uintptr_t)g_nps5_arena.bias, "bias 0x%llx, arena bias 0x%llx", (unsigned long long)agc.bias, (unsigned long long)(uintptr_t)g_nps5_arena.bias);
  MT_EXPECT(agc.alo == g_nps5_arena.guest_lo && agc.ahi == g_nps5_arena.guest_hi, "arena [0x%llx, 0x%llx)", (unsigned long long)agc.alo, (unsigned long long)agc.ahi);
  MT_EXPECT(agc.slo >= g_nps5_arena.guest_lo && agc.sbytes > 0 && agc.slo + agc.sbytes <= g_nps5_arena.guest_hi, "scratch 0x%llx+0x%llx outside the window", (unsigned long long)agc.slo, (unsigned long long)agc.sbytes);
  // the scratch window is address space the driver maps pages into: inaccessible, and taken in the
  // extent list so a guest mapping can never be placed there (neither unhinted nor fixed-excl)
  MT_EXPECT(rd_faults(agc.slo) && rd_faults(agc.slo + agc.sbytes - 1), "scratch window is accessible before the driver maps it");
  MT_EXPECT(is_mapped_extent(agc.slo) && is_mapped_extent(agc.slo + agc.sbytes - 1), "scratch window is free in the extent list");
  uint64_t rr = 0;
  MT_EXPECT(sys_mmap(agc.slo, 0x10000, PROT_RW, MAPF | F_FIXED | F_EXCL, &rr) != 0, "a guest EXCL mapping was placed in the driver's scratch window");
  mt_end();
#endif
  const Nps5Arena *a = &g_nps5_arena;
  mt_begin(N("null_is_not_biased"), AREA);
  MT_EXPECT(NPS5_G2H(ctx, 0) == NULL, "G2H(0) = %p", NPS5_G2H(ctx, 0));
  MT_EXPECT(NPS5_H2G(ctx, NULL) == 0, "H2G(NULL) = 0x%llx", (unsigned long long)NPS5_H2G(ctx, NULL));
  MT_EXPECT(nps5_g2h(ctx, 0) == NULL && nps5_h2g(ctx, NULL) == 0, "function forms");
#if !NPS5_ADDR_MODE_IDENTITY
  MT_EXPECT(NPS5_G2H(ctx, 1) == (void *)(a->host_lo + 1), "G2H(1) must be bias+1, got %p", NPS5_G2H(ctx, 1));
  MT_EXPECT(NPS5_G2H(ctx, 1) != NULL, "G2H(1) is NULL");
  MT_EXPECT(NPS5_H2G(ctx, (void *)(uintptr_t)(a->host_lo)) == 0, "host bias maps to guest 0");
#endif
  mt_end();

  mt_begin(N("round_trip_across_the_area"), AREA);
  uint64_t gs[] = {G(1), nps5_mem_config.guest_lo, G(0x200000000ULL), G(0x3000200000ULL), ORG + 0x7000000000ULL, nps5_mem_config.guest_hi - 8, nps5_mem_config.guest_hi - 1};
  for (size_t i = 0; i < sizeof gs / sizeof *gs; i++) {
    void *h = NPS5_G2H(ctx, gs[i]);
    MT_EXPECT(h == (void *)(uintptr_t)(a->host_lo + (gs[i] - a->guest_org)), "G2H(0x%llx) = %p", (unsigned long long)gs[i], h);
    MT_EXPECT(NPS5_H2G(ctx, h) == gs[i], "round trip of 0x%llx gives 0x%llx", (unsigned long long)gs[i], (unsigned long long)NPS5_H2G(ctx, h));
  }
  // last byte of the area really is the last accessible host byte once mapped
  uint64_t r;
  MT_EXPECT(sys_mmap(nps5_mem_config.guest_hi - PAGE, PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0, "map last page");
  *(volatile uint8_t *)NPS5_G2H(ctx, nps5_mem_config.guest_hi - 1) = 7;
  MT_EXPECT(*(uint8_t *)(a->host_lo + a->guest_hi - a->guest_org - 1) == 7, "last guest byte does not alias the last host byte of the area");
  mt_end();

#if !NPS5_ADDR_MODE_IDENTITY
  mt_begin(N("out_of_range_guest_pointer_fails_loudly"), AREA);
  uint64_t bad[] = {a->guest_hi, a->guest_hi + 1, NPS5_BASE_SPAN - 1, NPS5_BASE_SPAN, NPS5_BASE_SPAN + 0x1000, 1ULL << 47, 1ULL << 63, ~0ULL, ~0ULL - 7, a->host_lo + 0x200000000ULL /* a host pointer used as a guest one */};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) check_g2h(bad[i], "out of range");
  mt_end();
  mt_begin(N("out_of_range_host_pointer_fails_loudly"), AREA);
  int local = 0;
  check_h2g((uint64_t)(uintptr_t)&local, "host stack");
  void *hp = malloc(64); check_h2g((uint64_t)(uintptr_t)hp, "host heap"); free(hp);
  check_h2g(0x10, "host null page");
  check_h2g(a->host_lo + a->host_len, "one past the reservation");
  check_h2g(a->host_lo - 8, "just below the reservation");
  mt_end();
#else
  mt_begin(N("identity_translation_is_a_cast"), AREA);
  uint64_t vals[] = {1, 0x1234, ORG, ORG + 0x400000, 0xFC00000000ULL, ~0ULL, 1ULL << 63};
  for (size_t i = 0; i < sizeof vals / sizeof *vals; i++) {
    MT_EXPECT((uint64_t)(uintptr_t)NPS5_G2H(ctx, vals[i]) == vals[i], "G2H(0x%llx) not identity", (unsigned long long)vals[i]);
    MT_EXPECT(NPS5_H2G(ctx, (void *)(uintptr_t)vals[i]) == vals[i], "H2G(0x%llx) not identity", (unsigned long long)vals[i]);
  }
  MT_EXPECT(ctx->mem_base == NULL && g_nps5_arena.bias == NULL, "bias is not NULL in identity mode");
  mt_end();
#endif

  mt_begin(N("thunk_arg_translation_through_real_shims"), AREA);
  // sceKernelMmap through the generated thunk: hint (Ptr) in, result slot (PtrSlot) out; NULL hint
  uint64_t out = 0;
  MT_EXPECT(sys_mmap(0, 0x10000, PROT_RW, MAPF, &out) == 0 && out == G(0x200000000ULL), "NULL hint: result 0x%llx", (unsigned long long)out);
  MT_EXPECT(*(uint64_t *)P(arg_slot) == out, "result slot holds a host pointer or garbage: 0x%llx", (unsigned long long)*(uint64_t *)P(arg_slot));
  // the seventh argument NULL: no crash, EINVAL
  setrsp(); ctx->rdi = 0; ctx->rsi = 0x10000; ctx->rdx = 3; ctx->rcx = MAPF; ctx->r8 = (uint64_t)-1; ctx->r9 = 0; *(uint64_t *)P(ctx->rsp + 8) = 0;
  shim_sceKernelMmap(ctx);
  MT_EXPECT((uint32_t)ctx->rax == E_INVAL, "sceKernelMmap with res == NULL returned 0x%x", (uint32_t)ctx->rax);
  sys_munmap(out, 0x10000);
  // result sign extension: an SCE error code in RAX must keep its low 32 bits
  mt_end();
  return mt_status();
}
