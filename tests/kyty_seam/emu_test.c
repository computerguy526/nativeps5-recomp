// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential test of the trapped-access emulator (nps5_kyty_emu.c) against the CPU: every snippet is one load or
// store instruction. It runs once on plain memory (the CPU executes it) and once on an inaccessible page, where the
// fault handler emulates it on a second copy of the bytes; registers, vector registers, base writeback and memory
// must come out identical. Instructions the emulator does not cover must be declined untouched.
// The Advanced SIMD structure class (LD1-4/ST1-4, lanes, LD1R-4R, post-index) is also enumerated exhaustively from
// generated code: every encoding the decoder accepts must match the CPU, every one it rejects must be undefined.
// Prints "EMU OK <snippets> <checks>".
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "nps5_kyty_emu.h"

typedef struct { uint64_t x[8]; __uint128_t v[4]; } State;
void emu_run(void (*fn)(void), State *s);
__asm__(".text\n.p2align 2\n.globl _emu_run\n_emu_run:\n"
        "  stp x29, x30, [sp, #-32]!\n  stp x19, x20, [sp, #16]\n  mov x19, x1\n  mov x16, x0\n"
        "  ldp q0, q1, [x19, #64]\n  ldp q2, q3, [x19, #96]\n  ldp x0, x1, [x19]\n  ldp x2, x3, [x19, #16]\n  ldp x4, x5, [x19, #32]\n  ldp x6, x7, [x19, #48]\n"
        "  blr x16\n"
        "  stp x0, x1, [x19]\n  stp x2, x3, [x19, #16]\n  stp x4, x5, [x19, #32]\n  stp x6, x7, [x19, #48]\n  stp q0, q1, [x19, #64]\n  stp q2, q3, [x19, #96]\n"
        "  ldp x19, x20, [sp, #16]\n  ldp x29, x30, [sp], #32\n  ret\n");

