/* Stack privacy (-tso-elide, docs/m3/tsoelide.md): functional coverage of the frame shapes the
 * analysis classifies. Every result is deterministic; the racy ordering litmus tests are in
 * tests/ps5/tsoelide/t_tsolitmus.c (tools/tsoelide_test.sh).
 *   1. a stack buffer handed to another thread (synchronised by create/join)
 *   2. setjmp/longjmp back into a function whose frame escaped after the setjmp
 *   3. a guest exception handler reading the interrupted thread's frame
 *   4. pop of a slot whose address a callee received; frame pointer, realigned frame, alloca,
 *      red zone leaf, incoming stack arguments */
#include "ps5test.h"
#include <pthread.h>
#include <setjmp.h>

/* ---- 1. stack buffer handed to a thread ---- */
struct job { uint64_t in[8]; uint64_t out[8]; uint64_t seed; };
static void *job_thread(void *a) {
  struct job *j = (struct job *)a;
  for (int i = 0; i < 8; i++) { j->out[i] = j->in[i] * 0x9e3779b97f4a7c15ULL + j->seed; j->in[i] ^= j->out[i] >> 7; }
  return 0;
}
static T_NOINLINE uint64_t handoff(uint64_t seed) {
  struct job j;
  uint64_t h = T_FNV0;
  for (int round = 0; round < 50; round++) {
    for (int i = 0; i < 8; i++) j.in[i] = t_lcg(&seed);
    j.seed = seed;
    pthread_t t;
    pthread_create(&t, 0, job_thread, &j);
    pthread_join(t, 0);
    h = t_fnv(h, j.in, sizeof j.in);
    h = t_fnv(h, j.out, sizeof j.out);
  }
  return h;
}

/* ---- 2. setjmp/longjmp ---- */
static jmp_buf g_jb;
static T_NOINLINE void bump_and_jump(volatile uint64_t *p, volatile uint64_t *arr, int n) {
  *p += (uint64_t)n;
  arr[n & 3] ^= 0x55;
  if (n < 4) longjmp(g_jb, n + 1);
}
static T_NOINLINE uint64_t sj(uint64_t x) {
  volatile uint64_t a = x, b = 2;
  volatile uint64_t arr[4] = {1, 2, 3, 4};
  volatile uint64_t before = 0;
  int r = setjmp(g_jb);
  before += 1;              /* runs once per return of setjmp, also after the frame escaped */
  a = a * 3 + (uint64_t)r;
  if (r < 4) bump_and_jump(&b, arr, r);
  uint64_t out[5] = {arr[0], arr[1], arr[2], arr[3], a * 1000003 + b * 10007 + before};
  return t_fnv(T_FNV0, out, sizeof out);
}
/* jmp_buf on the stack: the setjmp call itself is the escape. */
static T_NOINLINE void jump_out(jmp_buf *jb, int v) { longjmp(*jb, v); }
static T_NOINLINE uint64_t sj_local(uint64_t x) {
  jmp_buf jb;
  volatile uint64_t acc = x;
  int r = setjmp(jb);
  acc = acc * 31 + (uint64_t)r;
  if (r < 6) jump_out(&jb, r + 1);
  return acc;
}

/* ---- 3. guest exception handler on top of interrupted code ---- */
struct pair { volatile uint64_t x, y; };
static struct pair *volatile g_pair;
static volatile uint64_t g_exc_calls, g_exc_bad, g_exc_stop, g_exc_ready;
static void exc_check(void) {
  struct pair *p = g_pair;
  if (p) {
    uint64_t y = p->y, x = p->x; /* the thread stores x then y: x is y or y+1 at every instruction boundary */
    if (x < y || x - y > 1) __atomic_add_fetch(&g_exc_bad, 1, __ATOMIC_SEQ_CST);
  }
  __atomic_add_fetch(&g_exc_calls, 1, __ATOMIC_SEQ_CST);
}
#ifdef NPS5_HOST
#include <signal.h>
#include <unistd.h>
static void exc_handler(int s) { (void)s; exc_check(); }
static void exc_install(void) { signal(SIGUSR1, exc_handler); }
static void exc_raise(pthread_t t) { pthread_kill(t, SIGUSR1); }
static void nap(unsigned us) { usleep(us); }
#else
extern int sceKernelInstallExceptionHandler(int, void (*)(int, void *));
extern int sceKernelRaiseException(pthread_t, int);
extern int sceKernelUsleep(unsigned);
static void exc_handler(int s, void *uc) { (void)s; (void)uc; exc_check(); }
static void exc_install(void) { sceKernelInstallExceptionHandler(30, exc_handler); }
static void exc_raise(pthread_t t) { sceKernelRaiseException(t, 30); }
static void nap(unsigned us) { sceKernelUsleep(us); }
#endif
static void *exc_thread(void *a) {
  (void)a;
  struct pair p = {0, 0};
  g_pair = &p;
  __atomic_store_n(&g_exc_ready, 1, __ATOMIC_SEQ_CST);
  uint64_t i = 0;
  while (!__atomic_load_n(&g_exc_stop, __ATOMIC_SEQ_CST)) { i++; p.x = i; p.y = i; }
  g_pair = 0;
  return 0;
}
static void exceptions(void) {
  pthread_t t;
  exc_install();
  pthread_create(&t, 0, exc_thread, 0);
  while (!__atomic_load_n(&g_exc_ready, __ATOMIC_SEQ_CST)) nap(100);
  for (int i = 0; i < 200; i++) { exc_raise(t); nap(200); }
  /* Every raise is delivered before the thread is told to stop; wait for the handlers. */
  for (int i = 0; i < 2000 && __atomic_load_n(&g_exc_calls, __ATOMIC_SEQ_CST) == 0; i++) nap(500);
  __atomic_store_n(&g_exc_stop, 1, __ATOMIC_SEQ_CST);
  pthread_join(t, 0);
  t_dec("exc_handler_ran", __atomic_load_n(&g_exc_calls, __ATOMIC_SEQ_CST) > 0);
  t_dec("exc_violations", (long long)g_exc_bad);
}

