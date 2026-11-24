// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure decision logic of the physics sub-step switch ([physsub], nps5_physsub.c), free of guest memory so
// tests/physsub/physsub_unit.c can drive it.
//
// With real-time game speed ([framerate], NPS5_FRAMERATE_BLOCK) a title that was written for a fixed 1/60 s step
// sees dt up to 1/20 s. Gameplay code that integrates with dt is fine; a physics engine stepped once per frame with a
// 3x step is not (stiff constraints, joints and springs overshoot). The shims wrap the title's physics step functions
// ("wrap": true in a -hle-replace table): one call with dt becomes n calls with dt/n, each <= 1/hz, so simulated time
// still equals real time and everything else keeps the real dt. Cloth can instead be clamped (one call with min(dt, 1/hz):
// cloth runs slower than real time below hz fps, the AstroHybrid behaviour) or substepped.
//
//   NPS5_PHYS_SUBSTEP=0           off: every shim calls the original once with the unchanged arguments (default on; the
//                                 shims exist only in a program translated with a physsub table, so other titles are untouched)
//   NPS5_PHYS_SUBSTEP_HZ=60       the longest step is 1/hz s (10..240); when set it is used for the world AND cloth
//   NPS5_PHYS_SUBSTEP_WORLD_HZ=25 the world's own hz (10..240; default 25 = steps <= 40 ms: ASTRO BOT's world step ran away at the
//                                 Sky Garden gate with 43-50 ms steps, and splitting frames of normal length (25-40 ms) into n > 1
//                                 world steps makes the walking character sink into the wooden planks; 40-50 ms frames still split
//                                 in 2). Set after NPS5_PHYS_SUBSTEP_HZ, so it wins for the world. Cloth keeps NPS5_PHYS_SUBSTEP_HZ.
//   NPS5_PHYS_SUBSTEP_MAX=4       at most this many steps per call (1..16); a longer dt is split into max equal steps
//   NPS5_PHYS_SUBSTEP_CLOTH=clamp clamp | substep | off  (off = cloth untouched)
//   NPS5_PHYS_SUBSTEP_WORLD=1     0 = rigid-body world untouched (cloth only)
//   NPS5_PHYS_SUBSTEP_LOG=1       one [physsub] line every 5 s (calls, steps, largest dt)
//   NPS5_PHYS_SUBSTEP_DTFIX=1     after a split world step, the step dt / 1/dt the simulation left behind (dt / n, n / dt) are set to the
//                                 frame's in the world (param physsub.world.stepdt) and in the physics engine's solver info (params
//                                 physsub.world.engine = pointer offset, physsub.engine.solver_dt = {dt x4, 1/dt x4}): per-frame readers
//                                 then see one step of the frame dt, as without sub-steps (ASTRO BOT: leaves flicker otherwise). 0 = off.
//   NPS5_PHYS_SUBSTEP_RELOCK=1    a split world step whose original releases the world lock its caller took (params
//                                 physsub.world.lock_obj/.lock_field/.lock_var) gets that lock back before each further sub-step,
//                                 so every sub-step runs under it and releases it once (ASTRO BOT: without it sub-steps 2..n
//                                 unlock as a non-owner, EPERM, the game's Mutex.cpp:418 assert, and simulate unlocked). 0 = off.
//   NPS5_PHYS_SUBSTEP_HOLD=1      (default 1) keep that world lock held through ALL sub-steps of a split world step (the step's release of
//                                 sub-steps 1..n-1 is balanced by one extra lock level), so threads that wait for the lock see the
//                                 frame's physics only when the whole frame is simulated, as with one unsplit step (ASTRO BOT: the
//                                 walking character sinks into floors with plain split; opt-in experiment). Needs the lock params.
//   NPS5_PHYS_SUBSTEP_ATTACH_ONCE=0  1 = the attaching update (wrap nps5_physsub_attach_step; bodies driven to game objects with
//                                 velocity = change / dt) runs once per split frame, first sub-step, frame dt, instead of in every
//                                 sub-step with dt / n (which leaves those bodies with velocity 0 after a moving frame). Opt-in experiment.
//   NPS5_PHYS_SUBSTEP_INNER=0     1 = a world step that needs n > 1 steps runs ONCE with the frame dt and only the engine step inside
//                                 it (wraps nps5_physsub_sim_step / nps5_physsub_pcollide_step) is split into n steps of dt / n: the
//                                 game-side per-call work of the world step (attaching, dynamic mesh rebuild, previous velocities,
//                                 contact / trigger point updates, lock release) runs once per frame, as unsplit (ASTRO BOT: the
//                                 walking character sinks into the Sky Garden planks with the whole-step split). Overrides HOLD /
//                                 ATTACH_ONCE / RELOCK (nothing to do: one world step). Opt-in.
//   NPS5_PHYS_SUBSTEP_WORLD_CLAMP=0  1 = never split the world step: one call with min(dt, 1/hz) (physics slower than real time
//                                 below hz fps; the frame dt is NOT written back unless NPS5_PHYS_SUBSTEP_CLAMP_DTFIX=1). Opt-in alternative to the split.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NPS5_PS_CLOTH_OFF = 0, NPS5_PS_CLOTH_CLAMP = 1, NPS5_PS_CLOTH_SUBSTEP = 2 };

