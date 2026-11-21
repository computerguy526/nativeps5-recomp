// SPDX-License-Identifier: GPL-2.0-or-later
//
// Render scale: the live part of a patch profile's choice setting "render_scale" (patchpoints.go, operations "override"
// and "data32"). ASTRO BOT keeps five pipeline sets of its own dynamic resolution and its application picks one every
// frame; the profile turns that pick (and the scene's light-tile level) into override sites, builds level 1 at 540p and
// lists the level-1 light-tile table words as data32 sites. This file decides the level while the game runs and writes the
// override slots; the game's own "level changed" path does the switch (MetalFX follows the new input size by itself).
//
// Options of the setting (settings file patch.render_scale=..., dialog, NPS5_PATCH_RENDER_SCALE): "auto" (default),
// "p540", "p720" (= the game untouched: every site keeps the game's value, nothing here runs).
//   NPS5_RENDER=auto|720|540        the same choice, easier to type (wins over the settings file).
//   NPS5_RENDER_LEVELS=<scene>=auto|720|540,...,*=<mode>
//                                   per level ("[scene-open]" names, e.g. underwater_aerial_garden); unlisted levels use
//                                   NPS5_RENDER / the setting.
//   NPS5_RENDER_TARGET_MS=<ms>      frame-time budget of auto (default 33.3 = 30 fps).
//   NPS5_RENDER_TUNE=down=0.05,up=0.05,gain=0.08,down_s=2,up_s=8,dwell_s=20,warm_s=3,tau_s=1
//   NPS5_RENDER_SWITCH=any|scene    any (default): switch mid-level; scene: a decision waits for the next level/load.
//   NPS5_RENDER_FILE=<path>         a file holding auto|720|540 overrides the mode while it exists (checked twice a second).
//   NPS5_RENDER_LOG=0|1|2           0 = switches only, 1 (default) = + a status line every 30 s, 2 = + every 5 s.
// The light-tile table words are written once, at the first switch to 540p, and not before flip 600 (the GPU backend's
// own level-0 table fix runs in the first flips and refuses a table it does not expect); until then the level stays 720p.
// Every switch is one "[rscale]" line on stderr.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nps5_mem.h"
#include "nps5_patches.h"
#include "nps5_rscale_logic.h"

const char *nps5_vfs_current_scene(void); // kernel/vfs/ps4_vfs.c

#define RS_KEY "render_scale"
#define RS_MAX_SITES 16
#define RS_DATA_FLIP 600
#define RS_RING 1024 // frames kept for the per-switch trace (+-2 s; 1024 frames = 17 s at 60 fps)

static struct {
    int state; // 0 = not started, 1 = live, -1 = off
    int choice; // index of the choice setting
    int nover, ndata;
    uint32_t over[RS_MAX_SITES];
    uint64_t data_addr[RS_MAX_SITES];
    uint32_t data_new[RS_MAX_SITES], data_old[RS_MAX_SITES];
    int data_done; // 0 not yet, 1 written, -1 refused (LOW never used)
    int mode_env, mode_cur, switch_scene, log;
    const char *levels;
    char file[1024];
    Nps5Rs r;
    uint64_t t0, last_ns, last_log_ns, last_file_ns;
    uint32_t flips;
    const char *scene;
    int file_mode, applied; // the level the slots hold
    int pending; // NPS5_RENDER_SWITCH=scene: a decision waiting for a level change or a load (-1 = none)
    double sum_ms[2];
    uint32_t n_ms[2];
    double ring_t[RS_RING], ring_ms[RS_RING]; // per-switch trace: the frames around a switch
    uint32_t ring_n;
    double trace_at; // switch time waiting for its +2 s report (0 = none)
    int trace_from, trace_to;
    uint32_t trace_no;
} g_rs = {.choice = -1, .mode_env = -1, .file_mode = -1, .log = 1, .pending = -1};

uint64_t *nps5_choices_values(void); // nps5_patches.c (not in the header: generated code includes it)
static _Atomic(uint64_t) *rs_slots(void) { return (_Atomic(uint64_t) *)nps5_choices_values(); }

