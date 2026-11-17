/* Guest fault / stack tests (memory.md sections 3 and 6). One case per run, selected by argv[1]:
 *   the program prints "CASE <name> start", does the access, and (if it survives) "CASE <name> SURVIVED".
 * tools/memtests.sh runs each case sandboxed and checks the exit status and the crash report's
 * classification (crash.txt: fault_range_name / "guest ..." class lines). */
#include <stddef.h>
#include <stdint.h>

extern long sceKernelWrite(int fd, const void *buf, size_t n);
extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);
extern int sceKernelMunmap(void *addr, size_t len);
extern int sceKernelMprotect(const void *addr, size_t len, int prot);
extern int scePthreadCreate(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
extern int scePthreadJoin(void *thr, void **ret);

#define MAPF 0x1002
static void put(const char *s) { size_t n = 0; while (s[n]) n++; sceKernelWrite(1, s, n); }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void hex(const char *l, uint64_t v) {
  static const char H[] = "0123456789abcdef"; char b[20]; put(l); b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; i++) b[2 + i] = H[(v >> (4 * (15 - i))) & 15];
  b[18] = '\n'; sceKernelWrite(1, b, 19);
}
static uint64_t org(void) { return (uint64_t)(uintptr_t)&org >= 0x7000000000ULL ? 0x7000000000ULL : 0; }

/* The payload SDK's crt start passes argv = NULL to main. The runtime still lays the arguments out
 * as the console does: [entry rsp] = argc, [rsp+8] = argv[0], and the argument block {argc, argv[0..]}
 * at argv[0] - 0x120. The entry rsp lies just under the main stack top (0x7_EEFF_C000); find it by
 * checking the two views agree. */
static volatile uint64_t SH = 12;
#define BIG(x) ((uint64_t)((x) >> 12) << SH)
static uint64_t org(void);
static const char *arg1(void) {
  uint64_t top = org() + BIG(0x7EEFFC000ULL);
  for (uint64_t sp = top - 0x1200; sp < top - 0x800; sp += 8) {
    int64_t argc = *(volatile int64_t *)sp;
    uint64_t p0 = *(volatile uint64_t *)(sp + 8);
    if (argc < 1 || argc > 8 || p0 < org() + 0x400000 || p0 > org() + BIG(0x200000000ULL)) continue;
    uint64_t blk = p0 - 0x120;
    if (*(volatile int32_t *)blk != (int32_t)argc || *(volatile uint64_t *)(blk + 8) != p0) continue;
    if (argc < 2) return "none";
    return (const char *)(uintptr_t)*(volatile uint64_t *)(blk + 16);
  }
  return "none";
}
static volatile uint64_t sink;
__attribute__((noinline)) static uint64_t rd(uint64_t a) { return *(volatile uint64_t *)a; }
__attribute__((noinline)) static void wr(uint64_t a, uint64_t v) { *(volatile uint64_t *)a = v; }
struct node { uint64_t a, b, c; struct node *next; };
__attribute__((noinline)) static uint64_t deref_field(struct node *p) { return p->next->c; }  /* p->next == NULL: access at offset 0x10 */
__attribute__((noinline)) static uint64_t recurse(uint64_t n, volatile uint8_t *base) {
  volatile uint8_t pad[512];
  pad[0] = (uint8_t)n; pad[511] = (uint8_t)(n >> 3);
  if (base && n == 0) return pad[0];
  return recurse(n + 1, base) + pad[(n * 7) & 511];   /* not a tail call: the frame stays */
}
__attribute__((noinline)) static uint64_t recurse_n(uint64_t n) {
  volatile uint8_t pad[1024];
  for (int i = 0; i < 1024; i += 256) pad[i] = (uint8_t)n;
  if (n == 0) return pad[0];
  return recurse_n(n - 1) + pad[256];
}
static void *thread_deep(void *a) { uint64_t d = (uint64_t)a; sink = recurse_n(d); return (void *)(uintptr_t)(sink + 1); }
static void *thread_inf(void *a) { (void)a; sink = recurse(1, 0); return 0; }
static void (*volatile null_fn)(void);

