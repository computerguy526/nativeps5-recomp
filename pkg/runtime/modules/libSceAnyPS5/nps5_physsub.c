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
//       params "physsub.world.lock_obj", ".lock_field", ".lock_var" (optional, all three): the title's world lock, a guest
//       mutex whose handle variable is at *( *(this + lock_obj) + lock_field ) + lock_var. A step that releases a lock
//       its caller took (ASTRO BOT: Step unlocks it once) would release it n times when split: n - 1 unlocks by a
//       non-owner (EPERM; ASTRO BOT asserts in Mutex.cpp:418) and sub-steps 2..n running without the lock. When the
//       calling thread held it at entry, the shim takes it again before each further sub-step, back to the entry depth.
//       NPS5_PHYS_SUBSTEP_RELOCK=0 turns that off.
//       NPS5_PHYS_SUBSTEP_HOLD=1 (opt-in, needs the same params): the shim instead keeps the lock held from the first sub-step
//       to the last one, as one unsplit step holds it: before each sub-step but the last it takes the lock once more, so
//       the step's own release leaves the caller's depth. Other threads that wait for the lock (the game waits for the
//       frame's physics this way) then get it only after the whole frame's simulation, not between sub-steps.
//       NPS5_PHYS_SUBSTEP_ATTACH_ONCE=1 (opt-in): the game's attaching update, called by the world step with its dt
//       (nps5_physsub_attach_step, ASTRO BOT 0xe002f0: drives bodies attached to game objects to the objects' transforms,
//       velocity = position change * 1/dt), runs once per split frame, in the first sub-step, with the frame dt. Split, it
//       otherwise runs in every sub-step with dt / n: sub-step 1 gives those bodies n times their velocity, the later ones 0, so
//       after the frame they report velocity 0 although the object moved (ASTRO BOT: the walking character sinks into floors).
//       With the frame dt once, the velocity is the frame's and the n sub-steps together move the body by the frame's change.
//       Calls outside a split world step run unchanged.
//       NPS5_PHYS_SUBSTEP_INNER=1 (opt-in, needs the wraps nps5_physsub_sim_step / nps5_physsub_pcollide_step): a world step
//       that needs n > 1 steps is called ONCE with the frame dt, and only the engine step inside it is split: the
//       simulation call (ASTRO BOT 0xe7b3b0: one hknp world step + its event dispatch; sets world+0x240 dt) and the particle
//       collide call (0xed6c40) each run n times with dt / n. Everything else the world step does per call (request queues,
//       attaching update, dynamic mesh rebuild with its prev/cur vertex swap and dt, previous body velocities, the
//       accumulator swap at its start, trigger / contact point updates, the world lock release) runs once per frame with the
//       frame dt, as unsplit. The world lock is released once (no relock / hold needed). dtfix is applied right after the
//       last inner simulation step, so the rest of the world step already sees the frame dt.
//       The world step's contact consumers keep pointers into the engine step's per-step manifold / event streams, valid
//       only until the next engine step (ASTRO BOT astro11: consuming the lists of n steps once read stale manifolds, SIGSEGV
//       in 0xb5b320 from "Update contact points"). So after every inner step but the last the shim runs those consumers
//       itself, in the world step's order: particle contact info (param physsub.inner.pinfo_fn(*(world + .particles),
//       byte *(*(world + .pinfo_obj) + .pinfo_flag)); after the simulation only when byte *(*(world + .cfg) + .cfg_pcollide)
//       == 0, else after each particle collide step), then trigger points and contact points (params .trigger_fn /
//       .contact_fn on *(world + .contacts), skipped when NULL). The last inner step's are left to the world step. Without
//       all these params the inner split stays off.
//       NPS5_PHYS_SUBSTEP_WORLD_CLAMP=1 (opt-in): the world step is never split; a dt above 1/hz is clamped to 1/hz (one
//       call, the engine simulates less than real time below hz fps, as the cloth clamp). The frame dt is NOT written back
//       (NPS5_PHYS_SUBSTEP_CLAMP_DTFIX=0 default; 1 = write it back as after a split). Overrides INNER / HOLD / ATTACH_ONCE (nothing to do: one step). Log "world clamped N".
//       Every split world step also counts the times another thread took the world lock between two sub-steps (log line
//       "world lock taken by another thread between sub-steps").
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
// dtfix: world step dt offset + 1, engine pointer offset + 1, engine solver dt offset + 1 (0 = not given); g_ps_dtfix = switch
static uint64_t g_ps_stepdt, g_ps_engine_ptr, g_ps_engine_dt;
static int g_ps_dtfix = 1;
// world lock: offsets + 1 (0 = not given); g_ps_relock = switch
static uint64_t g_ps_lock_obj, g_ps_lock_field, g_ps_lock_var;
static int g_ps_relock = 1, g_ps_hold = 1;
static _Atomic uint64_t g_ps_relocks, g_ps_relock_fail, g_ps_holds;
// gaps between sub-steps (lock released by the step until the shim has it again): how many, how many saw another thread take
// the world lock, and how many first-level acquisitions by others in total
static _Atomic uint64_t g_ps_gaps, g_ps_gaps_taken, g_ps_gap_locks;
static _Atomic uint64_t g_ps_gap_owner;
// attach once: switch, wrapped calls run with the frame dt / skipped
static int g_ps_attach_once = 0;
static _Atomic uint64_t g_ps_attach_ran, g_ps_attach_skipped;
// the split world step this thread is in: sub-step i of n (n = 0: none), frame dt
static _Thread_local int t_ps_sub_i, t_ps_sub_n;
static _Thread_local float t_ps_frame_dt; // last other owner id seen holding the lock when the shim re-took it
// inner split: switch; this thread is inside a world step that splits its engine step (1) and inside an inner wrap (depth)
static int g_ps_inner = 0;
static int g_ps_wclamp = 0;
static unsigned g_ps_poison;           // NPS5_PHYS_SUBSTEP_POISON groups (dtexec probe, default off)
static int g_ps_poison_n;
static unsigned g_ps_poison_list[5];
static unsigned g_ps_poison_period = 100;
static int g_ps_bt;                    // NPS5_PHYS_SUBSTEP_BT=1: one guest backtrace at world step g_ps_bt_at
static uint64_t g_ps_bt_at = 600;
static _Atomic uint64_t g_ps_world_n;
static int g_ps_clamp_dtfix = 0; // NPS5_PHYS_SUBSTEP_CLAMP_DTFIX: clamp mode writes the frame dt back (old behaviour), default off
static _Atomic uint64_t g_ps_wclamped;
static _Thread_local int t_ps_inner, t_ps_inner_depth;
static _Thread_local uint64_t t_ps_inner_world;
// inner split consumers (guest addresses / offsets from the table params; 0 = not given)
static uint64_t g_pi_trigger_fn, g_pi_contact_fn, g_pi_contacts, g_pi_pinfo_fn, g_pi_particles, g_pi_pinfo_obj,
    g_pi_pinfo_flag, g_pi_cfg, g_pi_cfg_pcollide;
