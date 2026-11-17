/* x87 cost microbenchmark (not part of tests.list): the long double paths that a recompiled libc's printf/strtod
 * run: (A) dependent multiply-add chain, (B) digit extraction as Dinkumware's _Ldtob does it (fmul by 10, fisttp,
 * fild, fsub) for 17 digits of 1e5 values.  Build: PATH=/opt/homebrew/opt/llvm/bin:$PATH prospero-clang -O1 -g0
 * x87_bench.c -o x87_bench.elf ; translate with -x87 f80 / -x87 f64 and compare the printed ns/op. */
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#ifdef CPUTIME /* -DCPUTIME: thread CPU time instead of wall time, for a machine with other load */
#define BENCH_CLOCK CLOCK_THREAD_CPUTIME_ID
#else
#define BENCH_CLOCK CLOCK_MONOTONIC
#endif
static double now(void) { struct timespec t; clock_gettime(BENCH_CLOCK, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

__attribute__((noinline)) static long double chain(long double acc, long n) {
  for (long i = 0; i < n; i++) acc = acc * 1.0000001L + 0.5L;
  return acc;
}
__attribute__((noinline)) static long digits(double v0, long n) {
  long sum = 0;
  for (long i = 0; i < n; i++) {
    long double x = (long double)(v0 + i * 0.37);
    x = x - (long double)(long long)x;
    for (int d = 0; d < 17; d++) { x *= 10.0L; int dig = (int)x; sum += dig; x -= dig; }
  }
  return sum;
}
/* (C) printf-like %f formatting of a long double as a recompiled libc does it: integer part by fisttp/fild/fsub, scale
 * by 10^6 with fmul, round with a compare, six digits out of the fraction; plus long double moves (fld/fstp m80). */
static long double table[64];
__attribute__((noinline)) static long format(long n) {
  long sum = 0;
  for (long i = 0; i < n; i++) {
    long double x = (long double)(i * 0.731) + 0.123456789L;
    long double neg = -x;
    long double ax = neg < 0 ? -neg : neg;
    long long ip = (long long)ax;
    long double fr = (ax - (long double)ip) * 1000000.0L;
    long long f = (long long)fr;
    if (fr - (long double)f >= 0.5L) f++;
    table[i & 63] = fr;
    long double mv = table[(i + 7) & 63];
    sum += ip + f + (mv > 1.0L);
  }
  return sum;
}
int main(void) {
#ifdef PC53 /* -DPC53: precision control 53 bits (0x027f) instead of the PS4/PS5 default 0x037f (64 bits) */
  { unsigned short cw = 0x027f; __asm__ volatile("fldcw %0" : : "m"(cw)); }
#endif
  double t0 = now();
  long double r = chain(1.0L, 2000000);
  double t1 = now();
  long s = digits(1234.5678, 100000);
  double t2 = now();
  double t3 = now();
  long fs = format(1000000);
  double t4 = now();
  printf("chain   %.2f ns per (fmul+fadd) iteration   result %.6f\n", (t1 - t0) / 2000000, (double)r);
  printf("digits  %.2f ns per digit (17 per value)    sum %ld\n", (t2 - t1) / (100000.0 * 17), s);
  printf("format  %.2f ns per value                    sum %ld\n", (t4 - t3) / 1000000, fs);
  return 0;
}
