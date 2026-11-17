// M1 exit test (SPEC §9): prints through libkernel, spawns two threads, uses a
// mutex, throws and catches a C++ exception across frames, calls setjmp/longjmp,
// prints deterministic checksums.
#include "ps5test.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdexcept>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_shared;
static uint64_t g_thread_sum[2];

struct Arg { int id; };

static void *worker(void *p) {
  Arg *a = (Arg *)p;
  uint64_t s = 1234 + a->id, local = 0;
  for (int i = 0; i < 2000; i++) {
    uint64_t v = t_lcg(&s) & 0xffff;
    local += v;
    pthread_mutex_lock(&g_mu);
    g_shared += v;
    pthread_mutex_unlock(&g_mu);
  }
  g_thread_sum[a->id] = local;
  return (void *)(uintptr_t)(local ^ 0x5a5a);
}

struct Guard {
  const char *name;
  explicit Guard(const char *n) : name(n) { t_str("  ctor "); t_str(name); t_str("\n"); }
  ~Guard() { t_str("  dtor "); t_str(name); t_str("\n"); }
};
struct MyErr { int code; };

static T_NOINLINE int thrower(int depth) {
  Guard g(depth == 0 ? "leaf" : depth == 1 ? "mid" : "top");
  if (depth == 0) throw MyErr{42};
  volatile int r = thrower(depth - 1);
  return r + 1;
}
static T_NOINLINE int stdthrower() {
  Guard g("std");
  throw std::runtime_error("boom");
}

static jmp_buf g_jb;
static T_NOINLINE void jumper(int n) {
  if (n == 0) longjmp(g_jb, 7);
  jumper(n - 1);
}

int main() {
  t_str("m1_main: start\n");

  pthread_t th[2];
  Arg args[2] = {{0}, {1}};
  for (int i = 0; i < 2; i++) pthread_create(&th[i], nullptr, worker, &args[i]);
  uint64_t rv[2];
  for (int i = 0; i < 2; i++) { void *r; pthread_join(th[i], &r); rv[i] = (uint64_t)(uintptr_t)r; }
  t_hex("thread0_sum", g_thread_sum[0]);
  t_hex("thread1_sum", g_thread_sum[1]);
  t_hex("shared_sum", g_shared);
  t_str(g_shared == g_thread_sum[0] + g_thread_sum[1] ? "mutex: consistent\n" : "mutex: BROKEN\n");
  t_hex("thread0_ret", rv[0]);

  try {
    thrower(2);
    t_str("exception: NOT thrown\n");
  } catch (const MyErr &e) {
    t_dec("caught_code", e.code);
  }
  try {
    stdthrower();
  } catch (const std::exception &e) {
    t_str("caught_what=");
    t_str(e.what());
    t_str("\n");
  }
  try {
    try { throw 17; } catch (int v) { t_dec("rethrow_inner", v); throw; }
  } catch (int v) { t_dec("rethrow_outer", v); }

  volatile int stage = 0;
  int rc = setjmp(g_jb);
  if (rc == 0) {
    stage = 1;
    jumper(25);
    t_str("longjmp: NOT taken\n");
  } else {
    t_dec("longjmp_rc", rc);
    t_dec("longjmp_stage", stage);
  }

  uint64_t h = T_FNV0;
  uint64_t s = 99;
  for (int i = 0; i < 4096; i++) { uint64_t v = t_lcg(&s); h = t_fnv(h, &v, 8); }
  t_hex("checksum", h);
  t_str("m1_main: done\n");
  return 0;
}
