/* Tier-1 miss fallback test (SPEC 3.4, M1 exit criterion): functions the static discovery cannot see.
 *
 * hidden_fn, hidden_loop and hidden_thr live in a custom section as naked assembly: no FDE, no
 * symbol of type FUNC, no relocation, no direct call. The only thing that reaches them is a
 * runtime-computed pointer (the address of a rel32 offset table plus an offset read through a
 * volatile). The translator therefore does not translate them; the first indirect call misses
 * in the dispatch table and is served by tier 1. hidden_fn also makes a direct call to an
 * ordinary function (tier1_callee, translated ahead of time), which a fragment reaches through
 * dispatch. Four threads race to call hidden_thr first: one compile, the others wait for it. */
#include "../src/ps5test.h"
#include <pthread.h>

__attribute__((noinline, visibility("hidden"))) uint64_t tier1_callee(uint64_t x) { return x * 5 + 1; }

__asm__(
    ".pushsection hidden_text,\"ax\",@progbits\n"
    ".balign 16\n"
    "hidden_fn:\n"              /* uint64_t hidden_fn(uint64_t x) = tier1_callee(3x) + x */
    "  push %rbx\n"
    "  mov %rdi, %rbx\n"
    "  lea (%rdi,%rdi,2), %rdi\n"
    "  call tier1_callee\n"
    "  add %rbx, %rax\n"
    "  pop %rbx\n"
    "  ret\n"
    ".balign 16\n"
    "hidden_loop:\n"            /* sum 1..x, flags and a conditional backward branch */
    "  xor %eax, %eax\n"
    "  test %rdi, %rdi\n"
    "  jz 2f\n"
    "1:\n"
    "  add %rdi, %rax\n"
    "  dec %rdi\n"
    "  jnz 1b\n"
    "2:\n"
    "  ret\n"
    ".balign 16\n"
    "hidden_thr:\n"             /* x*x + 11 */
    "  mov %rdi, %rax\n"
    "  imul %rdi, %rax\n"
    "  add $11, %rax\n"
    "  ret\n"
    ".popsection\n"
    ".pushsection .rodata.hidden_offs,\"a\",@progbits\n"
    ".balign 4\n"
    "hidden_offs:\n"
    "  .long hidden_fn - hidden_offs\n"
    "  .long hidden_loop - hidden_offs\n"
    "  .long hidden_thr - hidden_offs\n"
    ".popsection\n");

extern const int32_t hidden_offs[3] __attribute__((visibility("hidden")));
typedef uint64_t (*U64Fn)(uint64_t);

static U64Fn locate(int idx) {
  volatile int sel = idx;   /* keep the compiler from folding the address */
  return (U64Fn)((uintptr_t)hidden_offs + (intptr_t)hidden_offs[sel]);
}

/* One call site that sees a translated target, then a hidden one before and after tier 1 has
 * published it (inline caches, docs/m3/callic.md: a target that is not translated yet is never
 * cached; once it is, the site fills its slot and keeps working). */
__attribute__((noinline)) static uint64_t callp(U64Fn f, uint64_t x) { return f(x); }

static volatile int g_go;
static volatile uint64_t g_res[4];
static void *worker(void *arg) {
  long i = (long)arg;
  while (!g_go) {}
  g_res[i] = locate(2)(i + 3);
  return 0;
}

int main(void) {
  uint64_t acc = 0;
  for (int i = 0; i < 3; i++) acc += callp(tier1_callee, i);     /* 1 + 6 + 11 = 18: translated, cached */
  acc += callp(locate(2), 5);                                    /* hidden_thr: miss, served by tier 1 */
  for (int i = 0; i < 50; i++) acc += callp(locate(2), i) + callp(tier1_callee, i); /* two targets, one site */
  t_dec("callp_acc", (long long)acc);
  t_dec("hidden_fn_7", (long long)locate(0)(7));      /* 7*3*5+1 + 7 = 113 */
  t_dec("hidden_fn_again_9", (long long)locate(0)(9));/* 9*3*5+1 + 9 = 145: now a table hit */
  t_dec("hidden_loop_100", (long long)locate(1)(100));/* 5050 */
  t_dec("hidden_loop_0", (long long)locate(1)(0));
  pthread_t th[4];
  for (long i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, (void *)i);
  g_go = 1;
  for (int i = 0; i < 4; i++) pthread_join(th[i], 0);
  for (int i = 0; i < 4; i++) t_dec("thr", (long long)g_res[i]);  /* (i+3)^2 + 11 */
  t_str("done\n");
  return 0;
}
