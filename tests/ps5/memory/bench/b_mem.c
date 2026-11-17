/* Base-vs-identity cost probe (docs/m1/memory.md): memory-bound loops whose every guest access
 * pays the addressing mode. Not a correctness test; timed from outside (tools/mem_bench.sh).
 *   stream:  sum and update a 32 MiB array of u64 (sequential load+store)
 *   chase:   pointer chasing through a 4 MiB permutation (dependent loads)
 *   stack:   a call-heavy recursion (stack accesses only)                                  */
#include <stddef.h>
#include <stdint.h>

extern long sceKernelWrite(int fd, const void *buf, size_t n);
extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, long off, void **res);

static void hex(const char *label, uint64_t v) {
  static const char H[] = "0123456789abcdef";
  char b[32];
  int n = 0;
  while (label[n]) { b[n] = label[n]; n++; }
  for (int i = 0; i < 16; i++) b[n + i] = H[(v >> (4 * (15 - i))) & 15];
  b[n + 16] = '\n';
  sceKernelWrite(1, b, n + 17);
}

__attribute__((noinline)) static uint64_t stream(uint64_t *a, size_t n, int rounds) {
  uint64_t s = 0;
  for (int r = 0; r < rounds; r++)
    for (size_t i = 0; i < n; i++) { s += a[i]; a[i] = s ^ i; }
  return s;
}
__attribute__((noinline)) static uint64_t chase(const uint32_t *next, size_t steps) {
  uint32_t p = 0;
  uint64_t s = 0;
  for (size_t i = 0; i < steps; i++) { p = next[p]; s += p; }
  return s;
}
__attribute__((noinline)) static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  void *mem = 0;
  if (sceKernelMmap(0, 48u << 20, 3, 0x1002, -1, 0, &mem) != 0) return 1;
  uint64_t *a = (uint64_t *)mem;
  size_t n = (32u << 20) / 8;
  for (size_t i = 0; i < n; i++) a[i] = i * 0x9e3779b97f4a7c15ULL;
  uint32_t *next = (uint32_t *)((char *)mem + (32u << 20));
  size_t m = (4u << 20) / 4; /* a single cycle: next[i] = i + stride mod m, stride odd */
  for (size_t i = 0; i < m; i++) next[i] = (uint32_t)((i + 0x9e3779b1u) % m);
#ifdef BENCH_STREAM
  hex("stream=", stream(a, n, 24));
#elif defined(BENCH_CHASE)
  hex("chase=", chase(next, 400u * 1000 * 1000));
#else
  hex("fib=", fib(40));
#endif
  return 0;
}