typedef struct {
    int on, world, cloth, log;
    double hz; // cloth (and the world when NPS5_PHYS_SUBSTEP_HZ is set)
    double world_hz; // world step split / clamp
    int max;
} Nps5PhysCfg;

// One number from text in [lo, hi]; NULL keeps *out. Returns 1 when the text was set but not a number in range.
static inline int nps5_ps_num(const char *text, double lo, double hi, double *out) {
    if (!text || !*text) return 0;
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text || *end != 0 || !(v >= lo && v <= hi)) return 1;
    *out = v;
    return 0;
}

// Reads the switches (NULL = not set). Returns how many values were ignored (out of range / unknown word).
static inline int nps5_ps_config(Nps5PhysCfg *c, const char *on, const char *hz, const char *max, const char *cloth,
                                 const char *world, const char *log) {
    int bad = 0;
    double v;
    c->on = 1;
    c->world = 1;
    c->cloth = NPS5_PS_CLOTH_CLAMP;
    c->log = 0;
    c->hz = 60.0;
    c->world_hz = 25.0;
    c->max = 4;
    v = 1;
    bad += nps5_ps_num(on, 0, 1, &v);
    c->on = v == 1.0;
    v = c->hz;
    {
        const int b = nps5_ps_num(hz, 10, 240, &v);
        bad += b;
        if (!b && hz && *hz) c->world_hz = v; // an explicit HZ keeps meaning "every step <= 1/hz"
    }
    c->hz = v;
    v = c->max;
    bad += nps5_ps_num(max, 1, 16, &v);
    c->max = (int)v;
    if (cloth && *cloth) {
        if (!strcmp(cloth, "clamp"))
            c->cloth = NPS5_PS_CLOTH_CLAMP;
        else if (!strcmp(cloth, "substep"))
            c->cloth = NPS5_PS_CLOTH_SUBSTEP;
        else if (!strcmp(cloth, "off") || !strcmp(cloth, "0"))
            c->cloth = NPS5_PS_CLOTH_OFF;
        else
            bad++;
    }
    v = 1;
    bad += nps5_ps_num(world, 0, 1, &v);
    c->world = v == 1.0;
    v = 0;
    bad += nps5_ps_num(log, 0, 1, &v);
    c->log = v == 1.0;
    return bad;
}

// NPS5_PHYS_SUBSTEP_WORLD_HZ (NULL = not set): the world's own hz. Returns 1 when the text was set but not in 10..240.
static inline int nps5_ps_config_world_hz(Nps5PhysCfg *c, const char *world_hz) {
    return nps5_ps_num(world_hz, 10, 240, &c->world_hz);
}

// How many steps of dt / n a step of dt becomes: the least n with dt / n <= 1 / hz (a dt at most 0.1 % over 1/hz
// still counts as one step: the block's 1/60 is a rounded float), at most max. A dt that is not a positive finite
// number is not split (1).
static inline int nps5_ps_steps(double dt, double hz, int max) {
    if (!(dt > 0.0) || !isfinite(dt) || !(hz > 0.0) || max <= 1) return 1;
    const double r = dt * hz;
    if (r <= 1.001) return 1;
    double n = ceil(r - 0.001);
    if (n > (double)max) n = (double)max;
    return n < 1.0 ? 1 : (int)n;
}

