// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adversarial tests of the guest arena (pkg/runtime/core/nps5_mem.c), written from
// docs/m1/memory.md sections 2 and 6 and docs/m0/S1.md, not from the implementation.
// Build twice (base / -DNPS5_ADDR_MODE_IDENTITY=1) with core/nps5_mem.c; see tools/memtests.sh.
// Only non-destructive mappings are made outside the runtime's own reservation.
#include "mt.h"
#include "nps5_mem.h"

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/task_info.h>
#include <setjmp.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>

#define AREA "arena"
#if NPS5_ADDR_MODE_IDENTITY
#define MODE "identity"
static const Nps5MemConfig cfg = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_IDENTITY, .host_base = 0,
    .guest_lo = 0x7000400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x7000400000ULL,
    .dyn_lo = 0x7200000000ULL, .pool_lo = 0x8000000000ULL, .stack_top = 0x77EEFFC000ULL, .stack_len = 0x4000000ULL,
};
#define ORG NPS5_IDENTITY_LO
#else
#define MODE "base"
static const Nps5MemConfig cfg = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_BASE, .host_base = NPS5_BASE_SPAN,
    .guest_lo = 0x400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x400000ULL,
    .dyn_lo = 0x200000000ULL, .pool_lo = 0x1000000000ULL, .stack_top = 0x7EEFFC000ULL, .stack_len = 0x4000000ULL,
};
#define ORG 0ULL
#endif

static char err[4096];
static sigjmp_buf jb;
static volatile uint64_t fault_addr;
static void on_fault(int s, siginfo_t *si, void *u) {
  (void)s; (void)u;
  fault_addr = (uint64_t)(uintptr_t)si->si_addr;
  siglongjmp(jb, 1);
}
static int faults(uint64_t host) {
  if (sigsetjmp(jb, 1)) return 1;
  volatile uint8_t v = *(volatile uint8_t *)host;
  (void)v;
  return 0;
}
static int faults_w(uint64_t host) {
  if (sigsetjmp(jb, 1)) return 1;
  *(volatile uint8_t *)host = 1;
  return 0;
}
static uint64_t H(uint64_t guest) { return g_nps5_arena.host_lo + (guest - g_nps5_arena.guest_org); }

static kern_return_t map_page(uint64_t host, uint64_t len) {
  mach_vm_address_t a = host;
  return mach_vm_map(mach_task_self(), &a, len, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
}
static void unmap(uint64_t host, uint64_t len) { mach_vm_deallocate(mach_task_self(), host, len); }
// 1 when nothing is mapped in [lo, lo+len)
static int range_free(uint64_t lo, uint64_t len) {
  mach_vm_address_t a = lo;
  mach_vm_size_t sz = 0;
  natural_t depth = 0;
  vm_region_submap_info_data_64_t info;
  mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
  if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) return 1;
  return a >= lo + len;
}
static uint64_t virtual_size(void) {
  task_vm_info_data_t i;
  mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
  task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &c);
  return i.virtual_size;
}
static Nps5FaultInfo cls(uint64_t host) {
  Nps5FaultInfo f;
  nps5_mem_classify(host, &f);
  return f;
}
static int reserve(void) { return nps5_mem_reserve(&cfg, 0, err, sizeof err); }

#if !NPS5_ADDR_MODE_IDENTITY
#define SLOT(i) ((uint64_t)(i) * NPS5_BASE_SPAN)

