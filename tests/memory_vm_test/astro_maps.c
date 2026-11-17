// SPDX-License-Identifier: GPL-2.0-or-later
//
// ASTRO BOT (PPSA21564) memory shape on arm64 macOS, base address mode: a process with
// flexible_memory_size 343,932,928 maps 2 GiB at the fixed guest address 0x300000000 and a
// 3.2 GB direct-memory range at the fixed guest address 0x1000000000, and takes its 343,932,928
// bytes of flexible memory. Host = guest + bias, so the guest addresses are not host addresses.
// Built and run by pkg/runtime/mem_vm_darwin_test.go (TestAstroMaps). No guest code runs.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "nps5_mem.h"
#include "ps4_direct_mem.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int, off_t *);
int sceKernelReserveVirtualRange(GuestContext *, void **, size_t, int, size_t);
int sceKernelMapDirectMemory(GuestContext *, void **, size_t, int, int, off_t, size_t);
int sceKernelMapNamedFlexibleMemory(GuestContext *, void **, size_t, int, int, const char *);
int sceKernelConfiguredFlexibleMemorySize(GuestContext *, uint64_t *);
size_t sceKernelAvailableFlexibleMemorySize(GuestContext *);

static int g_fail;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      g_fail = 1;                                         \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

enum { FIXED = 0x10 };
#define GIB (1ULL << 30)
#define MIB (1ULL << 20)
#define FLEX_PARAM 343932928ULL
#define FIXED_2G 0x300000000ULL
#define FIXED_DM 0x1000000000ULL
#define DM_3_2G ((3200ULL * MIB) / 1) // 3.2 GB as the game asks (3,355,443,200 bytes)

static GuestContext C;

static void add_ext(uint64_t a, uint64_t s, bool fr, const char *n, int kind) {
  GuestVMExtent *e = calloc(1, sizeof(*e));
  e->addr = a;
  e->size = s;
  e->is_free = fr;
  e->kind = kind;
  if (n) strcpy(e->name, n);
  GuestVMExtent **pp = &C.vm_extents, *prev = NULL;
  while (*pp) { prev = *pp; pp = &(*pp)->next; }
  e->prev = prev;
  *pp = e;
}

static void setup(void) {
  char err[1024];
  if (nps5_mem_reserve(&nps5_mem_config, 0, err, sizeof err)) { printf("reserve: %s\n", err); exit(1); }
  C.mem_base = g_nps5_arena.bias;
  C.mem_size = g_nps5_arena.guest_hi;
  C.process_ctx = &C;
  pthread_mutex_init(&C.vm_mutex, NULL);
  uint64_t sys_end = 0x500000, top = nps5_mem_config.stack_top, len = nps5_mem_config.stack_len;
  add_ext(sys_end, top - len - NPS5_PAGE - sys_end, true, NULL, 0);
  add_ext(top - len - NPS5_PAGE, NPS5_PAGE, false, "stack_guard", NPS5_EXT_STACK_GUARD);
  add_ext(top - len, len, false, "main_stack", NPS5_EXT_MAIN_STACK);
  add_ext(top, g_nps5_arena.guest_hi - top, true, NULL, 0);
  nps5_mem_commit(top - len, len);
  g_nps5_vm_root = &C;
  recomp_vm_note_change();
}

static uint8_t *host(uint64_t g) { return g_nps5_arena.bias + g; }