// The clamped single step: min(dt, 1 / hz); a dt that is not a positive finite number stays as it is.
static inline float nps5_ps_clamp(float dt, double hz) {
    if (!(dt > 0.0f) || !isfinite(dt) || !(hz > 0.0)) return dt;
    const float m = (float)(1.0 / hz);
    return dt > m ? m : dt;
}

// Clamp mode (WORLD_CLAMP=1): write the frame dt back over the clamped step's values? Only with CLAMP_DTFIX=1 (and DTFIX on) and when the step was
// really clamped. Default 0 (dtexec F1): the stored step dt then equals the dt the engine really stepped with, so per-frame readers match the motion.
static inline int nps5_ps_clamp_writeback(int dtfix, int clamp_dtfix, float stepped, float frame_dt) {
    return dtfix && clamp_dtfix && stepped != frame_dt;
}

// The frame's step values after a split step: world {dt, 1/dt}, engine solver info {dt x4, 1/dt x4} (NULL = not written).
static inline void nps5_ps_frame_dt(float dt, float *world2, float *engine8) {
    if (!(dt > 0.0f) || !isfinite(dt)) return;
    const float inv = 1.0f / dt;
    if (world2) {
        world2[0] = dt;
        world2[1] = inv;
    }
    if (engine8)
        for (int k = 0; k < 4; k++) {
            engine8[k] = dt;
            engine8[4 + k] = inv;
        }
}

// ---- dtexec probes (default off, diagnostics only) ----------------------------------------------------------------------------------------
// NPS5_PHYS_SUBSTEP_BT=1: one guest backtrace from the world shim (the virtual call chain from the main loop to the physics manager).
// Walks the rbp chain: [rsp] = the caller's return address, then ret = [rbp+8], rbp = [rbp]. mem = host address of guest address 0 (NULL in the identity
// address mode: guest pointer == host pointer, as in the title builds), bounds-checked against mem_size, stops on a null / misaligned / out-of-range rbp,
// on an rbp that does not increase or lies more than 32 MiB above rsp (a stale chain must not walk into unmapped memory), and at max entries.
// Returns the number of return addresses written.
static inline int nps5_ps_bt_walk(const uint8_t *mem, uint64_t mem_size, uint64_t rsp, uint64_t rbp, uint64_t *out, int max) {
    int n = 0;
    if (!out || max <= 0) return 0;
    if (rsp && !(rsp & 7) && rsp + 8 <= mem_size) out[n++] = *(const uint64_t *)(mem + rsp);
    uint64_t prev = 0;
    while (n < max) {
        if (!rbp || (rbp & 7) || rbp + 16 > mem_size || rbp <= prev || rbp < rsp || rbp - rsp > (32ull << 20)) break;
        const uint64_t ret = *(const uint64_t *)(mem + rbp + 8);
        const uint64_t next = *(const uint64_t *)(mem + rbp);
        if (!ret) break;
        out[n++] = ret;
        prev = rbp;
        rbp = next;
    }
    return n;
}

