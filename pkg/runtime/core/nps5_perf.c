// nps5 performance trace (docs/m3/perf.md): where a frame's time goes.
//
// Always on (one clock read per flip): frame-time statistics (frames, fps, p50/p95/p99) in the run
// bundle's perf.txt. A frame is the interval between two flip submissions.
//
// NPS5_PERF=1 adds the attribution (about 1 % overhead):
//   * per frame: wall time, CPU time of the flipping thread, process CPU time, wall time inside HLE calls;
//   * per HLE shim: calls, wall time and (sampled) CPU time;
//   * a sampling profiler (1 kHz for threads that run guest code, 250 Hz for the others; a sampler thread
//     reads the pc with thread_get_state, so no signal is involved and nothing is interrupted): CPU time split
//     into guest code (by guest function and by guest basic block, through the pc map), HLE, runtime/dispatch,
//     and every host image (AGC driver, KosmicKrisp, Metal, libsystem_kernel, ...), plus the top host symbols;
//   * per thread: CPU ms per frame, by thread name;
//   * the dispatch slow-path counters (shadow-stack slow returns, continuation-table/tier-1 dispatches).
// NPS5_PERF=2 also writes one record per frame to perf_frames.csv.
// NPS5_PERF_HUD=1 prints a one-line counter to stderr every second (implies NPS5_PERF=1 unless NPS5_PERF=0).
// NPS5_PERF_HZ=<n> changes the guest-thread sampling rate (default 1000).
#include "nps5_perf.h"
#include "nps5_lean.h"
#include "nps5_diag.h"
#include "nps5_shadow.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

int g_nps5_perf_level = 0;

static mach_timebase_info_data_t g_tb;
static inline uint64_t ticks_ns(uint64_t t) { return t * g_tb.numer / g_tb.denom; }
static inline uint64_t now_ns(void) { return ticks_ns(mach_absolute_time()); }
static inline uint64_t thread_cpu_ns(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }
static uint64_t proc_cpu_ns(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
}

// ------------------------------------------------------------------ frames
#define FRAME_CAP (1u << 18)
typedef struct { float wall_ms, cpu_ms, proc_ms, hle_ms; } FrameRec;
static FrameRec *g_frames;
static _Atomic uint32_t g_frame_n;
static uint64_t g_first_flip_ns, g_last_flip_ns, g_last_flip_cpu, g_last_flip_proc;
static pthread_mutex_t g_flip_mu = PTHREAD_MUTEX_INITIALIZER;
static void *g_flip_fns[4];
static int g_hud;
static uint64_t g_hud_t0;
static uint32_t g_hud_f0;

// ------------------------------------------------------------------ per-thread state (threads that make HLE calls)
#define HLE_DEPTH 8
typedef struct PerfThread {
  thread_t port;
  _Atomic(void *) cur_fn; // innermost HLE shim running on this thread (read by the sampler)
  int depth;
  void *fn[HLE_DEPTH];
  uint64_t t0[HLE_DEPTH];
  uint64_t hle_ns; // total wall time in outermost HLE calls
  uint64_t hle_ns_at_flip;
  uint64_t sampled_cpu_us; // sampler: CPU time at the previous sample
  struct PerfThread *next;
} PerfThread;
static pthread_key_t g_key;
static _Atomic(PerfThread *) g_threads;

static PerfThread *perf_thread(void) {
  PerfThread *t = (PerfThread *)pthread_getspecific(g_key);
  if (t) return t;
  t = (PerfThread *)calloc(1, sizeof *t);
  if (!t) return NULL;
  // Own send right (mach_thread_self), not pthread_mach_thread_np's borrowed name: once the thread exits a borrowed name
  // can be freed and reused by the kernel for an unrelated (possibly guarded) port, and thread_info on it kills the
  // process with EXC_GUARD. A name we hold a right on is never reused; the sampler drops the right when the thread dies.
  t->port = g_nps5_perf_level ? mach_thread_self() : MACH_PORT_NULL;
  pthread_setspecific(g_key, t);
  PerfThread *head = atomic_load(&g_threads);
  do { t->next = head; } while (!atomic_compare_exchange_weak(&g_threads, &head, t));
  return t;
}

