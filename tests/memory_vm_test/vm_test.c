// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host test of the guest VM extent manager (core/nps5_vm.c) and the direct-memory/flexible/pool
// entry points (kernel/memory/ps4_direct_mem.c) over the real arena (core/nps5_mem.c). It is the
// regression suite for docs/m1/memory-fixes-1.md (the findings of memory-review-1: B1, M1-M3, M5,
// M6, m1-m4, n3); each check names its finding. No guest or x86 code runs.
// Built and run by pkg/runtime/mem_vm_darwin_test.go.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "nps5_mem.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int, off_t *);
int sceKernelReserveVirtualRange(GuestContext *, void **, size_t, int, size_t);
int sceKernelMapDirectMemory(GuestContext *, void **, size_t, int, int, off_t, size_t);
int sceKernelMemoryPoolReserve(GuestContext *, void *, size_t, size_t, int, void **);
int sceKernelMapNamedFlexibleMemory(GuestContext *, void **, size_t, int, int, const char *);
int sceKernelSetVirtualRangeName(GuestContext *, const void *, size_t, const char *);
int sceKernelQueryMemoryProtection(GuestContext *, const void *, void **, void **, int *);

static int g_fail;
#define CHECK(id, cond, ...)                                                       \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      g_fail = 1;                                                                  \
      printf("FAIL [%s] %s:%d: ", id, __FILE__, __LINE__);                         \
      printf(__VA_ARGS__);                                                         \
      printf("\n");                                                                \
    }                                                                              \
  } while (0)

enum { FIXED = 0x10, NO_OVERWRITE = 0x80, EXCL = 0x4000 };
enum { ORBIS_EINVAL = 0x80020016, ORBIS_ENOMEM = 0x8002000c };
#define NONE ((uint64_t)-1)

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

static GuestVMExtent *ext_at(uint64_t a) {
  for (GuestVMExtent *e = C.vm_extents; e; e = e->next)
    if (a >= e->addr && a < e->addr + e->size) return e;
  return NULL;
}
static const char *ext_name(uint64_t a) {
  GuestVMExtent *e = ext_at(a);
  return !e ? "(none)" : e->is_free ? "(free)" : e->name;
}
static bool is_free(uint64_t a) { GuestVMExtent *e = ext_at(a); return e && e->is_free; }
static uint8_t *host(uint64_t g) { return g_nps5_arena.bias + g; }

// ---- B1: MapDirectMemory(MAP_FIXED) over an existing mapping must not deadlock ----
static void on_alarm(int s) {
  (void)s;
  static const char m[] = "FAIL [B1] sceKernelMapDirectMemory(MAP_FIXED over a mapping) did not return within 5 s (deadlock)\n";
  (void)!write(1, m, sizeof m - 1);
  _exit(2);
}
static void test_b1(void) {
  off_t phys = 0, phys2 = 0;
  CHECK("B1", sceKernelAllocateDirectMemory(0, 0x10000000, 0x200000, 0x10000, 0, &phys) == 0, "alloc dm");
  CHECK("B1", sceKernelAllocateDirectMemory(0, 0x10000000, 0x200000, 0x10000, 0, &phys2) == 0, "alloc dm 2");
  void *addr = NULL;
  CHECK("B1", sceKernelReserveVirtualRange(&C, &addr, 0x200000, 0, 0x10000) == 0, "reserve");
  uint64_t a = (uint64_t)(uintptr_t)addr;
  signal(SIGALRM, on_alarm);
  alarm(5);
  void *got = addr;
  int rc = sceKernelMapDirectMemory(&C, &got, 0x200000, 3, FIXED, phys, 0x10000);
  CHECK("B1", rc == 0 && (uint64_t)(uintptr_t)got == a, "map fixed into reservation rc=0x%x got=%p want=%p", rc, got, addr);
  CHECK("B1", ext_at(a) && ext_at(a)->kind == NPS5_EXT_DIRECT, "extent is direct memory");
  host(a)[0x10] = 0x5a;
  // The linearizer pattern: remap the same range onto other physical memory, still MAP_FIXED.
  got = addr;
  rc = sceKernelMapDirectMemory(&C, &got, 0x200000, 3, FIXED, phys2, 0x10000);
  CHECK("B1", rc == 0 && (uint64_t)(uintptr_t)got == a, "remap fixed over a direct mapping rc=0x%x", rc);
  alarm(0);
  // And the original phys still holds its data through a second alias elsewhere (not clobbered).
  void *other = NULL;
  rc = sceKernelMapDirectMemory(&C, &other, 0x200000, 3, 0, phys, 0x10000);
  CHECK("B1", rc == 0 && host((uint64_t)(uintptr_t)other)[0x10] == 0x5a, "first physical block kept its contents");
  recomp_vm_free(&C, a, 0x200000);
  recomp_vm_free(&C, (uint64_t)(uintptr_t)other, 0x200000);
}

