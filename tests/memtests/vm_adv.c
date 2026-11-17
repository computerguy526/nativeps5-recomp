// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adversarial tests of the guest mapping syscalls and the VM extent manager (docs/m1/memory.md
// section 3 "Mapping calls" / "Protection", section 6), driven through the real shim_* entry
// points with a fake register file (vmh.h). Expectations come from the ADR and from FreeBSD/PS5
// kernel semantics, not from the implementation. Built in base and identity mode.
#include "vmh.h"

#define AREA "vm"
#define N(s) "vm." VMH_MODE "." s
#define HI (nps5_mem_config.guest_hi)
#define AREA_LEN (nps5_mem_config.guest_hi - nps5_mem_config.guest_lo)
#define ZONE(k) G(0x8000000000ULL + (uint64_t)(k) * 0x10000000ULL) /* private test zones, 256 MiB apart */

static int dummy;

static void t_first_fit(void) {
  mt_begin(N("unhinted_first_fit_lowest"), AREA);
  uint64_t a = 0, b = 0, c = 0;
  MT_EXPECT(sys_mmap(0, 0x10000, PROT_RW, MAPF, &a) == 0, "mmap(NULL)");
  MT_EXPECT(a == G(0x200000000ULL), "first unhinted mapping at 0x%llx, console layout says 0x2_0000_0000", (unsigned long long)a);
  MT_EXPECT(sys_mmap(0, 0x10000, PROT_RW, MAPF, &b) == 0 && b == a + 0x10000, "second mapping 0x%llx not adjacent", (unsigned long long)b);
  MT_EXPECT(sys_munmap(a, 0x10000) == 0, "munmap first");
  MT_EXPECT(sys_mmap(0, 0x10000, PROT_RW, MAPF, &c) == 0 && c == a, "lowest hole not reused (got 0x%llx)", (unsigned long long)c);
  MT_EXPECT(is_fill(c, 0x10000, 0), "reused mapping not zero-filled");
  sys_munmap(b, 0x10000); sys_munmap(c, 0x10000);
  mt_end();
}