static int g_pi_ok;
static _Atomic uint64_t g_ps_inner_drain;
static _Atomic uint64_t g_ps_inner_frames, g_ps_inner_sim, g_ps_inner_sim_steps, g_ps_inner_pc, g_ps_inner_pc_steps;

// kernel/sync/ps4_sync.c
uint32_t ps4_sync_guest_mutex_depth(GuestContext *ctx, uint64_t var);
int ps4_sync_guest_mutex_lock(GuestContext *ctx, uint64_t var);
int ps4_sync_guest_mutex_unlock(GuestContext *ctx, uint64_t var);
uint64_t ps4_sync_guest_mutex_acquisitions(GuestContext *ctx, uint64_t var, uint64_t *owner);
uint64_t ps4_sync_guest_owner_id(GuestContext *ctx);

static _Atomic uint64_t g_ps_calls[2], g_ps_steps[2], g_ps_split[2], g_ps_clamped;
static _Atomic uint32_t g_ps_maxdt_bits[2];
static _Atomic uint64_t g_ps_last_log;

static void ps_init(void) {
    const int bad = nps5_ps_config(&g_ps, getenv("NPS5_PHYS_SUBSTEP"), getenv("NPS5_PHYS_SUBSTEP_HZ"),
                                   getenv("NPS5_PHYS_SUBSTEP_MAX"), getenv("NPS5_PHYS_SUBSTEP_CLOTH"),
                                   getenv("NPS5_PHYS_SUBSTEP_WORLD"), getenv("NPS5_PHYS_SUBSTEP_LOG"));
    if (nps5_ps_config_world_hz(&g_ps, getenv("NPS5_PHYS_SUBSTEP_WORLD_HZ")))
        fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_WORLD_HZ: 10..240\n");
    uint64_t off = 0;
    if (nps5_repl_param("physsub.cloth.skip_flag", &off)) g_ps_cloth_skip = off + 1;
    if (nps5_repl_param("physsub.world.stepdt", &off)) g_ps_stepdt = off + 1;
    if (nps5_repl_param("physsub.world.engine", &off)) g_ps_engine_ptr = off + 1;
    if (nps5_repl_param("physsub.engine.solver_dt", &off)) g_ps_engine_dt = off + 1;
    if (nps5_repl_param("physsub.world.lock_obj", &off)) g_ps_lock_obj = off + 1;
    if (nps5_repl_param("physsub.world.lock_field", &off)) g_ps_lock_field = off + 1;
    if (nps5_repl_param("physsub.world.lock_var", &off)) g_ps_lock_var = off + 1;
    {
        double v = 1;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_RELOCK"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_RELOCK: 0|1\n");
        g_ps_relock = v == 1.0 && g_ps_lock_obj && g_ps_lock_field && g_ps_lock_var;
    }
    {
        double v = 1; // default on (ASTRO BOT: game threads waiting on the world lock otherwise run between sub-steps)
        const char *e = getenv("NPS5_PHYS_SUBSTEP_HOLD");
        if (nps5_ps_num(e, 0, 1, &v)) fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_HOLD: 0|1\n");
        g_ps_hold = v == 1.0 && g_ps_lock_obj && g_ps_lock_field && g_ps_lock_var;
        if (e && *e && v == 1.0 && !g_ps_hold)
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_HOLD=1 needs the physsub.world.lock_* params\n");
    }
    {
        double v = 0;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_ATTACH_ONCE"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_ATTACH_ONCE: 0|1\n");
        g_ps_attach_once = v == 1.0;
        if (g_ps_attach_once)
            fprintf(
                stderr,
                "[physsub] attach once: the attaching update runs once per split frame (first sub-step, frame dt)%s\n",
                nps5_repl_wrap_body("nps5_physsub_attach_step", &(uint64_t){0})
                    ? ""
                    : " (W: no nps5_physsub_attach_step wrap: no effect)");
    }
    {
        double v = 0;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_WORLD_CLAMP"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_WORLD_CLAMP: 0|1\n");
        g_ps_wclamp = v == 1.0;
        v = 0;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_CLAMP_DTFIX"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_CLAMP_DTFIX: 0|1\n");
        g_ps_clamp_dtfix = v == 1.0;
        {   // dtexec probes
            const char *e = getenv("NPS5_PHYS_SUBSTEP_POISON");
            char buf[128];
            if (e && *e && g_ps_engine_ptr && g_ps_engine_dt == 0x560 + 1) {
                snprintf(buf, sizeof buf, "%s", e);
                for (char *sv = NULL, *t = strtok_r(buf, ",", &sv); t && g_ps_poison_n < 5; t = strtok_r(NULL, ",", &sv)) {
                    const unsigned g = nps5_ps_poison_group(t);
                    if (!g) fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_POISON: unknown group '%s' (dt,ratio,maxdt,substep,grav)\n", t);
                    else g_ps_poison_list[g_ps_poison_n++] = g, g_ps_poison |= g;
                }
                double pv = 100;
                if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_POISON_PERIOD"), 1, 100000, &pv))
                    fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_POISON_PERIOD: 1..100000\n");
                g_ps_poison_period = (unsigned)pv;
                fprintf(stderr, "[physsub] poison probe: %d groups, phase length %u world steps (phase 0 = clean)\n", g_ps_poison_n, g_ps_poison_period);
            } else if (e && *e)
                fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_POISON needs the physsub.world.engine / solver_dt (0x560) params\n");
            double bv = 0;
            if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_BT"), 0, 1, &bv)) fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_BT: 0|1\n");
            g_ps_bt = bv == 1.0;
            if (g_ps_bt) {
                double at = 600;
                if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_BT_AT"), 1, 1e9, &at)) fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_BT_AT: world step number\n");
                g_ps_bt_at = (uint64_t)at;
            }
        }
        if (g_ps_wclamp)
            fprintf(stderr, "[physsub] world clamp on: one world step per frame, dt clamped to 1/hz (no split, dt write-back %s)\n",
                    g_ps_clamp_dtfix ? "on" : "off");
    }
    {
        double v = 0;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_INNER"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_INNER: 0|1\n");
        const int sim = nps5_repl_wrap_body("nps5_physsub_sim_step", &(uint64_t){0}) != NULL;
        const int pc = nps5_repl_wrap_body("nps5_physsub_pcollide_step", &(uint64_t){0}) != NULL;
        {
            const char *k[9] = {"physsub.inner.trigger_fn", "physsub.inner.contact_fn", "physsub.inner.contacts",
                                "physsub.inner.pinfo_fn",   "physsub.inner.particles",  "physsub.inner.pinfo_obj",
                                "physsub.inner.pinfo_flag", "physsub.inner.cfg",        "physsub.inner.cfg_pcollide"};
            uint64_t *d[9] = {&g_pi_trigger_fn, &g_pi_contact_fn, &g_pi_contacts, &g_pi_pinfo_fn,    &g_pi_particles,
                              &g_pi_pinfo_obj,  &g_pi_pinfo_flag, &g_pi_cfg,      &g_pi_cfg_pcollide};
            g_pi_ok = 1;
            for (int i = 0; i < 9; i++)
                if (!nps5_repl_param(k[i], d[i])) g_pi_ok = 0;
        }
        g_ps_inner = v == 1.0 && sim && g_pi_ok;
        if (v == 1.0 && !g_pi_ok)
            fprintf(stderr,
                    "[physsub] W NPS5_PHYS_SUBSTEP_INNER=1 needs the physsub.inner.* params: whole-step split kept\n");
        if (v == 1.0)
            fprintf(
                stderr,
                "[physsub] inner split %s: the world step runs once per frame with the frame dt, only its engine step "
                "(simulation%s) is split%s\n",
                g_ps_inner ? "on" : "NOT on", pc ? " + particle collide" : ", no particle-collide wrap",
                sim ? "" : " (W: no nps5_physsub_sim_step wrap in this build: whole-step split kept)");
    }
    {
        double v = 1;
        if (nps5_ps_num(getenv("NPS5_PHYS_SUBSTEP_DTFIX"), 0, 1, &v))
            fprintf(stderr, "[physsub] W NPS5_PHYS_SUBSTEP_DTFIX: 0|1\n");
        g_ps_dtfix = v == 1.0;
    }
    if (bad)
        fprintf(stderr,
                "[physsub] W %d NPS5_PHYS_SUBSTEP* value(s) ignored (SUBSTEP 0|1, HZ 10..240, MAX 1..16, CLOTH "
                "clamp|substep|off, WORLD 0|1, LOG 0|1)\n",
                bad);
    if (g_ps.on && g_ps.world)
        fprintf(stderr,
                "[physsub] world dtfix %s (world step dt %s, engine solver dt %s), world lock kept per sub-step: %s\n",
                g_ps_dtfix ? "on" : "off", g_ps_stepdt ? "yes" : "no param",
                g_ps_engine_ptr && g_ps_engine_dt ? "yes" : "no param",
                g_ps_hold     ? "held across all sub-steps (NPS5_PHYS_SUBSTEP_HOLD=1)"
                : g_ps_relock ? "on"
                : g_ps_lock_obj && g_ps_lock_field && g_ps_lock_var ? "off (NPS5_PHYS_SUBSTEP_RELOCK=0)"
                                                                    : "no params");
    if (g_ps.on)
        fprintf(stderr, "[physsub] on: world %s (steps <= 1/%.0f s, at most %d per frame), cloth %s (1/%.0f s)\n",
                g_ps.world ? "substepped" : "untouched", g_ps.world_hz, g_ps.max,
                g_ps.cloth == NPS5_PS_CLOTH_CLAMP     ? "clamped to 1/hz"
                : g_ps.cloth == NPS5_PS_CLOTH_SUBSTEP ? "substepped"
                                                      : "untouched",
                g_ps.hz);
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
        fprintf(stderr,
                "[physsub] FATAL: %s is bound but the translation kept no guest body for it (the -hle-replace entry "
                "needs \"wrap\": true)\n",
                w->shim);
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
    a->rdi = ctx->rdi;
    a->rsi = ctx->rsi;
    a->rdx = ctx->rdx;
    a->rcx = ctx->rcx;
    a->r8 = ctx->r8;
    a->r9 = ctx->r9;
    a->rsp = ctx->rsp;
    for (int i = 0; i < 8; i++)
        a->xmm[i] = ctx->xmm[i];
}