// Touch the first, a middle and the last page of [g, g+len) through the host view and read them back.
static bool touch(uint64_t g, uint64_t len, uint8_t seed) {
  uint64_t offs[3] = {0, (len / 2) & ~(NPS5_PAGE - 1), len - 8};
  for (int i = 0; i < 3; i++) host(g + offs[i])[0] = (uint8_t)(seed + i);
  for (int i = 0; i < 3; i++) if (host(g + offs[i])[0] != (uint8_t)(seed + i)) return false;
  return true;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setup();
  CHECK(g_nps5_arena.mode == NPS5_ADDR_BASE, "test expects base address mode");
  CHECK(g_nps5_arena.guest_hi > 0x1100000000ULL, "guest space ends at 0x%llx, too small for 0x1000000000 + 3.2 GB",
        (unsigned long long)g_nps5_arena.guest_hi);

  // A process parameter block the way the game's eboot has it: sdk 9.00, flexible_memory_size 343,932,928.
  uint64_t pp = recomp_vm_map(&C, 0, 0x10000, 0, 3, 0, "param");
  CHECK(pp != (uint64_t)-1, "map the parameter page");
  if (pp == (uint64_t)-1) return 1;
  uint8_t *h = host(pp);
  uint64_t procparam = pp, memparam = pp + 0x100, flexp = pp + 0x200;
  uint64_t v;
  v = 0x48; memcpy(h + 0x000, &v, 8);                  // OrbisProcParam.size
  v = 0x09000000; memcpy(h + 0x010, &v, 8);            // sdk version
  v = memparam; memcpy(h + 0x040, &v, 8);              // mem_param
  v = 0x30; memcpy(h + 0x100, &v, 8);                  // OrbisKernelMemParam.size
  v = flexp; memcpy(h + 0x110, &v, 8);                 // flexible_memory_size pointer
  v = FLEX_PARAM; memcpy(h + 0x200, &v, 8);
  C.proc_param_addr = procparam;
  ps4_direct_mem_configure(&C, procparam);

  size_t dm_total = sceKernelGetDirectMemorySize();
  printf("direct memory %zu MiB\n", dm_total >> 20);
  CHECK(dm_total >= DM_3_2G, "direct memory %zu MiB cannot hold the 3.2 GB range", dm_total >> 20);
  // The PS4 sizing rule leaves 4856 MiB for this parameter block: enough for the 3.2 GB range, NOT for that plus a
  // 2 GiB direct-backed map. Which kind the 2 GiB is, is not known; the test maps it as a flexible map in a reservation.
  if (dm_total < DM_3_2G + 2 * GIB) printf("NOTE: direct memory %zu MiB < 3.2 GB + 2 GiB; set PS4_DIRECT_MEM_SIZE if the 2 GiB turns out to be direct memory\n", dm_total >> 20);

  // Flexible memory: the configured budget covers the title's 343,932,928 bytes and one map of that size works.
  uint64_t cfg = 0;
  CHECK(sceKernelConfiguredFlexibleMemorySize(&C, &cfg) == 0 && cfg >= FLEX_PARAM, "configured flexible memory %llu < %llu",
        (unsigned long long)cfg, (unsigned long long)FLEX_PARAM);
  void *fa = NULL;
  int rc = sceKernelMapNamedFlexibleMemory(&C, &fa, FLEX_PARAM, 3, 0, "astro_flex");
  CHECK(rc == 0 && fa, "map 343,932,928 bytes of flexible memory rc=0x%x", rc);
  if (rc == 0 && fa) {
    CHECK(touch((uint64_t)(uintptr_t)fa, FLEX_PARAM, 0x11), "flexible map is not readable/writable");
    CHECK(sceKernelAvailableFlexibleMemorySize(&C) <= cfg - FLEX_PARAM, "flexible budget is not reduced by the map");
    recomp_vm_free(&C, (uint64_t)(uintptr_t)fa, FLEX_PARAM);
  }

  // 2 GiB at the fixed guest address 0x300000000: reserve it, then map direct memory over it with MAP_FIXED.
  void *ra = (void *)(uintptr_t)FIXED_2G;
  rc = sceKernelReserveVirtualRange(&C, &ra, 2 * GIB, FIXED, 0x10000);
  CHECK(rc == 0 && (uint64_t)(uintptr_t)ra == FIXED_2G, "reserve 2 GiB at 0x300000000 rc=0x%x got=%p", rc, ra);
  off_t physdm = 0;
  void *ma = (void *)(uintptr_t)FIXED_2G;
  rc = sceKernelMapNamedFlexibleMemory(&C, &ma, 2 * GIB, 3, FIXED, "astro_2g");
  CHECK(rc == 0 && (uint64_t)(uintptr_t)ma == FIXED_2G, "fixed flexible map of 2 GiB at 0x300000000 rc=0x%x got=%p", rc, ma);
  if (rc == 0) CHECK(touch(FIXED_2G, 2 * GIB, 0x21), "2 GiB map at 0x300000000 is not readable/writable");

  // 3.2 GB direct memory at the fixed guest address 0x1000000000 (the host's own GPU carveout range is NOT guest memory).
  rc = sceKernelAllocateDirectMemory(0, (off_t)dm_total, DM_3_2G, 0x10000, 0, &physdm);
  CHECK(rc == 0, "allocate 3.2 GB of direct memory rc=0x%x (total %zu MiB)", rc, dm_total >> 20);
  if (rc == 0) {
    void *da = (void *)(uintptr_t)FIXED_DM;
    rc = sceKernelMapDirectMemory(&C, &da, DM_3_2G, 3, FIXED, physdm, 0x10000);
    CHECK(rc == 0 && (uint64_t)(uintptr_t)da == FIXED_DM, "fixed direct map of 3.2 GB at 0x1000000000 rc=0x%x got=%p", rc, da);
    if (rc == 0) CHECK(touch(FIXED_DM, DM_3_2G, 0x31), "3.2 GB map at 0x1000000000 is not readable/writable");
  }
  printf(g_fail ? "ASTRO MAPS FAIL\n" : "ASTRO MAPS OK\n");
  return g_fail;
}