static int rs_find_choice(void) {
    for (int i = 0; i < nps5_choices_count(); i++) {
        const Nps5ChoiceDesc *d = nps5_choices_desc(i);
        if (d && d->key && !strcmp(d->key, RS_KEY)) return i;
    }
    return -1;
}
static int rs_option(const Nps5ChoiceDesc *d, const char *name) {
    for (uint32_t k = 0; k < d->noptions; k++)
        if (!strcmp(d->names[k], name)) return (int)k;
    return -1;
}

// Before the choices are resolved: NPS5_RENDER picks the option ("720" without a per-level table = the game untouched).
void nps5_rscale_preresolve(void) {
    const int i = rs_find_choice();
    if (i < 0) return;
    const Nps5ChoiceDesc *d = nps5_choices_desc(i);
    const char *env = getenv("NPS5_RENDER"), *lv = getenv("NPS5_RENDER_LEVELS");
    if (!env || !*env) return;
    const int m = nps5_rs_mode(env);
    if (m < 0) {
        fprintf(stderr, "[rscale] NPS5_RENDER=%s is not auto|720|540 (ignored)\n", env);
        return;
    }
    g_rs.mode_env = m;
    const char *name = m == NPS5_RS_AUTO ? "auto" : m == NPS5_RS_FORCE_LOW ? "p540" : (lv && *lv) ? "auto" : "p720";
    const int o = rs_option(d, name);
    if (o >= 0) nps5_choices_set(i, o);
}

static double rs_now(uint64_t ns) { return (double)(ns - g_rs.t0) * 1e-9; }

static int rs_tune(Nps5Rs *r, const char *s) {
    struct {
        const char *k;
        double *v;
        double lo, hi;
    } t[] = {
        {"down", &r->down, 0, 1},
        {"up", &r->up, 0, 1},
        {"gain", &r->gain, 0, 1},
        {"down_s", &r->down_s, 0, 600},
        {"up_s", &r->up_s, 0, 600},
        {"dwell_s", &r->dwell_s, 0, 3600},
        {"warm_s", &r->warm_s, 0, 600},
        {"tau_s", &r->tau_s, 0.01, 60},
        {"hitch_ms", &r->hitch_ms, 20, 5000},
        {"backoff_s", &r->backoff_s, 0, 3600},
    };
    int bad = 0;
    while (s && *s) {
        const char *eq = strchr(s, '='), *c = strchr(s, ',');
        if (!eq || (c && c < eq)) return 1;
        int hit = 0;
        for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
            if (strlen(t[k].k) == (size_t)(eq - s) && !strncmp(s, t[k].k, (size_t)(eq - s))) {
                char *end;
                const double v = strtod(eq + 1, &end);
                if (end == eq + 1 || v < t[k].lo || v > t[k].hi)
                    bad = 1;
                else
                    *t[k].v = v;
                hit = 1;
            }
        }
        bad |= !hit;
        s = c ? c + 1 : NULL;
    }
    r->backoff_cur = r->backoff_s;
    return bad;
}