// ---- recheck: MapDirectMemory must fail (and undo) when the physical alias cannot be made ----
static void test_dm_alias_fail(void) {
  off_t p0 = 0, p1 = 0;
  CHECK("DA", sceKernelAllocateDirectMemory(0, 0x10000000, 0x1000, 0x1000, 0, &p0) == 0, "alloc dm 0");
  CHECK("DA", sceKernelAllocateDirectMemory(0, 0x10000000, 0x8000, 0x1000, 0, &p1) == 0, "alloc dm 1");
  if (p1 & 0x3fff) { // not 16 KiB aligned: mach_vm_remap cannot alias it
    void *got = NULL;
    int rc = sceKernelMapDirectMemory(&C, &got, 0x4000, 3, 0, p1, 0x1000);
    CHECK("DA", rc != 0, "map of a physical range that cannot be aliased returned success (phys=0x%llx)", (unsigned long long)p1);
    CHECK("DA", got == NULL, "failed map left an address in the out parameter: %p", got);
    for (GuestVMExtent *e = C.vm_extents; e; e = e->next)
      CHECK("DA", e->is_free || e->kind != NPS5_EXT_DIRECT, "failed map left a direct extent 0x%llx", (unsigned long long)e->addr);
  } else {
    printf("note [DA] second direct block landed 16 KiB aligned (0x%llx); alias-failure case not exercised\n", (unsigned long long)p1);
    g_fail = 1;
  }
}

// ---- recheck: VirtualQuery/QueryMemoryProtection read a copy, not a live extent ----
static atomic_int qstop;
static void *qmapper(void *arg) {
  (void)arg;
  while (!atomic_load(&qstop)) {
    void *a = NULL;
    if (sceKernelReserveVirtualRange(&C, &a, 0x10000, 0, 0x4000) == 0) {
      uint64_t g = (uint64_t)(uintptr_t)a;
      uint64_t r = recomp_vm_map(&C, g, 0x10000, 0, 3, FIXED, "q"); // folds the reservation in place
      if (r != NONE) recomp_vm_free(&C, g, 0x10000);
    }
  }
  return NULL;
}
static void test_query_copy(void) {
  GuestVMExtent cp;
  uint64_t m = recomp_vm_map_guest(&C, 0x8000, 0x4000, 3, 0, "qcopy");
  CHECK("QC", m != NONE && recomp_vm_query(&C, m + 0x4000, &cp), "query of a fresh mapping");
  CHECK("QC", cp.addr == m && cp.size == 0x8000 && strcmp(cp.name, "qcopy") == 0 && !cp.next && !cp.prev, "copy fields");
  recomp_vm_free(&C, m, 0x8000);
  CHECK("QC", cp.addr == m && strcmp(cp.name, "qcopy") == 0, "copy changed after the extent was freed");
  CHECK("QC", !recomp_vm_query(&C, 0x10, &cp) || cp.is_free || cp.addr <= 0x10, "query below the list");
  pthread_t t;
  atomic_store(&qstop, 0);
  pthread_create(&t, NULL, qmapper, NULL);
  uint64_t probe = recomp_vm_map_guest(&C, 0x4000, 0x4000, 3, 0, "probe");
  void *s, *e; int pr;
  for (int i = 0; i < 200000; i++) {
    sceKernelQueryMemoryProtection(&C, (void *)(uintptr_t)probe, &s, &e, &pr);
    recomp_vm_query(&C, probe + 0x2000, &cp);
  }
  atomic_store(&qstop, 1);
  pthread_join(t, NULL);
  recomp_vm_free(&C, probe, 0x4000);
}

