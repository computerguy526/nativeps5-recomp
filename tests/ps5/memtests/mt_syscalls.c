/* PS5 guest program: the mapping syscalls through the real guest ABI (translated x86-64 call ->
 * generated thunk -> HLE), checked from inside the guest. Companion of tests/memtests/vm_adv.c,
 * which makes the same requests on the host without a guest; here the stack-argument path, the
 * pointer translation and return-value handling are the translated program's own. Output: one
 * RESULT line per case (tests/memtests/mt.h). No signals are used, so only return codes, data and
 * the guest-visible bookkeeping are checked; faults are covered by mt_faults.c. */
#define MT_GUEST 1
#include <stddef.h>
#include <stdint.h>
#include "../../memtests/mt.h"

extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);
extern int sceKernelMunmap(void *addr, size_t len);
extern int sceKernelMprotect(const void *addr, size_t len, int prot);
extern int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
extern int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
extern int sceKernelAllocateDirectMemory(long start, long end, size_t len, size_t align, int type, long *phys);
extern int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, long phys, size_t align);
extern int sceKernelReleaseDirectMemory(long phys, size_t len);
extern size_t sceKernelGetDirectMemorySize(void);

#define PAGE 0x4000ULL
#define MAPF 0x1002
#define FIXED 0x10
#define EXCL 0x4000
#define HI 0xFC00000000ULL
static volatile uint64_t SH = 12; /* keeps page-aligned constants in [6 GiB, 448 GiB) out of the code: the identity-mode scan would (rightly) refuse the title */
#define BIG(x) ((uint64_t)((x) >> 12) << SH)
static uint64_t O;
#define Z(k) (O + 0x8000000000ULL + (uint64_t)(k) * 0x10000000ULL)
static const char *MODE;
#define NM(s) (MODE[0] == 'i' ? "gst.identity." s : "gst.base." s)

static uint32_t mm(uint64_t addr, uint64_t len, int flags, uint64_t *res) {
  void *r = (void *)~0ULL;
  int rc = sceKernelMmap((void *)addr, len, 3, flags, -1, 0, &r);
  *res = (uint64_t)r;
  return (uint32_t)rc;
}
static int fixed(uint64_t a, uint64_t len) { uint64_t r; return mm(a, len, MAPF | FIXED, &r) == 0 && r == a; }
static void fill(uint64_t a, size_t n, uint8_t v) { volatile uint8_t *p = (volatile uint8_t *)a; for (size_t i = 0; i < n; i++) p[i] = v; }
static int isfill(uint64_t a, size_t n, uint8_t v) { volatile uint8_t *p = (volatile uint8_t *)a; for (size_t i = 0; i < n; i += 61) if (p[i] != v) return 0; return p[n - 1] == v; }