int main(int argc, char **argv) {
  (void)argv; (void)argc; const char *c = arg1();
  put("CASE "); put(c); put(" start\n");
  uint64_t O = org();
  if (eq(c, "none")) { hex("argc ", (uint64_t)argc); }
  else if (eq(c, "null_read")) sink = rd(0);
  else if (eq(c, "null_write")) wr(8, 1);
  else if (eq(c, "null_near_end")) sink = rd(0x3FFFF8);
  else if (eq(c, "null_field")) { struct node n = {1, 2, 3, 0}; sink = deref_field(&n); }
  else if (eq(c, "null_call")) { null_fn = 0; null_fn(); }
  else if (eq(c, "after_munmap")) {
    void *p = 0; if (sceKernelMmap(0, 0x10000, 3, MAPF, -1, 0, &p) != 0) { put("mmap failed\n"); return 3; }
    wr((uint64_t)p, 7); sceKernelMunmap(p, 0x10000); sink = rd((uint64_t)p + 8);
  }
  else if (eq(c, "hole_in_mapping")) {
    void *p = 0; if (sceKernelMmap(0, 0x10000, 3, MAPF, -1, 0, &p) != 0) return 3;
    sceKernelMunmap((char *)p + 0x4000, 0x4000); sink = rd((uint64_t)p + 0x4010);
  }
  else if (eq(c, "mprotect_write")) {
    void *p = 0; if (sceKernelMmap(0, 0x8000, 3, MAPF, -1, 0, &p) != 0) return 3;
    wr((uint64_t)p, 1); if (sceKernelMprotect(p, 0x4000, 1) != 0) return 4; wr((uint64_t)p + 8, 2);
  }
  else if (eq(c, "mprotect_none_read")) {
    void *p = 0; if (sceKernelMmap(0, 0x8000, 3, MAPF, -1, 0, &p) != 0) return 3;
    if (sceKernelMprotect(p, 0x4000, 0) != 0) return 4; sink = rd((uint64_t)p + 8);
  }
  else if (eq(c, "write_code")) wr(O + 0x400000, 0);              /* first byte of the image: inside the RX text segment */
  else if (eq(c, "write_code_fn")) wr((uint64_t)(uintptr_t)&recurse_n, 0); /* a function: may share a 16 KiB page with data */
  else if (eq(c, "beyond_area")) sink = rd(0xFC00000000ULL + 8);
  else if (eq(c, "wild_40bit")) sink = rd(0xFC00000000ULL * 2 + 0x10);
  else if (eq(c, "wild_host_like")) sink = rd(0x0000000100000000ULL * 5 + 0x28);
  else if (eq(c, "stack_deep_ok")) { sink = recurse_n(20000); hex("deep ", sink); put("CASE stack_deep_ok OK\n"); }
  else if (eq(c, "stack_overflow")) sink = recurse(1, 0);
  else if (eq(c, "thread_deep_ok")) {
    uint64_t t[4]; void *r;
    for (int i = 0; i < 4; i++) scePthreadCreate(&t[i], 0, thread_deep, (void *)(uintptr_t)(300 + i * 50), "deep");
    uint64_t sum = 0; for (int i = 0; i < 4; i++) { scePthreadJoin((void *)t[i], &r); sum += (uint64_t)(uintptr_t)r; }
    hex("threads ", sum); put("CASE thread_deep_ok OK\n");
  }
  else if (eq(c, "thread_overflow")) { uint64_t t; void *r; scePthreadCreate(&t, 0, thread_inf, 0, "inf"); scePthreadJoin((void *)t, &r); }
  else if (eq(c, "big_alloc")) {
    uint64_t total = 0; void *p;
    for (int i = 0; i < 4; i++) {
      p = 0; int rc = sceKernelMmap(0, 1ULL << 30, 3, MAPF, -1, 0, &p);
      if (rc != 0) { hex("mmap 1GiB failed rc ", (uint64_t)(uint32_t)rc); return 3; }
      for (uint64_t o = 0; o < (1ULL << 30); o += 0x4000 * 64) { *(volatile uint64_t *)((uint64_t)p + o) = o ^ (uint64_t)i; }
      for (uint64_t o = 0; o < (1ULL << 30); o += 0x4000 * 64) if (*(volatile uint64_t *)((uint64_t)p + o) != (o ^ (uint64_t)i)) { put("corrupt\n"); return 4; }
      *(volatile uint64_t *)((uint64_t)p + (1ULL << 30) - 8) = 0x5a5a; total += 1ULL << 30;
    }
    hex("mapped ", total); put("CASE big_alloc OK\n");
  }
  else { put("unknown case\n"); return 9; }
  put("CASE "); put(c); put(" SURVIVED\n");
  return 0;
}
