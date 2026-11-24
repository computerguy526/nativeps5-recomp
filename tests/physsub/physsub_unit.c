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
              c.cloth == NPS5_PS_CLOTH_CLAMP && c.hz == 60 && c.world_hz == 25 && c.max == 4 && !c.log,
          "defaults");
    // world default 25 Hz: Sky Garden frames of 25-40 ms are one world step, 40-50 ms two, nothing above 40 ms
    CHECK(nps5_ps_steps(0.030, c.world_hz, c.max) == 1 && nps5_ps_steps(0.040, c.world_hz, c.max) == 1 &&
              nps5_ps_steps(0.045, c.world_hz, c.max) == 2 && nps5_ps_steps(0.050, c.world_hz, c.max) == 2,
          "world 25 Hz steps");
    CHECK(nps5_ps_config_world_hz(&c, NULL) == 0 && c.world_hz == 25, "WORLD_HZ unset changed it");
    CHECK(nps5_ps_config_world_hz(&c, "40") == 0 && c.world_hz == 40 && c.hz == 60, "WORLD_HZ 40");
    CHECK(nps5_ps_config_world_hz(&c, "5") == 1 && c.world_hz == 40, "WORLD_HZ out of range not ignored");
    CHECK(nps5_ps_config(&c, "1", "120", "8", "substep", "0", "1") == 0 && c.on && !c.world &&
              c.cloth == NPS5_PS_CLOTH_SUBSTEP && c.hz == 120 && c.world_hz == 120 && c.max == 8 && c.log,
          "all set (an explicit HZ also sets the world)");
    CHECK(nps5_ps_config(&c, "1", NULL, NULL, "off", NULL, NULL) == 0 && c.cloth == NPS5_PS_CLOTH_OFF, "cloth off");
    CHECK(nps5_ps_config(&c, "yes", "5", "0", "fast", "2", "x") == 6 && c.on && c.hz == 60 && c.world_hz == 25 &&
              c.max == 4 && c.cloth == NPS5_PS_CLOTH_CLAMP && c.world && !c.log,
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
    // clamp write-back: only with both switches and a really clamped step
    CHECK(!nps5_ps_clamp_writeback(1, 0, 0.04f, 0.05f), "clamp write-back default off");
    CHECK(nps5_ps_clamp_writeback(1, 1, 0.04f, 0.05f), "clamp write-back opt-in");
    CHECK(!nps5_ps_clamp_writeback(0, 1, 0.04f, 0.05f), "clamp write-back needs dtfix");
    CHECK(!nps5_ps_clamp_writeback(1, 1, 0.01f, 0.01f), "no write-back of an unclamped step");
    {   // clamp does not touch the stored step dt when the write-back is off
        float w[2] = {(float)(1.0 / 25), 25.0f};
        float c = nps5_ps_clamp(0.05f, 25);
        if (nps5_ps_clamp_writeback(1, 0, c, 0.05f)) nps5_ps_frame_dt(0.05f, w, NULL);
        CHECK(c == (float)(1.0 / 25) && w[0] == (float)(1.0 / 25) && w[1] == 25.0f, "clamped dt kept in the stored step dt");
    }
    // backtrace walker on a synthetic stack (guest "memory" = a host buffer, addresses = offsets)
    {
        static uint64_t mem[256]; // 2 KiB
        const uint64_t sz = sizeof mem;
        mem[0x100 / 8] = 0xAAA;                    // [rsp] = caller return
        mem[0x200 / 8] = 0x300;  mem[0x208 / 8] = 0x111; // frame 1: next rbp 0x300, ret 0x111
        mem[0x300 / 8] = 0x400;  mem[0x308 / 8] = 0x222;
        mem[0x400 / 8] = 0x0;    mem[0x408 / 8] = 0x333; // last frame (next rbp 0)
        uint64_t out[8];
        int n = nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 8);
        CHECK(n == 4 && out[0] == 0xAAA && out[1] == 0x111 && out[2] == 0x222 && out[3] == 0x333, "walk n=%d", n);
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 2) == 2, "max entries");
        mem[0x300 / 8] = 0x200; // next rbp not increasing
        n = nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 8);
        CHECK(n == 3 && out[2] == 0x222, "stops on a non-increasing rbp (n=%d)", n);
        mem[0x300 / 8] = 0x10000; // out of range
        n = nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 8);
        CHECK(n == 3, "stops on an out-of-range rbp (n=%d)", n);
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, sz, 0, 0x203, out, 8) == 0, "misaligned rbp / no rsp");
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x7fc, 0x7f8, out, 8) == 0, "rsp/rbp at the end of memory");
        CHECK(nps5_ps_bt_walk(NULL, sz, 0x100, 0x200, out, 8) == 0 && nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 0) == 0, "bad args");
    }
    // poison groups: each scales only its own fields, default (0 groups) writes nothing
    {
        float blk[0x680 / 4];
        for (int i = 0; i < 0x680 / 4; i++) blk[i] = 1.0f;
        CHECK(nps5_ps_poison((uint8_t *)blk, 0) == 0 && blk[0x560 / 4] == 1.0f, "no groups writes nothing");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_DT) == 8 && blk[0x560 / 4] == 0.5f && blk[0x573 / 4] == 2.0f && blk[0x590 / 4] == 1.0f, "dt group");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_RATIO | NPS5_PS_POI_MAXDT) == 3 && blk[0x590 / 4] == 0.5f && blk[0x5a0 / 4] == 0.5f && blk[0x5b0 / 4] == 0.5f && blk[0x5c0 / 4] == 1.0f, "ratio+maxdt");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_SUBSTEP | NPS5_PS_POI_GRAV) == 10 && blk[0x5c0 / 4] == 0.5f && blk[0x5d0 / 4] == 2.0f && blk[0x650 / 4] == 0.5f && blk[0x66c / 4] == 0.5f && blk[0x670 / 4] == 1.0f, "substep+grav");
        CHECK(nps5_ps_poison(NULL, 31) == 0, "NULL block");
        CHECK(nps5_ps_poison_group("dt") == 1 && nps5_ps_poison_group("grav") == 16 && nps5_ps_poison_group("x") == 0 && nps5_ps_poison_group(NULL) == 0, "group names");
        CHECK(nps5_ps_poison_phase(0, 100, 3) == 0 && nps5_ps_poison_phase(100, 100, 3) == 1 && nps5_ps_poison_phase(399, 100, 3) == 3 && nps5_ps_poison_phase(400, 100, 3) == 0 &&
                  nps5_ps_poison_phase(5, 0, 3) == 0 && nps5_ps_poison_phase(500, 100, 0) == 0, "poison phases");
    }
    if (fails) {
        printf("physsub_unit: %d FAILED\n", fails);
        return 1;
    }
    printf("physsub_unit: all passed\n");
    return 0;
}
