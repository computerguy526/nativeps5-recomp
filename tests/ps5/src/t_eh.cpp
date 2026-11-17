// Non-local control flow beyond m1_main: deep unwinds with cleanups, catch(...) and rethrow, an
// exception thrown and caught on a secondary thread, nested setjmp buffers, longjmp(.., 0) -> 1,
// repeated setjmp in a loop (fresh generation each time), sigsetjmp, and an exception caught in
// a function that then longjmps.  Output is fully determined by the language semantics.
#include "ps5test.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdexcept>

static int g_dtors;
struct Counter { int id; explicit Counter(int i) : id(i) {} ~Counter() { g_dtors += id; } };
struct Boom { int depth; };

static T_NOINLINE int deep(int n) {
  Counter c(n);
  if (n == 0) throw Boom{99};
  volatile int r = deep(n - 1);
  return r + 1;
}

static T_NOINLINE int catch_all_rethrow(int n) {
  try { return deep(n); }
  catch (...) { g_dtors += 1000; throw; }
}

static void *thread_eh(void *) {
  try { deep(3); } catch (const Boom &b) { return (void *)(uintptr_t)(b.depth + 1); }
  return nullptr;
}

static jmp_buf g_outer, g_inner;
static sigjmp_buf g_sig;
static int g_stage;

static T_NOINLINE void leaf_jump(int n, jmp_buf &jb, int v) {
  volatile int pad[4] = {n, n, n, n};
  if (n == 0) longjmp(jb, v);
  leaf_jump(n - 1, jb, v + pad[0] * 0);
}

static T_NOINLINE int catch_then_longjmp() {
  try { throw std::runtime_error("x"); }
  catch (const std::exception &) { g_stage += 10; longjmp(g_outer, 3); }
  return 0;
}

int main() {
  t_str("t_eh: start\n");

  // deep unwind: Counter(0..5) destructors run, 15 total
  try { deep(5); t_str("deep: NOT thrown\n"); }
  catch (const Boom &b) { t_dec("deep_caught", b.depth); }
  t_dec("deep_dtors", g_dtors);

  // catch(...) + rethrow: 1000 added once, plus Counter(0..4) = 10
  g_dtors = 0;
  try { catch_all_rethrow(4); }
  catch (const Boom &b) { t_dec("rethrow_caught", b.depth); }
  t_dec("rethrow_dtors", g_dtors);

  // exception on a secondary thread
  pthread_t th;
  pthread_create(&th, nullptr, thread_eh, nullptr);
  void *r = nullptr;
  pthread_join(th, &r);
  t_dec("thread_caught", (long long)(uintptr_t)r);

  // nested setjmp: inner longjmp with 0 returns 1; outer longjmp from 6 frames down returns 5
  volatile int s1 = 0, s2 = 0;
  if (setjmp(g_outer) == 0) {
    s1 = 1;
    int rc = setjmp(g_inner);
    if (rc == 0) leaf_jump(6, g_inner, 0);
    t_dec("inner_rc", rc);
    leaf_jump(6, g_outer, 5);
    t_str("outer: NOT taken\n");
  } else {
    s2 = 1;
  }
  t_dec("s1", s1);
  t_dec("s2", s2);

  // repeated setjmp on one buffer: each iteration is a new generation
  int total = 0;
  for (int i = 1; i <= 5; i++) {
    int rc = setjmp(g_inner);
    if (rc == 0) leaf_jump(i, g_inner, i * 10);
    total += rc;
  }
  t_dec("loop_total", total);

  // sigsetjmp / siglongjmp
  int src = sigsetjmp(g_sig, 0);
  if (src == 0) { g_stage = 1; siglongjmp(g_sig, 8); }
  t_dec("sig_rc", src);
  t_dec("sig_stage", g_stage);

  // an exception caught here, then a longjmp out of the handler
  g_stage = 0;
  int jrc = setjmp(g_outer);
  if (jrc == 0) catch_then_longjmp();
  t_dec("catch_longjmp_rc", jrc);
  t_dec("catch_longjmp_stage", g_stage);

  t_str("t_eh: done\n");
  return 0;
}
