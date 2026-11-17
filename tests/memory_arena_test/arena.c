// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host test of the guest arena (pkg/runtime/core/nps5_mem.c): collision-safe reservation in both
// addressing modes, commit/decommit, and fault classification. Built twice by
// pkg/runtime/mem_arena_darwin_test.go: plain (base mode) and with -DNPS5_ADDR_MODE_IDENTITY=1.
// The process only ever maps memory with non-destructive calls or inside its own reservation.

#include "nps5_mem.h"

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int g_fail;
#define CHECK(cond, ...)                                                                       \
  do {                                                                                         \
    if (!(cond)) {                                                                             \
      g_fail = 1;                                                                              \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                              \
      printf(__VA_ARGS__);                                                                     \
      printf("\n");                                                                            \
    }                                                                                          \
  } while (0)

// One page mapped at a fixed host address without overwriting anything; returns 0 on success.
static int block_at(uint64_t host) {
  mach_vm_address_t a = host;
  return mach_vm_map(mach_task_self(), &a, NPS5_PAGE, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) == KERN_SUCCESS
             ? 0
             : -1;
}

static sigjmp_buf g_jmp;
static volatile uint64_t g_fault_addr;
static void on_fault(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)uc;
  g_fault_addr = (uint64_t)(uintptr_t)si->si_addr;
  siglongjmp(g_jmp, 1);
}

// Returns 1 if reading *p faults.
static int faults(const volatile uint8_t *p) {
  if (sigsetjmp(g_jmp, 1)) return 1;
  volatile uint8_t v = *p;
  (void)v;
  return 0;
}

static uint8_t *host_of(uint64_t guest) { return g_nps5_arena.bias + guest; }

#if NPS5_ADDR_MODE_IDENTITY
static const Nps5MemConfig cfg = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_IDENTITY, .host_base = 0,
    .guest_lo = 0x7000400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x7000400000ULL,
    .dyn_lo = 0x7200000000ULL, .pool_lo = 0x8000000000ULL, .stack_top = 0x77EEFFC000ULL, .stack_len = 0x4000000ULL,
};
static const Nps5MemConfig wrong_mode = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_BASE, .host_base = NPS5_BASE_SPAN,
    .guest_lo = 0x400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x400000ULL,
    .dyn_lo = 0x200000000ULL, .pool_lo = 0x1000000000ULL, .stack_top = 0x7EEFFC000ULL, .stack_len = 0x4000000ULL,
};
#else
static const Nps5MemConfig cfg = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_BASE, .host_base = NPS5_BASE_SPAN,
    .guest_lo = 0x400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x400000ULL,
    .dyn_lo = 0x200000000ULL, .pool_lo = 0x1000000000ULL, .stack_top = 0x7EEFFC000ULL, .stack_len = 0x4000000ULL,
};
static const Nps5MemConfig wrong_mode = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_IDENTITY, .host_base = 0,
    .guest_lo = 0x7000400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x7000400000ULL,
    .dyn_lo = 0x7200000000ULL, .pool_lo = 0x8000000000ULL, .stack_top = 0x77EEFFC000ULL, .stack_len = 0x4000000ULL,
};
#endif