// name, instruction, x2 (offset register) value, covered by the emulator
#define SNIPPETS(S) \
  S(ldr_x, "ldr x1, [x0]", 0, 1) S(ldr_w, "ldr w1, [x0, #4]", 0, 1) S(ldrb, "ldrb w1, [x0, #3]", 0, 1) S(ldrh, "ldrh w1, [x0, #2]", 0, 1) \
  S(ldrsb_x, "ldrsb x1, [x0, #1]", 0, 1) S(ldrsb_w, "ldrsb w1, [x0, #1]", 0, 1) S(ldrsh_x, "ldrsh x1, [x0, #2]", 0, 1) S(ldrsh_w, "ldrsh w1, [x0, #6]", 0, 1) \
  S(ldrsw, "ldrsw x1, [x0, #4]", 0, 1) S(ldr_big, "ldr x1, [x0, #2048]", 0, 1) S(ldur, "ldur x1, [x0, #-3]", 0, 1) S(ldurb, "ldurb w1, [x0, #-1]", 0, 1) \
  S(ldursw, "ldursw x1, [x0, #-7]", 0, 1) S(ldr_post, "ldr x1, [x0], #8", 0, 1) S(ldr_pre, "ldr x1, [x0, #16]!", 0, 1) S(ldrb_post, "ldrb w1, [x0], #-1", 0, 1) \
  S(ldr_reg, "ldr x1, [x0, x2]", 40, 1) S(ldr_lsl, "ldr x1, [x0, x2, lsl #3]", 5, 1) S(ldr_uxtw, "ldr w1, [x0, w2, uxtw #2]", 0xffffffff00000003ull, 1) \
  S(ldr_sxtw, "ldr x1, [x0, w2, sxtw]", 0x00000000fffffff0ull, 1) S(ldrb_reg, "ldrb w1, [x0, x2]", 77, 1) S(ldrsh_reg, "ldrsh x1, [x0, x2, lsl #1]", 9, 1) \
  S(ldr_xzr, "ldr xzr, [x0, #8]", 0, 1) \
  S(str_x, "str x1, [x0]", 0, 1) S(str_w, "str w1, [x0, #4]", 0, 1) S(strb, "strb w1, [x0, #3]", 0, 1) S(strh, "strh w1, [x0, #2]", 0, 1) \
  S(stur, "stur x1, [x0, #-5]", 0, 1) S(str_post, "str x1, [x0], #-8", 0, 1) S(str_pre, "str x1, [x0, #8]!", 0, 1) S(str_lsl, "str x1, [x0, x2, lsl #3]", 3, 1) \
  S(str_xzr, "str xzr, [x0, #8]", 0, 1) S(strb_wzr, "strb wzr, [x0, #1]", 0, 1) S(strh_reg, "strh w3, [x0, x2]", 13, 1) \
  S(ldp_x, "ldp x1, x3, [x0]", 0, 1) S(ldp_w, "ldp w1, w3, [x0, #8]", 0, 1) S(ldpsw, "ldpsw x1, x3, [x0, #8]", 0, 1) S(ldp_post, "ldp x1, x3, [x0], #16", 0, 1) \
  S(ldp_pre, "ldp x1, x3, [x0, #-16]!", 0, 1) S(stp_x, "stp x1, x3, [x0, #16]", 0, 1) S(stp_w_post, "stp w1, w3, [x0], #8", 0, 1) S(stp_pre, "stp x1, x3, [x0, #-32]!", 0, 1) \
  S(stnp, "stnp x1, x3, [x0]", 0, 1) S(ldnp, "ldnp x1, x3, [x0, #-8]", 0, 1) S(stp_xzr, "stp xzr, x1, [x0]", 0, 1) \
  S(ldr_q, "ldr q0, [x0]", 0, 1) S(ldr_d, "ldr d0, [x0, #8]", 0, 1) S(ldr_s, "ldr s0, [x0, #4]", 0, 1) S(ldr_h, "ldr h0, [x0, #2]", 0, 1) S(ldr_b, "ldr b0, [x0, #1]", 0, 1) \
  S(ldur_q, "ldur q0, [x0, #-7]", 0, 1) S(ldr_q_reg, "ldr q1, [x0, x2]", 33, 1) S(ldr_q_lsl, "ldr q1, [x0, x2, lsl #4]", 2, 1) S(ldr_q_post, "ldr q0, [x0], #16", 0, 1) \
  S(str_q, "str q1, [x0, #16]", 0, 1) S(str_d, "str d1, [x0]", 0, 1) S(str_s, "str s1, [x0, #4]", 0, 1) S(str_b, "str b1, [x0, #1]", 0, 1) S(stur_q, "stur q1, [x0, #3]", 0, 1) \
  S(str_q_pre, "str q0, [x0, #-16]!", 0, 1) S(ldp_q, "ldp q0, q1, [x0]", 0, 1) S(stp_q, "stp q0, q1, [x0, #32]", 0, 1) S(ldp_d_post, "ldp d0, d1, [x0], #16", 0, 1) \
  S(stp_s_pre, "stp s0, s1, [x0, #-8]!", 0, 1) S(stp_q_post, "stp q1, q0, [x0], #-32", 0, 1) \
  S(dc_zva, "dc zva, x0", 0, 1) \
  S(ldapr_x, "ldapr x1, [x0]", 0, 1) S(ldapr_w, "ldapr w1, [x0]", 0, 1) S(ldaprb, "ldaprb w1, [x0]", 0, 1) S(ldapur_x, "ldapur x1, [x0, #2]", 0, 1) \
  S(ldapur_w, "ldapur w1, [x0, #-4]", 0, 1) S(ldapurb, "ldapurb w1, [x0, #3]", 0, 1) S(ldapurh, "ldapurh w1, [x0, #-2]", 0, 1) S(ldapursb, "ldapursb x1, [x0, #1]", 0, 1) \
  S(ldapursh_w, "ldapursh w1, [x0, #2]", 0, 1) S(ldapursw, "ldapursw x1, [x0, #4]", 0, 1) S(stlur_x, "stlur x1, [x0, #10]", 0, 1) S(stlur_w, "stlur w1, [x0, #12]", 0, 1) \
  S(stlurb, "stlurb w1, [x0, #1]", 0, 1) S(stlurh, "stlurh w3, [x0, #6]", 0, 1) S(ldar_x, "ldar x1, [x0]", 0, 1) S(ldar_w, "ldar w1, [x0]", 0, 1) S(ldarb, "ldarb w1, [x0]", 0, 1) \
  S(stlr_x, "stlr x1, [x0]", 0, 1) S(stlr_w, "stlr w3, [x0]", 0, 1) S(stlrh, "stlrh w1, [x0]", 0, 1) \
  S(ld1, "ld1 {v0.16b}, [x0]", 0, 1) S(st1, "st1 {v1.2d}, [x0]", 0, 1) S(ld1_8b, "ld1 {v1.8b}, [x0]", 0, 1) S(ld1_4s, "ld1 {v0.4s}, [x0]", 0, 1) \
  S(ld1_x2, "ld1 {v0.2d, v1.2d}, [x0]", 0, 1) S(ld1_x3, "ld1 {v0.4h, v1.4h, v2.4h}, [x0]", 0, 1) S(ld1_x4, "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x0]", 0, 1) \
  S(ld1_wrap, "ld1 {v31.16b, v0.16b}, [x0]", 0, 1) S(ld1_post, "ld1 {v0.16b}, [x0], #16", 0, 1) S(ld1_post_x, "ld1 {v0.4s, v1.4s}, [x0], x2", 12, 1) \
  S(ld2, "ld2 {v0.4s, v1.4s}, [x0]", 0, 1) S(ld3, "ld3 {v0.8b, v1.8b, v2.8b}, [x0]", 0, 1) S(ld4, "ld4 {v0.8h, v1.8h, v2.8h, v3.8h}, [x0]", 0, 1) \
  S(ld2_post, "ld2 {v2.2d, v3.2d}, [x0], #32", 0, 1) S(st1_x2, "st1 {v0.16b, v1.16b}, [x0]", 0, 1) S(st1_post, "st1 {v1.4s}, [x0], #16", 0, 1) \
  S(st2, "st2 {v0.4s, v1.4s}, [x0]", 0, 1) S(st3, "st3 {v1.2d, v2.2d, v3.2d}, [x0]", 0, 1) S(st4, "st4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], x2", 5, 1) \
  S(ld1r_4s, "mov x16, x0\n  ld1r {v0.4s}, [x16]", 0, 1) S(ld1r_v1, "ld1r {v1.4s}, [x0]", 0, 1) S(ld1r_2s, "ld1r {v0.2s}, [x0]", 0, 1) S(ld1r_16b, "ld1r {v1.16b}, [x0]", 0, 1) \
  S(ld1r_8h, "ld1r {v0.8h}, [x0]", 0, 1) S(ld1r_2d, "ld1r {v0.2d}, [x0]", 0, 1) S(ld1r_1d, "ld1r {v1.1d}, [x0]", 0, 1) S(ld1r_post, "ld1r {v0.4s}, [x0], #4", 0, 1) \
  S(ld1r_post_x, "ld1r {v0.2d}, [x0], x2", 100, 1) S(ld2r, "ld2r {v0.4s, v1.4s}, [x0]", 0, 1) S(ld3r, "ld3r {v0.8h, v1.8h, v2.8h}, [x0]", 0, 1) \
  S(ld4r, "ld4r {v0.2d, v1.2d, v2.2d, v3.2d}, [x0], #32", 0, 1) \
  S(ld1_lane_s, "ld1 {v0.s}[3], [x0]", 0, 1) S(ld1_lane_b, "ld1 {v1.b}[13], [x0]", 0, 1) S(ld1_lane_h, "ld1 {v0.h}[5], [x0]", 0, 1) S(ld1_lane_d, "ld1 {v0.d}[1], [x0]", 0, 1) \
  S(ld1_lane_post, "ld1 {v0.s}[1], [x0], #4", 0, 1) S(ld2_lane, "ld2 {v0.s, v1.s}[2], [x0]", 0, 1) S(ld3_lane, "ld3 {v0.b, v1.b, v2.b}[7], [x0]", 0, 1) \
  S(ld4_lane, "ld4 {v0.d, v1.d, v2.d, v3.d}[1], [x0], x2", 9, 1) S(st1_lane_s, "st1 {v1.s}[2], [x0]", 0, 1) S(st1_lane_b, "st1 {v0.b}[15], [x0]", 0, 1) \
  S(st1_lane_d, "st1 {v1.d}[0], [x0], #8", 0, 1) S(st2_lane, "st2 {v0.h, v1.h}[6], [x0]", 0, 1) S(st4_lane, "st4 {v0.s, v1.s, v2.s, v3.s}[3], [x0]", 0, 1) \
  S(ldxr, "ldxr x1, [x0]", 0, 0) S(ldadd, "ldadd x1, x3, [x0]", 0, 0) S(casal, "casal x1, x3, [x0]", 0, 0) \
  S(ldaxr, "ldaxr x1, [x0]", 0, 0) S(swp, "swp w1, w3, [x0]", 0, 0)