static void t_slot_fallback(void) {
  mt_begin("arena.base.slot_fallback_multi", AREA);
  // slot 1 blocked in its LAST page, slot 2 blocked in its FIRST page: both must be detected.
  uint64_t b1 = SLOT(2) - NPS5_PAGE, b2 = SLOT(2);
  MT_EXPECT(map_page(b1, NPS5_PAGE) == 0 && map_page(b2, NPS5_PAGE) == 0, "cannot place blockers");
  *(volatile uint64_t *)b1 = 0x1111; *(volatile uint64_t *)b2 = 0x2222;
  int rc = reserve();
  MT_EXPECT(rc == 0, "reserve failed: %s", err);
  if (rc == 0) {
    MT_EXPECT(g_nps5_arena.host_lo == SLOT(3), "host_lo=0x%llx want 0x%llx", (unsigned long long)g_nps5_arena.host_lo, (unsigned long long)SLOT(3));
    MT_EXPECT(g_nps5_arena.bias == (uint8_t *)SLOT(3), "bias");
    MT_EXPECT(g_nps5_arena.host_len == NPS5_BASE_SPAN, "host_len");
    nps5_mem_release();
  }
  MT_EXPECT(*(volatile uint64_t *)b1 == 0x1111 && *(volatile uint64_t *)b2 == 0x2222, "blocker clobbered");
  unmap(b1, NPS5_PAGE); unmap(b2, NPS5_PAGE);
  mt_end();
}

static void t_slot_exhaust16(void) {
  mt_begin("arena.base.slots_beyond_16", AREA);
  // ADR s2: "tries the preferred slot and then the following 1 TiB slots up to 2^47".
  int placed = 0;
  for (int i = 1; i <= 17; i++)
    if (map_page(SLOT(i) + 0x8000, NPS5_PAGE) == 0) placed++;
  if (placed < 17) mt_skip("could only place %d blockers", placed);
  int rc = reserve();
  MT_EXPECT(rc == 0, "reserve gives up after 16 slots although ADR says it walks to 2^47" " [" "slots 1..17 blocked: %s" "]", rc ? "reserve failed" : "ok");
  if (rc == 0) nps5_mem_release();
  for (int i = 1; i <= 17; i++) unmap(SLOT(i) + 0x8000, NPS5_PAGE);
  mt_end();
}

static void t_all_blocked_report(void) {
  mt_begin("arena.base.all_slots_blocked_report", AREA);
  int placed = 0;
  for (int i = 1; i <= 126; i++)
    if (map_page(SLOT(i) + 0x30004000ULL, NPS5_PAGE) == 0) placed++;
  if (placed < 100) mt_skip("only %d blockers placed", placed);
  *(volatile uint64_t *)(SLOT(1) + 0x30004000ULL) = 0xdeadbeef;
  int rc = reserve();
  MT_EXPECT(rc != 0, "reservation succeeded with every slot blocked?");
  if (rc != 0) {
    MT_EXPECT(g_nps5_arena.host_len == 0, "arena marked reserved after failure");
    MT_EXPECT(strstr(err, "0x10000000000") != NULL, "report does not name the preferred slot: %s", err);
    MT_EXPECT(strstr(err, "0x10030004000-0x10030008000") != NULL, "report does not list the blocking region: %s", err);
    MT_EXPECT(strstr(err, "prot rw-") != NULL, "report lacks protection of blocker");
    MT_EXPECT(strstr(err, "-addr-mode") == NULL || 1, "");
    MT_EXPECT(!strstr(err, "kern_return 0"), "report says kern_return 0");
  }
  MT_EXPECT(*(volatile uint64_t *)(SLOT(1) + 0x30004000ULL) == 0xdeadbeef, "blocker clobbered");
  // truncated / absent error buffers must not overflow
  char tiny[8]; memset(tiny, 'Z', sizeof tiny);
  int rc2 = nps5_mem_reserve(&cfg, 0, tiny, 1);
  MT_EXPECT(rc2 != 0 && tiny[0] == 0 && tiny[1] == 'Z', "errlen=1: wrote beyond buffer or not NUL terminated");
  rc2 = nps5_mem_reserve(&cfg, 0, NULL, 0);
  MT_EXPECT(rc2 != 0, "NULL err");
  char small[40]; memset(small, 'Z', sizeof small);
  rc2 = nps5_mem_reserve(&cfg, 0, small, 33);
  MT_EXPECT(rc2 != 0 && small[33] == 'Z' && memchr(small, 0, 33), "errlen=33: overflow or unterminated");
  for (int i = 1; i <= 126; i++) unmap(SLOT(i) + 0x30004000ULL, NPS5_PAGE);
  mt_end();
}

