// SPDX-License-Identifier: GPL-2.0-or-later
//
// [physsub] Physics sub-steps for titles that run at real-time speed below their fixed frame rate (see
// nps5_physsub_logic.h for the switches and the reasoning).
//
// The shims here are bound by a -hle-replace table with "wrap": true (docs/m3/function-hle.md, section 6): every
// caller of the guest step function (direct call, tail jump, vtable) reaches the shim, and the translated original
// stays in the build under _fn_0x<addr>_orig, listed in nps5_repl_wraps (funcrepl_params.c). The shim calls it once
// (switch off, or a dt that needs no split) or n times with dt / n. Only the dt argument changes between the calls;
// every other argument register is restored to its value at entry. The step functions this is meant for take their
// arguments in registers (no stack arguments): the table may only name such functions.
//
//   nps5_physsub_world_step   rigid-body world step: void step(this, a1, a2, float dt)   dt = xmm0[0]
//   nps5_physsub_cloth_step   cloth step:            void step(this, float dt)            dt = xmm0[0]
//       param "physsub.cloth.skip_flag" (optional): byte offset in *this; when that byte is nonzero the step does not
//       simulate (pose-only path) and is called once, unchanged.
//
// ASTRO BOT (PPSA21564 01.007): table in profiles/funcrepl/PPSA21564-physsub.json (local).
#include "nps5_funcrepl.h"
#include "nps5_physsub_logic.h"
#include "nps5_thunk.h"
#include "recomp_runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static Nps5PhysCfg g_ps;
static pthread_once_t g_ps_once = PTHREAD_ONCE_INIT;
static uint64_t g_ps_cloth_skip; // 0 = none, else byte offset + 1

static _Atomic uint64_t g_ps_calls[2], g_ps_steps[2], g_ps_split[2], g_ps_clamped;
static _Atomic uint32_t g_ps_maxdt_bits[2];
static _Atomic uint64_t g_ps_last_log;

static void ps_init(void) {
  const int bad = nps5_ps_config(&g_ps, getenv("NPS5_PHYS_SUBSTEP"), getenv("NPS5_PHYS_SUBSTEP_HZ"), getenv("NPS5_PHYS_SUBSTEP_MAX"),
                                 getenv("NPS5_PHYS_SUBSTEP_CLOTH"), getenv("NPS5_PHYS_SUBSTEP_WORLD"), getenv("NPS5_PHYS_SUBSTEP_LOG"));
  uint64_t off = 0;
  if (nps5_repl_param("physsub.cloth.skip_flag", &off)) g_ps_cloth_skip = off + 1;
  if (bad) fprintf(stderr, "[physsub] W %d NPS5_PHYS_SUBSTEP* value(s) ignored (SUBSTEP 0|1, HZ 10..240, MAX 1..16, CLOTH clamp|substep|off, WORLD 0|1, LOG 0|1)\n", bad);
  if (g_ps.on)
    fprintf(stderr, "[physsub] on: world %s (steps <= 1/%.0f s, at most %d per frame), cloth %s\n", g_ps.world ? "substepped" : "untouched", g_ps.hz, g_ps.max,
            g_ps.cloth == NPS5_PS_CLOTH_CLAMP ? "clamped to 1/hz" : g_ps.cloth == NPS5_PS_CLOTH_SUBSTEP ? "substepped" : "untouched");
  else
    fprintf(stderr, "[physsub] off (wrapped physics steps run unchanged; NPS5_PHYS_SUBSTEP=1 turns it on)\n");
}

typedef struct {
  const char *shim;
  _Atomic(void *) body;
  uint64_t guest;
} PsWrap;

static void *ps_body(PsWrap *w) {
  void *b = atomic_load_explicit(&w->body, memory_order_acquire);
  if (b) return b;
  uint64_t g = 0;
  b = nps5_repl_wrap_body(w->shim, &g);
  if (!b) {
    fprintf(stderr, "[physsub] FATAL: %s is bound but the translation kept no guest body for it (the -hle-replace entry needs \"wrap\": true)\n", w->shim);
    abort();
  }
  w->guest = g;
  atomic_store_explicit(&w->body, b, memory_order_release);
  return b;
}

typedef struct {
  uint64_t rdi, rsi, rdx, rcx, r8, r9, rsp;
  xmm_reg_t xmm[8];
} PsArgs;

static void ps_save(const GuestContext *ctx, PsArgs *a) {
  a->rdi = ctx->rdi; a->rsi = ctx->rsi; a->rdx = ctx->rdx; a->rcx = ctx->rcx; a->r8 = ctx->r8; a->r9 = ctx->r9; a->rsp = ctx->rsp;
  for (int i = 0; i < 8; i++) a->xmm[i] = ctx->xmm[i];
}