// After the choices are resolved (the slots hold the chosen option; the guest has not started).
void nps5_rscale_start(void) {
    if (g_rs.state) return;
    g_rs.state = -1;
    const int i = rs_find_choice();
    if (i < 0) return;
    const Nps5ChoiceDesc *d = nps5_choices_desc(i);
    const int o = nps5_choices_get(i);
    if (!strcmp(d->names[o], "p720") || !rs_slots()) return; // the game untouched
    for (uint32_t k = 0; k < d->nvalues; k++) {
        const Nps5ChoiceValue *v = &d->values[k];
        if ((v->f32 == 2 || v->f32 == 4) && g_rs.nover < RS_MAX_SITES) g_rs.over[g_rs.nover++] = v->slot;
        if (v->f32 == 3 && g_rs.ndata < RS_MAX_SITES) {
            g_rs.data_addr[g_rs.ndata] = v->site;
            g_rs.data_new[g_rs.ndata] = (uint32_t)v->values[o];
            g_rs.data_old[g_rs.ndata] = (uint32_t)v->values[d->noptions]; // the build's own word follows the options
            g_rs.ndata++;
        }
    }
    if (!g_rs.nover) {
        fprintf(stderr, "[rscale] setting %s has no override sites: off\n", RS_KEY);
        return;
    }
    g_rs.choice = i;
    g_rs.mode_cur = g_rs.mode_env >= 0             ? g_rs.mode_env
                    : !strcmp(d->names[o], "p540") ? NPS5_RS_FORCE_LOW
                                                   : NPS5_RS_AUTO;
    g_rs.levels = getenv("NPS5_RENDER_LEVELS");
    if (g_rs.levels && nps5_rs_table(g_rs.levels, "", 0) == -2) {
        fprintf(stderr, "[rscale] NPS5_RENDER_LEVELS=%s: want <level>=auto|720|540[,...][,*=<mode>] (ignored)\n",
                g_rs.levels);
        g_rs.levels = NULL;
    }
    double target = 1000.0 / 30.0;
    const char *t = getenv("NPS5_RENDER_TARGET_MS");
    if (t && *t) {
        char *end;
        const double v = strtod(t, &end);
        if (end != t && v >= 4 && v <= 500)
            target = v;
        else
            fprintf(stderr, "[rscale] NPS5_RENDER_TARGET_MS=%s not 4..500 (using %.1f)\n", t, target);
    }
    nps5_rs_defaults(&g_rs.r, target);
    if (rs_tune(&g_rs.r, getenv("NPS5_RENDER_TUNE")))
        fprintf(stderr, "[rscale] NPS5_RENDER_TUNE: an unknown key or a value out of range was ignored\n");
    const char *sw = getenv("NPS5_RENDER_SWITCH");
    g_rs.switch_scene = sw && !strcmp(sw, "scene");
    const char *lg = getenv("NPS5_RENDER_LOG");
    if (lg && *lg) g_rs.log = atoi(lg);
    const char *f = getenv("NPS5_RENDER_FILE");
    if (f && *f) snprintf(g_rs.file, sizeof g_rs.file, "%s", f);
    g_rs.t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_rs.state = 1;
    static const char *const mn[] = {"auto", "720p", "540p"};
    fprintf(stderr,
            "[rscale] live: mode %s, target %.1f ms (low above %.1f ms for %.0f s, back below %.1f ms for %.0f s, "
            "dwell %.0f s), "
            "%d override + %d table site%s, switch %s%s%s\n",
            mn[g_rs.mode_cur], target, target * (1 + g_rs.r.down), g_rs.r.down_s,
            target * (1 - g_rs.r.up) / (1 + g_rs.r.gain), g_rs.r.up_s, g_rs.r.dwell_s, g_rs.nover, g_rs.ndata,
            g_rs.ndata == 1 ? "" : "s", g_rs.switch_scene ? "at level changes" : "any frame",
            g_rs.levels ? ", levels " : "", g_rs.levels ? g_rs.levels : "");
}

// The build's words at the data sites, then the option's words: all or nothing. 1 = done, 0 = not yet, -1 = never.
static int rs_data_write(void) {
    if (!g_rs.ndata) return 1;
    if (!g_nps5_arena.bias) return 0;
    for (int k = 0; k < g_rs.ndata; k++) {
        uint32_t cur;
        mach_vm_size_t got = 0;
        if (!nps5_guest_range_ok(g_rs.data_addr[k], 4) ||
            mach_vm_read_overwrite(mach_task_self(),
                                   (mach_vm_address_t)(uintptr_t)(g_nps5_arena.bias + g_rs.data_addr[k]), 4,
                                   (mach_vm_address_t)(uintptr_t)&cur, &got) != KERN_SUCCESS ||
            got != 4)
            return 0;
        if (cur != g_rs.data_old[k] && cur != g_rs.data_new[k]) {
            fprintf(stderr, "[rscale] table word 0x%llx holds %u, not %u: 540p stays off\n",
                    (unsigned long long)g_rs.data_addr[k], cur, g_rs.data_old[k]);
            return -1;
        }
    }
    const size_t pg = (size_t)getpagesize();
    for (int k = 0; k < g_rs.ndata; k++) {
        uint8_t *h = g_nps5_arena.bias + g_rs.data_addr[k];
        void *page = (void *)((uintptr_t)h & ~(pg - 1));
        mach_vm_address_t ra = (mach_vm_address_t)(uintptr_t)page;
        mach_vm_size_t rsz = 0;
        vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rsz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) !=
                KERN_SUCCESS ||
            ra > (mach_vm_address_t)(uintptr_t)h) {
            fprintf(stderr, "[rscale] table word 0x%llx: no region (%d of %d written)\n",
                    (unsigned long long)g_rs.data_addr[k], k, g_rs.ndata);
            return -1;
        }
        const int writable = (bi.protection & VM_PROT_WRITE) != 0;
        if (!writable && mprotect(page, pg, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "[rscale] table word 0x%llx: mprotect failed (%d of %d written)\n",
                    (unsigned long long)g_rs.data_addr[k], k, g_rs.ndata);
            return -1;
        }
        memcpy(h, &g_rs.data_new[k], 4);
        if (!writable)
            mprotect(page, pg,
                     (bi.protection & VM_PROT_READ ? PROT_READ : 0) |
                         (bi.protection & VM_PROT_EXECUTE ? PROT_EXEC : 0));
    }
    fprintf(stderr, "[rscale] %d table word%s for 540p written (first 0x%llx: %u -> %u)\n", g_rs.ndata,
            g_rs.ndata == 1 ? "" : "s", (unsigned long long)g_rs.data_addr[0], g_rs.data_old[0], g_rs.data_new[0]);
    return 1;
}