// NPS5_PHYS_SUBSTEP_POISON=dt,ratio,maxdt,substep,grav: after an UNSPLIT world step the groups of dt-derived solver-info fields (hknp world block,
// engine pointer + 0x560 = dt) are scaled by what a 2-way split leaves behind (dt/2, 2/dt). Which per-frame reader moves a tile = which group.
enum {
    NPS5_PS_POI_DT = 1,
    NPS5_PS_POI_RATIO = 2,
    NPS5_PS_POI_MAXDT = 4,
    NPS5_PS_POI_SUBSTEP = 8,
    NPS5_PS_POI_GRAV = 16
};
static inline unsigned nps5_ps_poison_group(const char *name) {
    static const char *const n[5] = {"dt", "ratio", "maxdt", "substep", "grav"};
    if (!name) return 0;
    for (int i = 0; i < 5; i++)
        if (!strcmp(name, n[i])) return 1u << i;
    return 0;
}
// hk = host pointer of the block that holds the solver info at hk + 0x560 .. 0x66f (the hknpWorld). Returns the number of floats written.
static inline int nps5_ps_poison(uint8_t *hk, unsigned groups) {
    int w = 0;
    if (!hk) return 0;
#define PS_SCALE(off, cnt, f)                                                                                          \
    do {                                                                                                               \
        float *q = (float *)(hk + (off));                                                                              \
        for (int k_ = 0; k_ < (cnt); k_++)                                                                             \
            q[k_] *= (f);                                                                                              \
        w += (cnt);                                                                                                    \
    } while (0)
    if (groups & NPS5_PS_POI_DT) {
        PS_SCALE(0x560, 4, 0.5f); // dt
        PS_SCALE(0x570, 4, 2.0f); // 1/dt
    }
    if (groups & NPS5_PS_POI_RATIO) PS_SCALE(0x590, 1, 0.5f);
    if (groups & NPS5_PS_POI_MAXDT) {
        PS_SCALE(0x5a0, 1, 0.5f);
        PS_SCALE(0x5b0, 1, 0.5f);
    }
    if (groups & NPS5_PS_POI_SUBSTEP) {
        PS_SCALE(0x5c0, 1, 0.5f);
        PS_SCALE(0x5d0, 1, 2.0f);
    }
    if (groups & NPS5_PS_POI_GRAV) {
        PS_SCALE(0x650, 4, 0.5f);
        PS_SCALE(0x660, 4, 0.5f);
    }
#undef PS_SCALE
    return w;
}
// Cycle: phase 0 = clean baseline, phase k = group k-1 of the list, each lasting `period` world steps; returns the phase of world step `step`.
static inline int nps5_ps_poison_phase(uint64_t step, unsigned period, int ngroups) {
    if (period == 0 || ngroups <= 0) return 0;
    return (int)((step / period) % (uint64_t)(ngroups + 1));
}

// ---- dt clamp for single objects (dtexec, NPS5_PHYS_DTCLAMP_OBJ; table slots nps5_physsub_dtclamp_0..7) --------------------------------------
// Objects whose update takes the frame dt in xmm0[0] and is only stable / only looks right at <= 1/hz (delayed feedback gain grows with dt: the
// armadillo BigLeafPlant, SpringBone dampers, ropes, soft bodies). The shim calls the original ONCE with min(dt, 1/hz); only that object runs
// slower than real time below hz fps. NPS5_PHYS_DTCLAMP_OBJ=all | name,name,... | 0 (default off); NPS5_PHYS_DTCLAMP_HZ=name=hz,name=hz (5..240).
// Names are matched case-insensitively. Returns the bit mask of selected slots (up to 64); *bad = number of unknown / malformed items.
// "all" selects only the first nall slots (the verified, named ones); the generated auto slots (dtauto) are selected one by one by name.
static inline uint64_t nps5_ps_dtclamp_select(const char *sel, const char *const *names, int n, int nall, int *bad) {
    uint64_t m = 0;
    char buf[512], *sv = NULL;
    if (n > 64) n = 64;
    if (nall > n) nall = n;
    if (!sel || !*sel || !strcmp(sel, "0")) return 0;
    if (strlen(sel) >= sizeof buf) {
        if (bad) (*bad)++;
        return 0;
    }
    strcpy(buf, sel);
    for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        if (!strcasecmp(t, "all")) {
            m |= nall >= 64 ? ~0ull : ((1ull << nall) - 1ull);
            continue;
        }
        int hit = 0;
        for (int i = 0; i < n; i++)
            if (names[i] && !strcasecmp(t, names[i])) m |= 1ull << i, hit = 1;
        if (!hit && bad) (*bad)++;
    }
    return m;
}
// hz for one slot: the "name=hz" item of the list, else def. A malformed / out of range item counts in *bad and is ignored.
static inline double nps5_ps_dtclamp_hz(const char *list, const char *name, double def, int *bad) {
    char buf[512], *sv = NULL;
    double r = def;
    if (!list || !*list || strlen(list) >= sizeof buf) return def;
    strcpy(buf, list);
    for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        char *eq = strchr(t, '=');
        if (!eq) {
            if (bad) (*bad)++;
            continue;
        }
        *eq = 0;
        if (strcasecmp(t, name)) continue;
        char *end = NULL;
        const double v = strtod(eq + 1, &end);
        if (end == eq + 1 || *end || !(v >= 5.0 && v <= 240.0)) {
            if (bad) (*bad)++;
        } else
            r = v;
    }
    return r;
}