static void t_base_neighbours(void) {
  mt_begin("arena.base.exact_bounds_neighbours_untouched", AREA);
  // Blockers immediately outside [2^40, 2^41) must not stop the reservation, and must stay intact.
  uint64_t lo = SLOT(1) - NPS5_PAGE, hi = SLOT(2);
  MT_EXPECT(map_page(lo, NPS5_PAGE) == 0 && map_page(hi, NPS5_PAGE) == 0, "blockers");
  *(volatile uint64_t *)lo = 7; *(volatile uint64_t *)hi = 9;
  int rc = reserve();
  MT_EXPECT(rc == 0, "reserve: %s", err);
  if (rc == 0) {
    MT_EXPECT(g_nps5_arena.host_lo == SLOT(1) && g_nps5_arena.host_len == SLOT(1), "bounds lo=0x%llx len=0x%llx", (unsigned long long)g_nps5_arena.host_lo, (unsigned long long)g_nps5_arena.host_len);
    MT_EXPECT(nps5_mem_commit(g_nps5_arena.host_len - NPS5_PAGE, NPS5_PAGE) == 0, "commit last reservation page");
    MT_EXPECT(!faults_w(SLOT(2) - 1) && *(volatile uint8_t *)(SLOT(2) - 1) == 1, "last byte of reservation");
    errno = 0;
    MT_EXPECT(nps5_mem_commit(g_nps5_arena.host_len, NPS5_PAGE) != 0 && errno == EINVAL, "commit one page past the end accepted");
    MT_EXPECT(nps5_mem_decommit(g_nps5_arena.host_len - NPS5_PAGE, 2 * NPS5_PAGE) != 0, "decommit across the end accepted");
    MT_EXPECT(*(volatile uint64_t *)hi == 9 && *(volatile uint64_t *)lo == 7, "neighbour page touched");
    nps5_mem_release();
  }
  unmap(lo, NPS5_PAGE); unmap(hi, NPS5_PAGE);
  mt_end();
}
#else
static void t_identity_conflicts(void) {
  mt_begin("arena.identity.exact_bounds", AREA);
  // Just below and just at the end: outside the arena, must not conflict.
  uint64_t below = NPS5_IDENTITY_LO - NPS5_PAGE, at_hi = NPS5_IDENTITY_HI;
  int ok_hi = map_page(at_hi, NPS5_PAGE) == 0;
  int ok_lo = map_page(below, NPS5_PAGE) == 0;  // macOS keeps the carveout's last page unmappable: optional
  if (!ok_hi) mt_skip("cannot place the blocker at 0xFC_0000_0000");
  if (ok_hi) {
    *(volatile uint64_t *)at_hi = 6;
    if (ok_lo) *(volatile uint64_t *)below = 5;
    int rc = reserve();
    MT_EXPECT(rc == 0, "blockers just outside the arena stop the reservation: %s", err);
    if (rc == 0) {
      MT_EXPECT(g_nps5_arena.host_lo == NPS5_IDENTITY_LO && g_nps5_arena.host_len == NPS5_IDENTITY_HI - NPS5_IDENTITY_LO && g_nps5_arena.bias == NULL, "placement");
      MT_EXPECT(nps5_mem_commit(NPS5_IDENTITY_HI - NPS5_PAGE, NPS5_PAGE) == 0, "last page");
      MT_EXPECT(nps5_mem_commit(NPS5_IDENTITY_HI, NPS5_PAGE) != 0, "page at hi accepted");
      MT_EXPECT(nps5_mem_commit(NPS5_IDENTITY_LO - NPS5_PAGE, NPS5_PAGE) != 0, "page below lo accepted");
      MT_EXPECT(*(volatile uint64_t *)at_hi == 6 && (!ok_lo || *(volatile uint64_t *)below == 5), "outside neighbour touched");
      nps5_mem_release();
    }
    unmap(at_hi, NPS5_PAGE);
    if (ok_lo) unmap(below, NPS5_PAGE);
  }
  mt_end();

  mt_begin("arena.identity.conflict_first_middle_last_page", AREA);
  uint64_t spots[3] = {NPS5_IDENTITY_LO, 0xA123400000ULL & ~NPS5_PAGE_MASK, NPS5_IDENTITY_HI - NPS5_PAGE};
  for (int i = 0; i < 3; i++) {
    if (map_page(spots[i], NPS5_PAGE) != 0) { mt_skip("cannot place blocker %d", i); continue; }
    *(volatile uint64_t *)spots[i] = 0xabc0 + i;
    int rc = reserve();
    MT_EXPECT(rc != 0, "blocker %d (0x%llx) not detected", i, (unsigned long long)spots[i]);
    if (rc != 0) {
      char want[64];
      snprintf(want, sizeof want, "host 0x%llx-0x%llx", (unsigned long long)spots[i], (unsigned long long)spots[i] + NPS5_PAGE);
      MT_EXPECT(strstr(err, want) != NULL, "report lacks '%s'", want);
      MT_EXPECT(strstr(err, "-addr-mode base") != NULL, "report lacks remedy");
      MT_EXPECT(g_nps5_arena.host_len == 0, "arena marked reserved");
    } else nps5_mem_release();
    MT_EXPECT(*(volatile uint64_t *)spots[i] == (uint64_t)(0xabc0 + i), "blocker %d clobbered", i);
    unmap(spots[i], NPS5_PAGE);
  }
  mt_end();

  mt_begin("arena.identity.report_many_regions_capped", AREA);
  // 30 separate blockers: the report lists a bounded number ("... more") and stays NUL terminated.
  int placed = 0;
  for (int i = 0; i < 30; i++) if (map_page(0x8000000000ULL + (uint64_t)i * 0x100000, NPS5_PAGE) == 0) placed++;
  int rc = reserve();
  MT_EXPECT(rc != 0, "reserve succeeded over 30 blockers");
  MT_EXPECT(strstr(err, "more") != NULL && strlen(err) < sizeof err - 1, "no '... more' marker or overflow (len %zu)", strlen(err));
  char t[100]; memset(t, 'Z', sizeof t);
  nps5_mem_reserve(&cfg, 0, t, 50);
  MT_EXPECT(t[49] == 0 && t[50] == 'Z', "errlen=50 overflow/unterminated");
  for (int i = 0; i < 30; i++) unmap(0x8000000000ULL + (uint64_t)i * 0x100000, NPS5_PAGE);
  (void)placed;
  mt_end();
}
#endif

