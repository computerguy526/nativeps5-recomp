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
    { // clamp does not touch the stored step dt when the write-back is off
        float w[2] = {(float)(1.0 / 25), 25.0f};
        float c = nps5_ps_clamp(0.05f, 25);
        if (nps5_ps_clamp_writeback(1, 0, c, 0.05f)) nps5_ps_frame_dt(0.05f, w, NULL);
        CHECK(c == (float)(1.0 / 25) && w[0] == (float)(1.0 / 25) && w[1] == 25.0f,
              "clamped dt kept in the stored step dt");
    }
    // backtrace walker on a synthetic stack (guest "memory" = a host buffer, addresses = offsets)
    {
        static uint64_t mem[256]; // 2 KiB
        const uint64_t sz = sizeof mem;
        mem[0x100 / 8] = 0xAAA; // [rsp] = caller return
        mem[0x200 / 8] = 0x300;
        mem[0x208 / 8] = 0x111; // frame 1: next rbp 0x300, ret 0x111
        mem[0x300 / 8] = 0x400;
        mem[0x308 / 8] = 0x222;
        mem[0x400 / 8] = 0x0;
        mem[0x408 / 8] = 0x333; // last frame (next rbp 0)
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
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, 1ull << 30, 0x100, 0x4000100, out, 8) == 1,
              "rbp more than 32 MiB above rsp is not followed (only [rsp])");
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x200, 0x100, out, 8) == 1, "rbp below rsp is not followed");
        CHECK(nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, NULL, 8) == 0 &&
                  nps5_ps_bt_walk((const uint8_t *)mem, sz, 0x100, 0x200, out, 0) == 0,
              "bad args");
    }
    // poison groups: each scales only its own fields, default (0 groups) writes nothing
    {
        float blk[0x680 / 4];
        for (int i = 0; i < 0x680 / 4; i++)
            blk[i] = 1.0f;
        CHECK(nps5_ps_poison((uint8_t *)blk, 0) == 0 && blk[0x560 / 4] == 1.0f, "no groups writes nothing");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_DT) == 8 && blk[0x560 / 4] == 0.5f && blk[0x573 / 4] == 2.0f &&
                  blk[0x590 / 4] == 1.0f,
              "dt group");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_RATIO | NPS5_PS_POI_MAXDT) == 3 && blk[0x590 / 4] == 0.5f &&
                  blk[0x5a0 / 4] == 0.5f && blk[0x5b0 / 4] == 0.5f && blk[0x5c0 / 4] == 1.0f,
              "ratio+maxdt");
        CHECK(nps5_ps_poison((uint8_t *)blk, NPS5_PS_POI_SUBSTEP | NPS5_PS_POI_GRAV) == 10 && blk[0x5c0 / 4] == 0.5f &&
                  blk[0x5d0 / 4] == 2.0f && blk[0x650 / 4] == 0.5f && blk[0x66c / 4] == 0.5f && blk[0x670 / 4] == 1.0f,
              "substep+grav");
        CHECK(nps5_ps_poison(NULL, 31) == 0, "NULL block");
        CHECK(nps5_ps_poison_group("dt") == 1 && nps5_ps_poison_group("grav") == 16 && nps5_ps_poison_group("x") == 0 &&
                  nps5_ps_poison_group(NULL) == 0,
              "group names");
        CHECK(nps5_ps_poison_phase(0, 100, 3) == 0 && nps5_ps_poison_phase(100, 100, 3) == 1 &&
                  nps5_ps_poison_phase(399, 100, 3) == 3 && nps5_ps_poison_phase(400, 100, 3) == 0 &&
                  nps5_ps_poison_phase(5, 0, 3) == 0 && nps5_ps_poison_phase(500, 100, 0) == 0,
              "poison phases");
    }
    // dtclamp selection
    {
        static const char *const nm[4] = {"bigleafplant", "springbone", "aprope", "slingshot"};
        int bad = 0;
        CHECK(nps5_ps_dtclamp_select(NULL, nm, 4, 4, &bad) == 0 && nps5_ps_dtclamp_select("0", nm, 4, 4, &bad) == 0 &&
                  nps5_ps_dtclamp_select("", nm, 4, 4, &bad) == 0 && bad == 0,
              "off");
        CHECK(nps5_ps_dtclamp_select("all", nm, 4, 4, &bad) == 15 && bad == 0, "all");
        CHECK(nps5_ps_dtclamp_select("springbone,SlingShot", nm, 4, 4, &bad) == 10 && bad == 0,
              "list, case-insensitive");
        CHECK(nps5_ps_dtclamp_select("aprope,nope", nm, 4, 4, &bad) == 4 && bad == 1, "unknown item counted");
        CHECK(nps5_ps_dtclamp_select("all", nm, 32, 32, NULL) == 0xffffffffull, "32 slots");
        CHECK(nps5_ps_dtclamp_select("all", nm, 64, 64, NULL) == ~0ull, "64 slots");
        // auto slots (dtauto): "all" selects only the first nall (named) slots, auto ones only by name; a NULL name (unused slot) never matches
        {
            const char *const an[6] = {"bigleafplant", "springbone", "a537a360", "a1234", NULL, "a99"};
            CHECK(nps5_ps_dtclamp_select("all", an, 6, 2, &bad) == 3, "all = named only");
            CHECK(nps5_ps_dtclamp_select("all,A537A360", an, 6, 2, &bad) == 7,
                  "all + one auto slot (case-insensitive)");
            CHECK(nps5_ps_dtclamp_select("a99", an, 6, 2, &bad) == 32, "auto slot alone");
            bad = 0;
            CHECK(nps5_ps_dtclamp_select("a1234,", an, 6, 2, &bad) == 8 && bad == 0, "trailing comma");
            CHECK(nps5_ps_dtclamp_select("nothere", an, 6, 2, &bad) == 0 && bad == 1, "unknown auto name counted");
        }
        CHECK(nps5_ps_dtclamp_hz("springbone=45,aprope=x,bigleafplant=1000", "springbone", 30, &bad) == 45 && bad == 1,
              "hz item");
        CHECK(nps5_ps_dtclamp_hz("springbone=45", "aprope", 30, &bad) == 30, "hz default");
        bad = 0;
        CHECK(nps5_ps_dtclamp_hz("bigleafplant=1000,bigleafplant", "bigleafplant", 60, &bad) == 60 && bad == 2,
              "hz out of range / no '='");
        CHECK(nps5_ps_dtclamp_hz(NULL, "x", 7, NULL) == 7, "hz NULL list");
        // the clamp the shim applies: 26.8 ms at 60 -> 1/60, 10 ms unchanged, NaN / 0 / negative pass through
        CHECK(nps5_ps_clamp(0.0268f, 60) == (float)(1.0 / 60) && nps5_ps_clamp(0.01f, 60) == 0.01f &&
                  nps5_ps_clamp(-1.0f, 60) == -1.0f,
              "dtclamp values");
    }
    // counting-wrap probe rows (dtauto)
    {
        Nps5PrRow r;
        memset(&r, 0, sizeof r);
        // frame 1: 1 call, dt arg = frame dt; frame 2: 3 calls of the SAME object (a sub-stepped function), dt arg 1/60; frame 3: 1 call from another thread
        // (in the world window, another object), dt 40 ms
        nps5_pr_note(&r, 1, 0, 7, 0x1000, 0.0268f, 0.0268f);
        nps5_pr_note(&r, 2, 0, 7, 0x1000, 1.0f / 60, 0.0268f);
        nps5_pr_note(&r, 2, 0, 7, 0x1000, 1.0f / 60, 0.0268f);
        nps5_pr_note(&r, 2, 0, 7, 0x1000, 1.0f / 60, 0.0268f);
        nps5_pr_note(&r, 3, 1, 8, 0x2000, 0.040f, 0.040f);
        CHECK(r.calls == 5 && r.frames == 3 && nps5_pr_maxpf(&r) == 3 && r.in_world == 1 && r.multi_thread == 1,
              "counts: calls %llu frames %llu maxpf %llu inw %llu mt %d", (unsigned long long)r.calls,
              (unsigned long long)r.frames, (unsigned long long)nps5_pr_maxpf(&r), (unsigned long long)r.in_world,
              r.multi_thread);
        CHECK(r.maxpt == 3 && nps5_pr_maxinst(&r) == 1 && r.inst_over == 0,
              "same object 3x in a frame: maxpt %u inst %u", r.maxpt, nps5_pr_maxinst(&r));
        CHECK(r.argok == 5 && r.argframe == 2 && r.bins[4] == 1 && r.bins[2] == 3 && r.bins[5] == 1 && r.bins[0] == 0,
              "dt histogram: argok %llu argfr %llu", (unsigned long long)r.argok, (unsigned long long)r.argframe);
        CHECK(r.amin < 0.0167f && r.amin > 0.0166f && r.amax == 0.040f, "min/max");
        // many objects, one call each per frame: per-instance, maxpt 1; the table is cleared between frames
        Nps5PrRow m;
        memset(&m, 0, sizeof m);
        for (int f = 0; f < 3; f++)
            for (uint64_t o = 0; o < 40; o++)
                nps5_pr_note(&m, 10 + (uint64_t)f, 0, 1, 0x10000 + o * 0x230, 0.0167f, 0.0167f);
        CHECK(m.calls == 120 && m.frames == 3 && nps5_pr_maxpf(&m) == 40 && nps5_pr_maxinst(&m) == 40 && m.maxpt == 1 &&
                  m.inst_over == 0,
              "40 objects: maxpt %u inst %u over %u", m.maxpt, nps5_pr_maxinst(&m), m.inst_over);
        // more objects than the table tracks: counted as over, maxpt stays correct for the tracked ones
        Nps5PrRow big;
        memset(&big, 0, sizeof big);
        for (uint64_t o = 0; o < 400; o++)
            nps5_pr_note(&big, 1, 0, 1, 0x40000 + o * 0x10, 0.0167f, 0.0167f);
        CHECK(big.inst_over > 0 && nps5_pr_maxinst(&big) <= NPS5_PR_INST && big.maxpt == 1 && big.calls == 400,
              "table overflow: over %u inst %u", big.inst_over, nps5_pr_maxinst(&big));
        // not a dt: NaN, 0, negative, a pointer reinterpreted as float (huge), 2 s: counted as a call, not as a dt
        Nps5PrRow q;
        memset(&q, 0, sizeof q);
        nps5_pr_note(&q, 5, 0, 1, 1, NAN, 0.0167f);
        nps5_pr_note(&q, 5, 0, 1, 2, 0.0f, 0.0167f);
        nps5_pr_note(&q, 5, 0, 1, 3, -0.02f, 0.0167f);
        nps5_pr_note(&q, 5, 0, 1, 4, 3.0e9f, 0.0167f);
        nps5_pr_note(&q, 5, 0, 1, 5, 2.0f, 0.0167f);
        CHECK(q.calls == 5 && q.argok == 0 && q.frames == 1 && nps5_pr_maxpf(&q) == 5 && q.multi_thread == 0 &&
                  q.maxpt == 1,
              "non-dt arguments");
        nps5_pr_note(&q, 6, 0, 1, 1, 0.01f, 0.0f);
        CHECK(q.argok == 1 && q.argframe == 0 && q.frames == 2 && q.maxpf == 5, "unknown frame dt: argfr stays 0");
        CHECK(nps5_pr_bin(0.007f) == 0 && nps5_pr_bin(0.012f) == 1 && nps5_pr_bin(0.0167f) == 2 &&
                  nps5_pr_bin(0.02f) == 3 && nps5_pr_bin(0.03f) == 4 && nps5_pr_bin(0.05f) == 5 &&
                  nps5_pr_bin(0.1f) == 6,
              "bins");
        char line[520];
        const int n = nps5_pr_format(&r, 17, line, sizeof line);
        CHECK(n > 0 && n < (int)sizeof line &&
                  !strncmp(line,
                           "[physprobe] s=17 calls=5 frames=3 maxpf=3 inst=1 maxpt=3 over=0 inw=1 mt=1 argok=5 argfr=2 "
                           "amin=0.01667 amax=0.04000 bins=0,0,3,0,1,1,0",
                           135),
              "line: %s", line);
        // stack arguments: 8 qwords above the return address are copied, the return slot (index 0) is not
        uint64_t src[10], dst[10];
        for (int i = 0; i < 10; i++)
            src[i] = 100 + i, dst[i] = 0;
        nps5_pr_copy_stack_args(dst, src);
        CHECK(dst[0] == 0 && dst[1] == 101 && dst[8] == 108 && dst[9] == 0, "stack args copy");
    }
    if (fails) {
        printf("physsub_unit: %d FAILED\n", fails);
        return 1;
    }
    printf("physsub_unit: all passed\n");
    return 0;
}
