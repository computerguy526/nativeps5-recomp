// Host test of the F2 tick-split plan (nps5_physsub_logic.h: nps5_tick_*). Build/run: tools/tick_unit.sh
#include "../../pkg/runtime/modules/libSceAnyPS5/nps5_physsub_logic.h"
#include <stdio.h>
static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)
int main(void) {
    CHECK(nps5_tick_mode(NULL) == 0 && nps5_tick_mode("") == 0 && nps5_tick_mode("0") == 0 &&
              nps5_tick_mode("off") == 0,
          "off");
    CHECK(nps5_tick_mode("1") == 1 && nps5_tick_mode("Split") == 1 && nps5_tick_mode("fixed") == 2 &&
              nps5_tick_mode("FIXED") == 2 && nps5_tick_mode("bogus") == -1,
          "modes");
    float td;
    // split: n = ceil(dt*hz), tick = dt/n, the total is dt, every tick <= 1/hz, for 0.001 .. 0.3 s
    for (double dt = 0.001; dt < 0.3; dt += 0.00037) {
        Nps5Tick t = {1, 60, 3, 0};
        const int n = nps5_tick_plan(&t, dt, &td);
        CHECK(n >= 1 && n <= 3, "n %d for %.4f", n, dt);
        if (n < 3) CHECK(td <= 1.0f / 60 * 1.0011f, "tick %.5f > 1/60 for dt %.4f (n %d)", (double)td, dt, n);
        CHECK(fabs((double)td * n - dt) < 1e-6 * (1 + n) || n == 3, "total %.6f vs %.6f", (double)td * n, dt);
        if (n == 3) CHECK(fabs((double)td * 3 - dt) < 1e-6, "capped: total still dt (ticks longer than 1/60)");
    }
    {
        Nps5Tick t = {1, 60, 3, 0};
        CHECK(nps5_tick_plan(&t, 0.0268, &td) == 2 && fabs(td - 0.0134) < 1e-6, "26.8 ms -> 2 x 13.4 ms");
        CHECK(nps5_tick_plan(&t, 1.0 / 60, &td) == 1 && td == (float)(1.0 / 60), "1/60 -> pass-through");
        CHECK(nps5_tick_plan(&t, 0.0166, &td) == 1 && td == 0.0166f, "shorter than a tick -> pass-through");
        CHECK(nps5_tick_plan(&t, 0.05, &td) == 3 && fabs(td - 0.05 / 3) < 1e-6, "50 ms -> 3 x 16.7 ms");
        CHECK(nps5_tick_plan(&t, 1.0, &td) == 3, "a stall is capped at max ticks");
        CHECK(nps5_tick_plan(&t, 0.0, &td) == 1 && td == 0.0f, "0 -> pass-through");
        CHECK(nps5_tick_plan(&t, -1.0, &td) == 1 && td == -1.0f, "negative -> pass-through");
        CHECK(nps5_tick_plan(&t, NAN, &td) == 1 && isnan(td), "NaN -> pass-through");
        CHECK(nps5_tick_plan(&t, INFINITY, &td) == 1, "inf -> pass-through");
        Nps5Tick off = {0, 60, 3, 0};
        CHECK(nps5_tick_plan(&off, 0.05, &td) == 1 && td == 0.05f, "mode off -> pass-through");
        CHECK(nps5_tick_plan(NULL, 0.05, &td) == 1, "NULL state");
        Nps5Tick bad = {1, 0, 3, 0};
        CHECK(nps5_tick_plan(&bad, 0.05, &td) == 1, "hz 0");
        Nps5Tick m1 = {1, 60, 1, 0};
        CHECK(nps5_tick_plan(&m1, 0.05, &td) == 1 && td == 0.05f, "max 1 -> pass-through");
    }
    // fixed: exact 1/hz ticks; 26.7 ms frames alternate 1, 2 ticks (1.6 ticks/frame), the carry stays under one tick, the total time is conserved
    {
        Nps5Tick t = {2, 60, 3, 0};
        const double dt = 1.0 / 37.5, tick = 1.0 / 60;
        double total_ticks = 0, total_dt = 0, maxacc = 0;
        int ones = 0, twos = 0;
        for (int f = 0; f < 3750; f++) {
            const int n = nps5_tick_plan(&t, dt, &td);
            CHECK(td == (float)tick, "fixed tick value");
            total_ticks += n;
            total_dt += dt;
            if (n == 1) ones++;
            if (n == 2) twos++;
            if (t.acc > maxacc) maxacc = t.acc;
            CHECK(n >= 1 && n <= 2, "n %d", n);
        }
        CHECK(maxacc <= tick + 1e-9, "carry %.6f exceeds one tick", maxacc);
        CHECK(fabs(total_ticks * tick + t.acc - total_dt) < 1e-6, "time conserved: ticks %.0f carry %.6f dt %.6f",
              total_ticks, t.acc, total_dt);
        CHECK(ones > 1000 && twos > 2000 && ones + twos == 3750, "1/2 pattern: %d x1 %d x2", ones, twos);
        // a long frame drops the excess beyond max ticks and the carry never exceeds one tick
        CHECK(nps5_tick_plan(&t, 0.5, &td) == 3 && t.acc <= tick + 1e-9, "stall: 3 ticks, carry %.6f", t.acc);
        // a frame no longer than one tick is pass-through and keeps the carry
        const double keep = t.acc;
        CHECK(nps5_tick_plan(&t, 1.0 / 60, &td) == 1 && t.acc == keep && td == (float)(1.0 / 60),
              "short frame keeps the carry");
        Nps5Tick bad = {2, 60, 3, 0.005};
        CHECK(nps5_tick_plan(&bad, NAN, &td) == 1 && bad.acc == 0.005, "NaN leaves the carry");
    }
    if (fails) {
        printf("tick_unit: %d FAILED\n", fails);
        return 1;
    }
    printf("tick_unit: all passed\n");
    return 0;
}