// One call of the kept guest body with the entry arguments and xmm0[0] = dt. At shim entry rsp points at the
// caller's return address (rsp % 16 == 8); the callee gets its own null return slot below that, aligned as after a
// CALL (as nps5_libc_call_guest does).
static void ps_call(GuestContext *ctx, PsWrap *w, const PsArgs *a, float dt) {
    void *body = ps_body(w);
    ctx->rdi = a->rdi;
    ctx->rsi = a->rsi;
    ctx->rdx = a->rdx;
    ctx->rcx = a->rcx;
    ctx->r8 = a->r8;
    ctx->r9 = a->r9;
    for (int i = 0; i < 8; i++)
        ctx->xmm[i] = a->xmm[i];
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
    while (bits > cur && !atomic_compare_exchange_weak_explicit(&g_ps_maxdt_bits[k], &cur, bits, memory_order_relaxed,
                                                                memory_order_relaxed)) {
    }
}

static void ps_log(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    uint64_t last = atomic_load_explicit(&g_ps_last_log, memory_order_relaxed);
    if (last == 0) {
        atomic_compare_exchange_strong(&g_ps_last_log, &last, now);
        return;
    }
    if (now - last < 5000000000ull || !atomic_compare_exchange_strong(&g_ps_last_log, &last, now)) return;
    float mw, mc;
    uint32_t bw = atomic_exchange(&g_ps_maxdt_bits[0], 0), bc = atomic_exchange(&g_ps_maxdt_bits[1], 0);
    memcpy(&mw, &bw, 4);
    memcpy(&mc, &bc, 4);
    fprintf(
        stderr,
        "[physsub] world: %llu calls, %llu steps (%llu split), max dt %.4f s | cloth: %llu calls, %llu steps (%llu "
        "split, %llu clamped), max dt %.4f s | world lock re-taken %llu (%llu failed), held %llu | world lock taken by "
        "another thread between sub-steps: %llu of %llu gaps (%llu locks, last owner 0x%llx) | attach: %llu frame-dt "
        "calls, "
        "%llu skipped | inner: %llu frames, sim %llu calls -> %llu steps, particle collide %llu calls -> %llu steps, "
        "%llu "
        "consumer drains | world clamped %llu\n",
        (unsigned long long)g_ps_calls[0], (unsigned long long)g_ps_steps[0], (unsigned long long)g_ps_split[0],
        (double)mw, (unsigned long long)g_ps_calls[1], (unsigned long long)g_ps_steps[1],
        (unsigned long long)g_ps_split[1], (unsigned long long)g_ps_clamped, (double)mc,
        (unsigned long long)g_ps_relocks, (unsigned long long)g_ps_relock_fail, (unsigned long long)g_ps_holds,
        (unsigned long long)g_ps_gaps_taken, (unsigned long long)g_ps_gaps, (unsigned long long)g_ps_gap_locks,
        (unsigned long long)g_ps_gap_owner, (unsigned long long)g_ps_attach_ran,
        (unsigned long long)g_ps_attach_skipped, (unsigned long long)g_ps_inner_frames,
        (unsigned long long)g_ps_inner_sim, (unsigned long long)g_ps_inner_sim_steps, (unsigned long long)g_ps_inner_pc,
        (unsigned long long)g_ps_inner_pc_steps, (unsigned long long)g_ps_inner_drain,
        (unsigned long long)g_ps_wclamped);
}