// ---- counting-wrap probe (dtauto, NPS5_PHYS_PROBE=1; table slots nps5_physsub_probe_0..1023) ----------------------------------------------------
// One generic pass-through shim per wrapped candidate function (every object update that may read the frame dt: the callers of the dt accessors and
// the Update virtual of every vtable). It counts, per function: calls, distinct frames with a call (frame = world step number), the most calls in one
// frame, calls made inside the world step window, whether more than one thread called, and the histogram of xmm0[0] read as a float (the dt
// argument of Update(this, dt)), incl. how often it equals the block's frame dt. tools: work/dtscan/dtprobe.py classifies the rows
// (per-step / per-frame / sparse, dt-arg yes/no) and writes the candidate rows of the DTCLAMP table.
#define NPS5_PR_BINS 7
#define NPS5_PR_INST 64
// Per row: this-pointers (rdi) seen in the CURRENT frame with their call counts, so "the same object updated twice in one frame" (a per-step function,
// sub-stepped by the caller) is told from "many objects, one update each" (per-instance, once per frame). The table is cleared at each new frame; with
// more than NPS5_PR_INST instances in a frame the extra ones are not tracked (inst_over), so maxpt can only be underestimated.
typedef struct {
    uint64_t calls, frames, maxpf, curpf, last_frame, in_world, argok, argframe;
    uint64_t bins[NPS5_PR_BINS];
    float amin, amax;
    uint64_t tid0;
    int multi_thread, started;
    uint64_t thisv[NPS5_PR_INST];
    uint32_t cnt[NPS5_PR_INST];
    uint32_t ninst, maxinst, maxpt, inst_over;
} Nps5PrRow;
// dt histogram bin of an argument in seconds: <8 ms, <15.5, <18 (the 1/60 step), <24, <34, <60, >= 60 ms.
static inline int nps5_pr_bin(float a) {
    const float ms = a * 1000.0f;
    return ms < 8.0f ? 0 : ms < 15.5f ? 1 : ms < 18.0f ? 2 : ms < 24.0f ? 3 : ms < 34.0f ? 4 : ms < 60.0f ? 5 : 6;
}
static inline void nps5_pr_this(Nps5PrRow *r, uint64_t self) {
    unsigned h = (unsigned)(((self >> 4) * 0x9E3779B97F4A7C15ull) >> 58); // 6 bits
    for (int k = 0; k < 8; k++) {
        const unsigned i = (h + (unsigned)k) & (NPS5_PR_INST - 1);
        if (r->cnt[i] && r->thisv[i] == self) {
            if (++r->cnt[i] > r->maxpt) r->maxpt = r->cnt[i];
            return;
        }
        if (!r->cnt[i]) {
            r->thisv[i] = self;
            r->cnt[i] = 1;
            r->ninst++;
            if (r->maxpt < 1) r->maxpt = 1;
            return;
        }
    }
    r->inst_over++;
}
// One call of the wrapped function. frame = the world step counter, tid = any per-thread id, self = rdi, arg = xmm0[0] as a float, frame_dt = the block's dt (0 = unknown).
static inline void nps5_pr_note(Nps5PrRow *r, uint64_t frame, int in_world, uint64_t tid, uint64_t self, float arg, float frame_dt) {
    r->calls++;
    if (in_world) r->in_world++;
    if (!r->started) r->tid0 = tid;
    else if (tid != r->tid0) r->multi_thread = 1;
    if (!r->started || frame != r->last_frame) {
        if (r->curpf > r->maxpf) r->maxpf = r->curpf;
        if (r->ninst > r->maxinst) r->maxinst = r->ninst;
        r->frames++;
        r->curpf = 0;
        r->ninst = 0;
        memset(r->cnt, 0, sizeof r->cnt);
        r->last_frame = frame;
    }
    r->started = 1;
    r->curpf++;
    nps5_pr_this(r, self);
    if (isfinite(arg) && arg > 1e-4f && arg < 0.5f) {
        if (!r->argok || arg < r->amin) r->amin = arg;
        if (!r->argok || arg > r->amax) r->amax = arg;
        r->argok++;
        r->bins[nps5_pr_bin(arg)]++;
        if (frame_dt > 0.0f && fabsf(arg - frame_dt) <= 2e-4f * frame_dt) r->argframe++;
    }
}
static inline uint64_t nps5_pr_maxpf(const Nps5PrRow *r) { return r->curpf > r->maxpf ? r->curpf : r->maxpf; }
static inline uint32_t nps5_pr_maxinst(const Nps5PrRow *r) { return r->ninst > r->maxinst ? r->ninst : r->maxinst; }
// The log line (one per active slot; parsed by dtprobe.py). Returns the length written (snprintf semantics).
static inline int nps5_pr_format(const Nps5PrRow *r, int slot, char *buf, size_t n) {
    return snprintf(buf, n,
                    "[physprobe] s=%d calls=%llu frames=%llu maxpf=%llu inst=%u maxpt=%u over=%u inw=%llu mt=%d argok=%llu argfr=%llu amin=%.5f amax=%.5f bins=%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                    slot, (unsigned long long)r->calls, (unsigned long long)r->frames, (unsigned long long)nps5_pr_maxpf(r), nps5_pr_maxinst(r), r->maxpt, r->inst_over,
                    (unsigned long long)r->in_world, r->multi_thread, (unsigned long long)r->argok, (unsigned long long)r->argframe, (double)r->amin, (double)r->amax,
                    (unsigned long long)r->bins[0], (unsigned long long)r->bins[1], (unsigned long long)r->bins[2], (unsigned long long)r->bins[3], (unsigned long long)r->bins[4],
                    (unsigned long long)r->bins[5], (unsigned long long)r->bins[6]);
}
// Stack arguments for a pass-through call: a candidate may have more than 6 integer arguments, the callee gets its own frame (as ps_call), so the first
// 8 qwords above the return address are copied. dst/src are host pointers of the new / old frame at the return-address slot.
static inline void nps5_pr_copy_stack_args(uint64_t *dst, const uint64_t *src) {
    for (int i = 1; i <= 8; i++) dst[i] = src[i];
}


