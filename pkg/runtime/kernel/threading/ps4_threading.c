// SPDX-License-Identifier: GPL-2.0-or-later

#include "recomp_runtime.h"
#include "ps4_threading.h"
#include "nps5_thunk.h"
#include "nps5_libkernel_hle.h"
#include <inttypes.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

// scePthread* report failures as SCE kernel errors (0x80020000 | errno),
// while the POSIX shims they forward to return plain errno values.
#define SCE_PTHREAD_ERR(ctx)                                                   \
  do {                                                                         \
    uint32_t e_ = (uint32_t)(ctx)->rax;                                        \
    if (e_ != 0 && e_ < 0x100)                                                 \
      (ctx)->rax = 0x80020000ULL | e_;                                         \
  } while (0)

#define ORBIS_OK 0
#define ORBIS_KERNEL_PRIO_FIFO_LOWEST 256
#define ORBIS_KERNEL_PRIO_FIFO_HIGHEST 767

static RecompThread *g_threads = NULL;
static RecompThread *g_gc_list = NULL;
static pthread_mutex_t g_threads_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_next_tid = 1001;
static _Thread_local RecompThread *g_current_thread = NULL;

static void register_thread(RecompThread *t) {
  pthread_mutex_lock(&g_threads_mutex);
  t->next = g_threads;
  g_threads = t;
  pthread_mutex_unlock(&g_threads_mutex);
}

static void unregister_thread_locked(RecompThread *t) {
  RecompThread **curr = &g_threads;
  while (*curr) {
    if (*curr == t) {
      *curr = t->next;
      break;
    }
    curr = &(*curr)->next;
  }
}

// Garbage collection logic matching shadps4 ThreadState::ShouldCollect / TryCollect
static inline bool should_collect_locked(RecompThread *t) {
  return atomic_load_explicit(&t->refcount, memory_order_acquire) == 0 &&
         atomic_load_explicit(&t->state, memory_order_acquire) == PTHREAD_STATE_DEAD &&
         (atomic_load_explicit(&t->flags, memory_order_acquire) & THREAD_FLAG_DETACHED);
}

// Releases t->lock before returning
static void try_collect_locked(RecompThread *t) {
  if (!should_collect_locked(t)) {
    pthread_mutex_unlock(&t->lock);
    return;
  }

  pthread_mutex_lock(&g_threads_mutex);
  if (should_collect_locked(t)) {
    unregister_thread_locked(t);
    t->next = g_gc_list;
    g_gc_list = t;
  }
  pthread_mutex_unlock(&g_threads_mutex);

  pthread_mutex_unlock(&t->lock);
}

// Free resources of terminated detached threads in g_gc_list
void recomp_thread_collect(void) {
  RecompThread *reclaim_list = NULL;

  pthread_mutex_lock(&g_threads_mutex);
  RecompThread **curr = &g_gc_list;
  while (*curr) {
    RecompThread *t = *curr;
    if (atomic_load_explicit(&t->tid, memory_order_acquire) == TID_TERMINATED) {
      *curr = t->next;
      t->next = reclaim_list;
      reclaim_list = t;
    } else {
      curr = &(*curr)->next;
    }
  }
  pthread_mutex_unlock(&g_threads_mutex);

  while (reclaim_list) {
    RecompThread *t = reclaim_list;
    reclaim_list = t->next;

    if (t->ctx) {
      recomp_free_thread_context(t->ctx);
      t->ctx = NULL;
    }
    pthread_mutex_destroy(&t->lock);
    pthread_mutex_destroy(&t->join_wait_mutex);
    pthread_cond_destroy(&t->join_wait_cv);
    free(t);
  }
}

// Handle 0 finds no thread. AttrGet, Rename and Getname treat 0 as the calling thread themselves.
RecompThread *recomp_find_thread(uint64_t handle) {
  if (handle == 0) return NULL;
  pthread_mutex_lock(&g_threads_mutex);
  RecompThread *curr = g_threads;
  while (curr) {
    if ((uint64_t)curr == handle || curr->thread_id == handle) {
      pthread_mutex_unlock(&g_threads_mutex);
      return curr;
    }
    if (curr->ctx) {
      if (curr->ctx->fs_base == handle) {
        pthread_mutex_unlock(&g_threads_mutex);
        return curr;
      }
      if (curr->ctx->fs_base && RECOMP_MEM_OK(curr->ctx) &&
          curr->ctx->fs_base + 0x18ULL <= curr->ctx->mem_size) {
        uint64_t t = *(uint64_t *)(curr->ctx->mem_base + curr->ctx->fs_base + 0x10ULL);
        if (t != 0 && t == handle) {
          pthread_mutex_unlock(&g_threads_mutex);
          return curr;
        }
      }
    }
    curr = curr->next;
  }
  pthread_mutex_unlock(&g_threads_mutex);
  return NULL;
}

RecompThread *recomp_get_current_thread(void) {
  return g_current_thread;
}

void recomp_set_current_thread(RecompThread *t) {
  g_current_thread = t;
}

static inline RecompThread *find_thread(uint64_t handle) {
  return recomp_find_thread(handle);
}

const char *recomp_get_current_thread_name(void) {
  if (g_current_thread && g_current_thread->name[0]) {
    return g_current_thread->name;
  }
  return "main";
}

static RecompThread g_main_thread_obj = {0};

