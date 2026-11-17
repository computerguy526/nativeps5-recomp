// SPDX-License-Identifier: GPL-2.0-or-later
// Lean HLE calls: the mode switch, the register-preservation checker's reporting, and the lean
// variants of the hot string/memory shims that live outside the libc module (their full shims
// are in kernel/syscalls/ps4_syscalls.c). Design and rules: docs/m3/leanshim.md.
#include "nps5_lean.h"
#include "nps5_thunk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defaults to 1; the constructor applies the environment. The perf and trace code, which install
// their hooks from constructors of their own, call nps5_lean_require_hooks().
uint8_t g_nps5_lean_mode = 1;

static int env_on(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}

void nps5_lean_require_hooks(void) {
  if (g_nps5_lean_mode == 1) g_nps5_lean_mode = 2;
}

__attribute__((constructor)) static void nps5_lean_init(void) {
  const char *v = getenv("NPS5_LEAN");
  if (v && strcmp(v, "0") == 0) {
    g_nps5_lean_mode = 0;
    return;
  }
  if (env_on("NPS5_LEAN_CHECK")) g_nps5_lean_mode = 3;
  else if (env_on("NPS5_LEAN_HOOKS") && g_nps5_lean_mode == 1) g_nps5_lean_mode = 2;
}

void nps5_lean_violation(GuestContext *ctx, void *fn, const char *what, uint64_t before, uint64_t after) {
  fprintf(stderr,
          "\nFATAL: lean HLE function %p violated the host ABI: %s changed (0x%llx -> 0x%llx), guest rip 0x%llx.\n"
          "A function marked lean must be an AAPCS64 leaf that never enters guest code (docs/m3/leanshim.md).\n"
          "Remove its lean marking, or run with NPS5_LEAN=0.\n",
          fn, what, (unsigned long long)before, (unsigned long long)after, ctx ? (unsigned long long)ctx->rip : 0ULL);
  abort();
}


// Mode 3 (NPS5_LEAN_CHECK=1): called by the bridge after every lean call. before/after hold x19-x28 and
// d8-d15 (low 64 bits) as the bridge saw them around the call; entry[] holds ctx->shadow_sp and
// ctx->host_boundary at entry. The AAPCS64 callee-saved registers are exactly what the lean contract
// needs: x19-x25 are the guest's rax-rbp, x26 rsp, x27 FS base, x28 the memory base, and x29/sp carry
// the shadow stack frame link (docs/m3/leanshim.md).
void nps5_lean_check_regs(GuestContext *ctx, void *fn, const uint64_t *before, const uint64_t *after,
                          uint64_t sp_expect, uint64_t fp_live, const uint64_t *entry) {
  static const char *const names[18] = {"x19 (rax)", "x20 (rbx)", "x21 (rcx)", "x22 (rdx)", "x23 (rsi)", "x24 (rdi)",
                                        "x25 (rbp)", "x26 (rsp)", "x27 (FS base)", "x28 (memory base)", "d8", "d9",
                                        "d10", "d11", "d12", "d13", "d14", "d15"};
  for (int i = 0; i < 18; i++)
    if (before[i] != after[i]) nps5_lean_violation(ctx, fn, names[i], before[i], after[i]);
  if (fp_live != sp_expect) nps5_lean_violation(ctx, fn, "x29 / sp (frame record)", sp_expect, fp_live);
  if (ctx->shadow_sp != entry[0]) nps5_lean_violation(ctx, fn, "ctx->shadow_sp", entry[0], ctx->shadow_sp);
  if (ctx->host_boundary != entry[1]) nps5_lean_violation(ctx, fn, "ctx->host_boundary", entry[1], ctx->host_boundary);
  if (ctx->lean_active != fn) nps5_lean_violation(ctx, fn, "ctx->lean_active", (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)ctx->lean_active);
  ctx->lean_active = NULL;
}

// ---- lean variants of the string/memory shims of kernel/syscalls/ps4_syscalls.c
#define G(p) ((void *)(ctx->mem_base + (p)))

NPS5_LEAN_FN(memcpy) {
  (void)a3; (void)a4; (void)a5;
  memcpy(G(a0), G(a1), (size_t)a2);
  return a0;
}

NPS5_LEAN_FN(memmove) {
  (void)a3; (void)a4; (void)a5;
  memmove(G(a0), G(a1), (size_t)a2);
  return a0;
}

NPS5_LEAN_FN(memset) {
  (void)a3; (void)a4; (void)a5;
  memset(G(a0), (int)(uint8_t)a1, (size_t)a2);
  return a0;
}

NPS5_LEAN_FN(strlen) {
  (void)a1;
  return (uint64_t)strlen((const char *)G(a0));
}

NPS5_LEAN_FN(strcpy) {
  strcpy((char *)G(a0), (const char *)G(a1));
  return a0;
}

NPS5_LEAN_FN(strncpy) {
  strncpy((char *)G(a0), (const char *)G(a1), (size_t)a2);
  return a0;
}

NPS5_LEAN_FN(strcmp) {
  return (uint64_t)(uint32_t)(int32_t)strcmp((const char *)G(a0), (const char *)G(a1));
}