// ------------------------------------------------------------------ per-shim wall time
#define SHIM_SLOTS 4096
typedef struct { _Atomic(void *) fn; _Atomic uint64_t calls, ns; uint64_t cpu_samples; /* microseconds */ } ShimSlot;
static ShimSlot g_shims[SHIM_SLOTS];

static ShimSlot *shim_slot(void *fn) {
  size_t h = ((uintptr_t)fn >> 2) % SHIM_SLOTS;
  for (size_t i = 0; i < SHIM_SLOTS; i++) {
    ShimSlot *s = &g_shims[(h + i) % SHIM_SLOTS];
    void *cur = atomic_load_explicit(&s->fn, memory_order_acquire);
    if (!cur) {
      void *exp = NULL;
      cur = atomic_compare_exchange_strong(&s->fn, &exp, fn) ? fn : exp;
    }
    if (cur == fn) return s;
  }
  return NULL;
}

static void perf_flip(PerfThread *t);

static void perf_hle_enter(void *fn) {
  for (int i = 0; i < 4 && g_flip_fns[i]; i++)
    if (g_flip_fns[i] == fn) { perf_flip(g_nps5_perf_level ? perf_thread() : NULL); break; }
  if (!g_nps5_perf_level) return;
  PerfThread *t = perf_thread();
  if (!t) return;
  if (t->depth < HLE_DEPTH) {
    t->fn[t->depth] = fn;
    t->t0[t->depth] = mach_absolute_time();
  }
  t->depth++;
  atomic_store_explicit(&t->cur_fn, fn, memory_order_relaxed);
}

static void perf_hle_exit(void *ctx, void *fn) {
  (void)ctx;
  PerfThread *t = (PerfThread *)pthread_getspecific(g_key);
  if (!t || t->depth <= 0) return;
  // A shim that never returned (longjmp, thread exit, context switch) leaves entries behind: unwind to ours.
  int d = t->depth - 1;
  while (d >= HLE_DEPTH || (d > 0 && t->fn[d] != fn)) d--;
  if (t->fn[d] != fn) { t->depth = 0; atomic_store_explicit(&t->cur_fn, NULL, memory_order_relaxed); return; }
  uint64_t dt = ticks_ns(mach_absolute_time() - t->t0[d]);
  ShimSlot *s = shim_slot(fn);
  if (s) {
    atomic_fetch_add_explicit(&s->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->ns, dt, memory_order_relaxed);
  }
  if (d == 0) t->hle_ns += dt;
  t->depth = d;
  atomic_store_explicit(&t->cur_fn, d ? t->fn[d - 1] : NULL, memory_order_relaxed);
}

void nps5_perf_register_flip_fn(void *shim_fn) {
  for (int i = 0; i < 4; i++) {
    if (g_flip_fns[i] == shim_fn) return;
    if (!g_flip_fns[i]) { g_flip_fns[i] = shim_fn; return; }
  }
}

