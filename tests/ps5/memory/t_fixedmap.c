/* M1 memory model test: fixed guest mappings at addresses a native arm64 macOS process cannot
 * map itself, and the layout rules of docs/m1/memory.md.
 *
 *   0x2_0000_0000   start of the PS5 application area; host: dyld shared cache
 *   0x30_0020_0000  the address 007 First Light hardcodes; host: 384 GiB GPU carveout
 *   0x70_0000_0000  first address above the carveout (the identity arena's lower bound)
 *   0xFB_FFE0_0000  last 2 MiB of the application area
 *
 * Every address is accessed through a constant the compiler bakes into the code, as a title
 * that hardcodes its arena does, so a runtime that hands back a different address fails here.
 * Output is deterministic: the guest layout does not depend on the host. */
#include <stddef.h>
#include <stdint.h>

extern long sceKernelWrite(int fd, const void *buf, size_t n);
/* libkernel imports. The payload SDK's own mmap() is a raw SYSCALL wrapper linked into the ELF,
 * not an import, so the test goes through the kernel library like a title does. */
extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);
extern int munmap(void *addr, size_t len);
extern int sceKernelMapFlexibleMemory(void **addr_in_out, size_t len, int prot, int flags);
extern int sceKernelMunmap(void *addr, size_t len);

#define PROT_RW 3
#define MAP_PRIVATE 0x2
#define MAP_FIXED 0x10
#define MAP_ANON 0x1000
#define MAP_EXCL 0x4000
#define MAP_FAILED ((void *)-1)
#define MIB2 0x200000UL

static void *map(void *addr, size_t len, int prot, int flags, int fd, long off) {
  void *res = MAP_FAILED;
  return sceKernelMmap(addr, len, prot, flags, fd, off, &res) == 0 ? res : MAP_FAILED;
}

static void put(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  sceKernelWrite(1, s, n);
}
static void hex(const char *label, uint64_t v) {
  static const char H[] = "0123456789abcdef";
  char b[20];
  put(label);
  b[0] = '0', b[1] = 'x';
  for (int i = 0; i < 16; i++) b[2 + i] = H[(v >> (4 * (15 - i))) & 15];
  b[18] = '\n';
  sceKernelWrite(1, b, 19);
}

/* Fill and verify a range through its hardcoded address; returns a checksum. */
static uint64_t exercise(volatile uint64_t *p, size_t bytes, uint64_t seed) {
  size_t n = bytes / 8;
  for (size_t i = 0; i < n; i += 509) p[i] = seed ^ (i * 0x9e3779b97f4a7c15ULL);
  p[n - 1] = ~seed;
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i += 509) {
    if (p[i] != (seed ^ (i * 0x9e3779b97f4a7c15ULL))) return 0;
    sum += p[i];
  }
  return p[n - 1] == ~seed ? sum | 1 : 0;
}

static int fixed(const char *label, uint64_t addr, uint64_t seed) {
  void *p = map((void *)addr, MIB2, PROT_RW, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  hex(label, (uint64_t)p);
  if (p != (void *)addr) return 1;
  /* fresh anonymous memory is zero */
  if (*(volatile uint64_t *)addr != 0 || *(volatile uint64_t *)(addr + MIB2 - 8) != 0) return 1;
  uint64_t sum = exercise((volatile uint64_t *)addr, MIB2, seed);
  hex("  sum   ", sum);
  return sum == 0;
}

int main(void) {
  int bad = 0;
  hex("main    ", (uint64_t)&main & ~0xfffffULL); /* image is at the console base */

  bad |= fixed("map 2G  ", 0x200000000ULL, 0x1111);
  bad |= fixed("map 30G ", 0x3000200000ULL, 0x2222);
  bad |= fixed("map 70G ", 0x7000000000ULL, 0x3333);
  bad |= fixed("map top ", 0xfbffe00000ULL, 0x4444);

  /* The First Light pattern: a store through an absolute address inside the mapping. */
  *(volatile uint64_t *)0x3000200010ULL = 0x00c0ffee00c0ffeeULL;
  hex("abs 30G ", *(volatile uint64_t *)0x3000200010ULL);

  /* Mappings are independent memory: 0x2_0000_0000 still holds its own data. */
  hex("keep 2G ", *(volatile uint64_t *)0x200000000ULL);

  /* Unhinted mapping: user zone, lowest free address at or above 0x2_0000_0000. */
  void *dyn = map(0, 0x10000, PROT_RW, MAP_PRIVATE | MAP_ANON, -1, 0);
  hex("dyn     ", (uint64_t)dyn);
  if (dyn == MAP_FAILED || (uint64_t)dyn < 0x200200000ULL) bad |= 1;
  *(volatile uint32_t *)dyn = 0xabcd;

  /* MAP_FIXED|MAP_EXCL over a live mapping must fail and leave it intact. */
  void *excl = map((void *)0x200000000ULL, 0x4000, PROT_RW, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0);
  hex("excl    ", (uint64_t)excl);
  if (excl != MAP_FAILED || *(volatile uint64_t *)0x200000000ULL != 0x1111) bad |= 1;

  /* Plain MAP_FIXED over part of a live mapping replaces just those pages with zeros. */
  *(volatile uint64_t *)0x200004000ULL = 0x55;
  *(volatile uint64_t *)0x200008000ULL = 0x66;
  void *rep = map((void *)0x200004000ULL, 0x4000, PROT_RW, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  hex("replace ", (uint64_t)rep);
  *(volatile uint64_t *)0x200003ff8ULL = 0x77; /* page before: still mapped */
  if (rep != (void *)0x200004000ULL || *(volatile uint64_t *)0x200004000ULL != 0 ||
      *(volatile uint64_t *)0x200000000ULL != 0x1111 || *(volatile uint64_t *)0x200008000ULL != 0x66 || *(volatile uint64_t *)0x200003ff8ULL != 0x77)
    bad |= 1;

  /* Pages are 16 KiB: a fixed address that is not page aligned is refused. */
  void *odd = map((void *)0x240001000ULL, 0x4000, PROT_RW, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  hex("unalign ", (uint64_t)odd);
  if (odd != MAP_FAILED) bad |= 1;

  /* A fixed mapping may not replace the executable image. */
  void *img = map((void *)((uint64_t)&main & ~0x3fffULL), 0x4000, PROT_RW, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  hex("image   ", (uint64_t)img);
  if (img != MAP_FAILED) bad |= 1;

  /* sceKernel flexible memory at a fixed address, then unmap and map again: zero-filled. */
  void *flex = (void *)0x2400000000ULL;
  int rc = sceKernelMapFlexibleMemory(&flex, MIB2, PROT_RW, MAP_FIXED);
  hex("flex rc ", (uint64_t)(uint32_t)rc);
  hex("flex    ", (uint64_t)flex);
  if (rc != 0 || flex != (void *)0x2400000000ULL) bad |= 1;
  else {
    *(volatile uint64_t *)0x2400000000ULL = 0xdead;
    rc = sceKernelMunmap(flex, MIB2);
    hex("unmap rc", (uint64_t)(uint32_t)rc);
    flex = (void *)0x2400000000ULL;
    rc = sceKernelMapFlexibleMemory(&flex, MIB2, PROT_RW, MAP_FIXED);
    hex("remap   ", *(volatile uint64_t *)0x2400000000ULL);
    if (rc != 0 || *(volatile uint64_t *)0x2400000000ULL != 0) bad |= 1;
  }

  if (munmap((void *)0x3000200000ULL, MIB2) != 0) bad |= 1;
  put(bad ? "FIXEDMAP FAIL\n" : "FIXEDMAP OK\n");
  return bad;
}
