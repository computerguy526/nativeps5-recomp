// Host test of the guest mutex / condition variable HLE (pkg/runtime/kernel/sync/ps4_sync.c) on real host threads:
// owner checks (EPERM), recursion depth, ERRORCHECK / NORMAL / ADAPTIVE relocks (EDEADLK, EBUSY), condition wait release
// and re-acquire with the saved depth, timed waits, fibers sharing a guest thread id, SCE error codes, and the
// physsub world-lock helpers (ps4_sync_guest_mutex_depth / _lock) in the split-step pattern that asserted in ASTRO BOT
// (Mutex.cpp:418). Build and run with tools/mutex_unit.sh (native arm64; no guest code runs).
#include "ps4_sync.h"
#include "ps4_threading.h"
#include "nps5_libkernel_hle.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// ---- runtime stubs the sync code needs
uint64_t g_recomp_call_model;
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
uintptr_t recomp_dispatch_miss(GuestContext *c, uint64_t a, uint32_t r) { (void)c; (void)a; (void)r; abort(); }
static _Thread_local RecompThread *tl_thread;
RecompThread *recomp_get_current_thread(void) { return tl_thread; }
static _Atomic uint64_t g_bump = 0x400000;
uint64_t recomp_vm_alloc_named(GuestContext *c, size_t size, int prot, int fl, const char *n) {
  (void)c; (void)prot; (void)fl; (void)n;
  return atomic_fetch_add(&g_bump, (size + 0xfff) & ~(uint64_t)0xfff);
}

#define ARENA (256ULL << 20)
static uint8_t *g_mem;
static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                                              \
      printf(__VA_ARGS__);                                                                                             \
      printf("\n");                                                                                                    \
    }                                                                                                                  \
  } while (0)

// A guest thread: its own context and RecompThread, guest thread id tid.
typedef struct {
  GuestContext ctx;
  RecompThread t;
} GThread;
static void gthread_init(GThread *g, uint64_t tid, const char *name) {
  memset(g, 0, sizeof *g);
  g->ctx.mem_base = g_mem;
  g->ctx.mem_size = ARENA;
  g->ctx.thread_id = tid;
  g->ctx.rsp = 0x10000 + tid * 0x100; // SHIM_RETURN pops 8 bytes; nothing is read there
  g->t.thread_id = tid;
  g->t.ctx = &g->ctx;
  snprintf(g->t.name, sizeof g->t.name, "%s", name);
}
static void gthread_enter(GThread *g) { tl_thread = &g->t; }

// Guest variables (handle words) and attributes, at fixed guest addresses.
static uint64_t g_var_next = 0x1000;
static uint64_t gvar(void) { uint64_t a = g_var_next; g_var_next += 0x40; return a; }
static void *H(uint64_t g) { return g_mem + g; }

static uint64_t new_mutex(GThread *g, int type) {
  const uint64_t m = gvar(), attr = gvar();
  g->ctx.rdi = attr;
  shim_pthread_mutexattr_init(&g->ctx);
  if (type >= 0) {
    g->ctx.rdi = attr;
    g->ctx.rsi = (uint64_t)type;
    shim_pthread_mutexattr_settype(&g->ctx);
  }
  CHECK(hle_pthread_mutex_init(&g->ctx, H(m), H(attr)) == 0, "init");
  return m;
}
static int lk(GThread *g, uint64_t m) { return hle_pthread_mutex_lock(&g->ctx, H(m)); }
static int tlk(GThread *g, uint64_t m) { return hle_pthread_mutex_trylock(&g->ctx, H(m)); }
static int ulk(GThread *g, uint64_t m) { return hle_pthread_mutex_unlock(&g->ctx, H(m)); }
static uint64_t new_cond(GThread *g) {
  const uint64_t c = gvar();
  g->ctx.rdi = c;
  g->ctx.rsi = 0;
  shim_pthread_cond_init(&g->ctx);
  return c;
}
static int cwait(GThread *g, uint64_t c, uint64_t m) {
  g->ctx.rdi = c;
  g->ctx.rsi = m;
  shim_pthread_cond_wait(&g->ctx);
  return (int)g->ctx.rax;
}
static int ctimedwait_us(GThread *g, uint64_t c, uint64_t m, uint64_t us) {
  g->ctx.rdi = c;
  g->ctx.rsi = m;
  g->ctx.rdx = us;
  shim_scePthreadCondTimedwait(&g->ctx);
  return (int)g->ctx.rax;
}
static void csignal(GThread *g, uint64_t c, int all) {
  g->ctx.rdi = c;
  if (all) shim_pthread_cond_broadcast(&g->ctx);
  else shim_pthread_cond_signal(&g->ctx);
}