static int cmp_float(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

static void perf_flip(PerfThread *t) {
  uint64_t now = now_ns();
  uint64_t cpu = g_nps5_perf_level ? thread_cpu_ns() : 0, proc = g_nps5_perf_level ? proc_cpu_ns() : 0;
  pthread_mutex_lock(&g_flip_mu);
  if (g_last_flip_ns) {
    uint32_t n = atomic_load_explicit(&g_frame_n, memory_order_relaxed);
    if (n < FRAME_CAP && g_frames) {
      FrameRec *r = &g_frames[n];
      r->wall_ms = (float)((double)(now - g_last_flip_ns) / 1e6);
      r->cpu_ms = (float)((double)(cpu - g_last_flip_cpu) / 1e6);
      r->proc_ms = (float)((double)(proc - g_last_flip_proc) / 1e6);
      r->hle_ms = t ? (float)((double)(t->hle_ns - t->hle_ns_at_flip) / 1e6) : 0;
      atomic_store_explicit(&g_frame_n, n + 1, memory_order_release);
    }
  } else {
    g_first_flip_ns = now;
    g_hud_t0 = now;
  }
  g_last_flip_ns = now;
  g_last_flip_cpu = cpu;
  g_last_flip_proc = proc;
  if (t) t->hle_ns_at_flip = t->hle_ns;
  if (g_hud && now - g_hud_t0 >= 1000000000ull) {
    uint32_t n = atomic_load(&g_frame_n), k = n - g_hud_f0;
    if (k) {
      float w[512];
      double cs = 0, ps = 0, hs = 0;
      uint32_t m = k > 512 ? 512 : k;
      for (uint32_t i = 0; i < m; i++) {
        FrameRec *r = &g_frames[n - m + i];
        w[i] = r->wall_ms; cs += r->cpu_ms; ps += r->proc_ms; hs += r->hle_ms;
      }
      qsort(w, m, sizeof(float), cmp_float);
      fprintf(stderr, "[perf] %5.1f fps | frame p50 %.2f p95 %.2f max %.2f ms | flip thread cpu %.2f ms (in HLE calls %.2f ms wall) | process cpu %.2f ms/frame\n",
              (double)k * 1e9 / (double)(now - g_hud_t0), w[m / 2], w[(m * 95) / 100], w[m - 1], cs / m, hs / m, ps / m);
    }
    g_hud_t0 = now;
    g_hud_f0 = n;
  }
  pthread_mutex_unlock(&g_flip_mu);
}

// ------------------------------------------------------------------ sampler
#define PC_SLOTS (1u << 17)
typedef struct { uint64_t pc; uint32_t n; uint32_t hle; } PcSlot; // hle: samples taken while the thread was in an HLE call
static PcSlot *g_pcs;
static uint64_t g_pc_dropped, g_samples_guest_thr, g_samples_other;
static _Atomic int g_sampler_stop;
static pthread_t g_sampler;
static int g_sampler_on;
static thread_t g_sampler_port;
static int g_hz = 1000;

static void pc_add(uint64_t pc, uint32_t w, int in_hle) {
  size_t h = (size_t)((pc >> 2) * 0x9E3779B97F4A7C15ull >> 47) & (PC_SLOTS - 1);
  for (size_t i = 0; i < 64; i++) {
    PcSlot *s = &g_pcs[(h + i) & (PC_SLOTS - 1)];
    if (s->pc == pc || !s->pc) {
      s->pc = pc;
      s->n += w;
      if (in_hle) s->hle += w;
      return;
    }
  }
  g_pc_dropped += w;
}

// CPU time a thread has consumed so far, in microseconds (0 when it is gone).
// *gone is set when the port is dead (thread exited / invalid name): the caller must drop it and never use it again.
static uint64_t thread_cpu_us_ex(thread_t th, int *gone) {
  thread_basic_info_data_t bi;
  mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
  if (gone) *gone = 0;
  if (th == MACH_PORT_NULL || th == MACH_PORT_DEAD) { if (gone) *gone = 1; return 0; }
  kern_return_t kr = thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &c);
  if (kr != KERN_SUCCESS) {
    if (gone && (kr == KERN_TERMINATED || kr == KERN_INVALID_ARGUMENT || kr == MACH_SEND_INVALID_DEST || kr == KERN_INVALID_NAME || kr == KERN_INVALID_RIGHT)) *gone = 1;
    return 0;
  }
  return (uint64_t)bi.user_time.seconds * 1000000ull + (uint64_t)bi.user_time.microseconds + (uint64_t)bi.system_time.seconds * 1000000ull +
         (uint64_t)bi.system_time.microseconds;
}

static uint64_t thread_cpu_us(thread_t th) { return thread_cpu_us_ex(th, NULL); }

static int thread_pc(thread_t th, uint64_t *pc) {
  arm_thread_state64_t st;
  mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
  if (th == MACH_PORT_NULL || th == MACH_PORT_DEAD) return 0;
  if (thread_suspend(th) != KERN_SUCCESS) return 0;
  kern_return_t kr = thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &c);
  thread_resume(th);
  if (kr != KERN_SUCCESS) return 0;
  *pc = (uint64_t)arm_thread_state64_get_pc(st);
  return 1;
}

// The other threads (driver, KosmicKrisp, Metal, audio, ...), by mach port name: CPU time at the last sample.
#define OTHER_SLOTS 512
static struct { thread_t port; uint64_t cpu_us; } g_other[OTHER_SLOTS];