void recomp_init_main_thread(GuestContext *ctx) {
  if (ctx->thread_id == 0) ctx->thread_id = 1000;
  g_main_thread_obj.magic = THREAD_MAGIC;
  atomic_store_explicit(&g_main_thread_obj.tid, (int)ctx->thread_id, memory_order_relaxed);
  atomic_store_explicit(&g_main_thread_obj.refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&g_main_thread_obj.state, PTHREAD_STATE_RUNNING, memory_order_relaxed);
  atomic_store_explicit(&g_main_thread_obj.flags, 0, memory_order_relaxed);
  pthread_mutex_init(&g_main_thread_obj.lock, NULL);
  pthread_mutex_init(&g_main_thread_obj.join_wait_mutex, NULL);
  pthread_cond_init(&g_main_thread_obj.join_wait_cv, NULL);
  g_main_thread_obj.host_thread = pthread_self();
  g_main_thread_obj.thread_id = ctx->thread_id;
  g_main_thread_obj.ctx = ctx;
  g_main_thread_obj.stack_base = ctx->stack_base;
  g_main_thread_obj.stack_size = ctx->stack_alloc_size;
  atomic_store_explicit(&g_main_thread_obj.start_ready, 1, memory_order_relaxed);
  strncpy(g_main_thread_obj.name, "main", sizeof(g_main_thread_obj.name) - 1);
  if (ctx->fs_base && RECOMP_MEM_OK(ctx)) {
    *(uint64_t *)(ctx->mem_base + ctx->fs_base + 0x10ULL) = (uint64_t)&g_main_thread_obj;
  }
  g_current_thread = &g_main_thread_obj;
  register_thread(&g_main_thread_obj);
}

RecompThread *recomp_init_system_thread(GuestContext *ctx, const char *name) {
  if (!ctx) return NULL;
  RecompThread *t = (RecompThread *)calloc(1, sizeof(RecompThread));
  if (!t) return NULL;

  int tid = atomic_fetch_add(&g_next_tid, 1);
  ctx->thread_id = (uint64_t)tid;
  t->magic = THREAD_MAGIC;
  atomic_store_explicit(&t->tid, tid, memory_order_relaxed);
  atomic_store_explicit(&t->refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&t->state, PTHREAD_STATE_RUNNING, memory_order_relaxed);
  atomic_store_explicit(&t->flags, THREAD_FLAG_DETACHED, memory_order_relaxed);
  t->detached = true;

  pthread_mutex_init(&t->lock, NULL);
  pthread_mutex_init(&t->join_wait_mutex, NULL);
  pthread_cond_init(&t->join_wait_cv, NULL);

  t->host_thread = pthread_self();
  t->thread_id = (uint64_t)tid;
  t->ctx = ctx;
  t->stack_base = ctx->stack_base;
  t->stack_size = ctx->stack_alloc_size;
  atomic_store_explicit(&t->start_ready, 1, memory_order_relaxed);
  if (name && name[0]) {
    strncpy(t->name, name, sizeof(t->name) - 1);
  } else {
    strncpy(t->name, "system_worker", sizeof(t->name) - 1);
  }

  if (ctx->fs_base && RECOMP_MEM_OK(ctx)) {
    *(uint64_t *)(ctx->mem_base + ctx->fs_base + 0x10ULL) = (uint64_t)t;
  }
  g_current_ctx = ctx;
  g_current_thread = t;
  register_thread(t);
  return t;
}

void recomp_cleanup_system_thread(RecompThread *t) {
  if (!t) return;
  pthread_mutex_lock(&t->lock);
  atomic_store_explicit(&t->state, PTHREAD_STATE_DEAD, memory_order_release);
  atomic_store_explicit(&t->refcount, 0, memory_order_relaxed);
  try_collect_locked(t);
  if (g_current_thread == t) {
    g_current_thread = NULL;
  }
}

// Exit routine matching shadps4 ExitThread
static void recomp_thread_exit(RecompThread *t, void *status) {
  GuestContext *ctx = t->ctx;

  // Run registered pthread TLS key destructors
  if (ctx) {
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    for (int iter = 0; iter < 4; iter++) {
      bool has_active = false;
      for (uint32_t k = 0; k < NPS5_TLS_KEYS_MAX; k++) {
        uint64_t val = *nps5_tls_val(ctx, k);
        uint64_t dtor = *nps5_tls_dtor(proc, k);
        if (val != 0 && dtor != 0) {
          *nps5_tls_val(ctx, k) = 0;
          has_active = true;
          ctx->rdi = val;
          ctx->rsp -= 8;
          MEM_U64(ctx->rsp) = 0xdeadbeefULL;
          ctx->rip = dtor;
          recomp_dispatch(ctx, dtor);
        }
      }
      if (!has_active) break;
    }
  }

  // Run cleanup handlers
  while (t->cleanup_stack) {
    PthreadCleanupEntry *c = t->cleanup_stack;
    t->cleanup_stack = c->next;
    if (c->routine && ctx) {
      ctx->rdi = c->arg;
      ctx->rsp -= 8;
      MEM_U64(ctx->rsp) = 0xdeadbeefULL;
      ctx->rip = c->routine;
      recomp_dispatch(ctx, c->routine);
    }
    if (c->onheap) free(c);
  }

  t->ret_val = (uint64_t)status;

  pthread_mutex_lock(&t->lock);
  atomic_store_explicit(&t->state, PTHREAD_STATE_DEAD, memory_order_release);
  t->finished = true;
  atomic_fetch_sub_explicit(&t->refcount, 1, memory_order_acq_rel);

  // If detached and refcount reached 0, queue for collection
  try_collect_locked(t); // unlocks t->lock

  // Wake up any joiner waiting on join_wait_cv
  pthread_mutex_lock(&t->join_wait_mutex);
  atomic_store_explicit(&t->tid, TID_TERMINATED, memory_order_release);
  pthread_cond_broadcast(&t->join_wait_cv);
  pthread_mutex_unlock(&t->join_wait_mutex);

  pthread_exit(NULL);
}

#if defined(__APPLE__)
static void guest_worker_apply_qos(const char *name) {
  static const char kPrefix[] = "NdJobWorkerThread";
  if (strncmp(name, kPrefix, sizeof kPrefix - 1) != 0) return;
  const char *e = getenv("PS4_GUEST_WORKER_QOS");
  if (!e || !e[0]) e = "initiated";
  if (strcmp(e, "default") == 0) return;
  qos_class_t cls;
  if (strcmp(e, "utility") == 0) cls = QOS_CLASS_UTILITY;
  else if (strcmp(e, "initiated") == 0) cls = QOS_CLASS_USER_INITIATED;
  else if (strcmp(e, "interactive") == 0) cls = QOS_CLASS_USER_INTERACTIVE;
  else return;
  pthread_set_qos_class_self_np(cls, 0);
}
#endif