#define DEF(n, text, x2, ok) void snip_##n(void); __asm__(".text\n.arch armv8.5-a\n.p2align 2\n_snip_" #n ":\n  " text "\n  ret\n");
SNIPPETS(DEF)
typedef struct { const char *name; void (*fn)(void); uint64_t x2; int covered; } Snip;
#define ROW(n, text, x2v, ok) {text, snip_##n, x2v, ok},
static const Snip g_snips[] = {SNIPPETS(ROW)};

#define PAGE 0x4000
#define BASE_OFF 0x1040 // room on both sides of the base pointer, not 64-byte aligned + ... (0x1040 is; the snippet offsets vary)
static uint8_t *g_plain, *g_trap, *g_mirror;
static volatile long g_sigs, g_emulated, g_declined;
static long g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static void *redirect(void *user, const Nps5A64Access *a) {
  (void)user;
  const uint64_t lo = (uint64_t)(uintptr_t)g_trap;
  if (a->addr < lo || a->addr + a->size > lo + PAGE) return NULL;
  return g_mirror + (a->addr - lo);
}
static void on_fault(int sig, siginfo_t *si, void *uctx) {
  (void)sig;
  g_sigs++;
  if (nps5_a64_emulate_access(uctx, (uint64_t)(uintptr_t)si->si_addr, redirect, NULL)) { g_emulated++; return; }
  // Declined: open the page so the CPU runs the instruction itself (the caller closes it again).
  g_declined++;
  if (g_declined > 100) _exit(4);
  mprotect(g_trap, PAGE, PROT_READ | PROT_WRITE);
}

