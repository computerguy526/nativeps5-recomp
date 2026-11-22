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
//   NPS5_PHYS_SUBSTEP_HZ=60       the longest step is 1/hz s (10..240)
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
//   NPS5_PHYS_SUBSTEP_HOLD=0      1 = keep that world lock held through ALL sub-steps of a split world step (the step's release of
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
//                                 below hz fps; dtfix writes the frame dt back). Opt-in alternative to the split.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { NPS5_PS_CLOTH_OFF = 0, NPS5_PS_CLOTH_CLAMP = 1, NPS5_PS_CLOTH_SUBSTEP = 2 };

typedef struct {
    int on, world, cloth, log;
    double hz;
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
    c->max = 4;
    v = 1;
    bad += nps5_ps_num(on, 0, 1, &v);
    c->on = v == 1.0;
    v = c->hz;
    bad += nps5_ps_num(hz, 10, 240, &v);
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