// QoS of the host threads that run guest code (docs/m3/perf.md). A thread without a QoS class has the
// scheduler priority of any command-line job, so a build running next to the title takes its cores (and the
// efficiency cores take the title): Syobon dropped from its 30 fps to 18 that way. The guest's main thread is
// user-interactive (as a native game's render thread is), its other threads user-initiated.
// NPS5_GUEST_QOS=default|utility|initiated|interactive sets one class for all of them (default: leave them alone).
void nps5_guest_thread_qos(int is_main) {
#if defined(__APPLE__)
  qos_class_t cls = is_main ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED;
  const char *e = getenv("NPS5_GUEST_QOS");
  if (e && e[0]) {
    if (strcmp(e, "default") == 0) return;
    else if (strcmp(e, "utility") == 0) cls = QOS_CLASS_UTILITY;
    else if (strcmp(e, "initiated") == 0) cls = QOS_CLASS_USER_INITIATED;
    else if (strcmp(e, "interactive") == 0) cls = QOS_CLASS_USER_INTERACTIVE;
  }
  pthread_set_qos_class_self_np(cls, 0);
#else
  (void)is_main;
#endif
}

static void *recomp_host_thread_runner(void *arg) {
  RecompThread *t = (RecompThread *)arg;
  nps5_guest_thread_qos(0);

  while (!atomic_load_explicit(&t->start_ready, memory_order_acquire)) {
    sched_yield();
  }
  atomic_thread_fence(memory_order_seq_cst);

  GuestContext *ctx = t->ctx;
  g_current_ctx = ctx;
  g_current_thread = t;

  if (t->name[0] != '\0') {
#if defined(__APPLE__)
    pthread_setname_np(t->name);
    guest_worker_apply_qos(t->name);
#elif defined(__linux__)
    pthread_setname_np(t->host_thread, t->name);
#endif
  }

  // Push dummy return address on guest stack
  ctx->rsp -= 8;
  MEM_U64(ctx->rsp) = 0xdeadbeefULL;

  // First parameter in AMD64 calling convention is RDI
  ctx->rdi = t->arg;
  ctx->rip = t->start_routine;

  recomp_dispatch(ctx, t->start_routine);

  recomp_thread_exit(t, (void *)ctx->rax);
  return NULL;
}

// Join implementation matching shadps4 JoinThread: waits on join_wait_cv
static int do_thread_join(RecompThread *t, void **thread_return) {
  if (!t) return ESRCH;
  if (t == g_current_thread) return EDEADLK;

  pthread_mutex_lock(&t->lock);
  if (atomic_load_explicit(&t->flags, memory_order_acquire) & THREAD_FLAG_DETACHED) {
    pthread_mutex_unlock(&t->lock);
    return EINVAL;
  }
  if (t->joiner != NULL) {
    pthread_mutex_unlock(&t->lock);
    return ENOTSUP;
  }
  t->joiner = g_current_thread;
  if (g_current_thread) {
    g_current_thread->join_target = t;
  }
  pthread_mutex_unlock(&t->lock);

  // Wait for the target thread to signal termination via join_wait_cv
  pthread_mutex_lock(&t->join_wait_mutex);
  while (atomic_load_explicit(&t->tid, memory_order_acquire) != TID_TERMINATED) {
    pthread_cond_wait(&t->join_wait_cv, &t->join_wait_mutex);
  }
  pthread_mutex_unlock(&t->join_wait_mutex);

  if (g_current_thread) {
    g_current_thread->join_target = NULL;
  }

  void *ret_val = (void *)t->ret_val;

  pthread_join(t->host_thread, NULL);

  pthread_mutex_lock(&t->lock);
  atomic_fetch_or_explicit(&t->flags, THREAD_FLAG_DETACHED, memory_order_release);
  t->joined = true;
  t->detached = true;
  t->joiner = NULL;
  try_collect_locked(t); // unlocks t->lock

  if (thread_return != NULL) {
    *thread_return = ret_val;
  }

  recomp_thread_collect();
  return 0;
}

// Detach implementation matching shadps4 posix_pthread_detach
static int do_thread_detach(RecompThread *t) {
  if (!t) return EINVAL;

  pthread_mutex_lock(&t->lock);
  if ((atomic_load_explicit(&t->flags, memory_order_acquire) & THREAD_FLAG_DETACHED) ||
      t->joiner != NULL) {
    pthread_mutex_unlock(&t->lock);
    return EINVAL;
  }

  pthread_detach(t->host_thread);

  atomic_fetch_or_explicit(&t->flags, THREAD_FLAG_DETACHED, memory_order_release);
  t->detached = true;
  try_collect_locked(t); // unlocks t->lock

  recomp_thread_collect();
  return 0;
}