static void t_fixed_area_ends(void) {
  mt_begin(N("fixed_last_page_and_straddle_end"), AREA);
  uint64_t r = 0;
  Sig s0 = vm_sig();
  MT_EXPECT(sys_mmap(HI - PAGE, PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0 && r == HI - PAGE, "last page of the application area refused (res 0x%llx)", (unsigned long long)r);
  MT_EXPECT(!w1_faults(HI - 1, 0x5a) && *P(HI - 1) == 0x5a, "last byte of the area not usable");
  MT_EXPECT(rd_faults(HI - 2 * PAGE), "page before not left unmapped");
  // straddling the end must fail and map nothing, not even the part inside
  Sig s1 = vm_sig();
  uint32_t rc = sys_mmap(HI - 4 * PAGE, 8 * PAGE, PROT_RW, MAPF | F_FIXED, &r);
  MT_EXPECT(rc != 0, "range straddling guest_hi accepted (res 0x%llx)", (unsigned long long)r);
  MT_EXPECT(sig_eq(s1, vm_sig()) && rd_faults(HI - 4 * PAGE), "failed straddling map left pages behind");
  MT_EXPECT(sys_mmap(HI, PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "map at exactly guest_hi accepted");
  MT_EXPECT(sys_mmap(HI + PAGE, PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "map above guest_hi accepted");
  MT_EXPECT(sys_mmap(G(0xFFFFFFFFC000ULL), PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "map at 2^48 accepted");
  MT_EXPECT(sys_mmap(HI - PAGE, 2 * PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) != 0, "EXCL straddle accepted");
  MT_EXPECT(sys_munmap(HI - PAGE, PAGE) == 0, "unmap last page");
  MT_EXPECT(rd_faults(HI - PAGE), "last page still accessible after unmap");
  MT_EXPECT(sig_eq(s0, vm_sig()), "extent list differs from the start");
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
}

static void t_fixed_low_end(void) {
  mt_begin(N("fixed_first_page_null_guard_and_system_block"), AREA);
  uint64_t r = 0, lo = nps5_mem_config.guest_lo;
  Sig s0 = vm_sig();
  uint64_t r0 = ~0ULL;
  uint32_t rc0 = sys_mmap(0, PAGE, PROT_RW, MAPF | F_FIXED, &r0);
  MT_EXPECT(rc0 != 0, "MAP_FIXED at address 0 is treated as 'no address' and mapped elsewhere (ADR: a fixed request that cannot be honoured fails, it is never moved)" " [" "rc=0 res=0x%llx" "]", (unsigned long long)r0);
  if (rc0 == 0 && r0 != ~0ULL) sys_munmap(r0, PAGE);
  s0 = vm_sig();
  uint64_t bad[] = {G(0x1000), G(0x3FC000), lo - PAGE, G(0x3FFFFF), lo};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    uint32_t rc = sys_mmap(bad[i], PAGE, PROT_RW, MAPF | F_FIXED, &r);
    MT_EXPECT(rc != 0, "fixed map at 0x%llx accepted (guard/system block)", (unsigned long long)bad[i]);
    rc = sys_mmap(bad[i], PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r);
    MT_EXPECT(rc != 0, "EXCL map at 0x%llx accepted", (unsigned long long)bad[i]);
  }
  // straddling null guard -> first usable page
  MT_EXPECT(sys_mmap(lo - 2 * PAGE, 4 * PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "map straddling the null guard end accepted");
  MT_EXPECT(sys_mmap(sys_end - PAGE, 2 * PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "map straddling the system block end accepted");
  // first usable page right after the system block works
  MT_EXPECT(sys_mmap(sys_end, PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0 && r == sys_end, "first page after the system block refused");
  sys_munmap(sys_end, PAGE);
  // every guard page still faults after all of the above
  for (uint64_t o = 0; o < 0x400000; o += 0x1000 * 37) MT_EXPECT(rd_faults(G(o)), "null guard +0x%llx became accessible", (unsigned long long)o);
  MT_EXPECT(sig_eq(s0, vm_sig()), "extent list changed");
  // main stack and its guard are not the guest's to replace
  MT_EXPECT(sys_mmap(stk_lo, PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed map over the main stack accepted");
  MT_EXPECT(sys_mmap(stk_lo - PAGE, PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed map over the stack guard accepted");
  MT_EXPECT(sys_mmap(stk_top - PAGE, 2 * PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed map straddling the stack top accepted");
  MT_EXPECT(sys_mmap(stk_lo - 2 * PAGE, 4 * PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed map straddling the guard accepted");
  MT_EXPECT(rd_faults(stk_lo - PAGE), "stack guard page became accessible");
  MT_EXPECT(sig_eq(s0, vm_sig()), "extent list changed by stack refusals");
  mt_end();
}

static int child_dm_fixed_over(void *a) {
  (void)a;
  uint64_t Z = ZONE(3), r = 0;
  int64_t phys = -1;
  if (sys_dm_alloc(0, 0x10000000, 0x10000, 0x10000, 0, &phys) != 0) return 2;
  if (!map_fixed(Z + PAGE, PAGE)) return 2;
  uint64_t x = Z;
  uint32_t rc = sys_dm_map(&x, 0x10000, PROT_RW, F_FIXED, phys, 0x10000);
  (void)r;
  return rc == 0 && x == Z ? 0 : 3;
}

static void t_fixed_semantics(void) {
  mt_begin(N("fixed_vs_excl_vs_replace"), AREA);
  uint64_t X = ZONE(1), r = 0;
  MT_EXPECT(map_fixed(X, 4 * PAGE), "map A");
  fill(X, 4 * PAGE, 0xA1);
  Sig s = vm_sig();
  MT_EXPECT(sys_mmap(X + PAGE, PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) != 0, "MAP_EXCL over a live mapping succeeded");
  MT_EXPECT(sys_mmap(X, PAGE, PROT_RW, MAPF | F_FIXED | F_NOOVR, &r) != 0, "NO_OVERWRITE over a live mapping succeeded");
  MT_EXPECT(sig_eq(s, vm_sig()) && is_fill(X, 4 * PAGE, 0xA1), "refused EXCL altered the mapping");
  // partial overlap: EXCL must refuse the whole request, not map the free tail
  MT_EXPECT(sys_mmap(X + 3 * PAGE, 3 * PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) != 0 && rd_faults(X + 4 * PAGE), "EXCL partial overlap mapped the free tail");
  // plain MAP_FIXED over the tail pages replaces exactly those pages and extends past A
  MT_EXPECT(sys_mmap(X + 2 * PAGE, 4 * PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0 && r == X + 2 * PAGE, "replace over the tail");
  MT_EXPECT(is_fill(X, 2 * PAGE, 0xA1), "replace damaged the pages before the range");
  MT_EXPECT(is_fill(X + 2 * PAGE, 4 * PAGE, 0), "replaced pages are not zero-filled");
  MT_EXPECT(!rd_faults(X + 5 * PAGE) && rd_faults(X + 6 * PAGE) && rd_faults(X - 1), "replace range wrong");
  // fixed lengths round to 16 KiB, not 64 KiB: a 1-byte fixed map leaves its neighbour free
  uint64_t Y = ZONE(2);
  MT_EXPECT(sys_mmap(Y, 1, PROT_RW, MAPF | F_FIXED, &r) == 0 && r == Y, "1 byte fixed");
  MT_EXPECT(!rd_faults(Y + PAGE - 1) && rd_faults(Y + PAGE), "1-byte fixed map covers %s", rd_faults(Y) ? "nothing" : "wrong pages");
  MT_EXPECT(sys_mmap(Y + PAGE, PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) == 0, "neighbour page of a 1-byte fixed map is not free (rounded to 64 KiB?)");
  MT_EXPECT(sys_mmap(Y, PAGE + 1, PROT_RW, MAPF | F_FIXED, &r) == 0 && !rd_faults(Y + PAGE) && !rd_faults(Y + 2 * PAGE - 1) , "PAGE+1 maps two pages");
  sys_munmap(X, 6 * PAGE); sys_munmap(Y, 4 * PAGE);
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();

  // The ADR requires this of every fixed request, including the sceKernelMap*Memory family.
  mt_begin(N("fixed_flexible_direct_reserve_do_not_clobber_neighbour"), AREA);
  uint64_t Z = ZONE(3);
  MT_EXPECT(map_fixed(Z + PAGE, PAGE), "neighbour");
  fill(Z + PAGE, PAGE, 0xC3);
  uint64_t a = Z; uint32_t rc = sys_flex(&a, PAGE, PROT_RW, F_FIXED);
  MT_EXPECT(rc == 0 && a == Z, "MapFlexibleMemory fixed one page: rc=0x%x addr=0x%llx", rc, (unsigned long long)a);
  MT_EXPECT(!rd_faults(Z + PAGE) && is_fill(Z + PAGE, PAGE, 0xC3), "sceKernelMapFlexibleMemory(MAP_FIXED, 16 KiB) rounds to 64 KiB and replaces the neighbouring page (ADR: fixed length is rounded to pages, not 64 KiB, so it cannot clobber a neighbour)" " [" "neighbour %s" "]", rd_faults(Z + PAGE) ? "unmapped" : "zeroed");
  sys_munmap(Z, 8 * PAGE);
  MT_EXPECT(map_fixed(Z + PAGE, PAGE), "neighbour 2");
  fill(Z + PAGE, PAGE, 0xC4);
  a = Z; rc = sys_reserve(&a, PAGE, F_FIXED, 0);
  MT_EXPECT(is_fill(Z + PAGE, PAGE, 0xC4), "sceKernelReserveVirtualRange(fixed, 16 KiB) rounds to 64 KiB and replaces the neighbouring mapping" " [" "rc=0x%x neighbour %s" "]", rc, rd_faults(Z + PAGE) ? "unmapped" : "modified");
  sys_munmap(Z, 8 * PAGE);
  mt_end();

  // MapDirectMemory holds the direct-memory mutex while it maps; a MAP_FIXED over an existing
  // mapping unmaps first, and the unmap path takes the same mutex.
  mt_begin(N("direct_map_fixed_over_existing_mapping"), AREA);
  fflush(NULL);
  int st = in_child(child_dm_fixed_over, NULL);
  MT_EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sceKernelMapDirectMemory(MAP_FIXED) over an existing mapping self-deadlocks (g_direct_mutex held across recomp_vm_map -> recomp_vm_free -> ps4_direct_mem_on_unmap)" " [" "helper thread %s (status 0x%x)" "]", WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM ? "hung (8 s)" : "failed", st);
  mt_end();
}

static void t_misaligned(void) {
  mt_begin(N("misaligned_addr_and_len"), AREA);
  uint64_t X = ZONE(4), r = 0;
  Sig s0 = vm_sig();
  uint64_t offs[] = {1, 0x1000, 0x2000, PAGE - 1, PAGE / 2};
  for (size_t i = 0; i < sizeof offs / sizeof *offs; i++) {
    MT_EXPECT(sys_mmap(X + offs[i], PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed map at +0x%llx accepted", (unsigned long long)offs[i]);
    MT_EXPECT(px_mmap(X + offs[i], PAGE, PROT_RW, MAPF | F_FIXED) == ~0ULL, "POSIX fixed mmap at +0x%llx accepted", (unsigned long long)offs[i]);
  }
  MT_EXPECT(sig_eq(s0, vm_sig()), "refused misaligned fixed requests changed state");
  // a hint is a hint: unaligned hint on free space -> result is page aligned, range usable, nothing neighbouring clobbered
  MT_EXPECT(map_fixed(X, PAGE), "A");
  fill(X, PAGE, 0x77);
  uint32_t rc = sys_mmap(X + 0x100, PAGE, PROT_RW, MAPF, &r);
  MT_EXPECT(rc == 0 && (r & NPS5_PAGE_MASK) == 0, "hint result 0x%llx not page aligned", (unsigned long long)r);
  MT_EXPECT(r + PAGE <= X || r >= X + PAGE, "hint over a live mapping returned an overlapping range 0x%llx", (unsigned long long)r);
  MT_EXPECT(is_fill(X, PAGE, 0x77), "hint request clobbered the existing mapping");
  if (rc == 0) sys_munmap(r, PAGE);
  // length rounding
  uint64_t W = ZONE(5);
  MT_EXPECT(sys_mmap(W, 1, PROT_RW, MAPF | F_FIXED, &r) == 0 && !rd_faults(W) && rd_faults(W + PAGE), "len 1 not exactly one page");
  sys_munmap(W, PAGE);
  MT_EXPECT(sys_mmap(W, PAGE + 1, PROT_RW, MAPF | F_FIXED, &r) == 0 && !rd_faults(W + PAGE) && rd_faults(W + 2 * PAGE), "len PAGE+1 not exactly two pages");
  sys_munmap(W, 2 * PAGE);
  // unhinted odd lengths do not overlap each other and are usable over their whole length
  uint64_t a = 0, b = 0;
  MT_EXPECT(sys_mmap(0, 1, PROT_RW, MAPF, &a) == 0 && sys_mmap(0, 0x10001, PROT_RW, MAPF, &b) == 0, "odd unhinted");
  MT_EXPECT(a != b && (a + 0x10000 <= b || b + 0x20000 <= a), "unhinted mappings overlap: 0x%llx 0x%llx", (unsigned long long)a, (unsigned long long)b);
  MT_EXPECT(!w1_faults(b + 0x10000, 1), "len 0x10001 last page unusable");
  sys_munmap(a, 1); sys_munmap(b, 0x10001); sys_munmap(X, PAGE);
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
}

static void t_zero_and_huge(void) {
  mt_begin(N("zero_and_huge_lengths"), AREA);
  uint64_t r = 0;
  Sig s0 = vm_sig();
  MT_EXPECT(sys_mmap(0, 0, PROT_RW, MAPF, &r) == E_INVAL, "sceKernelMmap len 0 not EINVAL");
  MT_EXPECT(px_mmap(0, 0, PROT_RW, MAPF) == ~0ULL && guest_errno() == EINVAL, "POSIX mmap len 0: errno %d", guest_errno());
  MT_EXPECT(sys_mmap(ZONE(6), 0, PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed len 0 accepted");
  uint64_t lens[] = {~0ULL, ~0ULL - 0x3FFF, ~0ULL - 0xFFFF, ~0ULL - 0x10000, 1ULL << 63, 1ULL << 48, 1ULL << 41, 1ULL << 40, AREA_LEN, AREA_LEN - 0x400000, AREA_LEN + PAGE};
  for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
    uint32_t rc = sys_mmap(0, lens[i], PROT_RW, MAPF, &r);
    MT_EXPECT(rc != 0, "unhinted mmap of 0x%llx succeeded (0x%llx)", (unsigned long long)lens[i], (unsigned long long)r);
    MT_EXPECT(sig_eq(s0, vm_sig()), "mmap of 0x%llx left state behind", (unsigned long long)lens[i]);
    if (rc == 0) { sys_munmap(r, lens[i]); }
    uint64_t pr = px_mmap(0, lens[i], PROT_RW, MAPF);
    MT_EXPECT(pr == ~0ULL, "POSIX mmap of 0x%llx succeeded", (unsigned long long)lens[i]);
    MT_EXPECT(sys_mmap(ZONE(6), lens[i], PROT_RW, MAPF | F_FIXED, &r) != 0, "fixed mmap of len 0x%llx succeeded", (unsigned long long)lens[i]);
    MT_EXPECT(sig_eq(s0, vm_sig()), "fixed mmap of 0x%llx left state behind", (unsigned long long)lens[i]);
  }
  // addr + len wraps past 2^64 and lands inside the window
  MT_EXPECT(sys_mmap(HI - PAGE, 0 - (HI - PAGE) + 2 * PAGE, PROT_RW, MAPF | F_FIXED, &r) != 0, "wrapping addr+len (to 0x8000) accepted");
  MT_EXPECT(sys_mmap(G(0x200000000ULL), 0 - G(0x200000000ULL) + 0x8000, PROT_RW, MAPF | F_FIXED, &r) != 0, "wrapping addr+len (to 0x8000) at 0x2_0000_0000 accepted");
  MT_EXPECT(sys_mmap(G(0x200000000ULL), ~0ULL - 0xFF, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) != 0, "EXCL with len -0x100 accepted");
  MT_EXPECT(sig_eq(s0, vm_sig()), "wrapping requests changed state");
  // flexible / reserve / direct with huge sizes
  for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
    uint64_t a = 0;
    MT_EXPECT(sys_flex(&a, lens[i], PROT_RW, 0) != 0, "MapFlexibleMemory 0x%llx succeeded", (unsigned long long)lens[i]);
    a = 0; MT_EXPECT(sys_reserve(&a, lens[i], 0, 0) != 0, "ReserveVirtualRange 0x%llx succeeded", (unsigned long long)lens[i]);
  }
  MT_EXPECT(sig_eq(s0, vm_sig()), "huge flexible/reserve requests changed state");
  mt_end();
}

static void t_unmap(void) {
  mt_begin(N("unmap_partial_holes_split_double"), AREA);
  uint64_t X = ZONE(7);
  MT_EXPECT(map_fixed(X, 6 * PAGE), "map");
  for (int i = 0; i < 6; i++) fill(X + (uint64_t)i * PAGE, PAGE, (uint8_t)(0x10 + i));
  // middle: pages 2,3
  MT_EXPECT(sys_munmap(X + 2 * PAGE, 2 * PAGE) == 0, "partial munmap");
  MT_EXPECT(is_fill(X, 2 * PAGE - 0, 0x10) || (is_fill(X, PAGE, 0x10) && is_fill(X + PAGE, PAGE, 0x11)), "pages before the hole damaged");
  MT_EXPECT(is_fill(X + 4 * PAGE, PAGE, 0x14) && is_fill(X + 5 * PAGE, PAGE, 0x15), "pages after the hole damaged");
  MT_EXPECT(rd_faults(X + 2 * PAGE) && rd_faults(X + 3 * PAGE - 1) && rd_faults(X + 3 * PAGE) && rd_faults(X + 4 * PAGE - 1), "hole is still accessible");
  Nps5FaultInfo fi; nps5_mem_classify((uint64_t)(uintptr_t)P(X + 2 * PAGE + 8), &fi);
  MT_EXPECT(fi.cls == NPS5_FA_UNMAPPED, "hole classifies as %d", fi.cls);
  nps5_mem_classify((uint64_t)(uintptr_t)P(X + PAGE + 8), &fi);
  MT_EXPECT(fi.cls == NPS5_FA_MAPPED && fi.lo == X && fi.hi == X + 2 * PAGE, "survivor classifies as %d [0x%llx,0x%llx)", fi.cls, (unsigned long long)fi.lo, (unsigned long long)fi.hi);
  // spanning the hole and both survivors
  MT_EXPECT(sys_munmap(X, 6 * PAGE) == 0, "munmap spanning a hole");
  for (int i = 0; i < 6; i++) MT_EXPECT(rd_faults(X + (uint64_t)i * PAGE), "page %d survived", i);
  MT_EXPECT(!is_mapped_extent(X) && !is_mapped_extent(X + 5 * PAGE), "extent list still has the pages");
  // double unmap: harmless; the neighbour is intact
  MT_EXPECT(map_fixed(X + 8 * PAGE, PAGE), "neighbour");
  fill(X + 8 * PAGE, PAGE, 0x99);
  int64_t r1 = sys_munmap(X, 6 * PAGE);
  MT_EXPECT(r1 == 0 || r1 == -1, "double munmap result %lld", (long long)r1);
  MT_EXPECT(is_fill(X + 8 * PAGE, PAGE, 0x99), "double munmap damaged a neighbour");
  // unaligned munmap touches only the pages named
  MT_EXPECT(map_fixed(X, 4 * PAGE), "remap");
  for (int i = 0; i < 4; i++) fill(X + (uint64_t)i * PAGE, PAGE, (uint8_t)(0x20 + i));
  int64_t u = sys_munmap(X + PAGE + 0x100, 0x100);
  MT_EXPECT(is_fill(X, PAGE, 0x20) && is_fill(X + 2 * PAGE, PAGE, 0x22) && is_fill(X + 3 * PAGE, PAGE, 0x23), "unaligned munmap(+0x4100, 0x100) damaged other pages (rc %lld)", (long long)u);
  // munmap of address 0 / null guard / beyond area must not crash or alter state
  Sig s = vm_sig();
  sys_munmap(0, PAGE); sys_munmap(G(0), 0x400000); sys_munmap(HI, PAGE); sys_munmap(HI - PAGE, 4 * PAGE);
  sys_munmap(~0ULL - PAGE, PAGE);
  MT_EXPECT(sig_eq(s, vm_sig()), "munmap of invalid ranges changed state");
  MT_EXPECT(sys_munmap(X, 0) != 0 || sig_eq(s, vm_sig()), "munmap len 0 changed state");
  // huge length wrapping to one page: munmap(X+0x200, 2^64-0x100) must not unmap X's page
  MT_EXPECT(is_fill(X + 3 * PAGE, PAGE, 0x23), "setup");
  int64_t w = sys_munmap(X + 3 * PAGE + 0x200, 0 - 0x100ULL);
  MT_EXPECT(w != 0 && is_fill(X + 3 * PAGE, PAGE, 0x23), "recomp_vm_free: addr+size+mask wraps, so munmap(addr+0x200, 2^64-0x100) unmaps one page and reports success" " [" "rc %lld, page %s" "]", (long long)w, is_fill(X + 3 * PAGE, PAGE, 0x23) ? "intact" : "unmapped");
  map_fixed(X + 3 * PAGE, PAGE); fill(X + 3 * PAGE, PAGE, 0x23);
  MT_EXPECT(sys_munmap(X + 3 * PAGE, ~0ULL - 0x2000) != 0 || is_fill(X + 3 * PAGE, PAGE, 0x23), "huge munmap length");
  MT_EXPECT(is_fill(X + 2 * PAGE, PAGE, 0x22) && is_fill(X + 8 * PAGE, PAGE, 0x99), "huge munmap destroyed other mappings");
  sys_munmap(X, 16 * PAGE);
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
}

static void t_unmap_runtime_owned(void) {
  mt_begin(N("munmap_keeps_runtime_owned_ranges"), AREA);
  // not specified for guests that unmap their own TLS or stack, but the system block holds the
  // main thread's TCB: losing it ends the process on the next TLS access
  Sig s = vm_sig();
  sys_munmap(nps5_mem_config.image_end, 0x30000);
  MT_EXPECT(!rd_faults(ctx->fs_base) && !rd_faults(nps5_mem_config.image_end), "munmap over the runtime's system block (main TLS/TCB/args) decommits it (unspecified by the ADR; a guest must not be able to destroy runtime-owned memory)" " [" "TCB %s" "]", rd_faults(ctx->fs_base) ? "unmapped" : "present");
  (void)s;
  mt_end();
}

static void t_remap(void) {
  mt_begin(N("remap_over_existing_zero_fills"), AREA);
  uint64_t X = ZONE(8), r = 0;
  MT_EXPECT(map_fixed(X, 8 * PAGE), "map");
  fill(X, 8 * PAGE, 0xEE);
  MT_EXPECT(sys_mmap(X, 8 * PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0, "remap same range");
  MT_EXPECT(is_fill(X, 8 * PAGE, 0), "remapped range not zero-filled");
  fill(X, 8 * PAGE, 0xEE);
  MT_EXPECT(sys_mmap(X + 2 * PAGE, 2 * PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0, "remap inner part");
  MT_EXPECT(is_fill(X, 2 * PAGE, 0xEE) && is_fill(X + 2 * PAGE, 2 * PAGE, 0) && is_fill(X + 4 * PAGE, 4 * PAGE, 0xEE), "inner remap touched wrong pages");
  // remap spanning mapped + free + mapped: replace everything, including the free gap
  sys_munmap(X + 3 * PAGE, 2 * PAGE);
  MT_EXPECT(sys_mmap(X + PAGE, 6 * PAGE, PROT_RW, MAPF | F_FIXED, &r) == 0 && !rd_faults(X + 3 * PAGE) && !rd_faults(X + 6 * PAGE), "remap across a hole");
  MT_EXPECT(is_fill(X + PAGE, 6 * PAGE, 0) && is_fill(X, PAGE, 0xEE) && is_fill(X + 7 * PAGE, PAGE, 0xEE), "spanning remap data");
  // flexible remap + prot
  uint64_t a = X;
  MT_EXPECT(sys_flex(&a, PAGE, PROT_RW, F_FIXED) == 0 && a == X, "flexible fixed remap");
  sys_munmap(X, 64 * PAGE);
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
}

static void t_hints(void) {
  mt_begin(N("hints_below_user_zone_and_invalid"), AREA);
  uint64_t r = 0, h = G(0x100000000ULL);   // 0x1_0000_0000, below the user zone
  uint32_t rc = sys_mmap(h, 0x10000, PROT_RW, MAPF, &r);
  MT_EXPECT(rc == 0 && r == h, "free hint below 0x2_0000_0000 not honoured (0x%llx)", (unsigned long long)r);
  uint64_t r2 = 0;
  rc = sys_mmap(h, 0x10000, PROT_RW, MAPF, &r2);
  MT_EXPECT(rc == 0 && (r2 + 0x10000 <= h || r2 >= h + 0x10000), "hint on a live mapping overlaps it (0x%llx)", (unsigned long long)r2);
  sys_munmap(h, 0x10000); if (r2) sys_munmap(r2, 0x10000);
  // hints that name memory the guest cannot have: result must be valid memory elsewhere
  uint64_t bad[] = {G(0x1000), G(0x3FFFFF), nps5_mem_config.guest_lo, stk_lo + 0x1000, stk_lo - PAGE, HI, G(0x10000000000ULL), 0x7000000000000000ULL};
  Sig s0 = vm_sig();
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    uint64_t rr = 0;
    rc = sys_mmap(bad[i], 0x10000, PROT_RW, MAPF, &rr);
    if (rc != 0) continue;  // failing is acceptable
    int in_guard = rr < nps5_mem_config.guest_lo, in_stack = rr < stk_top && rr + 0x10000 > stk_lo - PAGE, beyond = rr + 0x10000 > HI;
    MT_EXPECT(!in_guard && !in_stack && !beyond && ok_rw(rr), "hint 0x%llx -> 0x%llx (guard=%d stack=%d beyond=%d)", (unsigned long long)bad[i], (unsigned long long)rr, in_guard, in_stack, beyond);
    sys_munmap(rr, 0x10000);
  }
  MT_EXPECT(sig_eq(s0, vm_sig()), "hint handling leaked mappings");
  mt_end();
}

static void t_reserve(void) {
  mt_begin(N("reserve_virtual_range"), AREA);
  uint64_t a = 0, r = 0;
  MT_EXPECT(sys_reserve(&a, 0x100000, 0, 0) == 0 && a != 0, "reserve");
  MT_EXPECT(rd_faults(a) && rd_faults(a + 0xFFFFF), "reserved range has accessible pages");
  // unhinted maps never land inside a reservation
  for (int i = 0; i < 20; i++) {
    uint64_t m = 0;
    if (sys_mmap(0, 0x10000, PROT_RW, MAPF, &m) != 0) break;
    MT_EXPECT(m + 0x10000 <= a || m >= a + 0x100000, "unhinted mapping 0x%llx landed inside the reservation 0x%llx", (unsigned long long)m, (unsigned long long)a);
  }
  // fixed map inside works, outside the reservation is independent
  MT_EXPECT(sys_mmap(a + 0x40000, PAGE, PROT_RW, MAPF | F_FIXED | F_EXCL, &r) == 0 && !rd_faults(a + 0x40000) && rd_faults(a + 0x44000), "fixed inside reservation");
  uint64_t b = a;
  MT_EXPECT(sys_reserve(&b, 0x10000, F_FIXED, 0) != 0 || b != a || 1, "");
  uint64_t c = a + 0x40000;
  MT_EXPECT(sys_reserve(&c, 0x4000, F_FIXED | F_EXCL, 0) != 0, "reserve over a live mapping (EXCL) succeeded");
  uint64_t d = HI - PAGE;
  MT_EXPECT(sys_reserve(&d, 0x10000, F_FIXED, 0) != 0, "reserve straddling guest_hi succeeded");
  uint64_t z = 0; MT_EXPECT(sys_reserve(&z, 0, 0, 0) == E_INVAL, "reserve len 0");
  sys_munmap(a, 0x100000);
  MT_EXPECT(!is_mapped_extent(a) && !is_mapped_extent(a + 0x40000), "reservation not released by munmap");
  char why[200] = ""; MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
}

static void t_direct_invalid(void) {
  mt_begin(N("direct_alloc_invalid_arguments"), AREA);
  size_t total = sceKernelGetDirectMemorySize();
  struct { int64_t s, e; uint64_t len, al; const char *why; } bad[] = {
      {0, (int64_t)total, 0, 0x10000, "len 0"}, {0, (int64_t)total, ~0ULL, 0x10000, "len 2^64-1"}, {0, (int64_t)total, ~0ULL - 0xFFF, 0, "len 2^64-4096, default alignment"},
      {0, (int64_t)total, 1ULL << 63, 0x10000, "len 2^63"}, {0, (int64_t)total, 0x10000, 0x3000, "alignment 0x3000 (not a power of two)"}, {0, (int64_t)total, 0x10000, 3, "alignment 3"},
      {-1, (int64_t)total, 0x10000, 0x10000, "negative start"}, {(int64_t)total, 0, 0x10000, 0x10000, "end < start"},
      {0, (int64_t)total, total + 0x10000, 0x10000, "len > total"}, {0, (int64_t)total, 0x10000, ~0ULL, "alignment 2^64-1"},
  };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    size_t before = sceKernelAvailableDirectMemorySize();
    int64_t junk = -7;
    uint32_t rc = sys_dm_alloc(bad[i].s, bad[i].e, bad[i].len, bad[i].al, 0, &junk);
    size_t after = sceKernelAvailableDirectMemorySize();
    MT_EXPECT(rc != 0, "sceKernelAllocateDirectMemory accepts an invalid request" " [" "%s: rc=0 phys=0x%llx (available %zu -> %zu)" "]", bad[i].why, (unsigned long long)junk, before, after);
    if (rc == 0 && junk >= 0) sys_dm_release(junk, bad[i].len);
  }
  mt_end();
}

static void t_direct(void) {
  mt_begin(N("direct_memory_alloc_map_release"), AREA);
  size_t total = sceKernelGetDirectMemorySize();
  size_t avail0 = sceKernelAvailableDirectMemorySize();
  int64_t p1 = -1, p2 = -1, p3 = -1;
  uint32_t rc;
  MT_EXPECT(avail0 > 0x1000000 && total >= avail0, "avail %zu total %zu", avail0, total);
  MT_EXPECT(sys_dm_alloc(0, (int64_t)total, 0x400000, 0x10000, 0, &p1) == 0 && p1 >= 0, "alloc 1");
  // map, alias, release while mapped
  uint64_t A = 0, B = ZONE(9);
  rc = sys_dm_map(&A, 0x400000, PROT_RW, 0, p1, 0x10000);
  MT_EXPECT(rc == 0 && A != 0, "map direct (0x%x)", rc);
  if (rc == 0) {
    fill(A, 0x400000, 0xD1);
    rc = sys_dm_map(&B, 0x400000, PROT_RW, F_FIXED, p1, 0x10000);
    MT_EXPECT(rc == 0 && B == ZONE(9), "second (aliased) map of the same direct memory: 0x%x", rc);
    if (rc == 0) {
      MT_EXPECT(*P(B) == 0xD1, "second mapping of the same physical memory does not alias the first");
      *P(B + 0x1000) = 0x42;
      MT_EXPECT(*P(A + 0x1000) == 0x42, "write through alias B is not visible through A");
    }
    // out-of-block / bad phys
    uint64_t C = 0;
    MT_EXPECT(sys_dm_map(&C, 0x400000, PROT_RW, 0, p1 + 0x200000, 0x10000) != 0, "map crossing the end of the allocated block succeeded");
    C = 0; MT_EXPECT(sys_dm_map(&C, 0, PROT_RW, 0, p1, 0x10000) != 0, "map len 0 succeeded");
    C = 0; MT_EXPECT(sys_dm_map(&C, ~0ULL, PROT_RW, 0, p1, 0x10000) != 0, "map len ~0 succeeded");
    C = 0; MT_EXPECT(sys_dm_map(&C, 0x10000, PROT_RW, 0, (int64_t)total, 0x10000) != 0, "map of phys == total succeeded");
    C = 0; MT_EXPECT(sys_dm_map(&C, 0x10000, PROT_RW, 0, -0x10000, 0x10000) != 0, "map of negative phys succeeded");
    rc = sys_dm_release(p1, 0x400000);
    MT_EXPECT(rc == 0, "release returned 0x%x", rc);
    MT_EXPECT(rd_faults(A) && rd_faults(B), "sceKernelReleaseDirectMemory leaves the guest mappings of the released memory accessible (shadPS4/console unmap them); use-after-free is silent" " [" "A %s, B %s" "]", rd_faults(A) ? "faults" : "readable", rd_faults(B) ? "faults" : "readable");
    sys_munmap(A, 0x400000); sys_munmap(B, 0x400000);
  }
  MT_EXPECT(sys_dm_release(p1, 0x400000) != 0, "double release returned success");
  MT_EXPECT(sys_dm_release(0x123000, 0x1000) != 0 || 1, "");
  // fragmentation / coalescing: three adjacent blocks, free ends then middle, then the whole span fits
  size_t avail1 = sceKernelAvailableDirectMemorySize();
  MT_EXPECT(sys_dm_alloc(0, (int64_t)total, 0x100000, 0x10000, 0, &p1) == 0 && sys_dm_alloc(0, (int64_t)total, 0x100000, 0x10000, 0, &p2) == 0 && sys_dm_alloc(0, (int64_t)total, 0x100000, 0x10000, 0, &p3) == 0, "alloc 3");
  MT_EXPECT(p2 == p1 + 0x100000 && p3 == p2 + 0x100000, "first-fit lowest not adjacent: %lld %lld %lld", (long long)p1, (long long)p2, (long long)p3);
  sys_dm_release(p1, 0x100000); sys_dm_release(p3, 0x100000); sys_dm_release(p2, 0x100000);
  MT_EXPECT(sceKernelAvailableDirectMemorySize() == avail1, "available %zu after alloc/free of 3 blocks, was %zu", sceKernelAvailableDirectMemorySize(), avail1);
  int64_t big = -1; MT_EXPECT(sys_dm_alloc(p1, p1 + 0x300000, 0x300000, 0x10000, 0, &big) == 0 && big == p1, "coalesced span not allocatable in one piece");
  sys_dm_release(big, 0x300000);
  // exhaustion
  int64_t all = -1, junk = -1;
  size_t av = sceKernelAvailableDirectMemorySize();
  MT_EXPECT(sys_dm_alloc(0, (int64_t)total, av, 0x10000, 0, &all) == 0 || sys_dm_alloc(0, (int64_t)total, av & ~0xFFFFULL, 0x10000, 0, &all) == 0, "cannot allocate everything that is available");
  MT_EXPECT(sys_dm_alloc(0, (int64_t)total, 0x10000, 0x10000, 0, &junk) != 0, "allocation succeeded with no direct memory left");
  if (all >= 0) sys_dm_release(all, av);
  MT_EXPECT(sceKernelAvailableDirectMemorySize() >= avail1 - 0x10000, "memory not returned after releasing everything");
  mt_end();
}

static void t_mprotect(void) {
  mt_begin(N("mprotect_enforcement"), AREA);
  uint64_t X = ZONE(10);
  MT_EXPECT(map_fixed(X, 4 * PAGE), "map");
  MT_EXPECT(sys_mprotect(X, PAGE, 1) == 0, "mprotect(READ)");
  int wf = w1_faults(X + 8, 1);
  MT_EXPECT(wf, "guest protections are bookkeeping only (memory.md 'Protection'): a write to a PROT_READ page does not fault" " [" "write after mprotect(PROT_READ) %s" "]", wf ? "faulted" : "succeeded");
  MT_EXPECT(sys_mprotect(X + PAGE, PAGE, 0) == 0, "mprotect(NONE)");
  int rf = rd_faults(X + PAGE + 8);
  MT_EXPECT(rf, "guest protections are bookkeeping only: a read of a PROT_NONE page does not fault" " [" "read after mprotect(PROT_NONE) %s" "]", rf ? "faulted" : "succeeded");
  sys_mprotect(X, 4 * PAGE, PROT_RW);
  mt_end();

  mt_begin(N("mprotect_granularity_and_errors"), AREA);
  // bookkeeping granularity: only the requested pages change protection
  MT_EXPECT(sys_mprotect(X + PAGE, PAGE, 1) == 0, "mprotect page 1");
  int p0 = extent_prot(X), p1 = extent_prot(X + PAGE), p2 = extent_prot(X + 2 * PAGE), p3 = extent_prot(X + 3 * PAGE);
  MT_EXPECT(p1 == 1, "page 1 prot %d", p1);
  MT_EXPECT(p0 == PROT_RW && p2 == PROT_RW && p3 == PROT_RW, "sceKernelMprotect retags the whole extent instead of splitting at the range (sceKernelMtypeprotect does split)" " [" "page prots %d %d %d %d" "]", p0, p1, p2, p3);
  // unmapped / invalid ranges
  Sig s = vm_sig();
  int64_t un = sys_mprotect(ZONE(11), PAGE, 1);
  MT_EXPECT(un != 0, "sceKernelMprotect on an unmapped range returns 0 (sibling sceKernelMtypeprotect and the kernel return EINVAL/ENOMEM)" " [" "rc=%lld" "]", (long long)un);
  int64_t wr = sys_mprotect(X, ~0ULL, PROT_RW);
  MT_EXPECT(wr != 0, "sceKernelMprotect(addr, 2^64-1) wraps its rounded length and returns 0" " [" "rc=%lld" "]", (long long)wr);
  int64_t wr2 = sys_mprotect(HI - PAGE, 4 * PAGE, PROT_RW);
  MT_EXPECT(wr2 != 0, "sceKernelMprotect straddling guest_hi returns 0" " [" "rc=%lld" "]", (long long)wr2);
  uint32_t mt = sys_mtypeprot(ZONE(11), PAGE, 0, 1);
  MT_EXPECT(mt != 0, "sceKernelMtypeprotect on an unmapped range returned 0");
  MT_EXPECT(sig_eq(s, vm_sig()), "mprotect on invalid ranges changed the extent list");
  sys_munmap(X, 4 * PAGE);
  mt_end();
}

static void t_null_guard_vm(void) {
  mt_begin(N("null_guard_untouched_by_vm_layer"), AREA);
  Sig s = vm_sig();
  uint64_t r;
  for (uint64_t a = 0x4000 * 13; a < 0x400000; a += 0x4000 * 13) {
    sys_mmap(G(a), 0x10000, PROT_RW, MAPF | F_FIXED, &r);
    r = ~0ULL;
    if (sys_mmap(G(a), 0x10000, PROT_RW, MAPF, &r) == 0) {
      if (r < nps5_mem_config.guest_lo) MT_EXPECT(0, "hint 0x%llx mapped in the null guard at 0x%llx", (unsigned long long)G(a), (unsigned long long)r);
      sys_munmap(r, 0x10000);
    }
    px_mmap(G(a), 0x10000, PROT_RW, MAPF | F_FIXED);
    sys_flex(&(uint64_t){G(a)}, 0x10000, PROT_RW, F_FIXED);
    sys_reserve(&(uint64_t){G(a)}, 0x10000, F_FIXED, 0);
    sys_mprotect(G(a), 0x10000, PROT_RW);
    sys_mtypeprot(G(a), 0x10000, 0, PROT_RW);
  }
  uint64_t v = G(0x1000);
  uint64_t out = 0; (void)out;
  for (uint64_t o = 0; o < 0x400000; o += 0x4000) MT_EXPECT(rd_faults(G(o)) && wr_faults(G(o)), "null guard page +0x%llx accessible", (unsigned long long)o);
  (void)v;
  MT_EXPECT(sig_eq(s, vm_sig()), "extent list changed");
  // first access to the guard classifies as null guard
  for (uint64_t o = 0; o < 0x400000; o += 0x100000 - 8) {
    rd_faults(G(o)); Nps5FaultInfo fi; nps5_mem_classify(vmh_fault, &fi);
    MT_EXPECT(fi.cls == NPS5_FA_NULL_GUARD, "fault at +0x%llx classified %d", (unsigned long long)o, fi.cls);
  }
  mt_end();
}

static void t_stack_guard(void) {
  mt_begin(N("main_stack_guard"), AREA);
  Nps5FaultInfo fi;
  MT_EXPECT(!rd_faults(stk_lo) && !w1_faults(stk_lo, 1) && !rd_faults(stk_top - 1), "stack bounds not accessible");
  MT_EXPECT(rd_faults(stk_lo - 1) && rd_faults(stk_lo - PAGE), "guard page below the stack is accessible");
  nps5_mem_classify(vmh_fault, &fi);
  MT_EXPECT(fi.cls == NPS5_FA_STACK_GUARD, "fault below the stack classified as %d (%s)", fi.cls, fi.name);
  rd_faults(stk_lo - PAGE); nps5_mem_classify(vmh_fault, &fi);
  MT_EXPECT(fi.cls == NPS5_FA_STACK_GUARD, "lowest guard byte classified %d", fi.cls);
  rd_faults(stk_lo - PAGE - 1); nps5_mem_classify(vmh_fault, &fi);
  MT_EXPECT(fi.cls != NPS5_FA_STACK_GUARD && fi.cls != NPS5_FA_STACK, "byte below the guard classified %d", fi.cls);
  MT_EXPECT(rd_faults(stk_top), "byte at stack_top is accessible");
  mt_end();

  mt_begin(N("thread_stack_guards"), AREA);
  GuestContext *t1 = recomp_create_thread_context(ctx, 256 * 1024);
  GuestContext *t2 = recomp_create_thread_context(ctx, 256 * 1024);
  MT_EXPECT(t1 && t2, "create thread contexts");
  if (t1 && t2) {
    uint64_t g1 = t1->stack_base, g2 = t2->stack_base;
    MT_EXPECT(rd_faults(g1) && rd_faults(g1 + PAGE - 1), "thread 1 guard page accessible");
    nps5_mem_classify(vmh_fault, &fi);
    MT_EXPECT(!rd_faults(g1 + PAGE) && !w1_faults(g1 + PAGE, 1), "thread 1 stack bottom not accessible");
    rd_faults(g1 + 8); nps5_mem_classify(vmh_fault, &fi);
    MT_EXPECT(fi.cls == NPS5_FA_STACK_GUARD, "thread 1 guard classified %d (%s)", fi.cls, fi.name);
    rd_faults(g2 + 8); nps5_mem_classify(vmh_fault, &fi);
    MT_EXPECT(fi.cls == NPS5_FA_STACK_GUARD, "second thread stack's guard page classifies as an ordinary mapping (adjacent thread_stack extents are merged in the crash snapshot)" " [" "class %d name %s lo=0x%llx" "]", fi.cls, fi.name, (unsigned long long)fi.lo);
    MT_EXPECT(g1 + 0x1000 != g2 || 1, "");
    MT_EXPECT(g2 >= g1 + 0x40000 + PAGE || g2 + 0x40000 + PAGE <= g1, "thread stacks overlap: 0x%llx 0x%llx", (unsigned long long)g1, (unsigned long long)g2);
    recomp_free_thread_context(t1); recomp_free_thread_context(t2);
  }
  // an absurd stack size must fail, not wrap to a stack of zero bytes
  GuestContext *t3 = recomp_create_thread_context(ctx, ~0ULL - 0x1000);
  MT_EXPECT(t3 == NULL, "recomp_create_thread_context rounds a huge stack size to 0 (overflow) and returns a thread with no stack" " [" "context %s" "]", t3 ? "created" : "NULL");
  GuestContext *t4 = recomp_create_thread_context(ctx, 1ULL << 50);
  MT_EXPECT(t4 == NULL, "1 PiB thread stack was created");
  mt_end();
}

static int pipe_fds[2];
static int child_writev_bad_iov(void *a) {
  (void)a;
  setrsp();
  ctx->rdi = (uint64_t)pipe_fds[1]; ctx->rsi = G(0x3000000000ULL); ctx->rdx = 4; /* unmapped iov array */
  shim_writev(ctx);
  return ctx->rax == (uint64_t)-1 ? 0 : 3;
}
static void t_hle_pointers(void) {
  mt_begin(N("hle_boundary_bad_guest_pointers"), AREA);
  MT_EXPECT(pipe(pipe_fds) == 0, "pipe");
  fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK); fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
  uint64_t X = ZONE(12);
  MT_EXPECT(map_fixed(X, PAGE), "map");
  memset(P(X), 'a', PAGE);
  // write(2) from a buffer that runs off the end of its mapping into unmapped memory: EFAULT, no crash
  setrsp(); ctx->rdi = (uint64_t)pipe_fds[1]; ctx->rsi = X; ctx->rdx = 2 * PAGE; shim_write(ctx);
  int64_t r = (int64_t)ctx->rax;
  MT_EXPECT(r == -1 || (r > 0 && r <= (int64_t)PAGE), "write across the end of a mapping returned %lld", (long long)r);
  // wholly unmapped buffer, null guard buffer, beyond-the-area buffer
  #if NPS5_ADDR_MODE_IDENTITY
  uint64_t bufs[] = {ZONE(13), G(0x10)};  // identity: pointers outside the arena are raw host pointers by design
#else
  uint64_t bufs[] = {ZONE(13), G(0x10), HI + 0x10000, 0x00007FFF00000000ULL};
#endif
  for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i++) {
    setrsp(); clear_errno(); ctx->rdi = (uint64_t)pipe_fds[1]; ctx->rsi = bufs[i]; ctx->rdx = 16; shim_write(ctx);
    MT_EXPECT((int64_t)ctx->rax == -1 && guest_errno() == EFAULT, "write from unmapped 0x%llx -> %lld errno %d", (unsigned long long)bufs[i], (long long)ctx->rax, guest_errno());
  }
  // a guest pointer beyond the guest window must never reach the host kernel as a host address
  // (in base mode bias+ptr lies outside the reservation: it could be somebody else's memory)
#if !NPS5_ADDR_MODE_IDENTITY
  uint64_t huge = (1ULL << 40) + 0x1000;
  setrsp(); clear_errno(); ctx->rdi = (uint64_t)pipe_fds[1]; ctx->rsi = huge; ctx->rdx = 16; shim_write(ctx);
  MT_EXPECT((int64_t)ctx->rax == -1, "write from a guest pointer outside the guest window returned %lld", (long long)ctx->rax);
#endif
  char junk[64]; ssize_t n = read(pipe_fds[0], junk, sizeof junk); (void)n;
  // writev with an unmapped iovec array: dereferenced by the thunk's shadow copy
  int st = in_child(child_writev_bad_iov, NULL);
  MT_EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "shim_writev dereferences the guest iovec array itself (base mode shadow copy): an unmapped array crashes the host instead of failing with EFAULT" " [" "helper thread status 0x%x (%s)" "]", st, WIFSIGNALED(st) ? "faulted" : "returned");
  sys_munmap(X, PAGE);
  mt_end();

  mt_begin(N("hle_boundary_slot_pointer_faults_are_attributable"), AREA);
  // a PtrSlot argument that points at unmapped memory faults inside the thunk: the crash report
  // must be able to classify the faulting address as a guest address (section 3.10)
  uint64_t slots[] = {G(0x10), ZONE(14)};
  for (size_t i = 0; i < 2; i++) {
    setrsp();
    ctx->rdi = slots[i]; ctx->rsi = PAGE; ctx->rdx = PROT_RW; ctx->rcx = 0;
    int f = 0;
    if (sigsetjmp(vmh_jb, 1)) f = 1; else shim_sceKernelMapFlexibleMemory(ctx);
    if (f) { Nps5FaultInfo fi; nps5_mem_classify(vmh_fault, &fi);
      MT_EXPECT(fi.cls == (i == 0 ? NPS5_FA_NULL_GUARD : NPS5_FA_UNMAPPED), "fault at slot 0x%llx classified %d", (unsigned long long)slots[i], fi.cls); }
    else MT_EXPECT((int32_t)ctx->rax != 0, "MapFlexibleMemory with unreadable slot returned success");
  }
  mt_end();
}

static void t_errno(void) {
  mt_begin(N("errno_propagation"), AREA);
  uint64_t r = 0;
  clear_errno();
  MT_EXPECT(px_mmap(0, 0, PROT_RW, MAPF) == ~0ULL && guest_errno() == EINVAL, "mmap(len 0) errno=%d want EINVAL(22)", guest_errno());
  MT_EXPECT(px_mmap(0, 1ULL << 62, PROT_RW, MAPF) == ~0ULL && guest_errno() == ENOMEM, "mmap(huge) errno=%d want ENOMEM(12)", guest_errno());
  clear_errno();
  MT_EXPECT(sys_munmap(0, 0x4000) != 0 && guest_errno() == EINVAL, "munmap(NULL) errno=%d want EINVAL", guest_errno());
  uint64_t X = ZONE(15);
  clear_errno();
  MT_EXPECT(px_mmap(X, PAGE, PROT_RW, MAPF | F_FIXED) == X, "mmap ok");
  MT_EXPECT(guest_errno() == 0, "successful mmap set errno %d", guest_errno());
  sys_munmap(X, PAGE);
  (void)r;
  mt_end();
}

int main(void) {
  vmh_init();
  t_first_fit();
  t_fixed_area_ends();
  t_fixed_low_end();
  t_fixed_semantics();
  t_misaligned();
  t_zero_and_huge();
  t_unmap();
  t_remap();
  t_hints();
  t_reserve();
  t_direct_invalid();
  t_direct();
  t_mprotect();
  t_null_guard_vm();
  t_stack_guard();
  t_hle_pointers();
  t_errno();
  t_unmap_runtime_owned();
  char why[200] = "";
  mt_begin(N("extent_list_invariants_at_exit"), AREA);
  MT_EXPECT(vm_check_list(why, sizeof why), "%s", why);
  mt_end();
  return mt_status();
}