static PsWrap g_w_world = {.shim = "nps5_physsub_world_step"};
static PsWrap g_w_cloth = {.shim = "nps5_physsub_cloth_step"};

// Guest address of the world lock's handle variable for the world `self`, 0 if a pointer on the way is not a guest address.
static uint64_t ps_world_lock_var(const GuestContext *ctx, uint64_t self) {
    if (!self || self >= ctx->mem_size) return 0;
    const uint64_t obj = *(const uint64_t *)NPS5_G2H(ctx, self + g_ps_lock_obj - 1);
    if (!obj || obj >= ctx->mem_size) return 0;
    const uint64_t mtx = *(const uint64_t *)NPS5_G2H(ctx, obj + g_ps_lock_field - 1);
    if (!mtx || mtx >= ctx->mem_size) return 0;
    return mtx + g_ps_lock_var - 1;
}

// Before sub-step 2..n: take the world lock again until the thread holds it as deep as at entry.
static void ps_world_relock(GuestContext *ctx, uint64_t var, uint32_t depth0) {
    static _Atomic int said;
    uint32_t d = ps4_sync_guest_mutex_depth(ctx, var);
    while (d < depth0) {
        const int r = ps4_sync_guest_mutex_lock(ctx, var);
        if (r != 0) {
            atomic_fetch_add_explicit(&g_ps_relock_fail, 1, memory_order_relaxed);
            fprintf(stderr, "[physsub] W world lock 0x%llx: re-taking it failed (%d)\n", (unsigned long long)var, r);
            return;
        }
        atomic_fetch_add_explicit(&g_ps_relocks, 1, memory_order_relaxed);
        if (!atomic_exchange(&said, 1))
            fprintf(stderr,
                    "[physsub] world: the step released the world lock 0x%llx its caller holds (depth %u -> %u): "
                    "taken again before each further sub-step\n",
                    (unsigned long long)var, depth0, d);
        d++;
    }
}