// Every sample is weighted by the CPU time the thread consumed since its previous sample, so the totals are CPU
// time (not wall time) whatever the machine load is, and a parked or preempted thread adds nothing.
static void *sampler_main(void *arg) {
  (void)arg;
  pthread_setname_np("nps5-perf");
  // No signal handler may run here: one that ran between thread_suspend and thread_resume (the SIGTERM finaliser
  // writes the report) would wait for a lock the suspended thread holds.
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, NULL);
  g_sampler_port = mach_thread_self();
  uint64_t period = (uint64_t)(1e9 / g_hz) * g_tb.denom / g_tb.numer;
  uint64_t next = mach_absolute_time() + period;
  unsigned tick = 0;
  int slow_every = g_hz / 250 > 0 ? g_hz / 250 : 1;
  while (!atomic_load_explicit(&g_sampler_stop, memory_order_relaxed)) {
    mach_wait_until(next);
    next += period;
    uint64_t t = mach_absolute_time();
    if (next < t) next = t + period; // fell behind: do not burst
    tick++;
    for (PerfThread *p = atomic_load(&g_threads); p; p = p->next) {
      if (p->port == MACH_PORT_NULL) continue; // exited: right already dropped
      int gone;
      uint64_t cpu = thread_cpu_us_ex(p->port, &gone), pc;
      if (gone) { // thread exited: drop our send right exactly once, never touch the name again
        thread_t dead = p->port;
        p->port = MACH_PORT_NULL;
        mach_port_deallocate(mach_task_self(), dead);
        continue;
      }
      if (cpu <= p->sampled_cpu_us) continue;
      uint32_t w = (uint32_t)(cpu - p->sampled_cpu_us);
      if (!p->sampled_cpu_us) w = 0; // first sight: no interval to attribute
      p->sampled_cpu_us = cpu;
      if (!w || !thread_pc(p->port, &pc)) continue;
      void *fn = atomic_load_explicit(&p->cur_fn, memory_order_relaxed);
      pc_add(pc, w, fn != NULL);
      g_samples_guest_thr += w;
      if (fn) { ShimSlot *s = shim_slot(fn); if (s) s->cpu_samples += w; }
    }
    if (tick % (unsigned)slow_every) continue;
    thread_act_array_t list;
    mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) continue;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
      thread_t th = list[i];
      int guest = th == g_sampler_port;
      for (PerfThread *p = atomic_load(&g_threads); p && !guest; p = p->next) guest = p->port != MACH_PORT_NULL && p->port == th;
      if (!guest) {
        size_t k = (size_t)th % OTHER_SLOTS, j = 0;
        while (j < OTHER_SLOTS && g_other[k].port && g_other[k].port != th) { k = (k + 1) % OTHER_SLOTS; j++; }
        if (j < OTHER_SLOTS) {
          uint64_t cpu = thread_cpu_us(th), pc;
          int seen = g_other[k].port == th;
          uint64_t prev = seen ? g_other[k].cpu_us : cpu;
          g_other[k].port = th;
          if (cpu != prev || !seen) g_other[k].cpu_us = cpu; // != : a reused name may restart lower
          if (cpu > prev && thread_pc(th, &pc)) {
            pc_add(pc, (uint32_t)(cpu - prev), 0);
            g_samples_other += cpu - prev;
          }
        }
      }
      mach_port_deallocate(mach_task_self(), th);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_t));
  }
  return NULL;
}