int main(void) {
  char err[2048];
  struct sigaction sa = {0};
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = on_fault;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  // A config for the other mode than this binary was compiled for is refused.
  CHECK(nps5_mem_reserve(&wrong_mode, 0, err, sizeof(err)) != 0 && strstr(err, "identity"), "mode mismatch: %s", err);
  Nps5MemConfig bad_abi = cfg;
  bad_abi.abi = NPS5_MEM_ABI + 1;
  CHECK(nps5_mem_reserve(&bad_abi, 0, err, sizeof(err)) != 0 && strstr(err, "ABI"), "abi mismatch: %s", err);

#if NPS5_ADDR_MODE_IDENTITY
  // Something already lives inside the identity arena: the reservation must fail, say what is
  // in the way, and leave it untouched.
  const uint64_t blocker = 0x8000000000ULL;
  CHECK(block_at(blocker) == 0, "cannot place the blocker page");
  *(volatile uint64_t *)blocker = 0x1234567890abcdefULL;
  CHECK(nps5_mem_reserve(&cfg, 0, err, sizeof(err)) != 0, "reservation over an occupied range succeeded");
  CHECK(strstr(err, "0x8000000000-0x8000004000") != NULL, "error does not list the conflicting region:\n%s", err);
  CHECK(strstr(err, "-addr-mode base") != NULL, "error does not say what to do:\n%s", err);
  CHECK(*(volatile uint64_t *)blocker == 0x1234567890abcdefULL, "the blocking mapping was clobbered");
  CHECK(g_nps5_arena.host_len == 0, "arena marked reserved after a failure");
  printf("identity conflict message:\n%s", err);
  mach_vm_deallocate(mach_task_self(), blocker, NPS5_PAGE);
  CHECK(nps5_mem_reserve(&cfg, 0, err, sizeof(err)) == 0, "reserve: %s", err);
  CHECK(g_nps5_arena.bias == NULL && g_nps5_arena.host_lo == NPS5_IDENTITY_LO &&
            g_nps5_arena.host_len == NPS5_IDENTITY_HI - NPS5_IDENTITY_LO,
        "identity arena placement");
  const uint64_t org = NPS5_IDENTITY_LO;
#else
  // The preferred 1 TiB slot is occupied: base mode moves to the next slot (the bias is a
  // run-time value), again without touching what is there.
  const uint64_t blocker = NPS5_BASE_SPAN + 0x30004000ULL;
  CHECK(block_at(blocker) == 0, "cannot place the blocker page");
  *(volatile uint64_t *)blocker = 0x1234567890abcdefULL;
  CHECK(nps5_mem_reserve(&cfg, 0, err, sizeof(err)) == 0, "reserve: %s", err);
  CHECK(g_nps5_arena.host_lo == 2 * NPS5_BASE_SPAN && g_nps5_arena.bias == (uint8_t *)(uintptr_t)(2 * NPS5_BASE_SPAN),
        "fallback slot: host_lo 0x%llx", (unsigned long long)g_nps5_arena.host_lo);
  CHECK(*(volatile uint64_t *)blocker == 0x1234567890abcdefULL, "the blocking mapping was clobbered");
  nps5_mem_release();
  mach_vm_deallocate(mach_task_self(), blocker, NPS5_PAGE);
  CHECK(nps5_mem_reserve(&cfg, 0, err, sizeof(err)) == 0, "reserve: %s", err);
  CHECK(g_nps5_arena.host_lo == NPS5_BASE_SPAN, "preferred slot: host_lo 0x%llx", (unsigned long long)g_nps5_arena.host_lo);
  const uint64_t org = 0;
#endif
  CHECK(nps5_mem_reserve(&cfg, 0, err, sizeof(err)) != 0, "second reservation accepted");

  // Nothing is accessible until committed.
  const uint64_t g1 = org + 0x200000000ULL, g2 = org + 0x3000200000ULL;
  CHECK(faults(host_of(g1)), "uncommitted guest page is readable");
  CHECK(nps5_mem_commit(g1 + 0x10, 0x100) == 0, "commit: %s", strerror(errno)); // widened to the page
  CHECK(!faults(host_of(g1)) && !faults(host_of(g1 + NPS5_PAGE - 1)), "committed page is not readable");
  CHECK(faults(host_of(g1 + NPS5_PAGE)), "the page after a one-page commit is readable");
  CHECK(nps5_mem_commit(g2, 2 * NPS5_PAGE) == 0, "commit high: %s", strerror(errno));
  memset(host_of(g2), 0xa5, 2 * NPS5_PAGE);
  CHECK(host_of(g2)[2 * NPS5_PAGE - 1] == 0xa5, "write to a committed page lost");

  // Decommit: inaccessible again, and zero when committed anew.
  CHECK(nps5_mem_decommit(g2, NPS5_PAGE) == 0, "decommit: %s", strerror(errno));
  CHECK(faults(host_of(g2)) && !faults(host_of(g2 + NPS5_PAGE)), "decommit did not affect exactly one page");
  CHECK(nps5_mem_commit(g2, NPS5_PAGE) == 0 && host_of(g2)[0] == 0 && host_of(g2 + NPS5_PAGE)[0] == 0xa5,
        "recommitted page is not zero or the neighbour changed");

  // Read-only protection of whole pages inside a range.
  CHECK(nps5_mem_protect_inner(g2 + 1, 2 * NPS5_PAGE - 1, PROT_READ) == 0, "protect");
  CHECK(!faults(host_of(g2 + NPS5_PAGE)), "read-only page is not readable");

  // Ranges outside the reservation are refused, never mapped.
  errno = 0;
  CHECK(nps5_mem_commit(org + g_nps5_arena.host_len, NPS5_PAGE) != 0 && errno == EINVAL, "commit past the arena accepted");
  CHECK(nps5_mem_decommit(org + g_nps5_arena.host_len - NPS5_PAGE, 2 * NPS5_PAGE) != 0, "decommit across the arena end accepted");
  CHECK(nps5_mem_commit(~0ULL - NPS5_PAGE, 2 * NPS5_PAGE) != 0, "wrapping commit accepted");
#if !NPS5_ADDR_MODE_IDENTITY
  CHECK(nps5_guest_range_ok(0x400000, 0x1000) && !nps5_guest_range_ok(0x3ff000, 0x2000) &&
            !nps5_guest_range_ok(0xFBFFFFF000ULL, 0x2000),
        "guest window check");
#endif

  // Classification: host address -> guest address and kind.
  Nps5FaultInfo fi;
  nps5_mem_note_range(NPS5_FA_IMAGE, org + 0x400000, org + 0x418000, "eboot.bin");
  nps5_mem_map_begin();
  nps5_mem_map_add(org + 0x500000, org + 0x600000, "thread_stack", NPS5_EXT_THREAD_STACK);
  nps5_mem_map_add(g1, g1 + NPS5_PAGE, "mmap", NPS5_EXT_MAPPED);
  nps5_mem_map_end();
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(org + 0x10), &fi);
  CHECK(fi.cls == NPS5_FA_NULL_GUARD && fi.guest == org + 0x10, "null guard: %d", fi.cls);
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(org + 0x401234), &fi);
  CHECK(fi.cls == NPS5_FA_IMAGE && fi.guest == org + 0x401234 && strcmp(fi.name, "eboot.bin") == 0, "image: %d", fi.cls);
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(g1 + 8), &fi);
  CHECK(fi.cls == NPS5_FA_MAPPED && strcmp(fi.name, "mmap") == 0 && fi.lo == g1, "mapping: %d %s", fi.cls, fi.name);
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(org + 0x500100), &fi);
  CHECK(fi.cls == NPS5_FA_STACK_GUARD, "thread stack guard: %d", fi.cls);
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(org + 0x504100), &fi);
  CHECK(fi.cls == NPS5_FA_MAPPED, "thread stack body: %d", fi.cls);
  nps5_mem_classify((uint64_t)(uintptr_t)host_of(g2 + 0x100000), &fi);
  CHECK(fi.cls == NPS5_FA_UNMAPPED && fi.guest == g2 + 0x100000, "unmapped: %d", fi.cls);
  nps5_mem_classify(0x1234, &fi);
  CHECK(fi.cls == NPS5_FA_NOT_GUEST, "host null page: %d", fi.cls);
  nps5_mem_classify(0x3000000000ULL, &fi); // host GPU carveout: never guest memory in either mode
  CHECK(fi.cls == NPS5_FA_NOT_GUEST, "host carveout: %d", fi.cls);
  // A real fault reports the address the classifier then explains.
  CHECK(faults(host_of(org + 0x20)) && g_fault_addr == (uint64_t)(uintptr_t)host_of(org + 0x20), "fault address");

  nps5_mem_release();
  CHECK(g_nps5_arena.host_len == 0, "release");
  printf(g_fail ? "ARENA FAIL\n" : "ARENA OK\n");
  return g_fail;
}