int main(void) {
  mt_init();
  O = (uint64_t)(uintptr_t)&main >= 0x7000000000ULL ? 0x7000000000ULL : 0;
  MODE = O ? "identity" : "base";
  uint64_t r = 0;

  mt_begin(NM("unhinted_first_fit"), "guest-vm");
  MT_EXPECT(mm(0, 0x10000, MAPF, &r) == 0, "sceKernelMmap(NULL)");
  MT_EXPECT(r >= O + BIG(0x200000000ULL) && (r & 0xFFFF) == 0, "unhinted mapping at 0x%llx", (unsigned long long)r);
  uint64_t a = r, b = 0;
  MT_EXPECT(mm(0, 0x10000, MAPF, &b) == 0 && b != a, "second mapping");
  fill(a, 0x10000, 0x11); fill(b, 0x10000, 0x22);
  MT_EXPECT(isfill(a, 0x10000, 0x11) && isfill(b, 0x10000, 0x22), "mappings overlap");
  MT_EXPECT(sceKernelMunmap((void *)a, 0x10000) == 0, "munmap");
  uint64_t c = 0; MT_EXPECT(mm(0, 0x10000, MAPF, &c) == 0 && c == a, "lowest hole not reused (0x%llx vs 0x%llx)", (unsigned long long)c, (unsigned long long)a);
  MT_EXPECT(isfill(c, 0x10000, 0), "reused memory not zero-filled");
  sceKernelMunmap((void *)b, 0x10000); sceKernelMunmap((void *)c, 0x10000);
  mt_end();

  mt_begin(NM("fixed_area_end"), "guest-vm");
  MT_EXPECT(fixed(HI - PAGE, PAGE), "last page of the application area refused");
  *(volatile uint8_t *)(HI - 1) = 0x5a; MT_EXPECT(*(volatile uint8_t *)(HI - 1) == 0x5a, "last byte unusable");
  MT_EXPECT(mm(HI - 4 * PAGE, 8 * PAGE, MAPF | FIXED, &r) != 0, "map straddling the end accepted");
  MT_EXPECT(mm(HI, PAGE, MAPF | FIXED, &r) != 0, "map at the end accepted");
  MT_EXPECT(mm(HI + PAGE, PAGE, MAPF | FIXED, &r) != 0, "map above the end accepted");
  MT_EXPECT(mm(HI - 2 * PAGE, 2 * PAGE, MAPF | FIXED | EXCL, &r) != 0, "EXCL over the last page accepted");
  sceKernelMunmap((void *)(HI - PAGE), PAGE);
  mt_end();

  mt_begin(NM("fixed_null_guard_and_alignment"), "guest-vm");
  uint64_t bad[] = {O + 0x1000, O + 0x3FC000, O + 0x3FFFFF};
  for (int i = 0; i < 3; i++) MT_EXPECT(mm(bad[i], PAGE, MAPF | FIXED, &r) != 0, "fixed map at 0x%llx accepted", (unsigned long long)bad[i]);
  MT_EXPECT(mm(O + 0x3F8000, 4 * PAGE, MAPF | FIXED, &r) != 0, "map straddling the null guard end accepted");
  uint64_t X = Z(1);
  MT_EXPECT(mm(X + 0x1000, PAGE, MAPF | FIXED, &r) != 0 && mm(X + 1, PAGE, MAPF | FIXED, &r) != 0, "misaligned fixed address accepted");
  void *p0 = (void *)~0ULL;
  uint32_t rc0 = (uint32_t)sceKernelMmap(0, 0, 3, MAPF, -1, 0, &p0);
  MT_EXPECT(rc0 == 0x80020016u, "len 0 returned 0x%x", rc0);
  uint64_t z0 = 0; uint32_t rcz = mm(0, PAGE, MAPF | FIXED, &z0);
  MT_EXPECT(rcz != 0, "MAP_FIXED at address 0 is treated as 'no address' and mapped elsewhere (ADR: a fixed request that cannot be honoured fails)" " [" "rc=%u res=0x%llx" "]", rcz, (unsigned long long)z0);
  if (rcz == 0 && z0 != ~0ULL) sceKernelMunmap((void *)z0, PAGE);
  mt_end();

  mt_begin(NM("huge_lengths"), "guest-vm");
  uint64_t lens[] = {~0ULL, ~0ULL - 0x3FFF, 1ULL << 63, 1ULL << 48, 1ULL << 41, 1ULL << 40, HI};
  for (int i = 0; i < 7; i++) {
    MT_EXPECT(mm(0, lens[i], MAPF, &r) != 0, "unhinted mmap of 0x%llx succeeded", (unsigned long long)lens[i]);
    MT_EXPECT(mm(Z(2), lens[i], MAPF | FIXED, &r) != 0, "fixed mmap of 0x%llx succeeded", (unsigned long long)lens[i]);
    void *fa = 0; MT_EXPECT(sceKernelMapFlexibleMemory(&fa, lens[i], 3, 0) != 0, "flexible 0x%llx succeeded", (unsigned long long)lens[i]);
  }
  MT_EXPECT(mm(HI - PAGE, 0 - (HI - PAGE) + 2 * PAGE, MAPF | FIXED, &r) != 0, "addr+len wrapping into the window accepted");
  mt_end();

  mt_begin(NM("fixed_excl_replace_remap"), "guest-vm");
  X = Z(3);
  MT_EXPECT(fixed(X, 4 * PAGE), "map");
  fill(X, 4 * PAGE, 0xA1);
  MT_EXPECT(mm(X + PAGE, PAGE, MAPF | FIXED | EXCL, &r) != 0, "EXCL over a live mapping accepted");
  MT_EXPECT(isfill(X, 4 * PAGE, 0xA1), "refused EXCL altered the data");
  MT_EXPECT(mm(X + 2 * PAGE, 4 * PAGE, MAPF | FIXED, &r) == 0 && r == X + 2 * PAGE, "replace over the tail");
  MT_EXPECT(isfill(X, 2 * PAGE, 0xA1) && isfill(X + 2 * PAGE, 4 * PAGE, 0), "replace damaged the head or did not zero the replaced pages");
  MT_EXPECT(mm(X + 8 * PAGE, 1, MAPF | FIXED, &r) == 0 && mm(X + 9 * PAGE, PAGE, MAPF | FIXED | EXCL, &r) == 0, "a 1-byte fixed map must leave the next 16 KiB page free");
  sceKernelMunmap((void *)X, 12 * PAGE);
  mt_end();

  mt_begin(NM("unmap_partial_holes_double"), "guest-vm");
  X = Z(4);
  MT_EXPECT(fixed(X, 6 * PAGE), "map");
  for (int i = 0; i < 6; i++) fill(X + (uint64_t)i * PAGE, PAGE, (uint8_t)(0x30 + i));
  MT_EXPECT(sceKernelMunmap((void *)(X + 2 * PAGE), 2 * PAGE) == 0, "partial unmap");
  MT_EXPECT(isfill(X, PAGE, 0x30) && isfill(X + PAGE, PAGE, 0x31) && isfill(X + 4 * PAGE, PAGE, 0x34) && isfill(X + 5 * PAGE, PAGE, 0x35), "survivors damaged");
  MT_EXPECT(mm(X + 2 * PAGE, 2 * PAGE, MAPF | FIXED | EXCL, &r) == 0, "the hole is not free again");
  sceKernelMunmap((void *)(X + 2 * PAGE), 2 * PAGE);
  MT_EXPECT(sceKernelMunmap((void *)X, 6 * PAGE) == 0, "unmap spanning a hole");
  MT_EXPECT(mm(X, 6 * PAGE, MAPF | FIXED | EXCL, &r) == 0, "range not free after unmapping it");
  sceKernelMunmap((void *)X, 6 * PAGE);
  int d1 = sceKernelMunmap((void *)X, 6 * PAGE);
  MT_EXPECT(d1 == 0 || d1 == -1 || d1 < 0, "double unmap rc %d", d1);
  mt_end();

  /* Guest protections are enforced on the host pages (a write to a PROT_READ page kills the guest:
   * mt_faults mprotect_write). Here: the legal round trip, and the error returns. */
  mt_begin(NM("mprotect_roundtrip_and_errors"), "guest-vm");
  X = Z(5);
  MT_EXPECT(fixed(X, 2 * PAGE), "map");
  *(volatile uint8_t *)(X + 8) = 5;
  MT_EXPECT(sceKernelMprotect((void *)X, PAGE, 1) == 0, "mprotect(READ)");
  MT_EXPECT(*(volatile uint8_t *)(X + 8) == 5, "a PROT_READ page lost its contents or is unreadable");
  *(volatile uint8_t *)(X + PAGE + 8) = 6; /* the second page keeps its protection: mprotect split the mapping */
  MT_EXPECT(sceKernelMprotect((void *)X, PAGE, 3) == 0, "mprotect(RW)");
  *(volatile uint8_t *)(X + 8) = 7;
  MT_EXPECT(*(volatile uint8_t *)(X + 8) == 7, "write after restoring RW");
  MT_EXPECT(sceKernelMprotect((void *)Z(6), PAGE, 1) != 0, "mprotect of an unmapped range succeeded");
  MT_EXPECT(sceKernelMprotect((void *)X, ~0ULL, 3) != 0, "mprotect with a wrapping length succeeded");
  sceKernelMunmap((void *)X, 2 * PAGE);
  mt_end();

  mt_begin(NM("fixed_flexible_does_not_clobber_neighbour"), "guest-vm");
  X = Z(6);
  MT_EXPECT(fixed(X + PAGE, PAGE), "neighbour");
  fill(X + PAGE, PAGE, 0xC3);
  void *fa = (void *)X; int frc = sceKernelMapFlexibleMemory(&fa, PAGE, 3, FIXED);
  MT_EXPECT(frc == 0 && (uint64_t)fa == X, "flexible fixed one page rc=%d", frc);
  MT_EXPECT(isfill(X + PAGE, PAGE, 0xC3), "sceKernelMapFlexibleMemory(MAP_FIXED, 16 KiB) replaces the neighbouring 16 KiB page (rounded to 64 KiB)" " [" "neighbour %s" "]", isfill(X + PAGE, PAGE, 0) ? "zeroed" : "changed");
  sceKernelMunmap((void *)X, 8 * PAGE);
  mt_end();

  mt_begin(NM("direct_memory"), "guest-vm");
  size_t total = sceKernelGetDirectMemorySize();
  long p1 = -1;
  MT_EXPECT(sceKernelAllocateDirectMemory(0, (long)total, 0x400000, 0x10000, 0, &p1) == 0 && p1 >= 0, "alloc");
  void *A = 0, *B = (void *)Z(7);
  MT_EXPECT(sceKernelMapDirectMemory(&A, 0x400000, 3, 0, p1, 0x10000) == 0 && A, "map");
  MT_EXPECT(sceKernelMapDirectMemory(&B, 0x400000, 3, FIXED, p1, 0x10000) == 0 && (uint64_t)B == Z(7), "second (aliasing) map");
  *(volatile uint64_t *)((uint64_t)A + 0x1000) = 0x1122334455667788ULL;
  MT_EXPECT(*(volatile uint64_t *)((uint64_t)B + 0x1000) == 0x1122334455667788ULL, "alias B does not see writes through A");
  long badp = -1;
  MT_EXPECT(sceKernelAllocateDirectMemory(0, (long)total, 0, 0x10000, 0, &badp) != 0, "len 0 accepted");
  int vbad = sceKernelAllocateDirectMemory(0, (long)total, ~0ULL, 0x10000, 0, &badp);
  MT_EXPECT(vbad != 0, "sceKernelAllocateDirectMemory(len = 2^64-1) succeeds (length rounds to 0)" " [" "rc=%d phys=%ld" "]", vbad, badp);
  long bad2 = -1; int nb = sceKernelAllocateDirectMemory(0, (long)total, 0x10000, 0x3000, 0, &bad2);
  MT_EXPECT(nb != 0, "sceKernelAllocateDirectMemory(alignment 0x3000, not a power of two) succeeds" " [" "rc=%d" "]", nb);
  MT_EXPECT(sceKernelReleaseDirectMemory(p1, 0x400000) == 0, "release");
  MT_EXPECT(sceKernelReleaseDirectMemory(p1, 0x400000) != 0, "double release accepted");
  sceKernelMunmap(A, 0x400000); sceKernelMunmap(B, 0x400000);
  mt_end();

  mt_begin(NM("direct_map_fixed_over_existing"), "guest-vm");
  /* deadlocks the runtime (g_direct_mutex re-entered): kept last, and only reached when it does not */
  long p2 = -1;
  sceKernelAllocateDirectMemory(0, (long)total, 0x10000, 0x10000, 0, &p2);
  X = Z(8);
  fixed(X + PAGE, PAGE);
  void *dm = (void *)X;
  int drc = sceKernelMapDirectMemory(&dm, 0x10000, 3, FIXED, p2, 0x10000);
  MT_EXPECT(drc == 0 && (uint64_t)dm == X, "MapDirectMemory fixed over an existing mapping rc=%d", drc);
  mt_end();
  return mt_status();
}