static void rs_apply(int level, const char *why) {
    if (level == g_rs.applied) return;
    if (level == NPS5_RS_LOW && g_rs.data_done <= 0) {
        if (g_rs.data_done < 0 || g_rs.flips < RS_DATA_FLIP) return;
        g_rs.data_done = rs_data_write();
        if (g_rs.data_done <= 0) return;
    }
    _Atomic(uint64_t) *s = rs_slots();
    for (int k = 0; k < g_rs.nover; k++)
        atomic_store_explicit(&s[g_rs.over[k]], (uint64_t)level, memory_order_release);
    const int from = g_rs.applied;
    g_rs.applied = level;
    const double now = rs_now(g_rs.last_ns);
    g_rs.trace_at = now;
    g_rs.trace_from = from;
    g_rs.trace_to = level;
    nps5_rs_set_level(&g_rs.r, level, now);
    fprintf(stderr, "[rscale] %s -> %s (%s; ema %.1f ms, target %.1f; scene %s) t=%.1f\n", from ? "540p" : "720p",
            level ? "540p" : "720p", why, g_rs.r.ema_ms, g_rs.r.target_ms,
            g_rs.scene && *g_rs.scene ? g_rs.scene : "(none)", now);
}

static void rs_file_poll(uint64_t now) {
    if (!g_rs.file[0] || now - g_rs.last_file_ns < 500000000ull) return;
    g_rs.last_file_ns = now;
    int m = -1;
    FILE *f = fopen(g_rs.file, "r");
    if (f) {
        char b[32] = {0};
        if (fgets(b, sizeof b, f)) m = nps5_rs_mode(b);
        fclose(f);
        if (m < 0) m = g_rs.file_mode; // unreadable: keep
    }
    if (m != g_rs.file_mode) {
        fprintf(stderr, "[rscale] %s: %s\n", g_rs.file,
                m < 0    ? "gone, the configured mode applies again"
                : m == 0 ? "auto"
                : m == 1 ? "720p"
                         : "540p");
        g_rs.file_mode = m;
    }
}

// Worst frame, frames over 50 ms and the mean of the ring frames with lo <= t < hi.
static void rs_window(double lo, double hi, double *worst, uint32_t *n50, uint32_t *n, double *mean) {
    *worst = 0;
    *n50 = *n = 0;
    double sum = 0;
    const uint32_t k0 = g_rs.ring_n > RS_RING ? g_rs.ring_n - RS_RING : 0;
    for (uint32_t k = k0; k < g_rs.ring_n; k++) {
        const double t = g_rs.ring_t[k % RS_RING], m = g_rs.ring_ms[k % RS_RING];
        if (t < lo || t >= hi) continue;
        (*n)++;
        sum += m;
        if (m > *worst) *worst = m;
        if (m > 50) (*n50)++;
    }
    *mean = *n ? sum / *n : 0;
}
// 2 s after a switch: one line with the +-2 s window around it and the 4 s before it (the switch's cost in frame time).
static void rs_trace(double now) {
    if (g_rs.trace_at <= 0 || now < g_rs.trace_at + 2.0) return;
    const double s = g_rs.trace_at;
    g_rs.trace_at = 0;
    double w1, w0, m1, m0;
    uint32_t a1, a0, n1, n0;
    rs_window(s - 2, s + 2, &w1, &a1, &n1, &m1);
    rs_window(s - 6, s - 2, &w0, &a0, &n0, &m0);
    fprintf(stderr,
            "[rscale] switch #%u %s -> %s at t=%.1f: +-2 s worst %.1f ms, >50 ms %u of %u, mean %.1f | 4 s before: "
            "worst %.1f ms, >50 ms %u of %u, mean %.1f\n",
            ++g_rs.trace_no, g_rs.trace_from ? "540p" : "720p", g_rs.trace_to ? "540p" : "720p", s, w1, a1, n1, m1, w0,
            a0, n0, m0);
}