// ---- M1: MAP_FIXED replace is atomic against concurrent unhinted maps ----
static atomic_int stop;
static void *mapper(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    uint64_t g = recomp_vm_map_guest(&C, 0x10000, 0x10000, 3, 0, "other");
    if (g != NONE) recomp_vm_free(&C, g, 0x10000);
  }
  return NULL;
}
static void test_m1(void) {
  uint64_t X = recomp_vm_map_guest(&C, 0x10000, 0x10000, 3, 0, "probe"); // where unhinted maps land
  recomp_vm_free(&C, X, 0x10000);
  CHECK("M1", recomp_vm_map(&C, X, 0x10000, 0, 3, FIXED, "mine") == X, "first fixed map");
  pthread_t th[2];
  for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, mapper, NULL);
  int fail = 0, iters = 100000;
  for (int i = 0; i < iters; i++)
    if (recomp_vm_map(&C, X, 0x10000, 0, 3, FIXED, "mine") != X) fail++;
  atomic_store(&stop, 1);
  for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
  CHECK("M1", fail == 0, "MAP_FIXED replace at 0x%llx failed %d/%d times against concurrent mmap(NULL)",
        (unsigned long long)X, fail, iters);
  recomp_vm_free(&C, X, 0x10000);
}

// ---- M2: fixed requests are never widened beyond 16 KiB pages ----
static void test_m2(void) {
  uint64_t X = 0x3100000000ULL;
  uint64_t nb = recomp_vm_map(&C, X + 0x4000, 0x4000, 0, 3, FIXED, "neighbour");
  CHECK("M2", nb == X + 0x4000, "neighbour mapped");
  memset(host(nb), 0x77, 0x4000);
  void *fa = (void *)(uintptr_t)X;
  int rc = sceKernelMapNamedFlexibleMemory(&C, &fa, 0x4000, 3, FIXED, "flex16k");
  CHECK("M2", rc == 0 && ext_at(X) && ext_at(X)->size == 0x4000, "fixed flexible map covers exactly 16 KiB (rc=0x%x)", rc);
  CHECK("M2", strcmp(ext_name(nb), "neighbour") == 0 && host(nb)[0] == 0x77, "neighbour survived (now '%s')", ext_name(nb));
  // Same through direct memory with a 2 MiB alignment argument.
  off_t phys = 0;
  CHECK("M2", sceKernelAllocateDirectMemory(0, 0x10000000, 0x200000, 0x10000, 0, &phys) == 0, "alloc dm");
  uint64_t Y = 0x3200000000ULL;
  uint64_t nb2 = recomp_vm_map(&C, Y + 0x10000, 0x10000, 0, 3, FIXED, "neighbour2");
  void *da = (void *)(uintptr_t)Y;
  rc = sceKernelMapDirectMemory(&C, &da, 0x10000, 3, FIXED, phys, 0x200000);
  CHECK("M2", rc == 0 && ext_at(Y)->size == 0x10000, "fixed direct map with 2 MiB alignment covers exactly its pages (rc=0x%x)", rc);
  CHECK("M2", strcmp(ext_name(nb2), "neighbour2") == 0, "direct-map neighbour survived");
  // Misaligned fixed address: EINVAL, nothing changes.
  fa = (void *)(uintptr_t)(X + 0x4000 + 0x1000);
  rc = sceKernelMapNamedFlexibleMemory(&C, &fa, 0x4000, 3, FIXED, "misaligned");
  CHECK("M2", rc == (int)ORBIS_EINVAL, "misaligned fixed flexible map rc=0x%x, want EINVAL", rc);
  CHECK("M2", strcmp(ext_name(nb), "neighbour") == 0, "misaligned request changed nothing");
  int err = 0;
  CHECK("M2", recomp_vm_map_ex(&C, X + 1, 0x4000, 0, 3, FIXED, "x", 0, &err) == NONE && err == EINVAL, "map_ex misaligned err=%d", err);
  // Reserve with FIXED and an alignment larger than the request.
  void *ra = (void *)(uintptr_t)0x3300000000ULL;
  rc = sceKernelReserveVirtualRange(&C, &ra, 0x4000, FIXED, 0x200000);
  CHECK("M2", rc == 0 && ext_at(0x3300000000ULL)->size == 0x4000, "fixed reserve covers exactly its pages (rc=0x%x)", rc);
  recomp_vm_free(&C, X, 0x8000);
  recomp_vm_free(&C, Y, 0x20000);
  recomp_vm_free(&C, 0x3300000000ULL, 0x4000);
}