// ------------------------------------------------------------------ report
typedef struct { char name[96]; double v; uint64_t a, b; } Row;
static int cmp_row(const void *a, const void *b) {
  double x = ((const Row *)a)->v, y = ((const Row *)b)->v;
  return x < y ? 1 : x > y ? -1 : 0;
}
static Row *row_find(Row *rows, size_t *n, size_t cap, const char *name) {
  for (size_t i = 0; i < *n; i++)
    if (!strcmp(rows[i].name, name)) return &rows[i];
  if (*n >= cap) return NULL;
  Row *r = &rows[(*n)++];
  memset(r, 0, sizeof *r);
  strlcpy(r->name, name, sizeof r->name);
  return r;
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void shim_name(void *fn, char *buf, size_t cap) {
  Dl_info di;
  if (dladdr(fn, &di) && di.dli_sname && di.dli_saddr == fn) {
    strlcpy(buf, di.dli_sname[0] == '_' ? di.dli_sname + 1 : di.dli_sname, cap);
  } else if (dladdr(fn, &di) && di.dli_fbase) {
    // stripped link (-Wl,-x): `atos -o <binary> -l 0x100000000 <addr>` or nm on an unstripped link names it
    snprintf(buf, cap, "shim@+0x%llx", (unsigned long long)((uintptr_t)fn - (uintptr_t)di.dli_fbase));
  } else {
    snprintf(buf, cap, "shim@%p", fn);
  }
}

static void thread_table(FILE *f, double frames) {
  thread_act_array_t list;
  mach_msg_type_number_t n;
  if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return;
  Row rows[128];
  size_t nr = 0;
  for (mach_msg_type_number_t i = 0; i < n; i++) {
    thread_basic_info_data_t bi;
    mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
    if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) {
      char name[96] = "";
      pthread_t pt = pthread_from_mach_thread_np(list[i]);
      if (pt) pthread_getname_np(pt, name, sizeof name);
      int guest = 0;
      for (PerfThread *p = atomic_load(&g_threads); p; p = p->next) guest |= p->port == list[i];
      if (!name[0]) strlcpy(name, guest ? "(guest thread)" : "(unnamed)", sizeof name);
      else if (guest) strlcat(name, " [guest]", sizeof name);
      Row *r = row_find(rows, &nr, 128, name);
      if (r) {
        r->v += (double)bi.user_time.seconds + bi.user_time.microseconds / 1e6 + (double)bi.system_time.seconds + bi.system_time.microseconds / 1e6;
        r->a++;
      }
    }
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_t));
  qsort(rows, nr, sizeof(Row), cmp_row);
  fprintf(f, "threads alive at exit (CPU since process start, incl. loading; ms per frame over %.0f frames):\n", frames);
  for (size_t i = 0; i < nr && i < 16; i++)
    if (rows[i].v >= 0.01) fprintf(f, "  %8.2f s  %7.3f ms/frame  %s%s\n", rows[i].v, rows[i].v * 1000.0 / frames, rows[i].name, rows[i].a > 1 ? " (several)" : "");
}


// ---- sampled CPU report. Everything is aggregated on numbers first and named last (dladdr and the guest
// function-name lookup are slow, and the SIGTERM finaliser has two seconds).
typedef struct { uint64_t key, sub, w; } Agg;
static int cmp_agg_key(const void *a, const void *b) {
  const Agg *x = (const Agg *)a, *y = (const Agg *)b;
  if (x->key != y->key) return x->key < y->key ? -1 : 1;
  return x->sub < y->sub ? -1 : x->sub > y->sub;
}
static int cmp_agg_w(const void *a, const void *b) {
  uint64_t x = ((const Agg *)a)->w, y = ((const Agg *)b)->w;
  return x < y ? 1 : x > y ? -1 : 0;
}
// Merges equal keys (when by_sub: equal key and sub) of a[0..n) in place and sorts by weight; returns the new count.
static size_t agg_merge(Agg *a, size_t n, int by_sub) {
  if (!n) return 0;
  qsort(a, n, sizeof(Agg), cmp_agg_key);
  size_t o = 0;
  for (size_t i = 1; i < n; i++) {
    if (a[i].key == a[o].key && (!by_sub || a[i].sub == a[o].sub)) a[o].w += a[i].w;
    else a[++o] = a[i];
  }
  qsort(a, o + 1, sizeof(Agg), cmp_agg_w);
  return o + 1;
}

typedef struct { uint64_t lo, hi; const char *name; } Img;
static int cmp_img(const void *a, const void *b) {
  uint64_t x = ((const Img *)a)->lo, y = ((const Img *)b)->lo;
  return x < y ? -1 : x > y;
}
static size_t load_images(Img *imgs, size_t cap) {
  size_t n = 0;
  uint32_t cnt = _dyld_image_count();
  for (uint32_t i = 0; i < cnt && n < cap; i++) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!mh || mh->magic != MH_MAGIC_64) continue;
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t c = 0; c < mh->ncmds; c++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
      if (lc->cmd != LC_SEGMENT_64) continue;
      const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
      if (strcmp(sg->segname, "__TEXT")) continue;
      imgs[n].lo = (uint64_t)mh;
      imgs[n].hi = (uint64_t)mh + sg->vmsize;
      imgs[n].name = base_name(_dyld_get_image_name(i));
      n++;
      break;
    }
  }
  qsort(imgs, n, sizeof(Img), cmp_img);
  return n;
}
static const Img *find_image(const Img *imgs, size_t n, uint64_t pc) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (imgs[mid].lo <= pc) lo = mid + 1; else hi = mid;
  }
  return lo && pc < imgs[lo - 1].hi ? &imgs[lo - 1] : NULL;
}