static int do_create_thread(GuestContext *ctx, uint64_t thread_ptr_addr, const PthreadAttr *attr,
                            uint64_t start_routine, uint64_t arg, const char *name) {
  recomp_thread_collect();

  RecompThread *t = (RecompThread *)calloc(1, sizeof(RecompThread));
  if (!t) {
    return ENOMEM;
  }

  t->magic = THREAD_MAGIC;
  int tid = atomic_fetch_add(&g_next_tid, 1);
  atomic_store_explicit(&t->tid, tid, memory_order_relaxed);
  atomic_store_explicit(&t->refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&t->state, PTHREAD_STATE_RUNNING, memory_order_relaxed);

  pthread_mutex_init(&t->lock, NULL);
  pthread_mutex_init(&t->join_wait_mutex, NULL);
  pthread_cond_init(&t->join_wait_cv, NULL);

  t->thread_id = (uint64_t)tid;
  t->start_routine = start_routine;
  t->arg = arg;

  uint64_t stack_size = 2 * 1024 * 1024;
  if (attr) {
    t->attr = *attr;
    if (attr->stacksize_attr >= 16384) {
      stack_size = attr->stacksize_attr;
    }
    if (attr->flags & PTHREAD_ATTR_DETACHED) {
      atomic_fetch_or_explicit(&t->flags, THREAD_FLAG_DETACHED, memory_order_relaxed);
      t->detached = true;
    }
  }

  if (name && name[0]) {
    strncpy(t->name, name, sizeof(t->name) - 1);
  }

  GuestContext *child_ctx = recomp_create_thread_context(ctx, stack_size);
  if (!child_ctx) {
    pthread_mutex_destroy(&t->lock);
    pthread_mutex_destroy(&t->join_wait_mutex);
    pthread_cond_destroy(&t->join_wait_cv);
    free(t);
    return ENOMEM;
  }

  child_ctx->thread_id = t->thread_id;
  t->ctx = child_ctx;
  t->stack_base = child_ctx->stack_base;
  t->stack_size = child_ctx->stack_alloc_size;

  if (child_ctx->fs_base && RECOMP_MEM_OK(child_ctx)) {
    *(uint64_t *)(child_ctx->mem_base + child_ctx->fs_base + 0x10ULL) = (uint64_t)t;
  }

  register_thread(t);

  if (thread_ptr_addr) {
    *(uint64_t *)(ctx->mem_base + thread_ptr_addr) = (uint64_t)t;
  }

  int ret = pthread_create(&t->host_thread, NULL, recomp_host_thread_runner, t);
  if (ret != 0) {
    pthread_mutex_lock(&t->lock);
    atomic_store_explicit(&t->refcount, 0, memory_order_relaxed);
    atomic_store_explicit(&t->state, PTHREAD_STATE_DEAD, memory_order_relaxed);
    try_collect_locked(t);
    recomp_free_thread_context(child_ctx);
    return ret;
  }

  if (t->detached) {
    pthread_detach(t->host_thread);
  }

  atomic_thread_fence(memory_order_seq_cst);
  atomic_store_explicit(&t->start_ready, 1, memory_order_release);
  return 0;
}

// Thread creation behind generated thunks. thread: where the new handle is stored; attr: the
// guest's pthread_attr_t word, which holds a host PthreadAttr pointer the attr functions put
// there; entry and arg stay guest values and are handed to the new guest thread.
static int create_thread_hle(GuestContext *ctx, uint64_t *thread, const uint64_t *attr, uint64_t entry, uint64_t arg,
                             const char *name) {
  PthreadAttr attr_val;
  PthreadAttr *p_attr = NULL;
  if (attr && *attr) {
    memcpy(&attr_val, (void *)(uintptr_t)*attr, sizeof(PthreadAttr));
    p_attr = &attr_val;
  }
  int err = do_create_thread(ctx, NPS5_H2G(ctx, thread), p_attr, entry, arg, name);
  if (err != 0) set_guest_errno(ctx, err);
  return err;
}

int hle_pthread_create(GuestContext *ctx, uint64_t *thread, const uint64_t *attr, uint64_t entry, uint64_t arg) {
  return create_thread_hle(ctx, thread, attr, entry, arg, NULL);
}

int hle_pthread_join(GuestContext *ctx, uint64_t thread, uint64_t *retval) {
  RecompThread *t = find_thread(thread);
  void *value = NULL;
  int err = do_thread_join(t, retval ? &value : NULL);
  if (err != 0) {
    set_guest_errno(ctx, err);
    return err;
  }
  if (retval) *retval = (uint64_t)(uintptr_t)value; // the guest thread's return value, a guest word
  return 0;
}