// ---- M3: pool reserve with an address ----
static void test_m3(void) {
  uint64_t a = recomp_vm_map(&C, 0x3000000000ULL, 0x200000, 0, 3, FIXED, "existing");
  void *out = NULL;
  int rc = sceKernelMemoryPoolReserve(&C, (void *)(uintptr_t)(a + 0x100000), 0x400000, 0x200000, FIXED | EXCL, &out);
  CHECK("M3", rc == (int)ORBIS_ENOMEM, "exclusive fixed pool reserve over an existing mapping rc=0x%x (out=%p)", rc, out);
  CHECK("M3", strcmp(ext_name(a + 0x100000), "existing") == 0, "the mapping is untouched");
  CHECK("M3", is_free(a + 0x300000), "nothing was taken past the mapping");
  // Without MAP_FIXED the address is a hint: the pool goes elsewhere and is really mapped there.
  out = NULL;
  rc = sceKernelMemoryPoolReserve(&C, (void *)(uintptr_t)(a + 0x100000), 0x400000, 0x200000, 0, &out);
  uint64_t o = (uint64_t)(uintptr_t)out;
  CHECK("M3", rc == 0 && (o + 0x400000 <= a || o >= a + 0x200000), "hinted pool reserve overlapped the mapping (out=%p)", out);
  CHECK("M3", rc == 0 && ext_at(o) && ext_at(o)->kind == NPS5_EXT_POOL, "hinted pool reserve is not an extent");
  if (rc == 0) { host(o)[0] = 1; recomp_vm_free(&C, o, 0x400000); }
  // outside the extent list (system block / images): fails
  rc = sceKernelMemoryPoolReserve(&C, (void *)(uintptr_t)0x100000, 0x200000, 0x200000, FIXED, &out);
  CHECK("M3", rc != 0, "pool reserve below the extent list reported success");
  // A free fixed address works, is owned and committed.
  rc = sceKernelMemoryPoolReserve(&C, (void *)(uintptr_t)0x3400000000ULL, 0x400000, 0x200000, FIXED, &out);
  CHECK("M3", rc == 0 && (uint64_t)(uintptr_t)out == 0x3400000000ULL, "free fixed pool reserve rc=0x%x", rc);
  CHECK("M3", ext_at(0x3400000000ULL) && ext_at(0x3400000000ULL)->kind == NPS5_EXT_POOL, "extent is a pool");
  host(0x3400000000ULL)[0] = 1; // must not fault
  recomp_vm_free(&C, a, 0x200000);
  recomp_vm_free(&C, 0x3400000000ULL, 0x400000);
}

// ---- M6: hints never split a reservation or a pool; kinds are immutable ----
static void test_m6(void) {
  void *ra = NULL;
  CHECK("M6", sceKernelReserveVirtualRange(&C, &ra, 0x100000, 0, 0x10000) == 0, "reserve");
  uint64_t r = (uint64_t)(uintptr_t)ra;
  uint64_t h = recomp_vm_map(&C, r + 0x40000, 0x10000, 0x10000, 3, 0, "hinted");
  CHECK("M6", h != NONE && (h < r || h >= r + 0x100000), "hint inside a reservation was honoured at 0x%llx", (unsigned long long)h);
  CHECK("M6", ext_at(r)->kind == NPS5_EXT_RESERVED && ext_at(r)->size == 0x100000, "reservation was split");
  // The same hint into a pool.
  void *pool = NULL;
  CHECK("M6", sceKernelMemoryPoolReserve(&C, NULL, 0x400000, 0x200000, 0, &pool) == 0, "pool reserve");
  uint64_t p = (uint64_t)(uintptr_t)pool;
  uint64_t h2 = recomp_vm_map(&C, p + 0x40000, 0x10000, 0x10000, 3, 0, "hinted2");
  CHECK("M6", h2 != NONE && (h2 < p || h2 >= p + 0x400000), "hint inside a pool was honoured");
  CHECK("M6", ext_at(p)->size == 0x400000, "pool was split by a hint");
  // A hint into free space is honoured.
  uint64_t fh = 0x3500000000ULL;
  CHECK("M6", recomp_vm_map(&C, fh, 0x10000, 0x10000, 3, 0, "freehint") == fh, "hint into free space moved");
  // An explicit fixed map into the reservation still works and carves it.
  CHECK("M6", recomp_vm_map(&C, r + 0x40000, 0x10000, 0, 3, FIXED | EXCL, "inside") == r + 0x40000, "fixed map into reservation");
  CHECK("M6", ext_at(r)->kind == NPS5_EXT_RESERVED && ext_at(r + 0x40000)->kind == NPS5_EXT_MAPPED, "carve kinds");
  // Renaming does not change behaviour: a renamed reservation is still one, a renamed "mapping"
  // called "reserved" is not.
  sceKernelSetVirtualRangeName(&C, (void *)(uintptr_t)r, 0x10000, "mine");
  CHECK("M6", ext_at(r)->kind == NPS5_EXT_RESERVED, "renamed reservation lost its kind");
  CHECK("M6", recomp_vm_map(&C, r, 0x10000, 0, 3, FIXED | EXCL, "again") == r, "map into the renamed reservation");
  sceKernelSetVirtualRangeName(&C, (void *)(uintptr_t)fh, 0x10000, "reserved");
  CHECK("M6", ext_at(fh)->kind == NPS5_EXT_MAPPED, "a mapping renamed 'reserved' became a reservation");
  CHECK("M6", recomp_vm_map(&C, fh, 0x10000, 0, 3, FIXED | EXCL, "steal") == NONE, "EXCL map into a mapping named 'reserved' succeeded");
  uint64_t un[64][2]; uint64_t gen;
  size_t n = recomp_vm_unmapped_ranges(&C, un, 64, &gen);
  bool fh_unmapped = false;
  for (size_t i = 0; i < n && i < 64; i++) if (fh >= un[i][0] && fh < un[i][1]) fh_unmapped = true;
  CHECK("M6", !fh_unmapped, "mapping renamed 'reserved' counts as unmapped for the GPU");
  recomp_vm_free(&C, r, 0x100000);
  recomp_vm_free(&C, h, 0x10000);
  recomp_vm_free(&C, p, 0x400000);
  recomp_vm_free(&C, h2, 0x10000);
  recomp_vm_free(&C, fh, 0x10000);
}