static void sampled_report(FILE *f, const char *bundle_dir, uint32_t n) {
  size_t np = 0;
  for (size_t i = 0; i < PC_SLOTS; i++) np += g_pcs[i].pc != 0;
  if (!np) return;
  Img *imgs = (Img *)calloc(1024, sizeof(Img));
  Agg *gfn = (Agg *)calloc(np, sizeof(Agg)), *gpc = (Agg *)calloc(np, sizeof(Agg)), *host = (Agg *)calloc(np, sizeof(Agg));
  Agg *cat = (Agg *)calloc(2 * np, sizeof(Agg)); // key: image index + 1 (0 = guest code, ~0 = unknown), sub: 1 = inside an HLE call
  if (!imgs || !gfn || !gpc || !host || !cat) goto out;
  size_t nimg = load_images(imgs, 1024), ng = 0, nh = 0, nc = 0;
  uint64_t total = 0, self_lo = 0;
  const Img *self = find_image(imgs, nimg, (uint64_t)(uintptr_t)&nps5_perf_register_flip_fn);
  if (self) self_lo = self->lo;
  for (size_t i = 0; i < PC_SLOTS; i++) {
    PcSlot *s = &g_pcs[i];
    if (!s->pc) continue;
    total += s->n;
    uint64_t gp, gf;
    if (nps5_diag_host_pc_to_guest(s->pc, &gp, &gf, NULL)) {
      gfn[ng] = (Agg){gf, 0, s->n};
      gpc[ng++] = (Agg){gp, gf, s->n};
      cat[nc++] = (Agg){0, 0, s->n};
      continue;
    }
    const Img *im = find_image(imgs, nimg, s->pc);
    uint64_t key = im ? (uint64_t)(im - imgs) + 1 : ~0ull;
    if (s->hle) cat[nc++] = (Agg){key, 1, s->hle};
    if (s->n > s->hle) cat[nc++] = (Agg){key, 0, s->n - s->hle};
    host[nh++] = (Agg){s->pc, 0, s->n};
  }
  double per = 1e-3 / n; // microseconds of CPU -> ms per frame
  fprintf(f, "sampled cpu: %.3f ms/frame in total (%.3f on threads running guest code, %.3f on host threads, %.3f not recorded)\n", total * per,
          g_samples_guest_thr * per, g_samples_other * per, g_pc_dropped * per);
  nc = agg_merge(cat, nc, 1);
  fprintf(f, "cpu by category (ms/frame, share):\n");
  for (size_t i = 0; i < nc; i++) {
    if (cat[i].w * per < 0.005) break;
    const char *nm = cat[i].key == 0 ? "guest code" : cat[i].key == ~0ull ? "?" : imgs[cat[i].key - 1].name;
    int in_self = cat[i].key && cat[i].key != ~0ull && imgs[cat[i].key - 1].lo == self_lo;
    if (in_self) nm = cat[i].sub ? "runtime shims" : "runtime (dispatch, trampolines, host threads)";
    fprintf(f, "  %8.3f  %5.1f%%  %s%s\n", cat[i].w * per, 100.0 * cat[i].w / total, cat[i].sub ? "HLE calls: " : "", nm);
  }
  size_t ngf = agg_merge(gfn, ng, 0);
  fprintf(f, "top guest functions (ms/frame, share of all cpu):\n");
  for (size_t i = 0; i < ngf && i < 25; i++) {
    const char *nm = NULL;
    nps5_diag_guest_fn_name(gfn[i].key, &nm);
    fprintf(f, "  %8.3f  %5.1f%%  %s\n", gfn[i].w * per, 100.0 * gfn[i].w / total, nm ? nm : "?");
  }
  // gpc: merge by guest pc, keep an address-ordered copy for the per-function listing
  qsort(gpc, ng, sizeof(Agg), cmp_agg_key);
  size_t o = 0;
  for (size_t i = 1; i < ng; i++) {
    if (gpc[i].key == gpc[o].key) gpc[o].w += gpc[i].w; else gpc[++o] = gpc[i];
  }
  size_t ngp = ng ? o + 1 : 0;
  char path[1200];
  snprintf(path, sizeof path, "%s/perf_hot.txt", bundle_dir);
  FILE *hf = fopen(path, "w");
  for (size_t k = 0; hf && k < ngf && k < 4; k++) {
    if (gfn[k].w * 50 < total) break; // below 2 % of all cpu
    const char *nm = NULL;
    nps5_diag_guest_fn_name(gfn[k].key, &nm);
    fprintf(hf, "%s: %.3f ms/frame (%.1f%% of all cpu); guest pc, ms/frame, share of the function\n", nm ? nm : "?", gfn[k].w * per, 100.0 * gfn[k].w / total);
    for (size_t i = 0; i < ngp; i++)
      if (gpc[i].sub == gfn[k].key) fprintf(hf, "  0x%llx  %8.4f  %5.2f%%\n", (unsigned long long)gpc[i].key, gpc[i].w * per, 100.0 * gpc[i].w / gfn[k].w);
  }
  if (hf) fclose(hf);
  qsort(gpc, ngp, sizeof(Agg), cmp_agg_w);
  fprintf(f, "top guest instructions (ms/frame, share; samples skid to the instruction after a slow one):\n");
  for (size_t i = 0; i < ngp && i < 30; i++) {
    const char *nm = NULL;
    nps5_diag_guest_fn_name(gpc[i].sub, &nm);
    fprintf(f, "  %8.3f  %5.1f%%  0x%llx in %s\n", gpc[i].w * per, 100.0 * gpc[i].w / total, (unsigned long long)gpc[i].key, nm ? nm : "?");
  }
  // Host symbols: name the 400 heaviest pcs only, then merge by symbol address.
  qsort(host, nh, sizeof(Agg), cmp_agg_w);
  size_t top = nh < 400 ? nh : 400;
  for (size_t i = 0; i < top; i++) {
    Dl_info di;
    host[i].sub = host[i].key;
    if (dladdr((void *)host[i].key, &di) && di.dli_saddr) host[i].key = (uint64_t)(uintptr_t)di.dli_saddr;
  }
  top = agg_merge(host, top, 0);
  fprintf(f, "top host symbols (ms/frame, share; nearest exported symbol, so approximate in stripped images):\n");
  for (size_t i = 0; i < top && i < 30; i++) {
    Dl_info di;
    const Img *im = find_image(imgs, nimg, host[i].sub);
    const char *sn = dladdr((void *)host[i].sub, &di) && di.dli_sname ? di.dli_sname : "?";
    fprintf(f, "  %8.3f  %5.1f%%  %.70s (%s)\n", host[i].w * per, 100.0 * host[i].w / total, sn, im ? im->name : "?");
  }
out:
  free(imgs); free(gfn); free(gpc); free(host); free(cat);
}

