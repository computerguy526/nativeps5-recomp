// Test of pkg/runtime/modules/libSceAnyPS5/nps5_log.h: parsing, levels per subsystem, rate limiting, line format.
#include <assert.h>
#include <unistd.h>
#include "nps5_log.h"

int main(void) {
  nps5_log_cfg c;
  nps5_log_parse(&c, NULL, NULL);
  assert(nps5_log_enabled_in(&c, "a", 'I') && nps5_log_enabled_in(&c, "a", 'E') && !nps5_log_enabled_in(&c, "a", 'D'));
  nps5_log_parse(&c, "kyty-seam:W,kk-archive:D", NULL);
  assert(!nps5_log_enabled_in(&c, "kyty-seam", 'I') && nps5_log_enabled_in(&c, "kyty-seam", 'W'));
  assert(nps5_log_enabled_in(&c, "kk-archive", 'D') && nps5_log_enabled_in(&c, "other", 'I') && !nps5_log_enabled_in(&c, "other", 'D'));
  nps5_log_parse(&c, "E", NULL);
  assert(!nps5_log_enabled_in(&c, "x", 'W') && nps5_log_enabled_in(&c, "x", 'E'));
  nps5_log_parse(&c, "kyty-seam=warn,kk:debug", NULL);   // the core's `name=word` form and words
  assert(!nps5_log_enabled_in(&c, "kyty-seam", 'I') && nps5_log_enabled_in(&c, "kk", 'D'));
  nps5_log_parse(&c, "*:W,foo:I,bogus,x:Q", NULL);
  assert(!nps5_log_enabled_in(&c, "x", 'I') && nps5_log_enabled_in(&c, "foo", 'I') && c.nrules == 1);
  // rate: first 3, then every 10th, hidden counts add up to the offered lines
  nps5_log_parse(&c, NULL, "3,10");
  nps5_log_rl rl = {0};
  unsigned long long hidden, printed = 0, total_hidden = 0;
  for (int i = 1; i <= 103; i++) {
    if (nps5_log_rl_pass_in(&c, &rl, &hidden)) {
      printed++;
      total_hidden += hidden;
      if (i > 3) assert(hidden == 9);
    }
  }
  assert(printed == 3 + 10);                    // lines 1-3, then 13, 23, ... 103
  assert(printed + total_hidden == 103);
  nps5_log_parse(&c, NULL, "0,0");
  nps5_log_rl r2 = {0};
  for (int i = 0; i < 50; i++) assert(nps5_log_rl_pass_in(&c, &r2, &hidden));   // unset: all lines
  // line format on stderr: I has no level token, others do
  NPS5_SLOG_EMIT("sub", 'I', "info %d", 1);
  NPS5_SLOG_EMIT("sub", 'W', "warn %d", 2);
  puts("LOG OK");
  return 0;
}
