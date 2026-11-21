// SPDX-License-Identifier: GPL-2.0-or-later
//
// Render scale controller, pure logic (no I/O; unit test tests/rscale/rscale_unit.c). The runtime part is nps5_rscale.c.
//
// Two render levels: HIGH (the game's level 0, 720p with the default upscaling) and LOW (its level 1, built at 540p by the
// "render_scale" setting). The controller sees one frame time per game frame (flip to flip) and keeps an exponential
// moving average with a time constant (tau_s). It goes LOW when the average stays over the budget (target_ms * (1 + down))
// for down_s seconds, and back HIGH when the average at LOW, grown by the expected cost of the higher level (gain), stays
// under target_ms * (1 - up) for up_s seconds. Every switch starts a dwell (dwell_s) in which nothing switches; a bounce
// (HIGH -> LOW within bounce_s of the last LOW -> HIGH) forbids the next return to HIGH for backoff_s, doubling per bounce
// up to backoff_max_s, until the scene changes. Frames longer than hitch_ms (loads, shader compiles) are not counted, and
// a decision needs warm_s seconds of counted frames since the last scene change or hitch, and dwell_s since the last switch.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { NPS5_RS_HIGH = 0, NPS5_RS_LOW = 1 };
enum { NPS5_RS_AUTO = 0, NPS5_RS_FORCE_HIGH = 1, NPS5_RS_FORCE_LOW = 2 }; // modes

typedef struct {
    // configuration
    double target_ms, down, up, gain, down_s, up_s, dwell_s, warm_s, tau_s, hitch_ms, bounce_s, backoff_s,
        backoff_max_s;
    // state
    int level, have;
    double ema_ms, t_last, t_switch, t_count0, over_since, under_since, last_up, ban_until, backoff_cur;
    uint32_t switches, bounces;
} Nps5Rs;

static inline void nps5_rs_defaults(Nps5Rs *r, double target_ms) {
    memset(r, 0, sizeof *r);
    r->target_ms = target_ms;
    r->down = 0.05;
    r->up = 0.05;
    r->gain = 0.08; // 540p -> 720p: about +6 % frame time measured (decisions D80); 8 % keeps a margin
    r->down_s = 2.0;
    r->up_s = 8.0;
    r->dwell_s = 20.0;
    r->warm_s = 3.0;
    r->tau_s = 1.0;
    r->hitch_ms = 250.0;
    r->bounce_s = 60.0;
    r->backoff_s = 120.0;
    r->backoff_max_s = 960.0;
    r->level = NPS5_RS_HIGH;
    r->over_since = r->under_since = -1;
    r->last_up = -1e9;
    r->backoff_cur = r->backoff_s;
}

// Forget the measurement (new scene): the level stays, the bounce backoff is reset. No dwell: the first decision of a
// level only waits for warm_s of counted frames (a level change is itself a load, where a switch is not seen).
static inline void nps5_rs_scene(Nps5Rs *r, double now) {
    r->have = 0;
    r->t_count0 = now;
    r->t_switch = now - r->dwell_s;
    r->over_since = r->under_since = -1;
    r->ban_until = 0;
    r->backoff_cur = r->backoff_s;
    r->last_up = -1e9;
}

// Record that the level was set from outside (forced mode, file): the controller continues from there.
static inline void nps5_rs_set_level(Nps5Rs *r, int level, double now) {
    if (r->level == level) return;
    r->level = level;
    r->t_switch = now;
    r->have = 0;
    r->t_count0 = now;
    r->over_since = r->under_since = -1;
}

// One frame: now = seconds (monotonic) at this flip, ms = time since the previous flip. Returns the level the auto mode
// wants (it is also stored in r->level; a change counts as a switch).
static inline int nps5_rs_frame(Nps5Rs *r, double now, double ms) {
    if (!(ms > 0) || ms > r->hitch_ms) { // a load or a compile: restart the count
        r->have = 0;
        r->t_count0 = now;
        r->over_since = r->under_since = -1;
        return r->level;
    }
    if (!r->have) {
        r->ema_ms = ms;
        r->have = 1;
    } else {
        const double a = 1.0 - exp(-(ms * 1e-3) / r->tau_s);
        r->ema_ms += a * (ms - r->ema_ms);
    }
    r->t_last = now;
    if (now - r->t_count0 < r->warm_s || now - r->t_switch < r->dwell_s) {
        r->over_since = r->under_since = -1;
        return r->level;
    }
    if (r->level == NPS5_RS_HIGH) {
        if (r->ema_ms > r->target_ms * (1.0 + r->down)) {
            if (r->over_since < 0) r->over_since = now;
            if (now - r->over_since >= r->down_s) {
                if (now - r->last_up < r->bounce_s) { // came back down soon after going up: hold LOW longer next time
                    r->ban_until = now + r->backoff_cur;
                    r->backoff_cur = fmin(r->backoff_cur * 2.0, r->backoff_max_s);
                    r->bounces++;
                }
                r->level = NPS5_RS_LOW;
                r->switches++;
                r->t_switch = now;
                r->over_since = -1;
            }
        } else {
            r->over_since = -1;
        }
    } else {
        if (r->ema_ms * (1.0 + r->gain) < r->target_ms * (1.0 - r->up) && now >= r->ban_until) {
            if (r->under_since < 0) r->under_since = now;
            if (now - r->under_since >= r->up_s) {
                r->level = NPS5_RS_HIGH;
                r->switches++;
                r->t_switch = now;
                r->last_up = now;
                r->under_since = -1;
            }
        } else {
            r->under_since = -1;
        }
    }
    return r->level;
}

// "auto" | "720" | "540" (also "high"/"low", "p720"/"p540", "full"): the mode, or -1.
static inline int nps5_rs_mode(const char *s) {
    if (!s) return -1;
    while (*s == ' ' || *s == '\t')
        s++;
    char t[16];
    size_t n = 0;
    for (; *s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != ',' && n + 1 < sizeof t; s++)
        t[n++] = *s;
    t[n] = 0;
    if (!strcasecmp(t, "auto")) return NPS5_RS_AUTO;
    if (!strcasecmp(t, "720") || !strcasecmp(t, "p720") || !strcasecmp(t, "high") || !strcasecmp(t, "full"))
        return NPS5_RS_FORCE_HIGH;
    if (!strcasecmp(t, "540") || !strcasecmp(t, "p540") || !strcasecmp(t, "low")) return NPS5_RS_FORCE_LOW;
    return -1;
}

// Per-level table "name=mode,name=mode,*=mode": the mode for scene (or def when the scene is not listed and there is no
// "*"). Returns -2 when the table is malformed. Names are [A-Za-z0-9_]+.
static inline int nps5_rs_table(const char *tab, const char *scene, int def) {
    if (!tab || !*tab) return def;
    int star = -1, hit = -1;
    const char *p = tab;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq || eq == p) return -2;
        const size_t nl = (size_t)(eq - p);
        for (size_t i = 0; i < nl; i++) {
            const char c = p[i];
            if (!(c == '_' || c == '*' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                return -2;
        }
        const int m = nps5_rs_mode(eq + 1);
        if (m < 0) return -2;
        if (nl == 1 && p[0] == '*')
            star = m;
        else if (scene && strlen(scene) == nl && !strncmp(scene, p, nl))
            hit = m;
        const char *c = strchr(eq, ',');
        if (!c) break;
        p = c + 1;
    }
    return hit >= 0 ? hit : star >= 0 ? star : def;
}