static void perf_finish(const char *bundle_dir) {
  if (g_sampler_on) {
    atomic_store(&g_sampler_stop, 1);
    pthread_join(g_sampler, NULL);
    g_sampler_on = 0;
  }
  uint32_t n = atomic_load(&g_frame_n);
  if (!n || !bundle_dir || !bundle_dir[0]) return;
  char path[1200];
  snprintf(path, sizeof path, "%s/perf.txt", bundle_dir);
  FILE *f = fopen(path, "w");
  if (!f) return;
  // The first 60 frames (loading, cold caches) are left out of the percentiles when there are enough.
  uint32_t skip = n > 240 ? 60 : 0, m = n - skip;
  float *w = (float *)malloc(m * sizeof(float));
  double ws = 0, cs = 0, ps = 0, hs = 0;
  for (uint32_t i = 0; i < m; i++) {
    FrameRec *r = &g_frames[skip + i];
    if (w) w[i] = r->wall_ms;
    ws += r->wall_ms; cs += r->cpu_ms; ps += r->proc_ms; hs += r->hle_ms;
  }
  if (w) qsort(w, m, sizeof(float), cmp_float);
  double span = (double)(g_last_flip_ns - g_first_flip_ns) / 1e9;
  fprintf(f, "frames: %u in %.2f s (first to last flip)  avg %.2f fps\n", n, span, span > 0 ? n / span : 0);
  if (w)
    fprintf(f, "frame_ms (after the first %u frames): avg %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f  => %.2f fps at p50\n", skip, ws / m, w[m / 2],
            w[(size_t)m * 95 / 100], w[(size_t)m * 99 / 100], w[m - 1], w[m / 2] > 0 ? 1000.0 / w[m / 2] : 0);
  if (g_nps5_perf_level) {
    fprintf(f, "per frame (avg): flip-thread cpu %.3f ms | flip-thread wall inside HLE calls %.3f ms | flip-thread guest+runtime cpu <= %.3f ms | "
               "flip-thread not running (blocked or preempted) %.3f ms | process cpu %.3f ms\n",
            cs / m, hs / m, cs / m, (ws - cs) / m, ps / m);
    RecompShadowStats st;
    memset(&st, 0, sizeof st);
    if (recomp_call_model_flat()) recomp_shadow_stats(&st);
    fprintf(f, "dispatch slow paths per frame: shadow slow returns %.3f, continuation-table/tier-1 dispatches %.3f\n", (double)st.slow_rets / n,
            (double)st.dispatch_slow / n);
    thread_table(f, (double)n);

    // HLE by shim
    Row *rows = (Row *)calloc(SHIM_SLOTS, sizeof(Row));
    size_t nr = 0;
    double ms_per_sample = 1e-3; // sample weights are microseconds of CPU time
    for (size_t i = 0; rows && i < SHIM_SLOTS; i++) {
      void *fn = atomic_load(&g_shims[i].fn);
      if (!fn) continue;
      Row *r = &rows[nr++];
      shim_name(fn, r->name, sizeof r->name);
      r->v = (double)atomic_load(&g_shims[i].ns) / 1e6;
      r->a = atomic_load(&g_shims[i].calls);
      r->b = g_shims[i].cpu_samples;
    }
    if (rows) {
      qsort(rows, nr, sizeof(Row), cmp_row);
      fprintf(f, "hle calls by wall time, all threads (calls/frame, wall ms/frame, sampled cpu ms/frame):\n");
      for (size_t i = 0; i < nr && i < 25; i++)
        fprintf(f, "  %9.2f  %8.3f  %8.3f  %s\n", (double)rows[i].a / n, rows[i].v / n, (double)rows[i].b * ms_per_sample / n, rows[i].name);
      free(rows);
    }

    sampled_report(f, bundle_dir, n);
  }
  fclose(f);
  if (g_nps5_perf_level >= 2) {
    snprintf(path, sizeof path, "%s/perf_frames.csv", bundle_dir);
    if ((f = fopen(path, "w"))) {
      fprintf(f, "frame,wall_ms,flip_thread_cpu_ms,process_cpu_ms,hle_wall_ms\n");
      for (uint32_t i = 0; i < n; i++) fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f\n", i, g_frames[i].wall_ms, g_frames[i].cpu_ms, g_frames[i].proc_ms, g_frames[i].hle_ms);
      fclose(f);
    }
  }
  free(w);
  if (g_hud || getenv("NPS5_PERF_PRINT")) {
    snprintf(path, sizeof path, "%s/perf.txt", bundle_dir);
    if ((f = fopen(path, "r"))) {
      char line[512];
      while (fgets(line, sizeof line, f)) fputs(line, stderr);
      fclose(f);
    }
  }
}