// One call of the kept guest body with the entry arguments and xmm0[0] = dt. At shim entry rsp points at the
// caller's return address (rsp % 16 == 8); the callee gets its own null return slot below that, aligned as after a
// CALL (as nps5_libc_call_guest does).
static void ps_call(GuestContext *ctx, PsWrap *w, const PsArgs *a, float dt) {
  void *body = ps_body(w);
  ctx->rdi = a->rdi; ctx->rsi = a->rsi; ctx->rdx = a->rdx; ctx->rcx = a->rcx; ctx->r8 = a->r8; ctx->r9 = a->r9;
  for (int i = 0; i < 8; i++) ctx->xmm[i] = a->xmm[i];
  ctx->xmm[0].f32[0] = dt;
  uint64_t sp = ((a->rsp - 128) & ~15ULL) - 8;
  *(uint64_t *)NPS5_G2H(ctx, sp) = 0;
  ctx->rsp = sp;
  ctx->rip = w->guest;
  recomp_call_entry(ctx, (recomp_fn_t)body);
  ctx->rsp = a->rsp;
}

static void ps_note_dt(int k, float dt) {
  uint32_t bits;
  memcpy(&bits, &dt, 4);
  if (!(dt > 0.0f)) return;
  uint32_t cur = atomic_load_explicit(&g_ps_maxdt_bits[k], memory_order_relaxed);
  while (bits > cur && !atomic_compare_exchange_weak_explicit(&g_ps_maxdt_bits[k], &cur, bits, memory_order_relaxed, memory_order_relaxed)) {
  }
}

static void ps_log(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  uint64_t last = atomic_load_explicit(&g_ps_last_log, memory_order_relaxed);
  if (last == 0) { atomic_compare_exchange_strong(&g_ps_last_log, &last, now); return; }
  if (now - last < 5000000000ull || !atomic_compare_exchange_strong(&g_ps_last_log, &last, now)) return;
  float mw, mc;
  uint32_t bw = atomic_exchange(&g_ps_maxdt_bits[0], 0), bc = atomic_exchange(&g_ps_maxdt_bits[1], 0);
  memcpy(&mw, &bw, 4); memcpy(&mc, &bc, 4);
  fprintf(stderr, "[physsub] world: %llu calls, %llu steps (%llu split), max dt %.4f s | cloth: %llu calls, %llu steps (%llu split, %llu clamped), max dt %.4f s\n",
          (unsigned long long)g_ps_calls[0], (unsigned long long)g_ps_steps[0], (unsigned long long)g_ps_split[0], (double)mw,
          (unsigned long long)g_ps_calls[1], (unsigned long long)g_ps_steps[1], (unsigned long long)g_ps_split[1], (unsigned long long)g_ps_clamped, (double)mc);
}

static PsWrap g_w_world = {.shim = "nps5_physsub_world_step"};
static PsWrap g_w_cloth = {.shim = "nps5_physsub_cloth_step"};

void nps5_physsub_world_step(GuestContext *ctx) {
  pthread_once(&g_ps_once, ps_init);
  PsArgs a;
  ps_save(ctx, &a);
  const float dt = a.xmm[0].f32[0];
  const int n = g_ps.on && g_ps.world ? nps5_ps_steps(dt, g_ps.hz, g_ps.max) : 1;
  if (n == 1) {
    ps_call(ctx, &g_w_world, &a, dt);
  } else {
    const float sub = dt / (float)n;
    for (int i = 0; i < n; i++) ps_call(ctx, &g_w_world, &a, sub);
  }
  if (g_ps.on) {
    atomic_fetch_add_explicit(&g_ps_calls[0], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ps_steps[0], (uint64_t)n, memory_order_relaxed);
    if (n > 1) atomic_fetch_add_explicit(&g_ps_split[0], 1, memory_order_relaxed);
    ps_note_dt(0, dt);
    if (g_ps.log) ps_log();
  }
  SHIM_RETURN();
}

void nps5_physsub_cloth_step(GuestContext *ctx) {
  pthread_once(&g_ps_once, ps_init);
  PsArgs a;
  ps_save(ctx, &a);
  const float dt = a.xmm[0].f32[0];
  int n = 1;
  float step = dt;
  if (g_ps.on && g_ps.cloth != NPS5_PS_CLOTH_OFF) {
    const uint8_t *self = (const uint8_t *)NPS5_G2H(ctx, a.rdi);
    const int posed = g_ps_cloth_skip && self && self[g_ps_cloth_skip - 1];
    if (!posed) {
      if (g_ps.cloth == NPS5_PS_CLOTH_CLAMP) {
        step = nps5_ps_clamp(dt, g_ps.hz);
        if (step != dt) atomic_fetch_add_explicit(&g_ps_clamped, 1, memory_order_relaxed);
      } else {
        n = nps5_ps_steps(dt, g_ps.hz, g_ps.max);
        step = n > 1 ? dt / (float)n : dt;
      }
    }
  }
  for (int i = 0; i < n; i++) ps_call(ctx, &g_w_cloth, &a, step);
  if (g_ps.on) {
    atomic_fetch_add_explicit(&g_ps_calls[1], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ps_steps[1], (uint64_t)n, memory_order_relaxed);
    if (n > 1) atomic_fetch_add_explicit(&g_ps_split[1], 1, memory_order_relaxed);
    ps_note_dt(1, dt);
  }
  SHIM_RETURN();
}
