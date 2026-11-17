/* Call/return model (SPEC 3.3, docs/m1/flatstack.md): everything a guest can do with CALL and RET
 * that a "CALL = host bl" translation gets wrong or only gets right by accident.
 *   ret imm16, push+ret (a RET that is not the return of the matching CALL), call+pop (a CALL
 *   that never returns), recursion deeper than the shadow return stack, longjmp out of such a
 *   recursion and across a compacted shadow stack, HLE -> guest callbacks that recurse and
 *   re-enter the HLE function, tail calls through pointers, and all of it on several threads.
 * The x86 paths are assembly; -DNPS5_HOST builds the portable models that define the expected
 * output. */
#include "ps5test.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>

#ifndef NPS5_HOST
uint64_t cm_call_retimm(uint64_t a, uint64_t b);
uint64_t cm_pushret(uint64_t x);
uint64_t cm_getpc_delta(void);
uint64_t cm_swapret(uint64_t x);
__asm__(
    ".text\n"
    /* callee pops its two stack arguments: RET imm16 */
    ".globl cm_retimm\n.type cm_retimm,@function\n"
    "cm_retimm:\n"
    "  movq 8(%rsp), %rax\n"
    "  leaq (%rax,%rax,2), %rax\n"
    "  addq 16(%rsp), %rax\n"
    "  retq $16\n"
    ".globl cm_call_retimm\n.type cm_call_retimm,@function\n"
    "cm_call_retimm:\n"
    "  pushq %rbx\n"
    "  movq %rsp, %rbx\n"
    "  pushq %rsi\n"
    "  pushq %rdi\n"
    "  callq cm_retimm\n"
    "  subq %rsp, %rbx\n"       /* 0 when the callee popped exactly 16 bytes */
    "  shlq $32, %rbx\n"
    "  orq %rbx, %rax\n"
    "  popq %rbx\n"
    "  retq\n"
    /* a RET used as an indirect jump: its target was never CALLed */
    ".globl cm_pushret_target\n.type cm_pushret_target,@function\n"
    "cm_pushret_target:\n"
    "  leaq 100(%rdi), %rax\n"
    "  retq\n"
    ".globl cm_pushret\n.type cm_pushret,@function\n"
    "cm_pushret:\n"
    "  leaq cm_pushret_target(%rip), %rax\n"
    "  pushq %rax\n"
    "  retq\n"
    /* a CALL that is never returned from: call next; pop */
    ".globl cm_getpc_delta\n.type cm_getpc_delta,@function\n"
    "cm_getpc_delta:\n"
    "  callq 1f\n"
    "1:\n"
    "  popq %rax\n"
    "  leaq cm_getpc_delta(%rip), %rcx\n"
    "  subq %rcx, %rax\n"
    "  retq\n"
    /* the return address is replaced while the callee runs: returns through cm_swap_land */
    ".globl cm_swap_land\n.type cm_swap_land,@function\n"
    "cm_swap_land:\n"
    "  addq $7, %rax\n"
    "  jmpq *%rdx\n"
    ".globl cm_swap_inner\n.type cm_swap_inner,@function\n"
    "cm_swap_inner:\n"
    "  movq (%rsp), %rdx\n"                 /* the real return address */
    "  leaq cm_swap_land(%rip), %rcx\n"
    "  movq %rcx, (%rsp)\n"
    "  leaq (%rdi,%rdi), %rax\n"
    "  retq\n"
    ".globl cm_swapret\n.type cm_swapret,@function\n"
    "cm_swapret:\n"
    "  pushq %rbx\n"
    "  callq cm_swap_inner\n"
    "  addq $1, %rax\n"
    "  popq %rbx\n"
    "  retq\n");
#else
static uint64_t cm_call_retimm(uint64_t a, uint64_t b) { return a * 3 + b; }
static uint64_t cm_pushret(uint64_t x) { return x + 100; }
static uint64_t cm_getpc_delta(void) { return 5; }
static uint64_t cm_swapret(uint64_t x) { return 2 * x + 7 + 1; }
#endif

/* ---- recursion deeper than the shadow stack (default 32768 entries) ---- */
static T_NOINLINE uint64_t deep(uint32_t n, volatile uint64_t *acc) {
  if (n == 0) return 1;
  uint64_t r = deep(n - 1, acc);
  *acc += r ^ n;
  return r * 3 + n;
}