static GThread A, B, F;

// ---- recursion and owner checks
static void t_recursive(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2);
  CHECK(lk(&A, m) == 0 && lk(&A, m) == 0 && tlk(&A, m) == 0, "recursive lock x3");
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 3, "depth %u", ps4_sync_guest_mutex_depth(&A.ctx, m));
  CHECK(ps4_sync_guest_mutex_depth(&B.ctx, m) == 0, "depth seen by a non-owner");
  CHECK(ulk(&B, m) == EPERM, "unlock by a non-owner");
  CHECK(ulk(&A, m) == 0 && ulk(&A, m) == 0 && ulk(&A, m) == 0, "unlock x3");
  CHECK(ulk(&A, m) == EPERM, "unlock of a free mutex");
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 0, "depth after unlocks");
  // type 0 (attribute never given a type) stays recursive (documented divergence from Orbis' ERRORCHECK default)
  const uint64_t d = new_mutex(&A, -1);
  CHECK(lk(&A, d) == 0 && lk(&A, d) == 0 && ulk(&A, d) == 0 && ulk(&A, d) == 0 && ulk(&A, d) == EPERM, "type 0");
  // a statically initialised (zero) variable gets a mutex on first use
  const uint64_t z = gvar();
  CHECK(lk(&A, z) == 0 && ulk(&A, z) == 0 && ulk(&A, z) == EPERM, "zero variable");
}

static void t_errorcheck(void) {
  gthread_enter(&A);
  for (int type = 1; type <= 4; type++) {
    if (type == 2) continue;
    const uint64_t m = new_mutex(&A, type);
    CHECK(lk(&A, m) == 0, "type %d lock", type);
    CHECK(lk(&A, m) == EDEADLK, "type %d relock", type);
    CHECK(tlk(&A, m) == EBUSY, "type %d trylock by the owner", type);
    CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 1, "type %d depth", type);
    CHECK(ulk(&B, m) == EPERM, "type %d unlock by a non-owner", type);
    CHECK(ulk(&A, m) == 0, "type %d unlock", type);
    CHECK(ulk(&A, m) == EPERM, "type %d second unlock", type);
  }
  // SCE names report SCE_KERNEL_ERROR_* (0x80020000 | errno)
  const uint64_t e = new_mutex(&A, 1);
  CHECK(hle_scePthreadMutexUnlock(&A.ctx, H(e)) == (int)0x80020001, "sce unlock EPERM 0x%x",
        hle_scePthreadMutexUnlock(&A.ctx, H(e)));
  CHECK(hle_scePthreadMutexLock(&A.ctx, H(e)) == 0 && hle_scePthreadMutexLock(&A.ctx, H(e)) == (int)0x8002000b,
        "sce relock EDEADLK");
  CHECK(hle_scePthreadMutexUnlock(&A.ctx, H(e)) == 0, "sce unlock");
}

// ---- contention between two host threads
typedef struct {
  GThread *g;
  uint64_t m, c;
  _Atomic int step;
  int rc[8];
} Job;
static void *th_lock_unlock(void *p) {
  Job *j = (Job *)p;
  gthread_enter(j->g);
  j->rc[0] = tlk(j->g, j->m); // A holds it: EBUSY
  atomic_store(&j->step, 1);
  j->rc[1] = lk(j->g, j->m); // blocks until A unlocks
  atomic_store(&j->step, 2);
  j->rc[2] = ulk(j->g, j->m);
  return NULL;
}
static void t_contention(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2);
  CHECK(lk(&A, m) == 0, "A lock");
  Job j = {.g = &B, .m = m};
  pthread_t th;
  pthread_create(&th, NULL, th_lock_unlock, &j);
  while (atomic_load(&j.step) < 1) usleep(100);
  usleep(20000);
  CHECK(atomic_load(&j.step) == 1, "B got a mutex A holds");
  CHECK(ulk(&A, m) == 0, "A unlock");
  pthread_join(th, NULL);
  CHECK(j.rc[0] == EBUSY && j.rc[1] == 0 && j.rc[2] == 0, "B: trylock %d lock %d unlock %d", j.rc[0], j.rc[1], j.rc[2]);
  CHECK(ulk(&A, m) == EPERM, "A unlock after B released it");
}

