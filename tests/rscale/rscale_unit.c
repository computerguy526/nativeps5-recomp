// Unit test of the render-scale controller logic (pkg/runtime/core/nps5_rscale_logic.h).
//   clang -std=c11 -O1 -g -fsanitize=address,undefined -I pkg/runtime/core tests/rscale/rscale_unit.c -o /tmp/rscale_unit && /tmp/rscale_unit
#include <assert.h>
#include <stdio.h>

#include "nps5_rscale_logic.h"

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

// Feeds frames of ms_high (at HIGH) / ms_low (at LOW) for secs seconds; returns the number of level changes.
static int run(Nps5Rs *r, double *t, double secs, double ms_high, double ms_low) {
    int changes = 0, last = r->level;
    const double end = *t + secs;
    while (*t < end) {
        const double ms = r->level == NPS5_RS_HIGH ? ms_high : ms_low;
        *t += ms * 1e-3;
        nps5_rs_frame(r, *t, ms);
        if (r->level != last) changes++, last = r->level;
    }
    return changes;
}

int main(void) {
    Nps5Rs r;
    double t = 100;
    // A light level holds the budget: never switches.
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    CHECK(run(&r, &t, 300, 25, 23) == 0 && r.level == NPS5_RS_HIGH);
    // Sky Garden on the laptop: 50 ms at 720p, 47 at 540p: goes low once and stays (no oscillation).
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    const double t0 = t;
    CHECK(run(&r, &t, 600, 50, 47) == 1 && r.level == NPS5_RS_LOW);
    // ... after the warm-up (3 s) plus the 2 s sustain at a level start, not before
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    const double ts = t;
    run(&r, &t, 4.8, 50, 47);
    CHECK(r.level == NPS5_RS_HIGH);
    run(&r, &t, 0.4, 50, 47);
    CHECK(r.level == NPS5_RS_LOW);
    // ... and the next switch waits for the dwell (20 s) even with headroom
    run(&r, &t, 19, 20, 18);
    CHECK(r.level == NPS5_RS_LOW);
    (void)t0;
    (void)ts;
    // Borderline: 720p a little over, 540p a little under the budget: the gain margin keeps it at LOW (no ping-pong).
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    CHECK(run(&r, &t, 900, 35.5, 32.0) == 1 && r.level == NPS5_RS_LOW);
    // A level that gets lighter: LOW, then back HIGH after up_s of real headroom (20 ms at 540p).
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    run(&r, &t, 60, 50, 47);
    CHECK(r.level == NPS5_RS_LOW);
    CHECK(run(&r, &t, 60, 22, 20) == 1 && r.level == NPS5_RS_HIGH);
    // Bounce: back up, then over budget within 60 s: down again and the next return is banned for 120 s.
    run(&r, &t, 25, 40, 30);
    CHECK(r.level == NPS5_RS_LOW && r.bounces == 1);
    const double tb = t;
    while (t < tb + 100) {
        t += 0.02;
        nps5_rs_frame(&r, t, 20);
    }
    CHECK(r.level == NPS5_RS_LOW); // banned
    while (t < tb + 140) {
        t += 0.02;
        nps5_rs_frame(&r, t, 20);
    }
    CHECK(r.level == NPS5_RS_HIGH); // ban over
    // Hitches (loads) are not counted and restart the warm-up.
    nps5_rs_defaults(&r, 33.3);
    nps5_rs_scene(&r, t);
    for (int i = 0; i < 2000; i++) {
        t += (i % 20 == 0) ? 1.0 : 0.02;
        nps5_rs_frame(&r, t, (i % 20 == 0) ? 1000 : 20);
    }
    CHECK(r.level == NPS5_RS_HIGH && r.ema_ms < 21);
    // Modes and the per-level table.
    CHECK(nps5_rs_mode("auto") == NPS5_RS_AUTO && nps5_rs_mode("720") == NPS5_RS_FORCE_HIGH &&
          nps5_rs_mode(" 540\n") == NPS5_RS_FORCE_LOW && nps5_rs_mode("1080") == -1 && nps5_rs_mode(NULL) == -1 &&
          nps5_rs_mode("LOW") == NPS5_RS_FORCE_LOW);
    CHECK(nps5_rs_table("underwater_aerial_garden=540,*=720", "underwater_aerial_garden", 0) == NPS5_RS_FORCE_LOW);
    CHECK(nps5_rs_table("underwater_aerial_garden=540,*=720", "hub", 0) == NPS5_RS_FORCE_HIGH);
    CHECK(nps5_rs_table("a=540,b=auto", "c", NPS5_RS_FORCE_HIGH) == NPS5_RS_FORCE_HIGH);
    CHECK(nps5_rs_table("a=540,b=auto", "b", NPS5_RS_FORCE_HIGH) == NPS5_RS_AUTO);
    CHECK(nps5_rs_table("a=9", "a", 0) == -2 && nps5_rs_table("=540", "a", 0) == -2 &&
          nps5_rs_table("a b=540", "a", 0) == -2);
    CHECK(nps5_rs_table(NULL, "a", 1) == 1 && nps5_rs_table("", "a", 2) == 2);
    if (fails) return 1;
    printf("rscale_unit: ok\n");
    return 0;
}