/* ---- longjmp out of a deep recursion ---- */
static jmp_buf g_jb;
static T_NOINLINE uint64_t dive(uint32_t n, uint32_t jump_at) {
  volatile uint64_t keep = n * 7u;
  if (n == jump_at) longjmp(g_jb, (int)(n & 0x7fff) + 1);
  return dive(n + 1, jump_at) + keep;
}
static T_NOINLINE uint64_t jump_round(uint32_t depth) {
  volatile uint64_t before = depth * 11u;
  int r = setjmp(g_jb);
  if (r == 0) return dive(0, depth);
  return before + (uint64_t)r;
}

/* ---- HLE -> guest callbacks: qsort comparators that recurse and call qsort again ---- */
static T_NOINLINE int weight(int v, int d) { return d == 0 ? v : weight(v ^ (d * 37), d - 1) + 1; }
static uint64_t g_cmp_calls;
static int cmp_plain(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  g_cmp_calls++;
  return (x > y) - (x < y);
}
static int cmp_nested(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  int t[6] = {x & 7, y & 7, (x >> 3) & 7, (y >> 3) & 7, (x >> 6) & 7, (y >> 6) & 7};
  qsort(t, 6, sizeof t[0], cmp_plain); /* HLE -> guest -> HLE -> guest */
  int wx = weight(x, 9) + t[0], wy = weight(y, 9) + t[0];
  if (wx != wy) return (wx > wy) - (wx < wy);
  return (x > y) - (x < y); /* total order: the result does not depend on the qsort algorithm */
}

/* ---- tail calls through pointers ---- */
typedef uint64_t (*Step)(uint64_t, uint64_t);
static Step g_steps[3];
static uint64_t step_a(uint64_t n, uint64_t acc) { return n == 0 ? acc : g_steps[n % 3](n - 1, acc * 31 + 1); }
static uint64_t step_b(uint64_t n, uint64_t acc) { return n == 0 ? acc : g_steps[n % 3](n - 1, acc * 17 + 2); }
static uint64_t step_c(uint64_t n, uint64_t acc) { return n == 0 ? acc : g_steps[n % 3](n - 1, acc ^ (acc >> 7) ^ 3); }

/* ---- the same on threads: every thread has its own shadow stack ---- */
static void *worker(void *p) {
  uint64_t id = (uint64_t)(uintptr_t)p;
  volatile uint64_t acc = id;
  uint64_t h = deep(9000 + (uint32_t)id * 1000, &acc);
  jmp_buf jb;
  volatile uint64_t rounds = 0;
  int r = setjmp(jb);
  if (r < 50) { rounds += (uint64_t)r; longjmp(jb, r + 1); }
  h = h * 1099511628211ULL ^ acc ^ rounds ^ step_a(3000 + id, id);
  return (void *)(uintptr_t)h;
}

int main(void) {
  t_hex("retimm", cm_call_retimm(0x1111, 0x2222));
  t_hex("pushret", cm_pushret(0x40));
  uint64_t d = 0;
  for (int i = 0; i < 1000; i++) d += cm_getpc_delta();
  t_dec("getpc_delta_x1000", (long long)d);
  uint64_t sw = 0;
  for (uint64_t i = 0; i < 1000; i++) sw += cm_swapret(i);
  t_dec("swapret_sum", (long long)sw);

  volatile uint64_t acc = 0;
  t_hex("deep_100000", deep(100000, &acc));
  t_hex("deep_acc", acc);
  t_hex("deep_again", deep(40000, &acc));

  uint64_t j = 0;
  for (uint32_t depth = 1; depth <= 100000; depth = depth * 3 + 1) j = j * 131 + jump_round(depth);
  t_hex("longjmp_depths", j);
  for (int i = 0; i < 2000; i++) j += jump_round(40 + (uint32_t)(i & 63));
  t_hex("longjmp_x2000", j);

  int v[200];
  uint64_t s = 99;
  for (int i = 0; i < 200; i++) v[i] = (int)(t_lcg(&s) & 0x3ff);
  qsort(v, 200, sizeof v[0], cmp_nested);
  t_hex("qsort_nested", t_fnv(T_FNV0, v, sizeof v));
  t_dec("cmp_calls_nonzero", g_cmp_calls != 0);

  g_steps[0] = step_a; g_steps[1] = step_b; g_steps[2] = step_c;
  t_hex("tail_steps", step_a(100000, 1));

  pthread_t th[4];
  uint64_t tot = 0;
  for (uintptr_t i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, (void *)(i + 1));
  for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); tot ^= (uint64_t)(uintptr_t)r + (uint64_t)i; }
  t_hex("threads", tot);
  t_str("t_callmodel: done\n");
  return 0;
}