// ---- m1-m4, n3 ----
static void test_minor(void) {
  int err = 0;
  CHECK("m1", recomp_vm_map_ex(&C, 0, 0x10000, 0x10000, 3, FIXED, "fixed0", 0, &err) == NONE && err == EINVAL,
        "MAP_FIXED at 0 was not refused (err=%d)", err);
  CHECK("m1", recomp_vm_map(&C, 0, 0x10000, 0x10000, 3, 0, "anywhere") != NONE, "addr 0 without FIXED still maps anywhere");

  uint64_t st = nps5_mem_config.stack_top - 0x100000;
  errno = 0;
  CHECK("m2", recomp_vm_free(&C, st, 0x4000) == -1 && errno == EINVAL, "munmap of the main stack accepted");
  CHECK("m2", strcmp(ext_name(st), "main_stack") == 0, "main stack extent changed: '%s'", ext_name(st));
  uint64_t guard = nps5_mem_config.stack_top - nps5_mem_config.stack_len - NPS5_PAGE;
  CHECK("m2", recomp_vm_free(&C, guard - 0x4000, 0x8000) == -1, "munmap overlapping the stack guard accepted");
  CHECK("m2", recomp_vm_map_ex(&C, st, 0x4000, 0, 3, FIXED, "x", 0, &err) == NONE, "MAP_FIXED over the main stack accepted");
  host(st)[0] = 1; // still committed

  uint64_t t1 = recomp_vm_alloc_named(&C, NPS5_PAGE + 0x40000, 3, 0, "thread_stack");
  uint64_t t2 = recomp_vm_alloc_named(&C, NPS5_PAGE + 0x40000, 3, 0, "thread_stack");
  CHECK("m3", t1 != NONE && t2 == t1 + NPS5_PAGE + 0x40000, "thread stacks not adjacent (0x%llx 0x%llx)", (unsigned long long)t1, (unsigned long long)t2);
  nps5_mem_protect_inner(t1, NPS5_PAGE, PROT_NONE);
  nps5_mem_protect_inner(t2, NPS5_PAGE, PROT_NONE);
  Nps5FaultInfo f1, f2, f3;
  nps5_mem_classify((uint64_t)(uintptr_t)host(t1 + 8), &f1);
  nps5_mem_classify((uint64_t)(uintptr_t)host(t2 + 8), &f2);
  nps5_mem_classify((uint64_t)(uintptr_t)host(t2 + NPS5_PAGE + 8), &f3);
  CHECK("m3", f1.cls == NPS5_FA_STACK_GUARD, "first guard: %s", nps5_fault_class_name(f1.cls));
  CHECK("m3", f2.cls == NPS5_FA_STACK_GUARD, "second thread's guard: %s [0x%llx-0x%llx]", nps5_fault_class_name(f2.cls),
        (unsigned long long)f2.lo, (unsigned long long)f2.hi);
  CHECK("m3", f3.cls == NPS5_FA_MAPPED && f3.lo == t2, "second thread's stack body: %s", nps5_fault_class_name(f3.cls));
  // Guest-renaming a thread stack must not change its classification.
  sceKernelSetVirtualRangeName(&C, (void *)(uintptr_t)t2, NPS5_PAGE + 0x40000, "renamed");
  nps5_mem_classify((uint64_t)(uintptr_t)host(t2 + 8), &f2);
  CHECK("m6", f2.cls == NPS5_FA_STACK_GUARD, "renamed thread stack lost its guard class: %s", nps5_fault_class_name(f2.cls));
  recomp_vm_free(&C, t1, NPS5_PAGE + 0x40000);
  recomp_vm_free(&C, t2, NPS5_PAGE + 0x40000);

  errno = 0;
  CHECK("m4", nps5_mem_commit(0x8000000, UINT64_MAX - 0x1000) == -1 && errno == EINVAL, "wrapping commit accepted");
  errno = 0;
  CHECK("m4", nps5_mem_decommit(0x8000000, UINT64_MAX - 0x1000) == -1 && errno == EINVAL, "wrapping decommit accepted");
  CHECK("m4", nps5_mem_protect_inner(0x8000000, UINT64_MAX - 0x1000, PROT_NONE) == -1, "wrapping protect accepted");

  CHECK("n3", recomp_vm_map(&C, 0, 0x4000, 1ULL << 63 | 1, 3, 0, "hugealign") == NONE, "alignment above 2^63 accepted");
  // The runtime-internal allocators take the alignment unchecked: the explicit guard in vm_alloc_zone
  // is what keeps a non-power-of-two above 2^63 away from the round-up shift (which would be 1 << 64).
  {
    size_t nb = 0;
    for (GuestVMExtent *e = C.vm_extents; e; e = e->next) nb++;
    static const uint64_t bad[] = {1ULL << 63 | 1, UINT64_MAX, 1ULL << 63, 1ULL << 62 | 1, 1ULL << 41 | 3};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
      CHECK("n3", recomp_vm_alloc_named_aligned(&C, 0x4000, bad[i], 3, 0, "hugealign") == NONE, "system alloc, alignment 0x%llx accepted", (unsigned long long)bad[i]);
      CHECK("n3", recomp_vm_map_guest(&C, 0x4000, bad[i], 3, 0, "hugealign") == NONE, "user alloc, alignment 0x%llx accepted", (unsigned long long)bad[i]);
    }
    size_t na = 0;
    for (GuestVMExtent *e = C.vm_extents; e; e = e->next) na++;
    CHECK("n3", na == nb, "a refused huge alignment changed the extent list (%zu -> %zu)", nb, na);
  }
  CHECK("n3", recomp_vm_map(&C, 0, 0x4000, 1ULL << 50, 3, 0, "hugealign") == NONE, "alignment beyond the window accepted");
}


