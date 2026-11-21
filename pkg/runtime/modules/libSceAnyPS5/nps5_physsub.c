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
//   nps5_physsub_spring_step_a / _b   a title's own mass-spring step: void step(this, float h)   h = xmm0[0]
//       (ASTRO BOT: JungleBridge / SuspentionBridge, called 10x per frame with h = dt / 10: explicit springs + Euler, stable
//       at h = 1/600 s, runs away to inf/NaN at the h of a 20..25 fps frame and never recovers: the plank deck and ropes vanish).
//       Own switches, independent of NPS5_PHYS_SUBSTEP:
//         NPS5_PHYS_SPRING=1        split each call into n calls of h / n with h / n <= 1 / NPS5_PHYS_SPRING_HZ (default on;
//                                   NPS5_PHYS_SPRING=0 = each call runs the original once)
//         NPS5_PHYS_SPRING_HZ=600   10..4000        NPS5_PHYS_SPRING_MAX=8   1..16
//         NPS5_PHYS_SPRING_LOG=1    per object: first step, first non-finite node, and a line every 5 s
//       params (per shim x = a|b): "physsub.spring.x.nodes" = offset in *this of the node vector {begin, end};
//       shared "physsub.spring.stride", ".pos", ".vel", ".fixed" (node layout). Without the nodes param nothing is read.
//
// ASTRO BOT (PPSA21564 01.007): table in profiles/funcrepl/PPSA21564-physsub.json (passed by profiles/PPSA21564.json;
// each entry is sha256-checked, so another build of the game is refused, as the patch profile is).
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
    fprintf(stderr, "[physsub] off (NPS5_PHYS_SUBSTEP=0: wrapped physics steps run unchanged)\n");
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

// ---- mass-spring objects (bridges) ----------------------------------------------------------------------------------

typedef struct {
  int on, log, max;
  double hz;
  uint64_t stride, pos, vel, fixed;
} PsSpringCfg;
static PsSpringCfg g_sp;
static pthread_once_t g_sp_once = PTHREAD_ONCE_INIT;

typedef struct {
  uint64_t self;
  uint64_t calls, steps, split, bad_calls;
  float max_h, win_max_h;
  int nodes, bad_now, said_bad;
} PsSpringObj;
enum { PS_SPRING_OBJS = 16 };
static PsSpringObj g_sp_obj[2][PS_SPRING_OBJS];
static pthread_mutex_t g_sp_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_sp_last_log;

static void sp_init(void) {
  double v;
  int bad = 0;
  v = 1; bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING"), 0, 1, &v); g_sp.on = v == 1.0;
  v = 0; bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_LOG"), 0, 1, &v); g_sp.log = v == 1.0;
  v = 600; bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_HZ"), 10, 4000, &v); g_sp.hz = v;
  v = 8; bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_MAX"), 1, 16, &v); g_sp.max = (int)v;
  g_sp.stride = 0; g_sp.pos = 0; g_sp.vel = 0; g_sp.fixed = 0;
  (void)nps5_repl_param("physsub.spring.stride", &g_sp.stride);
  (void)nps5_repl_param("physsub.spring.pos", &g_sp.pos);
  (void)nps5_repl_param("physsub.spring.vel", &g_sp.vel);
  (void)nps5_repl_param("physsub.spring.fixed", &g_sp.fixed);
  if (bad) fprintf(stderr, "[physsub] W %d NPS5_PHYS_SPRING* value(s) ignored (SPRING 0|1, LOG 0|1, HZ 10..4000, MAX 1..16)\n", bad);
  fprintf(stderr, "[physsub] spring: %s (steps <= 1/%.0f s, at most %d per call)%s\n", g_sp.on ? "substepped" : "untouched", g_sp.hz, g_sp.max,
          g_sp.log ? ", log on" : "");
}

static uint64_t sp_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Reads the node vector of *self. Returns the node count (0 = none / not readable); *bad = nodes with a non-finite
// position, *maxv = largest |velocity| component of the finite ones, mid[3] = position of the middle node.
static int sp_scan(GuestContext *ctx, uint64_t self, uint64_t nodes_off, int *bad, int *fixed, float *maxv, float mid[3], int *first_bad) {
  *bad = 0; *fixed = 0; *maxv = 0; *first_bad = -1; mid[0] = mid[1] = mid[2] = 0;
  if (!self || !g_sp.stride) return 0;
  uint64_t be[2];
  memcpy(be, NPS5_G2H(ctx, self + nodes_off), 16);
  if (!be[0] || be[1] <= be[0] || (be[1] - be[0]) % g_sp.stride) return 0;
  const uint64_t n = (be[1] - be[0]) / g_sp.stride;
  if (n > 4096) return 0;
  for (uint64_t i = 0; i < n; i++) {
    const uint8_t *nd = (const uint8_t *)NPS5_G2H(ctx, be[0] + i * g_sp.stride);
    float p[3], vv[3];
    memcpy(p, nd + g_sp.pos, 12);
    memcpy(vv, nd + g_sp.vel, 12);
    if (nd[g_sp.fixed]) (*fixed)++;
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
      if (*first_bad < 0) *first_bad = (int)i;
      (*bad)++;
    } else {
      for (int k = 0; k < 3; k++) {
        const float a = fabsf(vv[k]);
        if (isfinite(a) && a > *maxv) *maxv = a;
      }
    }
    if (i == n / 2) memcpy(mid, p, 12);
  }
  return (int)n;
}