static void t_boundaries_and_classify(void) {
  mt_begin("arena." MODE ".classify_boundaries", AREA);
  int rc = reserve();
  MT_EXPECT(rc == 0, "reserve: %s", err);
  if (rc) { mt_end(); return; }
  const Nps5Arena *a = &g_nps5_arena;
  uint64_t gl = a->guest_lo, gh = a->guest_hi, hl = a->host_lo, len = a->host_len;
  Nps5FaultInfo f;
  f = cls(H(a->guest_org)); MT_EXPECT(f.cls == NPS5_FA_NULL_GUARD && f.guest == a->guest_org, "guest NULL not null guard (%d)", f.cls);
  f = cls(H(gl - 1)); MT_EXPECT(f.cls == NPS5_FA_NULL_GUARD, "last byte below guest_lo is %d", f.cls);
  f = cls(H(gl)); MT_EXPECT(f.cls == NPS5_FA_UNMAPPED, "guest_lo is %d, want UNMAPPED", f.cls);
  f = cls(H(a->guest_org + 0x3FFFFF)); MT_EXPECT(f.cls == NPS5_FA_NULL_GUARD, "4 MiB - 1 is %d", f.cls);
  f = cls(H(a->guest_org + 0x400000)); MT_EXPECT(f.cls != NPS5_FA_NULL_GUARD, "4 MiB is still null guard");
  f = cls(H(gh - 1)); MT_EXPECT(f.cls == NPS5_FA_UNMAPPED, "guest_hi-1 is %d", f.cls);
#if !NPS5_ADDR_MODE_IDENTITY
  f = cls(H(gh)); MT_EXPECT(f.cls == NPS5_FA_BEYOND && f.guest == gh, "guest_hi is %d", f.cls);
  f = cls(hl + len - 1); MT_EXPECT(f.cls == NPS5_FA_BEYOND, "last reservation byte is %d", f.cls);
#endif
  f = cls(hl + len); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "byte after reservation is %d", f.cls);
  f = cls(hl - 1); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "byte before reservation is %d", f.cls);
  f = cls(0); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "host NULL is %d", f.cls);
  f = cls(0x4000); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "host low page is %d", f.cls);
  f = cls(~0ULL); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "UINT64_MAX is %d", f.cls);
  f = cls(NPS5_HOST_CARVEOUT_LO + 0x1000); MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "host carveout is %d", f.cls);
  // Real faults across the whole null guard.
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO; sa.sa_sigaction = on_fault;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  uint64_t offs[] = {0, 1, 8, 0x1000, 0x3FFFF8, 0x3FFFFF, 0x3FC000, 0x200000};
  for (size_t i = 0; i < sizeof offs / sizeof *offs; i++) {
    uint64_t h = H(a->guest_org + offs[i]);
    int r = faults(h), w = faults_w(h);
    MT_EXPECT(r && w, "null guard +0x%llx does not fault (r=%d w=%d)", (unsigned long long)offs[i], r, w);
    if (r) { f = cls(fault_addr); MT_EXPECT(f.cls == NPS5_FA_NULL_GUARD, "fault at +0x%llx classified %d", (unsigned long long)offs[i], f.cls); }
  }
  nps5_mem_release();
  mt_end();
}