void shim_pthread_detach(GuestContext *ctx) {
  uint64_t handle = ctx->rdi;
  RecompThread *t = find_thread(handle);
  int err = do_thread_detach(t);
  if (err != 0) {
    set_guest_errno(ctx, err);
    ctx->rax = (uint64_t)err;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

void shim_pthread_self(GuestContext *ctx) {
  if (ctx && ctx->fs_base) {
    uint64_t t = MEM_U64(ctx->fs_base + 0x10);
    ctx->rax = (t != 0) ? t : ctx->fs_base;
  } else if (g_current_thread) {
    ctx->rax = (uint64_t)g_current_thread;
  } else {
    ctx->rax = 0x1000ULL;
  }
  SHIM_RETURN();
}

void shim_pthread_equal(GuestContext *ctx) {
  uint64_t t1 = ctx->rdi;
  uint64_t t2 = ctx->rsi;
  ctx->rax = (t1 == t2) ? 1 : 0;
  SHIM_RETURN();
}

static uint32_t g_next_key = 1;
static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;

void shim_pthread_key_create(GuestContext *ctx) {
  uint64_t key_ptr_addr = ctx->rdi;
  uint64_t destructor_addr = ctx->rsi;
  pthread_mutex_lock(&g_key_lock);
  uint32_t key = g_next_key < NPS5_TLS_KEYS_MAX ? g_next_key++ : NPS5_TLS_KEYS_MAX;
  pthread_mutex_unlock(&g_key_lock);
  if (key >= NPS5_TLS_KEYS_MAX) { // FreeBSD PTHREAD_KEYS_MAX = 256: EAGAIN, as on the console
    ctx->rax = EAGAIN; // 35 on FreeBSD and macOS
    SHIM_RETURN();
  }
  if (key_ptr_addr) {
    *(uint32_t *)(ctx->mem_base + key_ptr_addr) = key;
  }
  {
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    *nps5_tls_dtor(proc, key) = destructor_addr;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_key_delete(GuestContext *ctx) {
  uint32_t key = (uint32_t)ctx->rdi;
  if (key < NPS5_TLS_KEYS_MAX) {
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    *nps5_tls_dtor(proc, key) = 0;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_setspecific(GuestContext *ctx) {
  uint32_t key = (uint32_t)ctx->rdi;
  uint64_t val = ctx->rsi;
  if (key < NPS5_TLS_KEYS_MAX) {
    *nps5_tls_val(ctx, key) = val;
    ctx->rax = 0;
  } else {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)EINVAL;
  }
  SHIM_RETURN();
}

void shim_pthread_getspecific(GuestContext *ctx) {
  uint32_t key = (uint32_t)ctx->rdi;
  if (key < NPS5_TLS_KEYS_MAX) {
    ctx->rax = *nps5_tls_val(ctx, key);
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

void shim_pthread_attr_init(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  if (!attr_addr) {
    ctx->rax = (uint64_t)EINVAL;
    SHIM_RETURN();
  }
  PthreadAttr *attr = (PthreadAttr *)calloc(1, sizeof(PthreadAttr));
  if (!attr) {
    ctx->rax = (uint64_t)ENOMEM;
    SHIM_RETURN();
  }
  attr->stacksize_attr = 2 * 1024 * 1024;
  attr->sched_policy = 2; // Other
  attr->prio = 700;
  *(uint64_t *)(ctx->mem_base + attr_addr) = (uint64_t)attr;
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_attr_destroy(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  if (attr_addr) {
    PthreadAttr *attr = (PthreadAttr *)(*(uint64_t *)(ctx->mem_base + attr_addr));
    if (attr) free(attr);
    *(uint64_t *)(ctx->mem_base + attr_addr) = 0;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_attr_setdetachstate(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  int detachstate = (int)ctx->rsi;
  if (!attr_addr) {
    ctx->rax = (uint64_t)EINVAL;
    SHIM_RETURN();
  }
  PthreadAttr *attr = (PthreadAttr *)(*(uint64_t *)(ctx->mem_base + attr_addr));
  if (!attr) {
    ctx->rax = (uint64_t)EINVAL;
    SHIM_RETURN();
  }
  if (detachstate == 1) { // PTHREAD_CREATE_DETACHED
    attr->flags |= PTHREAD_ATTR_DETACHED;
  } else {
    attr->flags &= ~PTHREAD_ATTR_DETACHED;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_attr_setstacksize(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  size_t stacksize = (size_t)ctx->rsi;
  if (!attr_addr) {
    ctx->rax = (uint64_t)EINVAL;
    SHIM_RETURN();
  }
  PthreadAttr *attr = (PthreadAttr *)(*(uint64_t *)(ctx->mem_base + attr_addr));
  if (!attr) {
    ctx->rax = (uint64_t)EINVAL;
    SHIM_RETURN();
  }
  attr->stacksize_attr = stacksize;
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_getschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_setschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_setcanceltype(GuestContext *ctx) {
  uint64_t oldtype_addr = ctx->rsi;
  if (oldtype_addr) {
    *(int32_t *)(ctx->mem_base + oldtype_addr) = 0;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_setcancelstate(GuestContext *ctx) {
  int state = (int)ctx->rdi;
  uint64_t oldstate_addr = ctx->rsi;
  RecompThread *t = g_current_thread;
  if (oldstate_addr && t) {
    *(int32_t *)(ctx->mem_base + oldstate_addr) = t->cancel_state;
  }
  if (t) t->cancel_state = state;
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim___pthread_cleanup_push_imp(GuestContext *ctx) {
  uint64_t routine = ctx->rdi;
  uint64_t arg = ctx->rsi;
  RecompThread *t = g_current_thread;
  if (t) {
    PthreadCleanupEntry *c = (PthreadCleanupEntry *)calloc(1, sizeof(PthreadCleanupEntry));
    if (c) {
      c->routine = routine;
      c->arg = arg;
      c->onheap = true;
      c->next = t->cleanup_stack;
      t->cleanup_stack = c;
    }
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim___pthread_cleanup_pop_imp(GuestContext *ctx) {
  int execute = (int)ctx->rdi;
  RecompThread *t = g_current_thread;
  if (t && t->cleanup_stack) {
    PthreadCleanupEntry *c = t->cleanup_stack;
    t->cleanup_stack = c->next;
    if (execute && c->routine) {
      ctx->rdi = c->arg;
      ctx->rsp -= 8;
      MEM_U64(ctx->rsp) = 0xdeadbeefULL;
      ctx->rip = c->routine;
      recomp_dispatch(ctx, c->routine);
    }
    if (c->onheap) free(c);
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_cleanup_push(GuestContext *ctx) {
  shim___pthread_cleanup_push_imp(ctx);
}

void shim_pthread_cleanup_pop(GuestContext *ctx) {
  shim___pthread_cleanup_pop_imp(ctx);
}

void shim_sched_get_priority_max(GuestContext *ctx) {
  ctx->rax = ORBIS_KERNEL_PRIO_FIFO_HIGHEST;
  SHIM_RETURN();
}

void shim_sched_get_priority_min(GuestContext *ctx) {
  ctx->rax = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
  SHIM_RETURN();
}

void shim_pthread_sigmask(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_pthread_cancel(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

// Orbis shims forwarding to core implementations
int hle_scePthreadCreate(GuestContext *ctx, uint64_t *thread, const uint64_t *attr, uint64_t entry, uint64_t arg,
                         const char *name) {
  int err = create_thread_hle(ctx, thread, attr, entry, arg, name);
  return err ? (int)(0x80020000u | (unsigned)err) : 0;
}

int hle_scePthreadJoin(GuestContext *ctx, uint64_t thread, uint64_t *retval) {
  int e = hle_pthread_join(ctx, thread, retval);
  return (e > 0 && e < 0x100) ? (int)(0x80020000u | (unsigned)e) : e;
}

void shim_scePthreadDetach(GuestContext *ctx) {
  shim_pthread_detach(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadExit(GuestContext *ctx) {
  RecompThread *t = g_current_thread;
  if (t && t != &g_main_thread_obj) {
    recomp_thread_exit(t, (void *)ctx->rdi);
  }
  pthread_exit(NULL);
}

void shim_scePthreadSelf(GuestContext *ctx) {
  shim_pthread_self(ctx);
}

void shim_scePthreadEqual(GuestContext *ctx) {
  shim_pthread_equal(ctx);
}

// ---- adaptive yield back-off (core/nps5_yield.h). NPS5_YIELD_BACKOFF_US=<cap us> (default 50; 0 = plain sched_yield),
// NPS5_YIELD_BACKOFF_MIN_US (default 2), NPS5_YIELD_SPIN_GAP_US (default 20), NPS5_YIELD_BACKOFF_AFTER (default 128).
// NPS5_YIELD_STATS=1: per-thread yield counts to stderr every 5 s and at exit.
#include "nps5_yield.h"
#include "nps5_sleep.h"
#include <stdatomic.h>
#define NPS5_YIELD_SLOTS 128
static nps5_yield_cfg g_yc = {50000, 2000, 20000, 128}; // default ON, 50 us cap (measured: Astro level)
static int g_ystats;
static pthread_once_t g_yonce = PTHREAD_ONCE_INIT;
static _Atomic uint32_t g_yslots;
static _Atomic uint64_t g_ynext_dump;
static struct { pthread_t th; _Atomic uint64_t calls, waits, wait_ns; } g_ys[NPS5_YIELD_SLOTS];
static __thread nps5_yield_state t_ys;
static __thread int t_yslot = -1;
static void nps5_yield_dump(void) {
  uint32_t n = atomic_load(&g_yslots);
  if (n > NPS5_YIELD_SLOTS) n = NPS5_YIELD_SLOTS;
  for (uint32_t i = 0; i < n; i++) {
    char nm[64] = "?";
    pthread_getname_np(g_ys[i].th, nm, sizeof nm);
    uint64_t c = atomic_load(&g_ys[i].calls);
    if (c) fprintf(stderr, "[yield] slot=%u name=%s calls=%llu waits=%llu wait_ms=%.1f\n", i, nm, (unsigned long long)c, (unsigned long long)atomic_load(&g_ys[i].waits), (double)atomic_load(&g_ys[i].wait_ns) / 1e6);
  }
}
static void nps5_yield_init(void) {
  const char *v = getenv("NPS5_YIELD_BACKOFF_US");
  if (v) g_yc.cap_ns = atoi(v) > 0 ? (uint64_t)atoi(v) * 1000ull : 0;
  v = getenv("NPS5_YIELD_BACKOFF_MIN_US");
  if (v && atoi(v) > 0) g_yc.min_ns = (uint64_t)atoi(v) * 1000ull;
  v = getenv("NPS5_YIELD_SPIN_GAP_US");
  if (v && atoi(v) > 0) g_yc.gap_ns = (uint64_t)atoi(v) * 1000ull;
  v = getenv("NPS5_YIELD_BACKOFF_AFTER");
  if (v && atoi(v) > 0) g_yc.after = (uint32_t)atoi(v);
  v = getenv("NPS5_YIELD_STATS");
  g_ystats = v && atoi(v) > 0;
  if (g_ystats) atexit(nps5_yield_dump);
}
// Shared by scePthreadYield and sched_yield.
void nps5_yield_adaptive(void) {
  pthread_once(&g_yonce, nps5_yield_init);
  if (!g_ystats && g_yc.cap_ns == 0) { sched_yield(); return; }
  uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t w = nps5_yield_step(&g_yc, &t_ys, now);
  if (g_ystats) {
    if (t_yslot < 0) {
      uint32_t i = atomic_fetch_add(&g_yslots, 1);
      t_yslot = i < NPS5_YIELD_SLOTS ? (int)i : NPS5_YIELD_SLOTS - 1;
      g_ys[t_yslot].th = pthread_self();
    }
    atomic_fetch_add(&g_ys[t_yslot].calls, 1);
    if (w) { atomic_fetch_add(&g_ys[t_yslot].waits, 1); atomic_fetch_add(&g_ys[t_yslot].wait_ns, w); }
    uint64_t nd = atomic_load(&g_ynext_dump);
    if (now >= nd && atomic_compare_exchange_strong(&g_ynext_dump, &nd, now + 5000000000ull)) {
      if (nd) nps5_yield_dump();
    }
  }
  if (w) {
    nps5_sleep_ns(w, NULL);
    t_ys.last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  } else {
    sched_yield();
    t_ys.last_ns = now;
  }
}

void shim_scePthreadYield(GuestContext *ctx) {
  nps5_yield_adaptive();
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadGetthreadid(GuestContext *ctx) {
  if (g_current_thread) {
    ctx->rax = (uint64_t)g_current_thread->thread_id;
  } else {
    ctx->rax = ctx->thread_id ? ctx->thread_id : 1000;
  }
  SHIM_RETURN();
}

void shim_scePthreadSetprio(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadGetprio(GuestContext *ctx) {
  uint64_t prio_ptr = ctx->rsi;
  if (prio_ptr) {
    *(int32_t *)(ctx->mem_base + prio_ptr) = 700;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSetaffinity(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadGetaffinity(GuestContext *ctx) {
  uint64_t mask_ptr = ctx->rsi;
  if (mask_ptr) {
    *(uint64_t *)(ctx->mem_base + mask_ptr) = 0x3f;
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrInit(GuestContext *ctx) {
  shim_pthread_attr_init(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadAttrDestroy(GuestContext *ctx) {
  shim_pthread_attr_destroy(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadAttrSetstacksize(GuestContext *ctx) {
  shim_pthread_attr_setstacksize(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadAttrSetdetachstate(GuestContext *ctx) {
  shim_pthread_attr_setdetachstate(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadAttrSetschedpolicy(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrSetschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrGetschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrSetinheritsched(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrSetaffinity(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSetcancelstate(GuestContext *ctx) {
  shim_pthread_setcancelstate(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadAttrGetstacksize(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  uint64_t size_ptr = ctx->rsi;
  if (attr_addr && size_ptr) {
    PthreadAttr *attr = (PthreadAttr *)(*(uint64_t *)(ctx->mem_base + attr_addr));
    if (attr) {
      *(uint64_t *)(ctx->mem_base + size_ptr) = attr->stacksize_attr;
    }
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrGetstack(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrGetstackaddr(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrSetstack(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadRename(GuestContext *ctx) {
  uint64_t thread_handle = ctx->rdi;
  uint64_t name_addr = ctx->rsi;
  RecompThread *t = thread_handle ? find_thread(thread_handle) : g_current_thread;
  if (t && name_addr && name_addr < ctx->mem_size) {
    const char *name = (const char *)(ctx->mem_base + name_addr);
    strncpy(t->name, name, sizeof(t->name) - 1);
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadGetschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadSetschedparam(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadKeyCreate(GuestContext *ctx) {
  shim_pthread_key_create(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadKeyDelete(GuestContext *ctx) {
  shim_pthread_key_delete(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadSetspecific(GuestContext *ctx) {
  shim_pthread_setspecific(ctx);
  SCE_PTHREAD_ERR(ctx);
}

void shim_scePthreadGetspecific(GuestContext *ctx) {
  shim_pthread_getspecific(ctx);
}

// __tls_get_addr(tls_index *ti): the address of a TLS variable of the calling thread, as a
// host pointer (the thunk hands the guest its guest address).
void *hle___tls_get_addr(GuestContext *ctx, const uint64_t *ti) {
  if (!ti) return NULL;
  uint64_t module_id = ti[0];
  uint64_t offset = ti[1];
  uint64_t mod_off = recomp_tls_module_offset(module_id);
  uint64_t g = mod_off ? ctx->fs_base - mod_off + offset : ctx->fs_base + offset;
  return NPS5_G2H(ctx, g);
}

void shim_scePthreadAttrGetaffinity(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrGetdetachstate(GuestContext *ctx) {
  uint64_t attr_addr = ctx->rdi;
  uint64_t state_ptr = ctx->rsi;
  if (attr_addr && state_ptr) {
    PthreadAttr *attr = (PthreadAttr *)(*(uint64_t *)(ctx->mem_base + attr_addr));
    if (attr) {
      *(int32_t *)(ctx->mem_base + state_ptr) = (attr->flags & PTHREAD_ATTR_DETACHED) ? 1 : 0;
    }
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadAttrGet(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_scePthreadGetname(GuestContext *ctx) {
  uint64_t thread_handle = ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  RecompThread *t = thread_handle ? find_thread(thread_handle) : g_current_thread;
  if (t && buf_addr && buf_addr < ctx->mem_size) {
    strncpy((char *)(ctx->mem_base + buf_addr), t->name[0] ? t->name : "unnamed", 32);
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceKernelIsStack(GuestContext *ctx) {
  uint64_t addr = ctx->rdi;
  RecompThread *curr = g_threads;
  pthread_mutex_lock(&g_threads_mutex);
  while (curr) {
    if (curr->stack_base && addr >= curr->stack_base &&
        addr < curr->stack_base + curr->stack_size) {
      pthread_mutex_unlock(&g_threads_mutex);
      ctx->rax = 1;
      SHIM_RETURN();
    }
    curr = curr->next;
  }
  pthread_mutex_unlock(&g_threads_mutex);
  ctx->rax = 0;
  SHIM_RETURN();
}

#include "ps4_stall_profile.h"
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>

static StallProfile *g_stall_prof;
static uint64_t g_prof_for;
static uint64_t g_prof_slot_ids[STALL_PROFILE_MAX_THREADS];
static int g_prof_nslots;

static void stall_profile_begin(void) {
  stall_profile_reset(g_stall_prof);
  g_prof_nslots = 0;
  stall_profile_cpu_begin(g_stall_prof);
}

static int stall_profile_slot(uint64_t thread_id) {
  for (int i = 0; i < g_prof_nslots; i++)
    if (g_prof_slot_ids[i] == thread_id) return i;
  if (g_prof_nslots == STALL_PROFILE_MAX_THREADS) return -1;
  g_prof_slot_ids[g_prof_nslots] = thread_id;
  return g_prof_nslots++;
}

#define STALL_SYM_CACHE 16384
static struct {
  uint64_t pc, start;
  const char *name;
} g_stall_sym_cache[STALL_SYM_CACHE];

static int stall_symbolize(uint64_t pc, uint64_t *start, const char **name) {
  const uint64_t h = (pc * 0x9E3779B97F4A7C15ull) >> 50;
  if (g_stall_sym_cache[h].pc != pc) {
    Dl_info di;
    const int ok = dladdr((void *)pc, &di) && di.dli_sname;
    g_stall_sym_cache[h].pc = pc;
    g_stall_sym_cache[h].start = ok ? (uint64_t)(uintptr_t)di.dli_saddr : 0;
    g_stall_sym_cache[h].name = ok ? di.dli_sname : NULL;
  }
  if (!g_stall_sym_cache[h].name) return 0;
  *start = g_stall_sym_cache[h].start;
  *name = g_stall_sym_cache[h].name;
  return 1;
}

static int g_prof_no_chain_noted;

static void stall_profile_sample_all(void) {
  int n = 0, in_guest_code = 0;
  pthread_mutex_lock(&g_threads_mutex);
  for (RecompThread *t = g_threads; t && n < STALL_PROFILE_MAX_THREADS; t = t->next) {
    if (t->finished || !t->ctx) continue;
    n++;
    const mach_port_t port = pthread_mach_thread_np(t->host_thread);
    if (port == MACH_PORT_NULL || thread_suspend(port) != KERN_SUCCESS) continue;
    uint64_t pcs[STALL_PROFILE_DEPTH];
    int np = 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
      pcs[np++] = (uint64_t)arm_thread_state64_get_pc(st);
      const int guest = recomp_pc_in_guest_code(pcs[0]);
      in_guest_code |= guest;
      if (!guest) pcs[np++] = (uint64_t)arm_thread_state64_get_lr(st);
      uint64_t fp = guest ? 0 : (uint64_t)arm_thread_state64_get_fp(st);
      while (np < STALL_PROFILE_DEPTH && fp && !(fp & 15)) {
        uint64_t frame[2];
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), fp, sizeof(frame), (mach_vm_address_t)(uintptr_t)frame,
                                   &got) != KERN_SUCCESS || got != sizeof(frame) || !frame[1])
          break;
        pcs[np++] = frame[1];
        if (frame[0] <= fp) break;
        fp = frame[0];
      }
    }
    thread_resume(port);
    if (np) {
      const int slot = stall_profile_slot(t->thread_id);
      stall_profile_add(g_stall_prof, slot, t->name[0] ? t->name : "unnamed", pcs, np);
      stall_profile_thread_cpu(g_stall_prof, slot, stall_profile_thread_cpu_us(t->host_thread));
    }
  }
  pthread_mutex_unlock(&g_threads_mutex);
  if (in_guest_code && !g_prof_no_chain_noted) {
    g_prof_no_chain_noted = 1;
    fprintf(stderr, "[stall-profile] samples in recompiled guest code: PC only (no frame chain)\n");
  }
}

static _Atomic uint64_t g_last_present_ns;

static uint64_t stall_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void stall_dump_threads(double stalled_ms) {
  fprintf(stderr, "[stall] no frame for %.0f ms; guest threads:\n", stalled_ms);
  pthread_mutex_lock(&g_threads_mutex);
  for (RecompThread *t = g_threads; t; t = t->next) {
    if (t->finished || !t->ctx) continue;
    const char *sym = "?";
    uint64_t pc = 0;
    mach_port_t port = pthread_mach_thread_np(t->host_thread);
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (port != MACH_PORT_NULL &&
        thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
      pc = (uint64_t)arm_thread_state64_get_pc(st);
      Dl_info di;
      if (dladdr((void *)pc, &di) && di.dli_sname) sym = di.dli_sname;
    }
    const GuestContext *run = t->ctx;
    if (RECOMP_MEM_OK(t->ctx) && t->ctx->fs_base) {
      const uint64_t slot = *(const uint64_t *)(t->ctx->mem_base + t->ctx->fs_base + RECOMP_TCB_CTX_SLOT);
      if (slot) run = (const GuestContext *)(uintptr_t)slot;
    }
    fprintf(stderr, "[stall]   %-28s last shim 0x%llx  mxcsr 0x%x%s  host %s\n", t->name[0] ? t->name : "unnamed",
            (unsigned long long)run->rip, run->mxcsr, run != t->ctx ? " (fiber)" : "", sym);
  }
  pthread_mutex_unlock(&g_threads_mutex);
}

static long g_stall_dump_ms, g_stall_prof_ms;

static void *stall_watchdog_main(void *arg) {
  (void)arg;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  const uint64_t dump_ns = (uint64_t)g_stall_dump_ms * 1000000ull;
  const uint64_t prof_ns = (uint64_t)g_stall_prof_ms * 1000000ull;
  uint64_t reported_for = 0;
  uint64_t prev_wake = 0, max_gap = 0;
  const uint64_t period_ns = prof_ns ? 1000000ull : 5000000ull;
  uint64_t deadline = stall_now_ns();
  for (;;) {
    deadline += period_ns;
    const uint64_t before = stall_now_ns();
    if (deadline > before)
      usleep((useconds_t)((deadline - before) / 1000));
    else
      deadline = before;
    const uint64_t last = g_last_present_ns;
    if (!last) continue;
    const uint64_t now = stall_now_ns();
    if (g_prof_for && prev_wake && now - prev_wake > max_gap) max_gap = now - prev_wake;
    prev_wake = now;
    if (dump_ns && now - last > dump_ns && reported_for != last) {
      reported_for = last;
      stall_dump_threads((double)(now - last) / 1e6);
    }
    if (!prof_ns) continue;
    if (g_prof_for && g_prof_for != last) {
      stall_profile_cpu_end(g_stall_prof);
      stall_profile_report_gap(g_stall_prof, stderr, (double)(last - g_prof_for) / 1e6, 6, (double)max_gap / 1e6);
      g_prof_for = 0;
      prev_wake = deadline = stall_now_ns();
    }
    if (now - last > prof_ns) {
      if (g_prof_for != last) {
        stall_profile_begin();
        g_prof_for = last;
        max_gap = 0;
      }
      stall_profile_sample_all();
    }
  }
  return NULL;
}

static pthread_once_t g_stall_once = PTHREAD_ONCE_INIT;

static void stall_watchdog_start_once(void) {
  const char *e = getenv("PS4_STALL_WATCHDOG");
  const char *p = getenv("PS4_STALL_PROFILE");
  g_stall_dump_ms = e ? atol(e) : 0;
  g_stall_prof_ms = p ? atol(p) : 0;
  if (g_stall_dump_ms < 0) g_stall_dump_ms = 0;
  if (g_stall_prof_ms < 0) g_stall_prof_ms = 0;
  if (g_stall_prof_ms > 0) g_stall_prof = stall_profile_create(stall_symbolize);
  if (g_stall_prof_ms > 0 && !g_stall_prof) g_stall_prof_ms = 0;
  if (g_stall_dump_ms > 0 || g_stall_prof_ms > 0) {
    pthread_t th;
    pthread_create(&th, NULL, stall_watchdog_main, NULL);
    pthread_detach(th);
  }
}

void ps4_stall_watchdog_arm(void) {
  pthread_once(&g_stall_once, stall_watchdog_start_once);
  uint64_t zero = 0;
  atomic_compare_exchange_strong(&g_last_present_ns, &zero, stall_now_ns());
}

void ps4_stall_watchdog_note_present(void) {
  pthread_once(&g_stall_once, stall_watchdog_start_once);
  g_last_present_ns = stall_now_ns();
}
#else
void ps4_stall_watchdog_arm(void) {}
void ps4_stall_watchdog_note_present(void) {}
#endif