// ---- F2 whole-update tick split (dtauto spike, NPS5_TICK_SPLIT=0|split|fixed, NPS5_TICK_HZ=60, NPS5_TICK_MAX=3) -----------------------------------
// The game's update root is called n times per frame with the block's dt = tick instead of once with the frame dt. split: n = ceil(dt * hz), tick = dt / n
// (total = dt, every tick <= 1/hz). fixed: exact 1/hz ticks from an accumulator (n = floor(acc / tick), at least 1 for a frame longer than one tick; the carry
// never exceeds one tick; more than max ticks drops the excess time). A dt that is not a positive finite number is one pass-through call.
typedef struct {
    int mode; // 0 off, 1 split, 2 fixed
    double hz;
    int max;
    double acc; // fixed mode carry, seconds
} Nps5Tick;
static inline int nps5_tick_mode(const char *s) {
    if (!s || !*s || !strcmp(s, "0") || !strcasecmp(s, "off")) return 0;
    if (!strcmp(s, "1") || !strcasecmp(s, "split")) return 1;
    if (!strcasecmp(s, "fixed")) return 2;
    return -1; // unknown: the caller warns and stays off
}
// Returns n (>= 1) and writes the dt of each tick to *tick_dt. dt <= 1/hz (the console step) is never split.
static inline int nps5_tick_plan(Nps5Tick *t, double dt, float *tick_dt) {
    *tick_dt = (float)dt;
    if (!t || t->mode <= 0 || !(dt > 0.0) || !isfinite(dt) || !(t->hz > 0.0) || t->max < 1) return 1;
    const double tick = 1.0 / t->hz;
    if (t->mode == 1) {
        const int n = nps5_ps_steps(dt, t->hz, t->max);
        *tick_dt = (float)(dt / n);
        return n;
    }
    if (dt * t->hz <= 1.001) { // a frame no longer than one tick: nothing to split, keep the carry
        return 1;
    }
    t->acc += dt;
    int n = (int)floor(t->acc / tick + 1e-6);
    if (n < 1) n = 1;
    if (n > t->max) n = t->max;
    t->acc -= n * tick;
    if (t->acc < 0.0) t->acc = 0.0;
    if (t->acc > tick) t->acc = 0.0; // dropped time (more than max ticks late)
    *tick_dt = (float)tick;
    return n;
}
