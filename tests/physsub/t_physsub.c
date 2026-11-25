// End-to-end test program of the wrapped replacements ([physsub], tools/physsub_test.sh). Two "step" functions in
// the shapes the shims expect: world_step(this, a1, a2, float dt) and cloth_step(this, float dt) with a pose-only
// flag byte at this+0x195. Each records how often it ran, the sum of its dt, the largest dt and the arguments it saw.
// They are reached directly, through a function pointer and through a tail call. The script translates this program
// with a -hle-replace table that wraps both and checks the counts with the switch off / on.
#include "../ps5/src/ps5test.h"
#include <stdint.h>

typedef struct {
    long calls;
    double sum;
    float max, min;
    uint64_t a1, a2;
    long bad;
    uint8_t pad[0x195 - 48];
    uint8_t posed;
} Obj;

__attribute__((noinline)) void world_step(Obj *o, uint64_t a1, uint64_t a2, float dt) {
    o->calls++;
    o->sum += dt;
    if (dt > o->max) o->max = dt;
    if (dt < o->min) o->min = dt;
    if (a1 != 0x1111 || a2 != 0x2222) o->bad++;
    o->a1 = a1;
    o->a2 = a2;
}

__attribute__((noinline)) void cloth_step(Obj *o, float dt) {
    o->calls++;
    o->sum += dt;
    if (dt > o->max) o->max = dt;
    if (dt < o->min) o->min = dt;
}

// dtauto: a candidate with 8 integer arguments (2 on the stack) and a float dt, for the counting probe (nps5_physsub_probe_0): arguments must arrive intact
__attribute__((noinline)) long probe_fn(Obj *o, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8,
                                        float dt) {
    o->calls++;
    o->sum += dt;
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + (o != 0);
}
// dtauto: an auto dtclamp candidate (nps5_physsub_dtclamp_8), void f(this, float dt)
__attribute__((noinline)) void auto_upd(Obj *o, float dt) {
    o->calls++;
    o->sum += dt;
    if (dt > o->max) o->max = dt;
}
long (*volatile g_probe)(Obj *, long, long, long, long, long, long, long, long, float) = probe_fn;
void (*volatile g_auto)(Obj *, float) = auto_upd;

__attribute__((noinline)) void tail_world(Obj *o, float dt) { world_step(o, 0x1111, 0x2222, dt); }

void (*volatile g_world)(Obj *, uint64_t, uint64_t, float) = world_step;
void (*volatile g_cloth)(Obj *, float) = cloth_step;
volatile float g_dt = 0.05f;

static void reset(Obj *o) {
    o->calls = 0;
    o->sum = 0;
    o->max = 0;
    o->min = 1e9f;
    o->bad = 0;
}
static void report(const char *tag, Obj *o) {
    t_str(tag);
    t_dec("_calls", o->calls);
    t_str(tag);
    t_dec("_sum_us", (long long)(o->sum * 1e6 + 0.5));
    t_str(tag);
    t_dec("_max_us", (long long)(o->max * 1e6 + 0.5));
    t_str(tag);
    t_dec("_bad", o->bad);
}

static Obj w, c, p;

int main(void) {
    reset(&w);
    world_step(&w, 0x1111, 0x2222, g_dt); // direct
    g_world(&w, 0x1111, 0x2222, g_dt); // pointer
    tail_world(&w, g_dt); // tail call
    static Obj q, u;
    reset(&q);
    long ret = 0;
    ret += probe_fn(&q, 1, 2, 3, 4, 5, 6, 7, 8, g_dt);
    ret += g_probe(&q, 1, 2, 3, 4, 5, 6, 7, 8, 0.0167f);
    ret += g_probe(&q, 1, 2, 3, 4, 5, 6, 7, 8, g_dt);
    t_str("probe_");
    t_dec("ret", ret);
    t_str("probe_");
    t_dec("calls", q.calls);
    reset(&u);
    auto_upd(&u, g_dt);
    g_auto(&u, g_dt);
    report("auto", &u);
    world_step(&w, 0x1111, 0x2222, 0.01f); // short: never split
    report("world", &w);
    reset(&c);
    cloth_step(&c, g_dt);
    g_cloth(&c, g_dt);
    report("cloth", &c);
    reset(&p);
    p.posed = 1;
    cloth_step(&p, g_dt);
    report("posed", &p);
    t_str("result=DONE\n");
    return 0;
}
