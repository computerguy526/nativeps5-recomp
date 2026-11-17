// SPDX-License-Identifier: GPL-2.0-or-later
// Host test of the cvar dump (pkg/runtime/core/nps5_uecvar.c): a fake TAutoConsoleVariable<int>,
// <float> and <bool> and a Ref int in the guest arena; the dump thread (started by the
// constructor from NPS5_UECVAR_*) must report them, and "unmapped" for an address outside it.
#include "nps5_mem.h"

#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

static const Nps5MemConfig cfg = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_BASE, .host_base = NPS5_BASE_SPAN,
    .guest_lo = 0x400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x400000ULL,
    .dyn_lo = 0x200000000ULL, .pool_lo = 0x1000000000ULL, .stack_top = 0x7EEFFC000ULL, .stack_len = 0x4000000ULL,
};

int main(int argc, char **argv) {
  char err[512];
  if (nps5_mem_reserve(&cfg, 0, err, sizeof err) != 0) { printf("reserve: %s\n", err); return 1; }
  if (nps5_mem_commit(0x500000, 0x10000) != 0) { printf("commit\n"); return 1; }
  uint8_t *h = g_nps5_arena.bias;
  // TAutoConsoleVariable objects at 0x500000 (int), 0x500100 (float), 0x500200 (bool); their IConsoleVariable at 0x501000..; a Ref int at 0x502000
  uint64_t ivar[3] = {0x501000, 0x501100, 0x501200};
  for (int i = 0; i < 3; i++) memcpy(h + 0x500000 + 0x100 * i + 8, &ivar[i], 8);
  int32_t vi = 7; float vf = 0.5f; uint8_t vb = 1;
  memcpy(h + 0x501000 + 0x50, &vi, 4);
  memcpy(h + 0x501100 + 0x50, &vf, 4);
  memcpy(h + 0x501200 + 0x50, &vb, 1);
  int32_t r = -3; memcpy(h + 0x502000, &r, 4);
  if (argc > 1 && !strcmp(argv[1], "set")) { // NPS5_UECVAR_SET: a writable cvar is stored; one in a read-only page (RELRO) is left alone, no crash
    if (nps5_mem_commit(0x510000, 0x4000) != 0) { printf("commit2\n"); return 1; }
    int32_t ro = 5; memcpy(h + 0x510000, &ro, 4);
    mprotect(h + 0x510000, 0x4000, PROT_READ);
    fflush(stdout);
    usleep(1000000);
    int32_t got_w, got_ro; memcpy(&got_w, h + 0x502000, 4); memcpy(&got_ro, h + 0x510000, 4);
    printf("SET w=%d ro=%d\n", got_w, got_ro);
    printf("TEST DONE\n");
    return 0;
  }
  fflush(stdout);
  usleep(1500000);
  printf("TEST DONE\n");
  return 0;
}