static void t_release_semantics(void) {
  mt_begin("arena." MODE ".repeated_init_teardown", AREA);
  uint64_t v0 = virtual_size();
  int bad = 0;
  uint64_t first_lo = 0;
  for (int i = 0; i < 200; i++) {
    if (reserve() != 0) { bad = i + 1; MT_EXPECT(0, "cycle %d reserve: %s", i, err); break; }
    if (i == 0) first_lo = g_nps5_arena.host_lo;
    uint64_t g = g_nps5_arena.guest_lo + 0x10000000ULL;
    nps5_mem_commit(g, NPS5_PAGE);
    uint8_t *p = (uint8_t *)H(g);
    if (p[0] != 0 || p[NPS5_PAGE - 1] != 0) { MT_EXPECT(0, "cycle %d: stale data after re-reserve", i); bad = 1; }
    memset(p, 0xAB, NPS5_PAGE);
    uint64_t lo = g_nps5_arena.host_lo, ln = g_nps5_arena.host_len;
    nps5_mem_release();
    if (!range_free(lo, ln)) { MT_EXPECT(0, "cycle %d: arena range not released", i); bad = 1; }
    if (g_nps5_arena.host_len != 0) { MT_EXPECT(0, "arena not cleared by release"); bad = 1; }
    if (bad) break;
  }
  MT_EXPECT(first_lo != 0, "never reserved");
  uint64_t v1 = virtual_size();
  MT_EXPECT(v1 < v0 + (64ULL << 20), "virtual size grew by %lld MiB over 200 cycles", (long long)((v1 - v0) >> 20));
  nps5_mem_release(); nps5_mem_release();  // double release: harmless
  MT_EXPECT(nps5_mem_commit(0x400000, NPS5_PAGE) != 0, "commit works after teardown");
  MT_EXPECT(nps5_mem_decommit(0x400000, NPS5_PAGE) != 0, "decommit works after teardown");
  Nps5FaultInfo f = cls(first_lo + 0x1000);
  MT_EXPECT(f.cls == NPS5_FA_NOT_GUEST, "classify works after teardown (%d)", f.cls);
  mt_end();

  mt_begin("arena." MODE ".teardown_resets_classification_tables", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  uint64_t g = g_nps5_arena.guest_org + 0x500000;
  nps5_mem_note_range(NPS5_FA_IMAGE, g, g + 0x100000, "old_image");
  nps5_mem_map_begin(); nps5_mem_map_add(g + 0x200000, g + 0x210000, "old_map", NPS5_EXT_MAPPED); nps5_mem_map_end();
  nps5_mem_release();
  MT_EXPECT(reserve() == 0, "re-reserve: %s", err);
  Nps5FaultInfo f1 = cls(H(g + 0x10)), f2 = cls(H(g + 0x200010));
  MT_EXPECT(f1.cls == NPS5_FA_UNMAPPED, "nps5_mem_release keeps the fixed-range table: the old image still classifies" " [" "after teardown+init 0x%llx is class %d name %s" "]", (unsigned long long)g, f1.cls, f1.name);
  MT_EXPECT(f2.cls == NPS5_FA_UNMAPPED, "nps5_mem_release keeps the mapping snapshot: a stale mapping still classifies" " [" "after teardown+init 0x%llx is class %d" "]", (unsigned long long)(g + 0x200000), f2.cls);
  nps5_mem_release();
  mt_end();

  mt_begin("arena." MODE ".fixed_range_table_capacity", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  uint64_t base = g_nps5_arena.guest_org + 0x10000000ULL;
  for (int i = 0; i < 80; i++) nps5_mem_note_range(NPS5_FA_IMAGE, base + (uint64_t)i * 0x100000, base + (uint64_t)i * 0x100000 + 0x10000, "mod");
  int lost = 0;
  for (int i = 0; i < 80; i++) if (cls(H(base + (uint64_t)i * 0x100000 + 8)).cls != NPS5_FA_IMAGE) lost++;
  MT_EXPECT(lost == 0, "nps5_mem_note_range silently drops ranges beyond 64 (titles with many modules lose image classification)" " [" "%d of 80 noted image ranges do not classify" "]", lost);
  nps5_mem_release();
  mt_end();
}

// Review 1 n1: after a decommit the pages must still have the reservation's maximum protection
// (read-write): guest memory is never executable on the host, also after the first munmap.
static void t_decommit_max_protection(void) {
  mt_begin("arena." MODE ".decommit_keeps_max_protection_rw", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  uint64_t g = g_nps5_arena.guest_lo + 0x20000000ULL;
  MT_EXPECT(nps5_mem_commit(g, 4 * NPS5_PAGE) == 0, "commit");
  MT_EXPECT(nps5_mem_decommit(g, 4 * NPS5_PAGE) == 0, "decommit");
  mach_vm_address_t a = (mach_vm_address_t)H(g);
  mach_vm_size_t sz = 0; natural_t depth = 0;
  vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
  kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt);
  MT_EXPECT(kr == KERN_SUCCESS && a <= (mach_vm_address_t)H(g), "region query failed (%d)", kr);
  if (kr == KERN_SUCCESS) {
    MT_EXPECT(info.protection == VM_PROT_NONE, "decommitted pages are not inaccessible (prot %d)", info.protection);
    MT_EXPECT(!(info.max_protection & VM_PROT_EXECUTE), "decommit made the pages potentially executable (max protection %d)", info.max_protection);
  }
  nps5_mem_release();
  mt_end();
}

static void t_wrap_and_zero(void) {
  mt_begin("arena." MODE ".commit_decommit_wrap_and_zero", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  uint64_t g = g_nps5_arena.guest_lo + 0x10000000ULL;
  errno = 0;
  int rc = nps5_mem_commit(g + 1, ~0ULL);
  MT_EXPECT(rc != 0, "nps5_mem_commit(addr, ~0) wraps to an empty range and reports success" " [" "rc=%d" "]", rc);
  rc = nps5_mem_decommit(g + 1, ~0ULL);
  MT_EXPECT(rc != 0, "nps5_mem_decommit(addr, ~0) wraps to an empty range and reports success" " [" "rc=%d" "]", rc);
  rc = nps5_mem_commit(g, 0);
  MT_EXPECT(rc == 0, "zero-length commit failed");
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO; sa.sa_sigaction = on_fault;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  MT_EXPECT(faults(H(g)), "zero-length commit made a page accessible");
  // sub-page range widens to exactly one page
  MT_EXPECT(nps5_mem_commit(g + NPS5_PAGE - 1, 2) == 0, "straddling commit");
  MT_EXPECT(!faults(H(g)) && !faults(H(g + NPS5_PAGE)) && faults(H(g + 2 * NPS5_PAGE)) && faults(H(g - 1)), "straddling 1+1 bytes must commit exactly 2 pages");
  // decommit zero-fills
  memset((void *)H(g), 0x77, 2 * NPS5_PAGE);
  nps5_mem_decommit(g, 2 * NPS5_PAGE);
  nps5_mem_commit(g, 2 * NPS5_PAGE);
  MT_EXPECT(*(uint8_t *)H(g) == 0 && *(uint8_t *)H(g + 2 * NPS5_PAGE - 1) == 0, "decommit then commit not zero-filled");
  // protect_inner: sub-page range changes nothing; range straddling the end refused
  MT_EXPECT(nps5_mem_protect_inner(g + 1, NPS5_PAGE - 2, PROT_NONE) == 0 && !faults(H(g)), "sub-page protect_inner changed a page");
  MT_EXPECT(nps5_mem_protect_inner(g_nps5_arena.guest_org + g_nps5_arena.host_len - NPS5_PAGE, 3 * NPS5_PAGE, PROT_READ) != 0, "protect_inner past the end accepted");
  MT_EXPECT(nps5_mem_protect_inner(g + 1, ~0ULL, PROT_NONE) != 0, "nps5_mem_protect_inner(addr, ~0) wraps to an empty range and reports success" " [" "accepted" "]");
  nps5_mem_release();
  mt_end();
}

static void t_map_snapshot(void) {
  mt_begin("arena." MODE ".mapping_snapshot_capacity", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  uint64_t base = g_nps5_arena.guest_lo + 0x10000000ULL;
  nps5_mem_map_begin();
  for (int i = 0; i < 3000; i++) {
    char n[16]; snprintf(n, sizeof n, "m%d", i);
    nps5_mem_map_add(base + (uint64_t)i * 0x8000, base + (uint64_t)i * 0x8000 + 0x4000, n, NPS5_EXT_MAPPED);
  }
  nps5_mem_map_end();
  Nps5FaultInfo f = cls(H(base + 5 * 0x8000 + 8));
  MT_EXPECT(f.cls == NPS5_FA_MAPPED && f.lo == base + 5 * 0x8000, "early entry class %d", f.cls);
  f = cls(H(base + 2999ULL * 0x8000 + 8));
  MT_EXPECT(f.cls != NPS5_FA_UNMAPPED, "an address in a mapping beyond the 2048-entry snapshot is reported as UNMAPPED (misleading crash report; no truncation marker in Nps5FaultInfo)" " [" "class %d" "]", f.cls);
  int fd = open("/dev/null", 1);
  nps5_mem_dump_map(fd);
  close(fd);
  nps5_mem_release();
  mt_end();
  mt_begin("arena." MODE ".snapshot_merges_adjacent_thread_stacks", AREA);
  MT_EXPECT(reserve() == 0, "reserve: %s", err);
  // pieces with the same name merge when adjacent: distinct allocations must stay distinct
  nps5_mem_map_begin();
  nps5_mem_map_add(base, base + NPS5_PAGE, "thread_stack", NPS5_EXT_THREAD_STACK);
  nps5_mem_map_add(base + NPS5_PAGE, base + 2 * NPS5_PAGE, "thread_stack", NPS5_EXT_THREAD_STACK);
  nps5_mem_map_end();
  f = cls(H(base + NPS5_PAGE + 8));  // second stack's guard page
  MT_EXPECT(f.cls == NPS5_FA_STACK_GUARD, "adjacent 'thread_stack' mappings are merged in the snapshot, so the second stack's guard page classifies as an ordinary mapping" " [" "class %d (%s) lo=0x%llx" "]", f.cls, f.name, (unsigned long long)f.lo);
  nps5_mem_release();
  mt_end();
}

// ---- Metal ----
extern void *MTLCreateSystemDefaultDevice(void);
extern void objc_msgSend(void);
extern void *sel_registerName(const char *);
typedef void *(*msg0_t)(void *, void *);
typedef void *(*msg2_t)(void *, void *, size_t, size_t);
#define MSG0(o, s) ((msg0_t)objc_msgSend)((o), sel_registerName(s))
#define NEWBUF(d, n) ((msg2_t)objc_msgSend)((d), sel_registerName("newBufferWithLength:options:"), (size_t)(n), (size_t)0)

static int owned_regions_all_ours(uint64_t lo, uint64_t len, unsigned *bad_tag, uint64_t *bad_addr) {
  mach_vm_address_t a = lo;
  while (a < lo + len) {
    mach_vm_size_t sz = 0; natural_t depth = 0;
    vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
    if (a >= lo + len) break;
    if (info.user_tag != 240) { *bad_tag = info.user_tag; *bad_addr = a; return 0; }
    a += sz;
  }
  return 1;
}

static void t_metal(void) {
  mt_begin("arena." MODE ".reserve_after_metal_init", AREA);
  void *dev = MTLCreateSystemDefaultDevice();
  if (!dev) { mt_skip("no Metal device in this environment"); mt_end(); mt_begin("arena." MODE ".metal_init_after_reserve", AREA); mt_skip("no Metal device"); mt_end(); return; }
  void *q = MSG0(dev, "newCommandQueue");
  void *buf = NEWBUF(dev, 512ULL << 20);
  (void)q;
  int rc = reserve();
  MT_EXPECT(rc == 0, "reserve after Metal init failed: %s", err);
  if (rc == 0) {
    MT_EXPECT(nps5_mem_commit(g_nps5_arena.guest_lo + 0x10000000ULL, NPS5_PAGE) == 0, "commit");
    nps5_mem_release();
  }
  (void)buf;
  mt_end();
  // The other order, on a fresh process state: reserve, then bring Metal up inside the process and
  // allocate; nothing of Metal may land inside the reservation.
  mt_begin("arena." MODE ".metal_allocations_avoid_reservation", AREA);
  rc = reserve();
  MT_EXPECT(rc == 0, "reserve: %s", err);
  if (rc == 0) {
    uint64_t g = g_nps5_arena.guest_lo + 0x10000000ULL;
    nps5_mem_commit(g, NPS5_PAGE);
    memset((void *)H(g), 0x5A, NPS5_PAGE);
    void *bufs[8];
    for (int i = 0; i < 8; i++) {
      bufs[i] = NEWBUF(dev, 256ULL << 20);
    }
    unsigned bt = 0; uint64_t ba = 0;
    MT_EXPECT(owned_regions_all_ours(g_nps5_arena.host_lo, g_nps5_arena.host_len, &bt, &ba), "foreign region (tag %u) at 0x%llx inside the reservation", bt, (unsigned long long)ba);
    MT_EXPECT(*(uint8_t *)H(g) == 0x5A && *(uint8_t *)H(g + NPS5_PAGE - 1) == 0x5A, "committed page changed after Metal allocations");
    for (int i = 0; i < 8; i++) if (bufs[i]) MSG0(bufs[i], "release");
    nps5_mem_release();
  }
  mt_end();
}

int main(void) {
  mt_init();
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO; sa.sa_sigaction = on_fault;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
#if NPS5_ADDR_MODE_IDENTITY
  t_identity_conflicts();
#else
  t_slot_fallback();
  t_base_neighbours();
  t_slot_exhaust16();
  t_all_blocked_report();
#endif
  t_boundaries_and_classify();
  t_release_semantics();
  t_wrap_and_zero();
  t_decommit_max_protection();
  t_map_snapshot();
  t_metal();
  return mt_status();
}
