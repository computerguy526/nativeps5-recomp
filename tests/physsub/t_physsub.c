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

__attribute__((noinline)) void tail_world(Obj *o, float dt) { world_step(o, 0x1111, 0x2222, dt); }

void (*volatile g_world)(Obj *, uint64_t, uint64_t, float) = world_step;
void (*volatile g_cloth)(Obj *, float) = cloth_step;
volatile float g_dt = 0.05f;

static void reset(Obj *o) {
  o->calls = 0; o->sum = 0; o->max = 0; o->min = 1e9f; o->bad = 0;
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
  g_world(&w, 0x1111, 0x2222, g_dt);    // pointer
  tail_world(&w, g_dt);                 // tail call
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
