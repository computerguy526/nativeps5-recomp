// Host test of the [physsub] decision logic (pkg/runtime/modules/libSceAnyPS5/nps5_physsub_logic.h). Build/run: tools/physsub_unit.sh
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
    // the console step and anything shorter is one step (the block's 1/60 is a rounded float)
    CHECK(nps5_ps_steps(1.0f / 60.0f, 60, 4) == 1, "1/60 split");
    CHECK(nps5_ps_steps((float)(1.0 / 60.0) * 1.0005f, 60, 4) == 1, "1/60 + rounding split");
    CHECK(nps5_ps_steps(0.010, 60, 4) == 1, "10 ms split");
    // 30 fps -> 2, 20 fps -> 3, 15 fps -> 4, slower -> capped at max
    CHECK(nps5_ps_steps(1.0 / 30, 60, 4) == 2, "30 fps: %d", nps5_ps_steps(1.0 / 30, 60, 4));
    CHECK(nps5_ps_steps(0.045, 60, 4) == 3, "45 ms: %d", nps5_ps_steps(0.045, 60, 4));
    CHECK(nps5_ps_steps(0.050, 60, 4) == 3, "50 ms: %d", nps5_ps_steps(0.050, 60, 4));
    CHECK(nps5_ps_steps(1.0 / 15, 60, 4) == 4, "15 fps: %d", nps5_ps_steps(1.0 / 15, 60, 4));
    CHECK(nps5_ps_steps(0.2, 60, 4) == 4, "cap: %d", nps5_ps_steps(0.2, 60, 4));
    CHECK(nps5_ps_steps(0.050, 30, 8) == 2, "hz 30: %d", nps5_ps_steps(0.050, 30, 8));
    CHECK(nps5_ps_steps(0.050, 60, 1) == 1, "max 1");
    // total simulated time stays dt
    for (double dt = 0.001; dt < 0.3; dt += 0.0007) {
        int n = nps5_ps_steps(dt, 60, 4);
        float sub = (float)dt / (float)n, tot = 0;
        for (int i = 0; i < n; i++)
            tot += sub;
        CHECK(fabs(tot - dt) < 1e-6 + dt * 1e-5, "sum %g != %g", (double)tot, dt);
        CHECK(n == 4 || sub <= 1.0 / 60 * 1.001 + 1e-9, "step %g > 1/60 with n %d < max", (double)sub, n);
    }
    // not a positive finite number: one unchanged step
    CHECK(nps5_ps_steps(0, 60, 4) == 1 && nps5_ps_steps(-0.1, 60, 4) == 1 && nps5_ps_steps(NAN, 60, 4) == 1 &&
              nps5_ps_steps(INFINITY, 60, 4) == 1,
          "bad dt split");
    // clamp
    CHECK(nps5_ps_clamp(0.05f, 60) == (float)(1.0 / 60), "clamp 50 ms");
    CHECK(nps5_ps_clamp(0.01f, 60) == 0.01f, "clamp 10 ms changed");
    CHECK(nps5_ps_clamp(0.0f, 60) == 0.0f && isnan(nps5_ps_clamp(NAN, 60)), "clamp of a bad dt changed it");
    // config
    Nps5PhysCfg c;
    CHECK(nps5_ps_config(&c, NULL, NULL, NULL, NULL, NULL, NULL) == 0 && c.on && c.world &&
              c.cloth == NPS5_PS_CLOTH_CLAMP && c.hz == 60 && c.max == 4 && !c.log,
          "defaults");
    CHECK(nps5_ps_config(&c, "1", "120", "8", "substep", "0", "1") == 0 && c.on && !c.world &&
              c.cloth == NPS5_PS_CLOTH_SUBSTEP && c.hz == 120 && c.max == 8 && c.log,
          "all set");
    CHECK(nps5_ps_config(&c, "1", NULL, NULL, "off", NULL, NULL) == 0 && c.cloth == NPS5_PS_CLOTH_OFF, "cloth off");
    CHECK(nps5_ps_config(&c, "yes", "5", "0", "fast", "2", "x") == 6 && c.on && c.hz == 60 && c.max == 4 &&
              c.cloth == NPS5_PS_CLOTH_CLAMP && c.world && !c.log,
          "bad values not ignored");
    CHECK(nps5_ps_config(&c, "0", NULL, NULL, NULL, NULL, NULL) == 0 && !c.on, "0 = off");
    // dtfix: the frame's step values
    {
        float w[2] = {0.1f, 10.0f}, e[8] = {0};
        nps5_ps_frame_dt(0.05f, w, e);
        CHECK(w[0] == 0.05f && w[1] == 1.0f / 0.05f && e[0] == 0.05f && e[3] == 0.05f && e[4] == 1.0f / 0.05f &&
                  e[7] == 1.0f / 0.05f,
              "frame dt");
        nps5_ps_frame_dt(NAN, w, e);
        nps5_ps_frame_dt(0.0f, w, NULL);
        CHECK(w[0] == 0.05f && e[0] == 0.05f, "bad dt written");
    }
    if (fails) {
        printf("physsub_unit: %d FAILED\n", fails);
        return 1;
    }
    printf("physsub_unit: all passed\n");
    return 0;
}