// ---- condition variables: the wait releases every level of a recursive mutex and takes them all back
static void *th_cond_partner(void *p) {
  Job *j = (Job *)p;
  gthread_enter(j->g);
  while (atomic_load(&j->step) < 1) usleep(100);
  j->rc[0] = lk(j->g, j->m); // possible only while A waits
  j->rc[1] = ps4_sync_guest_mutex_depth(&j->g->ctx, j->m);
  csignal(j->g, j->c, 0);
  j->rc[2] = ulk(j->g, j->m);
  return NULL;
}
static void t_condwait(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2), c = new_cond(&A);
  CHECK(lk(&A, m) == 0 && lk(&A, m) == 0, "A lock x2");
  Job j = {.g = &B, .m = m, .c = c};
  pthread_t th;
  pthread_create(&th, NULL, th_cond_partner, &j);
  atomic_store(&j.step, 1);
  CHECK(cwait(&A, c, m) == 0, "wait");
  pthread_join(th, NULL);
  CHECK(j.rc[0] == 0 && j.rc[1] == 1 && j.rc[2] == 0, "B during the wait: lock %d depth %d unlock %d", j.rc[0], j.rc[1],
        j.rc[2]);
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 2, "depth after the wait %u", ps4_sync_guest_mutex_depth(&A.ctx, m));
  CHECK(ulk(&A, m) == 0 && ulk(&A, m) == 0 && ulk(&A, m) == EPERM, "unlocks after the wait");
  // a wait on a mutex the caller does not hold fails at once
  CHECK(cwait(&A, c, m) == EPERM, "wait without the mutex");
  // timed wait: times out, still re-acquired at full depth
  CHECK(lk(&A, m) == 0 && lk(&A, m) == 0 && lk(&A, m) == 0, "A lock x3");
  const int r = ctimedwait_us(&A, c, m, 20000);
  CHECK(r == (int)0x8002003c, "timed wait 0x%x", r);
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 3, "depth after the timeout");
  CHECK(ulk(&A, m) == 0 && ulk(&A, m) == 0 && ulk(&A, m) == 0 && ulk(&A, m) == EPERM, "unlocks after the timeout");
}

// ---- a fiber runs with the guest thread id of the thread it runs on (ps4_fiber.c fiber_adopt_thread), possibly on
// another host thread: the same guest thread id is the same owner.
static void *th_fiber_unlock(void *p) {
  Job *j = (Job *)p;
  gthread_enter(j->g);
  j->rc[0] = ps4_sync_guest_mutex_depth(&j->g->ctx, j->m);
  j->rc[1] = ulk(j->g, j->m);
  return NULL;
}
static void t_fiber(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2);
  CHECK(lk(&A, m) == 0, "A lock");
  gthread_init(&F, A.ctx.thread_id, "fiber_of_A");
  Job j = {.g = &F, .m = m};
  pthread_t th;
  pthread_create(&th, NULL, th_fiber_unlock, &j);
  pthread_join(th, NULL);
  CHECK(j.rc[0] == 1 && j.rc[1] == 0, "fiber of A: depth %d unlock %d", j.rc[0], j.rc[1]);
  CHECK(ulk(&A, m) == EPERM, "A unlock after its fiber released it");
}