// ---- Mapping hooks (GPU registry follow-up): unregister BEFORE pages are freed ----
// A shadow registry stands for the AGC driver: on_map adds, on_unmap removes (and checks the pages are
// still there), on_protect changes flags. A checker thread reads every registered range under the registry
// mutex the whole time: if a hook ran after the host pages were released, the read would fail.
#define HK_MAX 64
typedef struct { uint64_t lo, hi; int prot; } HkRange;
static HkRange hk_reg[HK_MAX];
static int hk_n;
static pthread_mutex_t hk_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int hk_stop, hk_bad, hk_events;
static int hk_busy_ok = 1;

static void hk_map(void *u, uint64_t a, uint64_t s, int prot, int kind, const char *n) {
  (void)u; (void)n;
  if (prot == 0 || kind == NPS5_EXT_MAIN_STACK) return; // the driver ignores PROT_NONE and runtime-owned extents
  pthread_mutex_lock(&hk_mu);
  uint8_t b;
  if (!nps5_guest_safe_read(a, &b, 1) || !nps5_guest_safe_read(a + s - 1, &b, 1)) { atomic_store(&hk_bad, 1); printf("hk: map %llx+%llx unreadable\n", (unsigned long long)a, (unsigned long long)s); }
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo == a && hk_reg[i].hi == a + s) { pthread_mutex_unlock(&hk_mu); return; }
  // An observer may keep the host pages it is shown (the AGC driver imports them for the GPU): mark the page here,
  // test_hooks checks that the mark is still what the guest address reads after the map call returned.
  if (kind == NPS5_EXT_DIRECT && (prot & 2)) host(a)[0x20] = 0xa7;
  if (hk_n < HK_MAX) hk_reg[hk_n++] = (HkRange){a, a + s, prot};
  atomic_fetch_add(&hk_events, 1);
  pthread_mutex_unlock(&hk_mu);
}
static void hk_unmap(void *u, uint64_t a, uint64_t s) {
  (void)u;
  GuestVMExtent q; // before hk_mu (on_map takes hk_mu under vm_mutex): proves vm_mutex is not held (else this deadlocks)
  int busy = recomp_vm_query(&C, a, &q) && !q.is_free && q.busy;
  pthread_mutex_lock(&hk_mu);
  if (!busy) hk_busy_ok = 0;
  uint8_t b;
  if (!nps5_guest_safe_read(a, &b, 1) || !nps5_guest_safe_read(a + s - 1, &b, 1)) { atomic_store(&hk_bad, 1); printf("hk: unmap %llx+%llx unreadable\n", (unsigned long long)a, (unsigned long long)s); }
  for (int i = 0; i < hk_n; i++) { // [a, a+s) may be part of a registered range (partial unmap): keep the rest
    HkRange r = hk_reg[i];
    if (r.hi <= a || r.lo >= a + s) continue;
    hk_reg[i] = hk_reg[--hk_n];
    if (r.lo < a && hk_n < HK_MAX) hk_reg[hk_n++] = (HkRange){r.lo, a, r.prot};
    if (r.hi > a + s && hk_n < HK_MAX) hk_reg[hk_n++] = (HkRange){a + s, r.hi, r.prot};
    i = -1;
  }
  atomic_fetch_add(&hk_events, 1);
  pthread_mutex_unlock(&hk_mu);
}
static void hk_prot(void *u, uint64_t a, uint64_t s, int prot, int kind, const char *n) {
  (void)u; (void)kind; (void)n;
  pthread_mutex_lock(&hk_mu);
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo == a && hk_reg[i].hi == a + s) hk_reg[i].prot = prot;
  atomic_fetch_add(&hk_events, 1);
  pthread_mutex_unlock(&hk_mu);
}
static void *hk_checker(void *arg) {
  (void)arg;
  while (!atomic_load(&hk_stop)) {
    pthread_mutex_lock(&hk_mu);
    for (int i = 0; i < hk_n; i++) {
      uint8_t b;
      for (uint64_t a = hk_reg[i].lo; a < hk_reg[i].hi; a += NPS5_PAGE)
        if (!nps5_guest_safe_read(a, &b, 1)) { atomic_store(&hk_bad, 1); printf("hk: checker %llx in [%llx,%llx)\n", (unsigned long long)a, (unsigned long long)hk_reg[i].lo, (unsigned long long)hk_reg[i].hi); break; } // the driver would see freed pages
    }
    pthread_mutex_unlock(&hk_mu);
  }
  return NULL;
}
static void *hk_churn(void *arg) {
  (void)arg;
  for (int i = 0; i < 3000; i++) {
    uint64_t g = recomp_vm_map_guest(&C, 0x8000, 0x4000, 3, 0, "hk");
    if (g == NONE) continue;
    if (i % 3 == 0) recomp_vm_protect(&C, g, 0x4000, 1, -1);
    if (i % 5 == 0) recomp_vm_free(&C, g + 0x4000, 0x4000); // partial unmap
    recomp_vm_free(&C, g, 0x8000);
  }
  return NULL;
}
// on_alias: two direct mappings of one physical range are reported, before the on_map of the second, so an observer
// that watches pages by protection (the AGC write watch) never watches memory with a second address.
static uint64_t al_a, al_b, al_size;
static int al_calls, al_before_map;
static void hk_alias(void *u, uint64_t a, uint64_t b, uint64_t size) {
  (void)u;
  al_a = a; al_b = b; al_size = size; al_calls++;
  pthread_mutex_lock(&hk_mu);
  al_before_map = 1;
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo <= a && a < hk_reg[i].hi) al_before_map = 0;
  pthread_mutex_unlock(&hk_mu);
}

