// Unit test of the adaptive yield back-off state machine (core/nps5_yield.h). Pure logic, no syscalls.
#include <string.h>
#include "nps5_yield.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
  nps5_yield_cfg c = {50000, 2000, 20000, 4};
  nps5_yield_state s = {0};
  uint64_t t = 1000000;
  // disabled: never waits
  nps5_yield_cfg off = {0, 2000, 20000, 4};
  for (int i = 0; i < 100; i++) CHECK(nps5_yield_step(&off, &s, t += 100) == 0);
  memset(&s, 0, sizeof s);
  // first `after` yields of a run are plain (run counts from the 2nd call: first call has a long gap)
  uint64_t w[16];
  for (int i = 0; i < 16; i++) { w[i] = nps5_yield_step(&c, &s, t += 1000); s.last_ns = t; }
  CHECK(w[0] == 0 && w[1] == 0 && w[2] == 0);       // run 0,1,2 < after 4
  CHECK(w[3] == 0);                                  // run 3
  CHECK(w[4] == 2000);                               // run 4: back-off starts at min
  CHECK(w[5] == 4000 && w[6] == 8000 && w[7] == 16000 && w[8] == 32000);
  CHECK(w[9] == 50000 && w[15] == 50000);            // capped
  // real work (gap >= gap_ns) resets the run: next yields are plain again
  t += 20000;
  CHECK(nps5_yield_step(&c, &s, t) == 0 && s.run == 0 && s.cur_ns == 0);
  s.last_ns = t;
  for (int i = 0; i < 3; i++) { CHECK(nps5_yield_step(&c, &s, t += 1000) == 0); s.last_ns = t; }
  // a back-off wait of cap (50 us) ends at last_ns = after the sleep: the next call is within the gap, run continues
  nps5_yield_state s2 = {0};
  uint64_t n = 5000000; int waited = 0;
  for (int i = 0; i < 200; i++) { uint64_t x = nps5_yield_step(&c, &s2, n); if (x) { waited++; n += x; } n += 500; s2.last_ns = n; }
  CHECK(waited > 150);
  // cap_ns below min_ns: clamped to cap
  nps5_yield_cfg c3 = {1000, 2000, 20000, 1};
  nps5_yield_state s3 = {0}; uint64_t m = 1000000, x = 0;
  for (int i = 0; i < 5; i++) { x = nps5_yield_step(&c3, &s3, m += 100); s3.last_ns = m; }
  CHECK(x == 1000);
  // overflow safety of run counter
  s3.run = 0xffffffffu; s3.last_ns = m; CHECK(nps5_yield_step(&c3, &s3, m + 1) == 1000 && s3.run == 0xffffffffu);
  printf(fails ? "yield_unit FAILED (%d)\n" : "yield_unit OK\n", fails);
  return fails != 0;
}
