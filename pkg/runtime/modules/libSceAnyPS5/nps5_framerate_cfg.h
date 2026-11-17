// Real-time game speed switch parsing (pure, unit-tested in tests/hlesys/hlesys_unit.c).
//   NPS5_FRAMERATE_BLOCK=<guest addr hex>  generic: profile game.tsv key framerate_block
//   NPS5_ASTRO_REALTIME=1                  deprecated alias of NPS5_FRAMERATE_BLOCK=93d4040 (one release, logs a note)
//   NPS5_ASTRO_REALTIME=<hex addr>         (value > 1 is read as the address, so the alias can be pointed elsewhere)
// Returns the guest address, or 0 = off. *alias is set to 1 when the deprecated name decided the result.
#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint64_t nps5_framerate_cfg(const char *block, const char *astro, int *alias) {
  if (alias) *alias = 0;
  if (block && *block) { uint64_t a = strtoull(block, NULL, 16); if (a) return a; }
  if (astro && *astro) {
    char *end = NULL; unsigned long long v = strtoull(astro, &end, 16);
    if (end == astro || v == 0) return 0;
    if (alias) *alias = 1;
    return v == 1 ? 0x93d4040ull : (uint64_t)v;
  }
  return 0;
}