static volatile long g_ills;
static void on_ill(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si;
  g_ills++;
  ucontext_t *uc = (ucontext_t *)uctx;
  arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)(uintptr_t)(arm_thread_state64_get_pc(uc->uc_mcontext->__ss) + 4));
}

static void fill(uint8_t *p) {
  for (unsigned i = 0; i < PAGE; i++) p[i] = (uint8_t)(i * 131u + (i >> 8) * 17u + 0x80u); // sign bits set often
}
static State initial(uint8_t *page, uint64_t x2) {
  State s;
  memset(&s, 0, sizeof s);
  s.x[0] = (uint64_t)(uintptr_t)(page + BASE_OFF + 70); // not aligned: unscaled and zva forms see odd addresses
  s.x[1] = 0x1122334455667788ull;
  s.x[2] = x2;
  s.x[3] = 0x99aabbccddeeff00ull;
  for (int i = 4; i < 8; i++) s.x[i] = 0x4040404040404040ull + (uint64_t)i;
  s.v[0] = ((__uint128_t)0xa1a2a3a4a5a6a7a8ull << 64) | 0xb1b2b3b4b5b6b7b8ull;
  s.v[1] = ((__uint128_t)0xc1c2c3c4c5c6c7c8ull << 64) | 0xd1d2d3d4d5d6d7d8ull;
  s.v[2] = ((__uint128_t)0xe1e2e3e4e5e6e7e8ull << 64) | 0xf1f2f3f4f5f6f7f8ull;
  s.v[3] = ((__uint128_t)0x0102030405060708ull << 64) | 0x1112131415161718ull;
  return s;
}