/* ---- 4. frame shapes ---- */
static T_NOINLINE void store_through(uint64_t *p) { *p = *p * 7 + 1; }
#ifndef NPS5_HOST
/* uint64_t pop_escaped(void (*f)(uint64_t *), uint64_t v): push v; f(rsp); pop rax. The pop reads
 * a slot the callee wrote through the address it was given. */
__asm__(".text\n.globl pop_escaped\n.type pop_escaped,@function\npop_escaped:\n"
        "  push %rbx\n  mov %rdi, %rbx\n  sub $8, %rsp\n  push %rsi\n  mov %rsp, %rdi\n  call *%rbx\n"
        "  pop %rax\n  add $8, %rsp\n  pop %rbx\n  ret\n"
        /* uint64_t redzone_leaf(uint64_t a, uint64_t b): locals below rsp, no frame. */
        ".globl redzone_leaf\n.type redzone_leaf,@function\nredzone_leaf:\n"
        "  mov %rdi, -8(%rsp)\n  mov %rsi, -16(%rsp)\n  mov -8(%rsp), %rax\n  imul $31, %rax, %rax\n"
        "  add -16(%rsp), %rax\n  mov %rax, -24(%rsp)\n  xor -24(%rsp), %rdi\n  lea (%rdi,%rax,2), %rax\n  ret\n"
        /* uint64_t rbp_walker(const uint64_t *p, long n): rbp as a general pointer, not a frame. */
        ".globl rbp_walker\n.type rbp_walker,@function\nrbp_walker:\n"
        "  push %rbp\n  mov %rdi, %rbp\n  xor %eax, %eax\n1:\n  add (%rbp), %rax\n  rol $5, %rax\n"
        "  add $8, %rbp\n  dec %rsi\n  jnz 1b\n  pop %rbp\n  ret\n");
uint64_t pop_escaped(void (*f)(uint64_t *), uint64_t v);
uint64_t redzone_leaf(uint64_t a, uint64_t b);
uint64_t rbp_walker(const uint64_t *p, long n);
#else
static uint64_t pop_escaped(void (*f)(uint64_t *), uint64_t v) { f(&v); return v; }
static uint64_t redzone_leaf(uint64_t a, uint64_t b) { uint64_t r = a * 31 + b; return (a ^ r) + 2 * r; }
static uint64_t rbp_walker(const uint64_t *p, long n) {
  uint64_t r = 0;
  for (long i = 0; i < n; i++) { r += p[i]; r = r << 5 | r >> 59; }
  return r;
}
#endif
struct big { uint64_t v[6]; };
static T_NOINLINE uint64_t stack_args(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                      uint64_t g, uint64_t h, struct big s) {
  s.v[0] += a + g;           /* written in the caller's outgoing argument area */
  store_through(&s.v[1]);    /* ... whose address is taken here */
  return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19 + s.v[0] + s.v[1] * 23 + s.v[5];
}
static T_NOINLINE uint64_t private_frame(uint64_t x) { /* no address leaves: every slot is private */
  volatile uint64_t a = x, b = x * 3, c[4] = {1, 2, 3, 4};
  for (int i = 0; i < 16; i++) { c[i & 3] += a ^ b; a += c[(i + 1) & 3]; b ^= a >> 3; }
  return a + b + c[0] + c[1] + c[2] + c[3];
}
static T_NOINLINE uint64_t realigned(uint64_t x) {
  volatile uint64_t wide[8] __attribute__((aligned(64))) = {x, 1, 2, 3, 4, 5, 6, 7};
  volatile uint64_t s = 0;
  for (int i = 0; i < 8; i++) s += wide[i] * (uint64_t)(i + 1);
  uint64_t tmp = s;
  store_through(&tmp);
  return tmp + wide[0];
}
static T_NOINLINE uint64_t dyn_alloc(uint64_t x, int n) {
  uint64_t vla[n];
  volatile uint64_t keep = x;
  for (int i = 0; i < n; i++) vla[i] = x + (uint64_t)i * keep;
  store_through(&vla[n / 2]);
  uint64_t h = T_FNV0;
  h = t_fnv(h, vla, sizeof(uint64_t) * (size_t)n);
  return h + keep;
}

int main(void) {
  t_hex("handoff", handoff(0x1234));
  t_hex("setjmp_global", sj(5));
  t_hex("setjmp_local", sj_local(9));
  exceptions();
  t_hex("pop_escaped", pop_escaped(store_through, 0x1111));
  t_hex("redzone_leaf", redzone_leaf(0xabcdef, 77));
  uint64_t tab[16];
  for (int i = 0; i < 16; i++) tab[i] = 0x0101010101010101ULL * (uint64_t)(i + 1);
  t_hex("rbp_walker", rbp_walker(tab, 16));
  struct big s = {{10, 20, 30, 40, 50, 60}};
  t_hex("stack_args", stack_args(1, 2, 3, 4, 5, 6, 7, 8, s));
  t_hex("stack_args_caller_copy", s.v[0] + s.v[1]);
  t_hex("private_frame", private_frame(0x77));
  t_hex("realigned", realigned(0x4242));
  t_hex("dyn_alloc", dyn_alloc(0x99, 13));
  return 0;
}
