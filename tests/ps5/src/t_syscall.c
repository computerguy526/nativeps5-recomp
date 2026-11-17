/* Raw SYSCALL: the instruction itself (FreeBSD/Orbis convention: rax = number, args rdi rsi rdx
 * r10 r8 r9, result in rax, CF set and rax = errno on failure), as the payload SDK's mmap()/
 * munmap()/write() stubs use it.  On the host build the same operations go through libc. */
#include "ps5test.h"

#ifdef NPS5_HOST
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
/* returns the result, or -errno on failure */
static T_NOINLINE long sys_mmap(size_t len) {
  void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  return p == MAP_FAILED ? -errno : (long)p;
}
static T_NOINLINE long sys_munmap(long a, size_t len) { return munmap((void *)a, len) ? -errno : 0; }
static T_NOINLINE long sys_write(int fd, const void *p, size_t n) { long r = write(fd, p, n); return r < 0 ? -errno : r; }
#else
static inline long rawsys(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  long rax = n;
  int cf;
  __asm__ volatile("syscall" : "+a"(rax), "+D"(a), "+S"(b), "+d"(c), "=@ccc"(cf), "+r"(r10), "+r"(r8), "+r"(r9)
                   : : "rcx", "r11", "memory");
  return cf ? -rax : rax;
}
static T_NOINLINE long sys_mmap(size_t len) { return rawsys(477, 0, (long)len, 3, 0x1002, -1, 0); }
static T_NOINLINE long sys_munmap(long a, size_t len) { return rawsys(73, a, (long)len, 0, 0, 0, 0); }
static T_NOINLINE long sys_write(int fd, const void *p, size_t n) { return rawsys(4, fd, (long)p, (long)n, 0, 0, 0); }
#endif

int main(void) {
  long p = sys_mmap(0x20000);
  t_dec("mmap_ok", p > 0);
  if (p > 0) {
    volatile uint8_t *m = (volatile uint8_t *)p;
    uint64_t h = T_FNV0;
    for (int i = 0; i < 0x20000; i += 64) m[i] = (uint8_t)(i >> 6);
    for (int i = 0; i < 0x20000; i += 64) h = t_fnv(h, (const void *)&m[i], 1);
    t_hex("fill_hash", h);
    t_dec("munmap", sys_munmap(p, 0x20000));
  }
  /* the result of a raw write, and a failing one: errno comes back in rax with CF set */
  static const char msg[] = "raw write\n";
  t_dec("write_n", sys_write(1, msg, sizeof msg - 1));
  t_dec("write_badfd", sys_write(-1, msg, sizeof msg - 1)); /* -EBADF */
  t_str("done\n");
  return 0;
}