// Every game frame (the flip packet), on the game's thread.
void nps5_rscale_flip(void) {
    if (g_rs.state <= 0) return;
    const uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const double ms = g_rs.last_ns ? (double)(ns - g_rs.last_ns) * 1e-6 : 0;
    g_rs.last_ns = ns;
    g_rs.flips++;
    const double now = rs_now(ns);
    rs_file_poll(ns);
    const char *sc = nps5_vfs_current_scene();
    const int scene_changed = sc != g_rs.scene;
    if (scene_changed) {
        g_rs.scene = sc;
        nps5_rs_scene(&g_rs.r, now);
    }
    int mode = g_rs.file_mode >= 0 ? g_rs.file_mode : nps5_rs_table(g_rs.levels, sc, g_rs.mode_cur);
    if (mode < 0) mode = g_rs.mode_cur;
    const int low_ok = g_rs.data_done > 0 || (g_rs.data_done == 0 && g_rs.flips >= RS_DATA_FLIP);
    const int hitch = ms > g_rs.r.hitch_ms;
    int want = g_rs.applied;
    const char *why = "";
    if (mode == NPS5_RS_FORCE_HIGH) {
        want = NPS5_RS_HIGH;
        why = "720p set";
        g_rs.pending = -1;
    } else if (mode == NPS5_RS_FORCE_LOW) {
        want = NPS5_RS_LOW;
        why = "540p set";
        g_rs.pending = -1;
    } else {
        // the measurement runs at the applied level; a decision the slots cannot follow yet is taken back
        if (g_rs.r.level != g_rs.applied) nps5_rs_set_level(&g_rs.r, g_rs.applied, now);
        const int d = nps5_rs_frame(&g_rs.r, now, ms);
        if (d == NPS5_RS_LOW && !low_ok)
            g_rs.r.level = g_rs.applied;
        else if (d != g_rs.applied && g_rs.switch_scene) {
            g_rs.pending = d; // applied at the next level change or load
            g_rs.r.level = g_rs.applied;
        } else if (d != g_rs.applied) {
            want = d;
            why = d ? "auto: over budget" : "auto: headroom";
        }
        if (g_rs.pending >= 0 && (scene_changed || hitch)) {
            want = g_rs.pending;
            why = want ? "auto: over budget, at a level change/load" : "auto: headroom, at a level change/load";
            g_rs.pending = -1;
        }
    }
    if (ms > 0) {
        g_rs.ring_t[g_rs.ring_n % RS_RING] = now;
        g_rs.ring_ms[g_rs.ring_n % RS_RING] = ms;
        g_rs.ring_n++;
    }
    rs_trace(now);
    if (ms > 0 && ms <= g_rs.r.hitch_ms) {
        g_rs.sum_ms[g_rs.applied] += ms;
        g_rs.n_ms[g_rs.applied]++;
    }
    rs_apply(want, why);
    const uint64_t every = g_rs.log >= 2 ? 5000000000ull : 30000000000ull;
    if (g_rs.log >= 1 && ns - g_rs.last_log_ns >= every) {
        if (g_rs.last_log_ns)
            fprintf(stderr,
                    "[rscale] status t=%.0f: %s, ema %.1f ms, mean 720p %.1f ms (%u fr) / 540p %.1f ms (%u fr), %u "
                    "switches, %u bounces, scene %s\n",
                    now, g_rs.applied ? "540p" : "720p", g_rs.r.ema_ms,
                    g_rs.n_ms[0] ? g_rs.sum_ms[0] / g_rs.n_ms[0] : 0, g_rs.n_ms[0],
                    g_rs.n_ms[1] ? g_rs.sum_ms[1] / g_rs.n_ms[1] : 0, g_rs.n_ms[1], g_rs.r.switches, g_rs.r.bounces,
                    sc && *sc ? sc : "(none)");
        g_rs.last_log_ns = ns;
    }
}