void nps5_physsub_world_step(GuestContext *ctx) {
    pthread_once(&g_ps_once, ps_init);
    PsArgs a;
    ps_save(ctx, &a);
    const float dt = a.xmm[0].f32[0];
    const uint64_t wn = atomic_fetch_add_explicit(&g_ps_world_n, 1, memory_order_relaxed);
    if (g_ps_bt && wn == g_ps_bt_at) {
        uint64_t bt[24];
        const int k = nps5_ps_bt_walk((const uint8_t *)NPS5_G2H(ctx, 0), ctx->mem_size, ctx->rsp, ctx->rbp, bt, 24);
        fprintf(stderr, "[physsub] backtrace at world step %llu (thread %p), %d frames:", (unsigned long long)wn, (void *)pthread_self(), k);
        for (int i = 0; i < k; i++) fprintf(stderr, " 0x%llx", (unsigned long long)bt[i]);
        fprintf(stderr, "\n");
    }
    const int clamp = g_ps.on && g_ps.world && g_ps_wclamp;
    const int n = g_ps.on && g_ps.world && !clamp ? nps5_ps_steps(dt, g_ps.world_hz, g_ps.max) : 1;
    if (clamp) {
        const float c = nps5_ps_clamp(dt, g_ps.world_hz);
        ps_call(ctx, &g_w_world, &a, c);
        if (c != dt) {
            atomic_fetch_add_explicit(&g_ps_wclamped, 1, memory_order_relaxed);
            if (nps5_ps_clamp_writeback(g_ps_dtfix, g_ps_clamp_dtfix, c, dt)) { // old behaviour (CLAMP_DTFIX=1): readers see the frame dt
                float *w = g_ps_stepdt ? (float *)NPS5_G2H(ctx, a.rdi + g_ps_stepdt - 1) : NULL;
                float *e = NULL;
                if (g_ps_engine_ptr && g_ps_engine_dt) {
                    const uint64_t eng = *(const uint64_t *)NPS5_G2H(ctx, a.rdi + g_ps_engine_ptr - 1);
                    if (eng && eng < ctx->mem_size) e = (float *)NPS5_G2H(ctx, eng + g_ps_engine_dt - 1);
                }
                nps5_ps_frame_dt(dt, w, e);
            }
        }
    } else if (n == 1) {
        ps_call(ctx, &g_w_world, &a, dt);
    } else if (g_ps_inner && t_ps_inner == 0) {
        // inner split: one world step with the frame dt; the wraps below split only its engine step
        atomic_fetch_add_explicit(&g_ps_inner_frames, 1, memory_order_relaxed);
        t_ps_inner = 1;
        t_ps_inner_world = a.rdi;
        ps_call(ctx, &g_w_world, &a, dt);
        t_ps_inner = 0;
        t_ps_inner_world = 0;
    } else {
        const float sub = dt / (float)n;
        const uint64_t lv = g_ps_relock || g_ps_hold || (g_ps_lock_obj && g_ps_lock_field && g_ps_lock_var)
                                ? ps_world_lock_var(ctx, a.rdi)
                                : 0;
        const uint32_t depth0 = lv ? ps4_sync_guest_mutex_depth(ctx, lv) : 0;
        const int hold = g_ps_hold && depth0;
        const uint64_t me = depth0 ? ps4_sync_guest_owner_id(ctx) : 0;
        t_ps_sub_n = n;
        t_ps_frame_dt = dt;
        for (int i = 0; i < n; i++) {
            const int last = i == n - 1;
            t_ps_sub_i = i;
            // hold: one more level before each sub-step but the last; the step's release then leaves depth0 held
            if (hold && !last && ps4_sync_guest_mutex_lock(ctx, lv) == 0)
                atomic_fetch_add_explicit(&g_ps_holds, 1, memory_order_relaxed);
            const uint64_t acq0 = depth0 && !last ? ps4_sync_guest_mutex_acquisitions(ctx, lv, NULL) : 0;
            ps_call(ctx, &g_w_world, &a, sub);
            if (!depth0 || last) continue;
            // gap probe: did another thread take the lock after this sub-step released it (before the shim has it again)?
            uint64_t owner = 0;
            const uint32_t d = ps4_sync_guest_mutex_depth(ctx, lv);
            uint64_t acq1 = ps4_sync_guest_mutex_acquisitions(ctx, lv, &owner);
            if (d < depth0) {
                if (owner && owner != me) atomic_store_explicit(&g_ps_gap_owner, owner, memory_order_relaxed);
                if (g_ps_relock || hold) ps_world_relock(ctx, lv, depth0);
                // the shim's own first-level acquisition (from depth 0) is one of the counted ones
                if (d == 0 && (g_ps_relock || hold)) {
                    const uint64_t a2 = ps4_sync_guest_mutex_acquisitions(ctx, lv, NULL);
                    acq1 = a2 > 0 ? a2 - 1 : 0;
                }
            }
            while (hold && ps4_sync_guest_mutex_depth(ctx, lv) > depth0) // the step did not release it: keep depth0
                if (ps4_sync_guest_mutex_unlock(ctx, lv) != 0) break;
            if (d < depth0) atomic_fetch_add_explicit(&g_ps_gaps, 1, memory_order_relaxed);
            {
                if (acq1 > acq0) {
                    atomic_fetch_add_explicit(&g_ps_gaps_taken, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_ps_gap_locks, acq1 - acq0, memory_order_relaxed);
                }
            }
        }
        t_ps_sub_n = 0;
        if (g_ps_dtfix) {
            // the last sub-step left dt / n and n / dt as the step values; per-frame readers must see the frame's
            float *w = g_ps_stepdt ? (float *)NPS5_G2H(ctx, a.rdi + g_ps_stepdt - 1) : NULL;
            float *e = NULL;
            if (g_ps_engine_ptr && g_ps_engine_dt) {
                const uint64_t eng = *(const uint64_t *)NPS5_G2H(ctx, a.rdi + g_ps_engine_ptr - 1);
                if (eng) e = (float *)NPS5_G2H(ctx, eng + g_ps_engine_dt - 1);
            }
            nps5_ps_frame_dt(dt, w, e);
        }
    }
    if (g_ps_poison_n && (clamp || n == 1)) { // unsplit step: leave the solver info as a 2-way split would
        const int ph = nps5_ps_poison_phase(wn, g_ps_poison_period, g_ps_poison_n);
        static _Atomic int last_ph = -1;
        if (atomic_exchange_explicit(&last_ph, ph, memory_order_relaxed) != ph)
            fprintf(stderr, "[physsub] poison phase %d (%s) at world step %llu\n", ph, ph ? "poisoned" : "clean", (unsigned long long)wn);
        if (ph > 0 && g_ps_engine_ptr) {
            const uint64_t eng = *(const uint64_t *)NPS5_G2H(ctx, a.rdi + g_ps_engine_ptr - 1);
            if (eng && eng < ctx->mem_size) nps5_ps_poison((uint8_t *)NPS5_G2H(ctx, eng), g_ps_poison_list[ph - 1]);
        }
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

// The game's attaching update, void f(this, float dt), dt = xmm0[0] (see NPS5_PHYS_SUBSTEP_ATTACH_ONCE above).
static PsWrap g_w_attach = {.shim = "nps5_physsub_attach_step"};
static _Thread_local int t_ps_attach_depth;
void nps5_physsub_attach_step(GuestContext *ctx) {
    pthread_once(&g_ps_once, ps_init);
    PsArgs a;
    ps_save(ctx, &a);
    float dt = a.xmm[0].f32[0];
    if (g_ps_attach_once && t_ps_sub_n > 1 && t_ps_attach_depth == 0) {
        if (t_ps_sub_i != 0) {
            atomic_fetch_add_explicit(&g_ps_attach_skipped, 1, memory_order_relaxed);
            SHIM_RETURN();
        }
        dt = t_ps_frame_dt;
        atomic_fetch_add_explicit(&g_ps_attach_ran, 1, memory_order_relaxed);
    }
    t_ps_attach_depth++;
    ps_call(ctx, &g_w_attach, &a, dt);
    t_ps_attach_depth--;
    SHIM_RETURN();
}

// Inner split (NPS5_PHYS_SUBSTEP_INNER=1): the engine step calls inside one world step. void f(this, a1, a2, float dt).
// Inside an inner-mode world step (this thread, not nested) a call is split into n calls of dt / n; anywhere else it runs once.
static PsWrap g_w_sim = {.shim = "nps5_physsub_sim_step"};
static PsWrap g_w_pcollide = {.shim = "nps5_physsub_pcollide_step"};
// A guest function f(a0, a1) called from a shim (as nps5_libc_call_guest; argument registers are restored by the caller's ps_call).
static void ps_call_guest2(GuestContext *ctx, uint64_t fn, uint64_t a0, uint64_t a1) {
    const uint64_t s_rsp = ctx->rsp, s_rip = ctx->rip;
    ctx->rdi = a0;
    ctx->rsi = a1;
    uint64_t sp = ((s_rsp - 128) & ~15ULL) - 8;
    *(uint64_t *)NPS5_G2H(ctx, sp) = 0;
    ctx->rsp = sp;
    recomp_dispatch(ctx, fn);
    ctx->rsp = s_rsp;
    ctx->rip = s_rip;
}
static uint64_t ps_rd64(const GuestContext *ctx, uint64_t g) {
    return g && g + 8 <= ctx->mem_size ? *(const uint64_t *)NPS5_G2H(ctx, g) : 0;
}
// After an inner engine step that is not the last: what the world step does with that step's results before the next one
// (see NPS5_PHYS_SUBSTEP_INNER above). pinfo: run the particle contact info; contacts: trigger + contact points.
static void ps_inner_drain(GuestContext *ctx, int pinfo, int contacts) {
    const uint64_t w = t_ps_inner_world;
    if (!w) return;
    if (pinfo) {
        const uint64_t parts = ps_rd64(ctx, w + g_pi_particles);
        const uint64_t fo = ps_rd64(ctx, w + g_pi_pinfo_obj);
        if (parts && fo && fo + g_pi_pinfo_flag < ctx->mem_size)
            ps_call_guest2(ctx, g_pi_pinfo_fn, parts, *(const uint8_t *)NPS5_G2H(ctx, fo + g_pi_pinfo_flag));
    }
    if (contacts) {
        const uint64_t c = ps_rd64(ctx, w + g_pi_contacts);
        if (c) {
            ps_call_guest2(ctx, g_pi_trigger_fn, c, 0);
            ps_call_guest2(ctx, g_pi_contact_fn, c, 0);
        }
    }
    atomic_fetch_add_explicit(&g_ps_inner_drain, 1, memory_order_relaxed);
}
// 1 when the world uses the separate particle collide step (its particle contact info then follows that step)
static int ps_inner_pcollide_path(const GuestContext *ctx) {
    const uint64_t cfg = ps_rd64(ctx, t_ps_inner_world + g_pi_cfg);
    return cfg && cfg + g_pi_cfg_pcollide < ctx->mem_size &&
           *(const uint8_t *)NPS5_G2H(ctx, cfg + g_pi_cfg_pcollide) != 0;
}
static void ps_inner_split(GuestContext *ctx, PsWrap *w, int fix_dt, _Atomic uint64_t *calls, _Atomic uint64_t *steps) {
    pthread_once(&g_ps_once, ps_init);
    PsArgs a;
    ps_save(ctx, &a);
    const float dt = a.xmm[0].f32[0];
    const int n = t_ps_inner && t_ps_inner_depth == 0 ? nps5_ps_steps(dt, g_ps.world_hz, g_ps.max) : 1;
    t_ps_inner_depth++;
    if (n == 1) {
        ps_call(ctx, w, &a, dt);
    } else {
        const float sub = dt / (float)n;
        for (int i = 0; i < n; i++) {
            ps_call(ctx, w, &a, sub);
            if (i == n - 1) break;
            if (fix_dt) // simulation: its consumers (particle contact info only on the path without a separate particle collide)
                ps_inner_drain(ctx, !ps_inner_pcollide_path(ctx), 1);
            else // particle collide: its particle contact info
                ps_inner_drain(ctx, 1, 0);
        }
        atomic_fetch_add_explicit(calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(steps, (uint64_t)n, memory_order_relaxed);
        if (fix_dt && g_ps_dtfix) {
            // the simulation wrote dt / n, n / dt as the step values; the rest of the world step and per-frame readers see the frame's
            float *wv = g_ps_stepdt ? (float *)NPS5_G2H(ctx, a.rdi + g_ps_stepdt - 1) : NULL;
            float *e = NULL;
            if (g_ps_engine_ptr && g_ps_engine_dt) {
                const uint64_t eng = *(const uint64_t *)NPS5_G2H(ctx, a.rdi + g_ps_engine_ptr - 1);
                if (eng && eng < ctx->mem_size) e = (float *)NPS5_G2H(ctx, eng + g_ps_engine_dt - 1);
            }
            nps5_ps_frame_dt(dt, wv, e);
        }
    }
    t_ps_inner_depth--;
}
void nps5_physsub_sim_step(GuestContext *ctx) {
    ps_inner_split(ctx, &g_w_sim, 1, &g_ps_inner_sim, &g_ps_inner_sim_steps);
    SHIM_RETURN();
}
void nps5_physsub_pcollide_step(GuestContext *ctx) {
    ps_inner_split(ctx, &g_w_pcollide, 0, &g_ps_inner_pc, &g_ps_inner_pc_steps);
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
    for (int i = 0; i < n; i++)
        ps_call(ctx, &g_w_cloth, &a, step);
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
    v = 1;
    bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING"), 0, 1, &v);
    g_sp.on = v == 1.0;
    v = 0;
    bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_LOG"), 0, 1, &v);
    g_sp.log = v == 1.0;
    v = 600;
    bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_HZ"), 10, 4000, &v);
    g_sp.hz = v;
    v = 8;
    bad += nps5_ps_num(getenv("NPS5_PHYS_SPRING_MAX"), 1, 16, &v);
    g_sp.max = (int)v;
    g_sp.stride = 0;
    g_sp.pos = 0;
    g_sp.vel = 0;
    g_sp.fixed = 0;
    (void)nps5_repl_param("physsub.spring.stride", &g_sp.stride);
    (void)nps5_repl_param("physsub.spring.pos", &g_sp.pos);
    (void)nps5_repl_param("physsub.spring.vel", &g_sp.vel);
    (void)nps5_repl_param("physsub.spring.fixed", &g_sp.fixed);
    if (bad)
        fprintf(stderr,
                "[physsub] W %d NPS5_PHYS_SPRING* value(s) ignored (SPRING 0|1, LOG 0|1, HZ 10..4000, MAX 1..16)\n",
                bad);
    fprintf(stderr, "[physsub] spring: %s (steps <= 1/%.0f s, at most %d per call)%s\n",
            g_sp.on ? "substepped" : "untouched", g_sp.hz, g_sp.max, g_sp.log ? ", log on" : "");
}

static uint64_t sp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Reads the node vector of *self. Returns the node count (0 = none / not readable); *bad = nodes with a non-finite
// position, *maxv = largest |velocity| component of the finite ones, mid[3] = position of the middle node.
static int sp_scan(GuestContext *ctx, uint64_t self, uint64_t nodes_off, int *bad, int *fixed, float *maxv,
                   float mid[3], int *first_bad) {
    *bad = 0;
    *fixed = 0;
    *maxv = 0;
    *first_bad = -1;
    mid[0] = mid[1] = mid[2] = 0;
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
        if (g_sp_obj[k][i].self == self) {
            o = &g_sp_obj[k][i];
            break;
        }
        if (!g_sp_obj[k][i].self) {
            o = &g_sp_obj[k][i];
            o->self = self;
            break;
        }
    }
    if (o) {
        if (o->calls == 0)
            fprintf(stderr,
                    "[physsub] spring %c obj 0x%llx first step: %d nodes (%d fixed), h %.5f s -> %d step(s); "
                    "non-finite nodes %d\n",
                    'a' + k, (unsigned long long)self, nodes, fixed, (double)h, n, bad);
        o->calls++;
        o->steps += (uint64_t)n;
        if (n > 1) o->split++;
        if (h > o->max_h) o->max_h = h;
        if (h > o->win_max_h) o->win_max_h = h;
        o->nodes = nodes;
        o->bad_now = bad;
        if (bad) {
            o->bad_calls++;
            if (!o->said_bad) {
                o->said_bad = 1;
                fprintf(stderr,
                        "[physsub] spring %c obj 0x%llx NON-FINITE: node %d (%d of %d nodes) after call %llu, h %.5f s "
                        "(%d step(s)), largest h so far %.5f s\n",
                        'a' + k, (unsigned long long)self, first_bad, bad, nodes, (unsigned long long)o->calls,
                        (double)h, n, (double)o->max_h);
            }
        }
        const uint64_t now = sp_now();
        if (g_sp_last_log == 0) g_sp_last_log = now;
        if (now - g_sp_last_log >= 5000000000ull) {
            g_sp_last_log = now;
            for (int kk = 0; kk < 2; kk++)
                for (int i = 0; i < PS_SPRING_OBJS && g_sp_obj[kk][i].self; i++) {
                    PsSpringObj *q = &g_sp_obj[kk][i];
                    fprintf(stderr,
                            "[physsub] spring %c obj 0x%llx: %llu calls, %llu steps (%llu split), h max %.5f s (5 s) "
                            "%.5f s (ever), nodes %d, non-finite %d (%llu calls)",
                            'a' + kk, (unsigned long long)q->self, (unsigned long long)q->calls,
                            (unsigned long long)q->steps, (unsigned long long)q->split, (double)q->win_max_h,
                            (double)q->max_h, q->nodes, q->bad_now, (unsigned long long)q->bad_calls);
                    if (q == o)
                        fprintf(stderr, ", |v| max %.3g, mid node (%.3f, %.3f, %.3f)", (double)maxv, (double)mid[0],
                                (double)mid[1], (double)mid[2]);
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
        for (int i = 0; i < n; i++)
            ps_call(ctx, w, &a, sub);
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

// ---- texture-name guard (ASTRO BOT level-transition crash) -----------------------------------------------------------
// nps5_astro_texname_guard wraps a title's std::function invoker that forwards (this, *fullname, *name, *idx16, *rec) to
// the texture lookup of a model being loaded (ASTRO BOT 01.007: invoker 0x74587f0 -> 0x746b490). That lookup reads *name
// unchecked when fullname is NULL; on rare level transitions (about 1 in 20 hub -> Sky Garden) the model loader reaches it
// with both NULL and the game dies with a read of guest address 0. Both NULL: the shim returns 0xffff, the lookup's own
// "not found" (the loader then uses its default texture for that entry), and logs the first 20 cases. NPS5_TEXNAME_GUARD=0
// = call the original anyway (crash as before).
static PsWrap g_w_texname = {.shim = "nps5_astro_texname_guard"};
static _Atomic uint64_t g_texname_guarded;

void nps5_astro_texname_guard(GuestContext *ctx) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NPS5_TEXNAME_GUARD");
        on = !(e && *e == '0');
    }
    PsArgs a;
    ps_save(ctx, &a);
    const uint64_t full = a.rdx && a.rdx < ctx->mem_size ? *(const uint64_t *)NPS5_G2H(ctx, a.rdx) : 0;
    const uint64_t name = a.rcx && a.rcx < ctx->mem_size ? *(const uint64_t *)NPS5_G2H(ctx, a.rcx) : 0;
    if (on && !full && !name) {
        const uint64_t n = atomic_fetch_add_explicit(&g_texname_guarded, 1, memory_order_relaxed) + 1;
        if (n <= 20)
            fprintf(stderr,
                    "[texname-guard] model texture entry without a name (%llu so far): no texture instead of a crash\n",
                    (unsigned long long)n);
        ctx->rax = 0xffff;
        SHIM_RETURN();
    }
    void *body = ps_body(&g_w_texname);
    ctx->rdi = a.rdi;
    ctx->rsi = a.rsi;
    ctx->rdx = a.rdx;
    ctx->rcx = a.rcx;
    ctx->r8 = a.r8;
    ctx->r9 = a.r9;
    for (int i = 0; i < 8; i++)
        ctx->xmm[i] = a.xmm[i];
    uint64_t sp = ((a.rsp - 128) & ~15ULL) - 8;
    *(uint64_t *)NPS5_G2H(ctx, sp) = 0;
    ctx->rsp = sp;
    ctx->rip = g_w_texname.guest;
    recomp_call_entry(ctx, (recomp_fn_t)body);
    ctx->rsp = a.rsp;
    SHIM_RETURN();
}