static void test_hooks(void) {
  Nps5VmHooks h = {.on_map = hk_map, .on_protect = hk_prot, .on_unmap = hk_unmap, .on_alias = hk_alias};
  uint64_t pre = recomp_vm_map_guest(&C, 0x4000, 0x4000, 3, 0, "hk_pre");
  int id = recomp_vm_hooks_register(&h);
  CHECK("HK", id >= 0, "register failed");
  pthread_mutex_lock(&hk_mu);
  int replayed = 0;
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo == pre) replayed = 1;
  pthread_mutex_unlock(&hk_mu);
  CHECK("HK", replayed, "existing mapping was not replayed to on_map");

  uint64_t g = recomp_vm_map_guest(&C, 0x8000, 0x4000, 3, 0, "hk_one");
  CHECK("HK", g != NONE && atomic_load(&hk_events) >= 2, "on_map not called");
  recomp_vm_protect(&C, g, 0x8000, 1, -1);
  pthread_mutex_lock(&hk_mu);
  int prot = -1;
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo == g) prot = hk_reg[i].prot;
  pthread_mutex_unlock(&hk_mu);
  CHECK("HK", prot == 1, "on_protect not seen (prot %d)", prot);
  // MAP_FIXED over it: the old mapping is unregistered (pages still present), the new one registered
  uint64_t r = recomp_vm_map_fixed_replace(&C, g, 0x8000, 3, 0, "hk_new");
  CHECK("HK", r == g, "replace failed");
  recomp_vm_free(&C, g, 0x8000);
  pthread_mutex_lock(&hk_mu);
  int left = 0;
  for (int i = 0; i < hk_n; i++) if (hk_reg[i].lo >= g && hk_reg[i].lo < g + 0x8000) left = 1;
  pthread_mutex_unlock(&hk_mu);
  CHECK("HK", !left, "mapping still registered after free");

  // A direct mapping is reported only when its pages are final: sceKernelMapDirectMemory aliases the physical block
  // over the range after the extent exists, and on_map before that showed pages the alias then replaced (GPU label
  // stores through the driver's import of them never reached the guest: Greenhollow, "GPU rendering timed out").
  off_t dphys = 0;
  void *dm = NULL;
  int ev0 = atomic_load(&hk_events);
  CHECK("HK", sceKernelAllocateDirectMemory(0, 0x10000000, 0x200000, 0x10000, 0, &dphys) == 0, "alloc dm");
  CHECK("HK", sceKernelMapDirectMemory(&C, &dm, 0x200000, 3, 0, dphys, 0x10000) == 0 && dm, "map dm");
  uint64_t dg = (uint64_t)(uintptr_t)dm;
  CHECK("HK", atomic_load(&hk_events) == ev0 + 1, "a direct mapping must be reported exactly once (%d)", atomic_load(&hk_events) - ev0);
  CHECK("HK", host(dg)[0x20] == 0xa7, "on_map of a direct mapping ran before the physical alias (the pages it saw were replaced)");
  CHECK("HK", al_calls == 0, "a single direct mapping was reported as an alias");
  // The middle of the same physical block at a second address: one alias report naming both, before its on_map.
  void *dm2 = NULL;
  CHECK("HK", sceKernelMapDirectMemory(&C, &dm2, 0x100000, 3, 0, dphys + 0x80000, 0x10000) == 0 && dm2, "map dm twice");
  uint64_t dg2 = (uint64_t)(uintptr_t)dm2;
  CHECK("HK", al_calls == 1 && al_a == dg2 && al_b == dg + 0x80000 && al_size == 0x100000, "alias report (%d calls: 0x%llx 0x%llx +0x%llx)", al_calls,
        (unsigned long long)al_a, (unsigned long long)al_b, (unsigned long long)al_size);
  CHECK("HK", al_before_map, "the alias was reported after the on_map of the new mapping");
  host(dg2)[0x40] = 0x3c;
  CHECK("HK", host(dg + 0x80000)[0x40] == 0x3c, "the two mappings do not share pages");
  recomp_vm_free(&C, dg2, 0x100000);
  recomp_vm_free(&C, dg, 0x200000);

  pthread_t chk, w[3];
  atomic_store(&hk_stop, 0);
  pthread_create(&chk, NULL, hk_checker, NULL);
  for (int i = 0; i < 3; i++) pthread_create(&w[i], NULL, hk_churn, NULL);
  for (int i = 0; i < 3; i++) pthread_join(w[i], NULL);
  atomic_store(&hk_stop, 1);
  pthread_join(chk, NULL);
  CHECK("HK", !atomic_load(&hk_bad), "a hook ran when the pages were already gone (or on_map before they existed)");
  CHECK("HK", hk_busy_ok, "on_unmap saw a non-busy or free extent");

  recomp_vm_hooks_unregister(id);
  int ev = atomic_load(&hk_events);
  uint64_t m = recomp_vm_map_guest(&C, 0x4000, 0x4000, 3, 0, "hk_after");
  recomp_vm_free(&C, m, 0x4000);
  CHECK("HK", atomic_load(&hk_events) == ev, "hook called after unregister");
  recomp_vm_free(&C, pre, 0x4000);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setup();
  test_b1();
  test_dm_alias_fail();
  test_query_copy();
  test_m1();
  test_m2();
  test_m3();
  test_m6();
  test_minor();
  test_hooks();
  printf(g_fail ? "VM FAIL\n" : "VM OK\n");
  return g_fail;
}
