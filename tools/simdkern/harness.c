// Times one lifted leaf function (cmd/simdstat -emit) in each -simd-liveness mode; see run.sh.
// The function gets three pointer arguments (rdi, rsi, rdx) to 128-byte buffers of doubles, two
// float arguments (xmm0, xmm1), a guest stack and a TCB whose +0x70 slot points at a scratch
// GuestContext. Guest memory is one anonymous mapping (x28 = its base); the bytes the function
// addresses RIP-relatively are copied in from <fn>.consts. Outputs of the three modes are compared.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
struct regs { uint64_t rdi, rsi, rdx, rcx, rsp, fs, base, rax; uint8_t xmm0[16], xmm1[16]; };
void run_kern(void (*fn)(void), struct regs *r);
void KOFF(void); void KSTRICT(void); void KABI(void);
#include "consts.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define ARENA 0x8000000ull
#define A 0x10000
#define B 0x11000
#define C 0x12000
int main(int argc, char **argv) {
  long iters = argc > 1 ? atol(argv[1]) : 2000000; int reps = argc > 2 ? atoi(argv[2]) : 7;
  uint8_t *m = mmap(0, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (m == MAP_FAILED) return 1;
  for (unsigned i = 0; i < sizeof consts / sizeof consts[0]; i++) memcpy(m + consts[i].addr, consts[i].b, 32);
  static uint8_t ctx[16384];
  uint64_t c = (uint64_t)ctx; memcpy(m + 0x50000 + 0x70, &c, 8);
  double init[3][16];
  for (int k = 0; k < 3; k++) for (int i = 0; i < 16; i++) init[k][i] = (i % 5 == 0 ? 2.0 : 0.0) + ((i * 7 + k * 3) % 11) / 16.0 - 0.3;
  struct regs r0 = {A, B, C, 0, 0x40000 - 8, 0x50000, (uint64_t)m, 0, {0}, {0}};
  float f0 = 0.3f, f1 = 0.016f; memcpy(r0.xmm0, &f0, 4); memcpy(r0.xmm1, &f1, 4);
#ifdef SWAP_AB
  r0.rdi = B; r0.rsi = A;
#endif
  void (*fns[3])(void) = {KOFF, KSTRICT, KABI}; const char *names[3] = {"off", "strict", "abi"};
  uint8_t out[3][3 * 128]; uint64_t rax[3];
  for (int k = 0; k < 3; k++) {
    memcpy(m + A, init[0], 128); memcpy(m + B, init[1], 128); memcpy(m + C, init[2], 128);
    struct regs r = r0; run_kern(fns[k], &r); rax[k] = r.rax & 0xff;
    memcpy(out[k], m + A, 128); memcpy(out[k] + 128, m + B, 128); memcpy(out[k] + 256, m + C, 128);
  }
  int same = !memcmp(out[0], out[1], sizeof out[0]) && !memcmp(out[0], out[2], sizeof out[0]) && rax[0] == rax[1] && rax[0] == rax[2];
  int changed = memcmp(out[0], init[0], 128) || memcmp(out[0] + 128, init[1], 128);
  printf("outputs %s across modes (al=%llu, memory %s)\n", same ? "equal" : "DIFFER", (unsigned long long)rax[0], changed ? "written" : "NOT written");
  double best[3] = {1e9, 1e9, 1e9};
  for (int rep = 0; rep < reps; rep++)
    for (int k = 0; k < 3; k++) {
      double t = now();
      for (long i = 0; i < iters; i++) {
        memcpy(m + A, init[0], 128); memcpy(m + B, init[1], 128); memcpy(m + C, init[2], 128);
        struct regs r = r0; run_kern(fns[k], &r);
      }
      t = (now() - t) / iters * 1e9;
      if (t < best[k]) best[k] = t;
    }
  // the same loop without the kernel call body: harness overhead
  double t = now();
  for (long i = 0; i < iters; i++) { memcpy(m + A, init[0], 128); memcpy(m + B, init[1], 128); memcpy(m + C, init[2], 128); struct regs r = r0; __asm__ volatile("" : : "r"(&r) : "memory"); }
  double ovh = (now() - t) / iters * 1e9;
  for (int k = 0; k < 3; k++) printf("%-7s %8.2f ns/call  (%.2fx off)\n", names[k], best[k], best[k] / best[0]);
  printf("harness loop without the call: %.2f ns\n", ovh);
  return same ? 0 : 3;
}
