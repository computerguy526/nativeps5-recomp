/* nps5 diagnostics test programs (SPEC 8.2 acceptance). Build one with -DCASE=<n>:
 *   1 bad memory access   2 unsupported instruction (AVX2 VPERMQ)   3 unresolved import   4 guest ud2 trap
 * Only sceKernelWrite is needed from the host, so each runs with -fplt on the PS4Recomp shim set. */
#include <stddef.h>
extern long sceKernelWrite(int fd, const void *buf, size_t n);
extern int sceKernelAprCtrl(int a);

static void say(const char *s) { size_t n = 0; while (s[n]) n++; sceKernelWrite(1, s, n); }

__attribute__((noinline)) int guest_poke_bad_pointer(volatile int *p) { return *p; }
__attribute__((noinline)) int guest_use_avx2(int x) {
  int out[8] = {x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7};
  __asm__ volatile("vmovdqu (%0), %%ymm0\n\tvpermq $0x1b, %%ymm0, %%ymm1\n\tvmovdqu %%ymm1, (%0)\n\tvzeroupper" : : "r"(out) : "xmm0", "xmm1", "memory");
  return out[0];
}
__attribute__((noinline)) int guest_call_missing_import(int x) { return sceKernelAprCtrl(x); }
__attribute__((noinline)) int guest_ud2(int x) { if (x > 0) __builtin_trap(); return x; }

int main(void) {
  say("about to fail\n");
  int r = 0;
#if CASE == 1
  r = guest_poke_bad_pointer((volatile int *)0x200000000000UL);
#elif CASE == 2
  r = guest_use_avx2(1);
#elif CASE == 3
  r = guest_call_missing_import(7);
#elif CASE == 4
  r = guest_ud2(1);
#endif
  say("not reached\n");
  return r;
}