__attribute__((constructor)) static void nps5_perf_init(void) {
  mach_timebase_info(&g_tb);
  const char *p = getenv("NPS5_PERF"), *h = getenv("NPS5_PERF_HUD");
  g_hud = h && atoi(h) > 0;
  g_nps5_perf_level = p ? atoi(p) : (g_hud ? 1 : 0);
  if (getenv("NPS5_NO_PERF")) return;
  g_frames = (FrameRec *)calloc(FRAME_CAP, sizeof(FrameRec)); // 4 MiB of address space, touched as frames arrive
  pthread_key_create(&g_key, NULL);
  g_nps5_perf_hle_enter = perf_hle_enter;
  g_nps5_perf_finish = perf_finish;
  if (!g_nps5_perf_level) return;
  const char *hz = getenv("NPS5_PERF_HZ");
  if (hz && atoi(hz) >= 10 && atoi(hz) <= 10000) g_hz = atoi(hz);
  g_pcs = (PcSlot *)calloc(PC_SLOTS, sizeof(PcSlot));
  if (!g_pcs) return;
  g_nps5_hle_exit_hook = perf_hle_exit;
  nps5_lean_require_hooks(); // lean HLE calls are timed like the others
  g_sampler_on = pthread_create(&g_sampler, NULL, sampler_main, NULL) == 0;
}