static void sp_note(GuestContext *ctx, int k, uint64_t self, uint64_t nodes_off, int has_nodes, float h, int n) {
  int bad = 0, fixed = 0, first_bad = -1;
  float maxv = 0, mid[3] = {0, 0, 0};
  const int nodes = has_nodes ? sp_scan(ctx, self, nodes_off, &bad, &fixed, &maxv, mid, &first_bad) : 0;
  pthread_mutex_lock(&g_sp_mu);
  PsSpringObj *o = NULL;
  for (int i = 0; i < PS_SPRING_OBJS; i++) {
    if (g_sp_obj[k][i].self == self) { o = &g_sp_obj[k][i]; break; }
    if (!g_sp_obj[k][i].self) { o = &g_sp_obj[k][i]; o->self = self; break; }
  }
  if (o) {
    if (o->calls == 0)
      fprintf(stderr, "[physsub] spring %c obj 0x%llx first step: %d nodes (%d fixed), h %.5f s -> %d step(s); non-finite nodes %d\n", 'a' + k,
              (unsigned long long)self, nodes, fixed, (double)h, n, bad);
    o->calls++; o->steps += (uint64_t)n;
    if (n > 1) o->split++;
    if (h > o->max_h) o->max_h = h;
    if (h > o->win_max_h) o->win_max_h = h;
    o->nodes = nodes; o->bad_now = bad;
    if (bad) {
      o->bad_calls++;
      if (!o->said_bad) {
        o->said_bad = 1;
        fprintf(stderr, "[physsub] spring %c obj 0x%llx NON-FINITE: node %d (%d of %d nodes) after call %llu, h %.5f s (%d step(s)), largest h so far %.5f s\n",
                'a' + k, (unsigned long long)self, first_bad, bad, nodes, (unsigned long long)o->calls, (double)h, n, (double)o->max_h);
      }
    }
    const uint64_t now = sp_now();
    if (g_sp_last_log == 0) g_sp_last_log = now;
    if (now - g_sp_last_log >= 5000000000ull) {
      g_sp_last_log = now;
      for (int kk = 0; kk < 2; kk++)
        for (int i = 0; i < PS_SPRING_OBJS && g_sp_obj[kk][i].self; i++) {
          PsSpringObj *q = &g_sp_obj[kk][i];
          fprintf(stderr, "[physsub] spring %c obj 0x%llx: %llu calls, %llu steps (%llu split), h max %.5f s (5 s) %.5f s (ever), nodes %d, non-finite %d (%llu calls)",
                  'a' + kk, (unsigned long long)q->self, (unsigned long long)q->calls, (unsigned long long)q->steps, (unsigned long long)q->split,
                  (double)q->win_max_h, (double)q->max_h, q->nodes, q->bad_now, (unsigned long long)q->bad_calls);
          if (q == o) fprintf(stderr, ", |v| max %.3g, mid node (%.3f, %.3f, %.3f)", (double)maxv, (double)mid[0], (double)mid[1], (double)mid[2]);
          fputc('\n', stderr);
          q->win_max_h = 0;
        }
    }
  }
  pthread_mutex_unlock(&g_sp_mu);
}

static void sp_step(GuestContext *ctx, PsWrap *w, int k, const char *nodes_key) {
  pthread_once(&g_sp_once, sp_init);
  PsArgs a;
  ps_save(ctx, &a);
  const float h = a.xmm[0].f32[0];
  const int n = g_sp.on ? nps5_ps_steps(h, g_sp.hz, g_sp.max) : 1;
  if (n == 1) {
    ps_call(ctx, w, &a, h);
  } else {
    const float sub = h / (float)n;
    for (int i = 0; i < n; i++) ps_call(ctx, w, &a, sub);
  }
  if (g_sp.log) {
    uint64_t off = 0;
    const int has = nps5_repl_param(nodes_key, &off) ? 1 : 0;
    sp_note(ctx, k, a.rdi, off, has, h, n);
  }
}

static PsWrap g_w_spring_a = {.shim = "nps5_physsub_spring_step_a"};
static PsWrap g_w_spring_b = {.shim = "nps5_physsub_spring_step_b"};

void nps5_physsub_spring_step_a(GuestContext *ctx) {
  sp_step(ctx, &g_w_spring_a, 0, "physsub.spring.a.nodes");
  SHIM_RETURN();
}

void nps5_physsub_spring_step_b(GuestContext *ctx) {
  sp_step(ctx, &g_w_spring_b, 1, "physsub.spring.b.nodes");
  SHIM_RETURN();
}
