// SPDX-License-Identifier: GPL-2.0-or-later
//
// Checked pointer translation (nps5_thunk.h, NPS5_CHECKED_PTR; review 1, m5). Built twice by
// pkg/runtime/mem_vm_darwin_test.go: with -DNPS5_CHECKED_PTR=1 a wild guest pointer aborts with
// a message; by default translation is unchecked (a bare add) and returns.
//   ptrcheck good   translate a valid pointer and NULL: must work in both builds
//   ptrcheck wild   translate 1<<41: aborts when checked, returns when not
//   ptrcheck neg    translate NULL-8 (0xfff...f8): aborts when checked, returns when not
#include "nps5_diag.h"
#include "nps5_thunk.h"
#include <stdio.h>
#include <string.h>

uint8_t g_nps5_level[NPS5_S_COUNT];
int main(int argc, char **argv) {
  char err[512];
  if (nps5_mem_reserve(&nps5_mem_config, 0, err, sizeof err)) { printf("reserve: %s\n", err); return 3; }
  GuestContext ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.mem_base = g_nps5_arena.bias;
  ctx.mem_size = g_nps5_arena.guest_hi;
  const char *what = argc > 1 ? argv[1] : "good";
  if (strcmp(what, "good") == 0) {
    void *p = NPS5_G2H(&ctx, 0x500000);
    return p == (void *)(ctx.mem_base + 0x500000) && NPS5_G2H(&ctx, 0) == NULL ? 0 : 1;
  }
  uint64_t g = strcmp(what, "wild") == 0 ? 1ULL << 41 : (uint64_t)0 - 8;
  volatile void *p = NPS5_G2H(&ctx, g);
  (void)p;
  printf("returned\n");
  return 0;
}