// ---- the physsub split world step (ASTRO BOT PhysicsWorldImplHavok::Step unlocks the world lock its caller took):
// without the relock every further sub-step unlocks a free mutex (EPERM, the game's Mutex.cpp:418 assert); with it every
// sub-step runs under the lock and the frame ends as with one step.
static int fake_step(GThread *g, uint64_t m, int *held_during) {
  *held_during = ps4_sync_guest_mutex_depth(&g->ctx, m) > 0;
  return ulk(g, m);
}
static void t_physsub_pattern(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2);
  int held, eperm = 0, unheld = 0;
  // as before the fix
  CHECK(lk(&A, m) == 0, "caller lock");
  for (int i = 0; i < 3; i++) {
    if (fake_step(&A, m, &held) == EPERM) eperm++;
    unheld += !held;
  }
  CHECK(eperm == 2 && unheld == 2, "without relock: %d EPERM, %d sub-steps unlocked", eperm, unheld);
  // the fix: back to the entry depth before each further sub-step
  eperm = unheld = 0;
  CHECK(lk(&A, m) == 0, "caller lock");
  const uint32_t depth0 = ps4_sync_guest_mutex_depth(&A.ctx, m);
  for (int i = 0; i < 3; i++) {
    if (i > 0)
      for (uint32_t d = ps4_sync_guest_mutex_depth(&A.ctx, m); d < depth0; d++)
        CHECK(ps4_sync_guest_mutex_lock(&A.ctx, m) == 0, "relock");
    if (fake_step(&A, m, &held) == EPERM) eperm++;
    unheld += !held;
  }
  CHECK(eperm == 0 && unheld == 0, "with relock: %d EPERM, %d sub-steps unlocked", eperm, unheld);
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, m) == 0, "frame ends released, as with one step");
  // a caller that holds it twice: the step's one unlock leaves it held, nothing to re-take
  CHECK(lk(&A, m) == 0 && lk(&A, m) == 0, "lock x2");
  CHECK(fake_step(&A, m, &held) == 0 && ps4_sync_guest_mutex_depth(&A.ctx, m) == 1, "depth 2 -> 1");
  CHECK(ulk(&A, m) == 0, "unlock");
  // no mutex in the variable / no variable
  CHECK(ps4_sync_guest_mutex_depth(&A.ctx, gvar()) == 0 && ps4_sync_guest_mutex_depth(&A.ctx, 0) == 0, "no mutex");
  CHECK(ps4_sync_guest_mutex_lock(&A.ctx, 0) == EINVAL, "lock of no variable");
}

// ---- many threads: a recursive mutex as a counter lock stays exclusive
typedef struct {
  GThread g;
  uint64_t m;
  int errors;
} Stress;
static uint64_t g_counter;
static void *th_stress(void *p) {
  Stress *s = (Stress *)p;
  gthread_enter(&s->g);
  for (int i = 0; i < 20000; i++) {
    if (lk(&s->g, s->m) || lk(&s->g, s->m)) s->errors++;
    uint64_t v = g_counter;
    __asm__ __volatile__("" ::: "memory");
    g_counter = v + 1;
    if (ulk(&s->g, s->m) || ulk(&s->g, s->m)) s->errors++;
  }
  return NULL;
}
static void t_stress(void) {
  gthread_enter(&A);
  const uint64_t m = new_mutex(&A, 2);
  enum { N = 6 };
  Stress s[N];
  pthread_t th[N];
  for (int i = 0; i < N; i++) {
    gthread_init(&s[i].g, 2000 + i, "stress");
    s[i].m = m;
    s[i].errors = 0;
    pthread_create(&th[i], NULL, th_stress, &s[i]);
  }
  int errors = 0;
  for (int i = 0; i < N; i++) {
    pthread_join(th[i], NULL);
    errors += s[i].errors;
  }
  CHECK(errors == 0 && g_counter == (uint64_t)N * 20000, "stress: %d errors, counter %llu", errors,
        (unsigned long long)g_counter);
}

int main(void) {
  g_mem = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (g_mem == MAP_FAILED) return 2;
  gthread_init(&A, 1001, "A");
  gthread_init(&B, 1002, "B");
  t_recursive();
  t_errorcheck();
  t_contention();
  t_condwait();
  t_fiber();
  t_physsub_pattern();
  t_stress();
  printf(fails ? "mutex_unit FAILED (%d of %d checks)\n" : "mutex_unit OK (%d checks)\n", fails ? fails : checks, checks);
  return fails ? 1 : 0;
}