int main(void) {
  CHECK(getpagesize() == PAGE, "16 KiB pages expected");
  g_plain = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  g_trap = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  g_mirror = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(g_plain != MAP_FAILED && g_trap != MAP_FAILED && g_mirror != MAP_FAILED, "mmap");
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  const unsigned n = sizeof g_snips / sizeof *g_snips;
  unsigned covered = 0;
  for (unsigned i = 0; i < n; i++) {
    const Snip *sn = &g_snips[i];
    // Reference: the CPU on plain memory.
    fill(g_plain);
    State ref = initial(g_plain, sn->x2);
    const uint64_t ref_base = ref.x[0];
    emu_run(sn->fn, &ref);
    // Under test: the page is inaccessible; its bytes live in the mirror.
    fill(g_mirror);
    fill(g_trap); // what the CPU runs on when the emulator declines
    State got = initial(g_trap, sn->x2);
    const uint64_t got_base = got.x[0];
    CHECK(mprotect(g_trap, PAGE, PROT_NONE) == 0, "mprotect");
    const long s0 = g_sigs, e0 = g_emulated, d0 = g_declined;
    emu_run(sn->fn, &got);
    CHECK(mprotect(g_trap, PAGE, PROT_READ | PROT_WRITE) == 0, "mprotect");
    CHECK(g_sigs == s0 + 1, "%s: %ld faults", sn->name, g_sigs - s0);
    if (!sn->covered) {
      CHECK(g_declined == d0 + 1 && g_emulated == e0, "%s must be declined", sn->name);
      CHECK(memcmp(g_mirror, (fill(g_plain), g_plain), PAGE) == 0, "%s: a declined instruction touched the mirror", sn->name);
      continue;
    }
    covered++;
    CHECK(g_emulated == e0 + 1 && g_declined == d0, "%s was not emulated", sn->name);
    CHECK(got.x[0] - got_base == ref.x[0] - ref_base, "%s: base writeback %lld, CPU %lld", sn->name, (long long)(got.x[0] - got_base), (long long)(ref.x[0] - ref_base));
    for (int r = 1; r < 8; r++) CHECK(got.x[r] == ref.x[r], "%s: x%d = 0x%llx, CPU 0x%llx", sn->name, r, (unsigned long long)got.x[r], (unsigned long long)ref.x[r]);
    for (int r = 0; r < 4; r++) CHECK(got.v[r] == ref.v[r], "%s: v%d differs from the CPU", sn->name, r);
    CHECK(memcmp(g_mirror, g_plain, PAGE) == 0, "%s: memory differs from the CPU", sn->name);
    // The decoder reports the range the CPU touched (compare with the bytes that changed for stores).
    uint32_t insn;
    memcpy(&insn, (void *)(uintptr_t)sn->fn, 4);
    if (insn == 0xaa0003f0u) continue; // "mov x16, x0" first: the decoder check below takes one instruction
    State pre = initial(g_trap, sn->x2);
    uint64_t xr[31] = {0};
    memcpy(xr, pre.x, sizeof pre.x);
    Nps5A64Access acc;
    CHECK(nps5_a64_decode_access(insn, xr, 0, &acc), "%s: decoder", sn->name);
    if (acc.is_store) {
      fill(g_trap);
      unsigned lo = PAGE, hi = 0;
      for (unsigned b = 0; b < PAGE; b++)
        if (g_mirror[b] != g_trap[b]) { if (b < lo) lo = b; hi = b + 1; }
      const uint64_t off = acc.addr - (uint64_t)(uintptr_t)g_trap;
      CHECK(lo >= off && hi <= off + acc.size, "%s: bytes [%u,%u) changed, decoder says [%llu,+%u)", sn->name, lo, hi, (unsigned long long)off, acc.size);
    }
  }
  // Exhaustive: the Advanced SIMD load/store structure class with base x0, first register v0 and offset register x2
  // (or the immediate form), generated into a code page. 2 (Q) x 2 (multiple/single) x 2 (post) x 2 (L) x 2 (R) x
  // 16 (opcode) x 4 (size) x 2 (Rm = x2 / 31) = 4096 words, plus the no-offset forms with Rm != 0.
  struct sigaction ill = {0};
  ill.sa_sigaction = on_ill;
  ill.sa_flags = SA_SIGINFO;
  sigaction(SIGILL, &ill, NULL);
  uint32_t *code = mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  CHECK(code != MAP_FAILED, "mmap jit");
  unsigned simd_ok = 0, simd_undef = 0;
  for (uint32_t bits = 0; bits < (1u << 13); bits++) {
    const uint32_t q = bits & 1, single = (bits >> 1) & 1, post = (bits >> 2) & 1, l = (bits >> 3) & 1, r = (bits >> 4) & 1;
    const uint32_t opcode = (bits >> 5) & 15, size = (bits >> 9) & 3, rmsel = (bits >> 11) & 3;
    // Rm: 0 (required by the no-offset forms; as a post-index register it is the base itself, whose value differs
    // between the two runs, so x3 stands in), x2, 31 (the immediate form), x3.
    const uint32_t rm = rmsel == 0 ? (post ? 3 : 0) : rmsel == 1 ? 2 : rmsel == 2 ? 31 : 3;
    const uint32_t insn = q << 30 | 0x0C000000u | single << 24 | post << 23 | l << 22 | r << 21 | rm << 16 | opcode << 12 | size << 10 | 0u << 5 | 0u;
    pthread_jit_write_protect_np(0);
    code[0] = insn;
    code[1] = 0xd65f03c0u; // ret
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, 8);
    void (*fn)(void) = (void (*)(void))(uintptr_t)code;
    const uint64_t x2 = 24 + (bits % 7);
    fill(g_plain);
    State ref = initial(g_plain, x2);
    ref.x[3] = 40;
    const uint64_t ref_base = ref.x[0];
    const long i0 = g_ills;
    emu_run(fn, &ref);
    const bool undefined = g_ills != i0;
    State pre = initial(g_trap, x2);
    pre.x[3] = 40;
    uint64_t xr[31] = {0};
    memcpy(xr, pre.x, sizeof pre.x);
    Nps5A64Access acc;
    const bool decoded = nps5_a64_decode_access(insn, xr, 0, &acc);
    CHECK(decoded == !undefined, "0x%08x: decoder %s, CPU %s", insn, decoded ? "accepts" : "rejects", undefined ? "undefined" : "executes");
    if (undefined) { simd_undef++; continue; }
    simd_ok++;
    CHECK(acc.is_store == !l && acc.addr == pre.x[0] && acc.size >= 1 && acc.size <= 64, "0x%08x: decoded access", insn);
    fill(g_mirror);
    fill(g_trap);
    State got = pre;
    const uint64_t got_base = got.x[0];
    CHECK(mprotect(g_trap, PAGE, PROT_NONE) == 0, "mprotect");
    const long s0 = g_sigs, e0 = g_emulated;
    emu_run(fn, &got);
    CHECK(mprotect(g_trap, PAGE, PROT_READ | PROT_WRITE) == 0, "mprotect");
    CHECK(g_sigs == s0 + 1 && g_emulated == e0 + 1, "0x%08x was not emulated (%ld faults)", insn, g_sigs - s0);
    CHECK(got.x[0] - got_base == ref.x[0] - ref_base, "0x%08x: base writeback %lld, CPU %lld", insn, (long long)(got.x[0] - got_base), (long long)(ref.x[0] - ref_base));
    for (int k = 1; k < 8; k++) CHECK(got.x[k] == ref.x[k], "0x%08x: x%d", insn, k);
    for (int k = 0; k < 4; k++) CHECK(got.v[k] == ref.v[k], "0x%08x: v%d differs from the CPU", insn, k);
    CHECK(memcmp(g_mirror, g_plain, PAGE) == 0, "0x%08x: memory differs from the CPU", insn);
    // The decoded range covers exactly what a store changed (the fill pattern has no repeats within 64 bytes... a
    // stored byte may equal the old one, so only "inside" is checked) and loads leave memory alone.
    fill(g_trap);
    unsigned lo = PAGE, hi = 0;
    for (unsigned b = 0; b < PAGE; b++)
      if (g_mirror[b] != g_trap[b]) { if (b < lo) lo = b; hi = b + 1; }
    const uint64_t off = acc.addr - (uint64_t)(uintptr_t)g_trap;
    if (l) CHECK(hi == 0, "0x%08x: a load changed memory", insn);
    else CHECK(hi == 0 || (lo >= off && hi <= off + acc.size), "0x%08x: bytes [%u,%u) changed, decoder says [%llu,+%u)", insn, lo, hi, (unsigned long long)off, acc.size);
  }
  CHECK(simd_ok > 300 && simd_undef > 300, "structure class: %u defined, %u undefined", simd_ok, simd_undef);
  // The game's own words (hoover_beach fluid threads): LD1R {v0.4s}/{v6.4s}, [x16] and LDR d31/d0, [x28, x16].
  {
    uint64_t gx[31] = {0};
    Nps5A64Access acc;
    gx[16] = 0x10575c30f00ull;
    CHECK(nps5_a64_decode_access(0x4d40ca00u, gx, 0, &acc) && acc.addr == 0x10575c30f00ull && acc.size == 4 && !acc.is_store, "ld1r {v0.4s}, [x16]");
    CHECK(nps5_a64_decode_access(0x4d40ca06u, gx, 0, &acc) && acc.size == 4 && !acc.is_store, "ld1r {v6.4s}, [x16]");
    gx[28] = 0x10000000000ull;
    gx[16] = 0x577ed1ffcull;
    CHECK(nps5_a64_decode_access(0xfc706b9fu, gx, 0, &acc) && acc.addr == 0x10577ed1ffcull && acc.size == 8 && !acc.is_store, "ldr d31, [x28, x16]");
  }
  printf("EMU SIMD %u defined %u undefined\n", simd_ok, simd_undef);
  // Not loads or stores at all, and a fault address the instruction does not access: declined.
  uint64_t xr[31] = {0};
  Nps5A64Access acc;
  const uint32_t others[] = {0xd503201f /* nop */, 0x8b010000 /* add */, 0xd65f03c0 /* ret */, 0x58000041 /* ldr literal */, 0xf9800000 /* prfm */, 0x14000001 /* b */};
  for (unsigned i = 0; i < sizeof others / sizeof *others; i++) CHECK(!nps5_a64_decode_access(others[i], xr, 0, &acc), "0x%08x decoded as an access", others[i]);
  xr[5] = 0x1000;
  CHECK(nps5_a64_decode_access(0xf94004a1 /* ldr x1, [x5, #8] */, xr, 0, &acc) && acc.addr == 0x1008 && acc.size == 8 && !acc.is_store, "ldr x1,[x5,#8]");
  CHECK(nps5_a64_decode_access(0xa9be7bfd /* stp x29, x30, [sp, #-32]! */, xr, 0x8000, &acc) && acc.addr == 0x8000 - 32 && acc.size == 16 && acc.is_store, "stp pre sp");
  CHECK(!nps5_a64_decode_access(0xf8408400 /* ldr x0, [x0], #8: writeback of the loaded register */, xr, 0, &acc), "unpredictable writeback");
  printf("EMU OK %u/%u %ld\n", covered, n, g_checks);
  return 0;
}
