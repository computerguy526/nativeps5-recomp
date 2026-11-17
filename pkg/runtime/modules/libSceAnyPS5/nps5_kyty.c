// SPDX-License-Identifier: GPL-2.0-or-later
//
// NPS5_GPU=kyty: the host side of KytyPS5's GPU stack inside the ps5recomp runtime (docs/m3/kyty-seam.md).
// The idea of running Kyty's GPU stack as a library behind another loader's NID exports, and the interface it is
// reached through (kytyGpuApi.h), come from the AstroHybrid project (kyty_gpu.dll + AnyPS5 patches 0021-0025);
// KytyPS5 is by Jetsku. This file is the nativeps5 host: guest memory stays with the runtime's VM layer.
//
//   routes     the 84 graphics exports (nps5_kyty_routes, generated) go to the library; everything else stays AnyPS5
//   memory     VM hooks -> register/unregister/protect_memory; an always-writable alias view per range (mach_vm_remap)
//   watches    the library arms 4 KiB tracker pages through protect_guest; nps5_kyty_watch.c folds them into 16 KiB
//              host pages; faults come in through the runtime's access resolver, system-call buffers through the host
//              access hooks. AnyPS5's allocation registry and write watch are not installed: one owner per range.
//   events     AGC/VideoOut event registrations go to AnyPS5's libkernel queues (libnps5_gpu: Nps5KytyHost*)
//   window     the library owns its window; run_on_main_thread gives it the main thread
//   exit       request_exit -> AnyPS5 libc LibcRequestExit (as its own window close does)

#define _DARWIN_C_SOURCE 1
#include "nps5_anyps5_config.h"

#if NPS5_HAVE_ANYPS5_AGC

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

#include "kytyGpuApi.h"
#include "nps5_diag.h"
#include "nps5_hostcall.h"
#include "nps5_kyty.h"
#include "nps5_kyty_watch.h"
#include "nps5_mem.h"
#include "ps4_vfs.h"
#include "recomp_runtime.h"

#define USER_ZONE 0x200000000ULL
#define AGC_DRIVER_PAGE 0xEE0000000ULL
#define AGC_DRIVER_PAGE_BYTES (2u << 20)
// Kyty's AGC driver memory (function table, validator shader, register default tables): 2 MiB the PS5 system owns at a
// fixed guest address. The host maps it (zeroed, read-write) before the renderer starts, so it is an ordinary
// registered range with an alias; the library no longer mmaps into the arena view itself.
#define KYTY_DRIVER_DMEM 0xFE0000000ULL
#define KYTY_DRIVER_DMEM_BYTES (2u << 20)
#define KYTY_DRIVER_DMEM_NAME "kyty_driver_dmem"

static const KytyGpuApi *g_api;
static void *g_lib;
static void **g_entries; // per route
static bool g_active;
static int g_trace;
static KytyGpuHostInfo g_info;

// Host-side helpers in libnps5_gpu.dylib (AnyPS5 nps5-kyty-seam: libSceVideoOut/src/Nps5KytyHost.cpp). Optional: without
// them events are not delivered (the library is told so with an error from add_gpu_event).
static int (*p_add_event)(int64_t equeue, uint32_t kind, int64_t ident, void *udata);
static int (*p_delete_event)(int64_t equeue, uint32_t kind, int64_t ident);
static void (*p_deliver_event)(uint32_t kind, int64_t ident, uint64_t data);
static void (*p_publish_pad)(int port, const KytyGpuPadState *state);
static int (*p_start_pad_script)(void);
static void (*p_request_exit)(int code);

// Registered ranges: sorted, disjoint. The alias is a second mapping of the same pages that no watch ever protects.
typedef struct {
  uint64_t lo, hi;
  uint32_t flags;
  uint8_t *alias; // host address of the alias of [lo, hi), or NULL
} KRange;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static KRange *g_ranges;
static size_t g_nranges, g_capranges;
static bool g_want_alias = true;

// A copy of the range table the fault handler can read without a lock (it may interrupt a thread that holds g_mu):
// a sequence lock around a fixed array, rewritten under g_mu after every change of g_ranges.
#define SNAP_MAX 4096
typedef struct { uint64_t lo, hi; uint8_t *alias; } SnapRange;
static SnapRange g_snap[SNAP_MAX];
static _Atomic uint32_t g_snap_n;
static _Atomic uint64_t g_snap_seq;

static void snap_publish(void) { // under g_mu
  atomic_fetch_add_explicit(&g_snap_seq, 1, memory_order_acq_rel); // odd: being written
  const size_t n = g_nranges <= SNAP_MAX ? g_nranges : 0;          // too many: nothing is served (the slow path stays correct)
  for (size_t i = 0; i < n; i++) g_snap[i] = (SnapRange){g_ranges[i].lo, g_ranges[i].hi, g_ranges[i].alias};
  atomic_store_explicit(&g_snap_n, (uint32_t)n, memory_order_release);
  atomic_fetch_add_explicit(&g_snap_seq, 1, memory_order_acq_rel);
}

// The alias of guest [guest, guest+size), or NULL. Signal handler: no lock, no allocation.
static void *alias_of(uint64_t guest, uint64_t size) {
  for (int attempt = 0; attempt < 64; attempt++) {
    const uint64_t s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
    if (s1 & 1) continue;
    uint32_t lo = 0, hi = atomic_load_explicit(&g_snap_n, memory_order_acquire);
    void *found = NULL;
    while (lo < hi) { // sorted by address, disjoint
      const uint32_t mid = (lo + hi) / 2;
      const SnapRange r = g_snap[mid];
      if (guest < r.lo) hi = mid;
      else if (guest >= r.hi) lo = mid + 1;
      else {
        if (r.alias && size <= r.hi - guest) found = r.alias + (guest - r.lo);
        break;
      }
    }
    // The entry reads above must be complete before the sequence is read again (arm64 may reorder plain loads
    // past an acquire load): a torn entry never passes.
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&g_snap_seq, memory_order_acquire) == s1) return found;
  }
  return NULL; // a writer is stuck behind this very thread: no emulation this time
}

#define KLOG(...) do { fprintf(stderr, "[kyty-seam] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define KTRACE(...) do { if (g_trace) KLOG(__VA_ARGS__); } while (0)

__attribute__((noreturn)) static void fatal(const char *what) {
  fprintf(stderr, "[kyty-seam] FATAL: %s\n", what);
  abort();
}

bool nps5_kyty_active(void) { return g_active; }
GuestContext *nps5_kyty_ctx(void) {
  GuestContext *c = nps5_hostcall_ctx();
  return c ? c : recomp_get_current_ctx();
}

void *nps5_kyty_entry(unsigned index) {
  void *e = index < nps5_kyty_route_count && g_entries ? g_entries[index] : NULL;
  if (!e) {
    const Nps5KytyRoute *r = index < nps5_kyty_route_count ? &nps5_kyty_routes[index] : NULL;
    fprintf(stderr, "[kyty-seam] FATAL: the game called %s (NID %s, %s) and kyty_gpu does not implement it\n", r ? r->name : "?", r ? r->nid : "?",
            r ? r->library : "?");
    abort();
  }
  return e;
}

// ---- alias views --------------------------------------------------------------------------------------------------

static uint8_t *alias_make(uint64_t lo, uint64_t hi) {
  if (!g_want_alias || g_nps5_arena.flat) return NULL;
  mach_vm_address_t at = 0;
  vm_prot_t cur = 0, max = 0;
  const mach_vm_address_t src = (mach_vm_address_t)(uintptr_t)g_nps5_arena.bias + lo;
  kern_return_t kr = mach_vm_remap(mach_task_self(), &at, hi - lo, 0, VM_FLAGS_ANYWHERE, mach_task_self(), src, FALSE, &cur, &max, VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) {
    KLOG("no alias for guest 0x%llx +0x%llx (mach_vm_remap %d): the library reads and writes the guest view", (unsigned long long)lo, (unsigned long long)(hi - lo), kr);
    return NULL;
  }
  // The copy inherits the protection of the guest view at this moment; the alias is always read-write.
  if (mach_vm_protect(mach_task_self(), at, hi - lo, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
    mach_vm_deallocate(mach_task_self(), at, hi - lo);
    return NULL;
  }
  return (uint8_t *)(uintptr_t)at;
}
static void alias_free(uint8_t *alias, uint64_t bytes) {
  if (alias) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)alias, bytes);
}

static int range_insert(size_t at, KRange r) {
  if (g_nranges == g_capranges) {
    const size_t c = g_capranges ? g_capranges * 2 : 64;
    KRange *n = realloc(g_ranges, c * sizeof *n);
    if (!n) return -1;
    g_ranges = n;
    g_capranges = c;
  }
  memmove(g_ranges + at + 1, g_ranges + at, (g_nranges - at) * sizeof *g_ranges);
  g_ranges[at] = r;
  g_nranges++;
  return 0;
}

// Removes [lo, hi) from the table (under g_mu). A range that sticks out keeps its outer parts, each with a fresh alias
// (an alias cannot be split: the parts are re-reported to the library by the caller through `keep`).
static void ranges_cut(uint64_t lo, uint64_t hi, KRange *keep, size_t *nkeep, size_t maxkeep) {
  for (size_t i = 0; i < g_nranges;) {
    const KRange r = g_ranges[i];
    if (r.hi <= lo || r.lo >= hi) { i++; continue; }
    memmove(g_ranges + i, g_ranges + i + 1, (g_nranges - i - 1) * sizeof *g_ranges);
    g_nranges--;
    // The range leaves the lock-free snapshot before its alias is unmapped: a fault handler that looks it up from
    // now on does not find it (one that already holds the pointer is not waited for).
    snap_publish();
    alias_free(r.alias, r.hi - r.lo);
    if (r.lo < lo && *nkeep < maxkeep) keep[(*nkeep)++] = (KRange){r.lo, lo, r.flags, NULL};
    if (hi < r.hi && *nkeep < maxkeep) keep[(*nkeep)++] = (KRange){hi, r.hi, r.flags, NULL};
  }
}

static void range_add(uint64_t lo, uint64_t hi, uint32_t flags) {
  size_t at = 0;
  while (at < g_nranges && g_ranges[at].lo < lo) at++;
  KRange r = {lo, hi, flags, alias_make(lo, hi)};
  if (range_insert(at, r) != 0) { alias_free(r.alias, hi - lo); return; }
  snap_publish();
  const int rc = g_api->register_memory(lo, hi - lo, flags, r.alias);
  if (rc != KYTY_GPU_OK) KLOG("register_memory(0x%llx, 0x%llx, 0x%x) -> %d", (unsigned long long)lo, (unsigned long long)(hi - lo), flags, rc);
  KTRACE("register 0x%llx +0x%llx flags 0x%x alias %p", (unsigned long long)lo, (unsigned long long)(hi - lo), flags, (void *)r.alias);
}

// What the GPU may see, as nps5_anyps5.c decides it for AnyPS5's driver: committed direct, flexible and mmap memory of
// the application area. 0 = not registered.
static uint32_t flags_for(uint64_t g, uint64_t size, int prot, int kind, const char *name) {
  if (prot == 0) return 0;
  uint32_t f = 0;
  switch (kind) {
  case NPS5_EXT_MAPPED: break;
  case NPS5_EXT_FLEXIBLE: f |= KYTY_GPU_MEMORY_FLEXIBLE; break;
  case NPS5_EXT_DIRECT: f |= KYTY_GPU_MEMORY_DIRECT; break;
  default: return 0;
  }
  if (g < USER_ZONE) return 0;
  if (g < AGC_DRIVER_PAGE + AGC_DRIVER_PAGE_BYTES && g + size > AGC_DRIVER_PAGE) return 0;
  if (name && strncmp(name, "agc_", 4) == 0) return 0;
  if (name && strcmp(name, KYTY_DRIVER_DMEM_NAME) == 0) f |= KYTY_GPU_MEMORY_DIRECT; // the library treats it as direct memory
  if (prot & 1) f |= KYTY_GPU_MEMORY_CPU_READ;
  if (prot & 2) f |= KYTY_GPU_MEMORY_CPU_WRITE | KYTY_GPU_MEMORY_CPU_READ;
  // The GPU may read whatever is registered (command buffers and shader binaries also live in plain mmap memory) and
  // write what the guest may write or opened for the GPU (SCE_KERNEL_PROT_GPU_WRITE 0x20).
  f |= KYTY_GPU_MEMORY_GPU_READ;
  if (prot & 0x22) f |= KYTY_GPU_MEMORY_GPU_WRITE;
  return f;
}

// Takes [lo, hi) away from the library and the tables. The library disarms its watches inside unregister_memory (it is
// called first, while the pages still exist); whatever it left is dropped.
static void drop_span(uint64_t lo, uint64_t hi) {
  bool any = false;
  for (size_t i = 0; i < g_nranges; i++)
    if (g_ranges[i].hi > lo && g_ranges[i].lo < hi) { any = true; break; }
  if (!any) return;
  const int rc = g_api->unregister_memory(lo, hi - lo);
  if (rc != KYTY_GPU_OK) KLOG("unregister_memory(0x%llx, 0x%llx) -> %d", (unsigned long long)lo, (unsigned long long)(hi - lo), rc);
  nps5_kyty_watch_forget(lo, hi - lo);
  KRange keep[2];
  size_t nkeep = 0;
  ranges_cut(lo, hi, keep, &nkeep, 2);
  snap_publish();
  KTRACE("unregister 0x%llx +0x%llx", (unsigned long long)lo, (unsigned long long)(hi - lo));
  // The outer parts of a range that was cut in the middle lost their alias with it: report them again (the library
  // replaces what it has for them and learns the new alias).
  for (size_t i = 0; i < nkeep; i++) range_add(keep[i].lo, keep[i].hi, keep[i].flags);
}

static void hook_map(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  (void)user;
  const uint32_t f = flags_for(addr, size, prot, kind, name);
  pthread_mutex_lock(&g_mu);
  for (size_t i = 0; i < g_nranges; i++) // a replayed range that is already registered as it is
    if (g_ranges[i].lo == addr && g_ranges[i].hi == addr + size && g_ranges[i].flags == f) { pthread_mutex_unlock(&g_mu); return; }
  drop_span(addr, addr + size);
  if (f) range_add(addr, addr + size, f);
  pthread_mutex_unlock(&g_mu);
}

static void hook_unmap(void *user, uint64_t addr, uint64_t size) {
  (void)user;
  pthread_mutex_lock(&g_mu);
  drop_span(addr, addr + size);
  pthread_mutex_unlock(&g_mu);
}

static void hook_protect(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  (void)user;
  const uint32_t f = flags_for(addr, size, prot, kind, name);
  const uint64_t hi = addr + size;
  pthread_mutex_lock(&g_mu);
  if (!f) {
    drop_span(addr, hi);
    pthread_mutex_unlock(&g_mu);
    return;
  }
  // Fully inside one registered range: a protection change the library is told about; the alias stays. Anything
  // else (parts were inaccessible and never registered): register the whole span afresh.
  for (size_t i = 0; i < g_nranges; i++) {
    KRange *r = &g_ranges[i];
    if (r->lo > addr || r->hi < hi) continue;
    if (r->lo == addr && r->hi == hi) r->flags = f; // a partial change keeps the range's flags as registered
    if (g_api->protect_memory) {
      const int rc = g_api->protect_memory(addr, size, f);
      if (rc != KYTY_GPU_OK) KLOG("protect_memory(0x%llx, 0x%llx, 0x%x) -> %d", (unsigned long long)addr, (unsigned long long)size, f, rc);
    }
    KTRACE("protect 0x%llx +0x%llx flags 0x%x", (unsigned long long)addr, (unsigned long long)size, f);
    pthread_mutex_unlock(&g_mu);
    return;
  }
  drop_span(addr, hi);
  range_add(addr, hi, f);
  pthread_mutex_unlock(&g_mu);
}

// Two guest ranges share their pages (direct memory mapped twice). A watch on one does not trap a store through the
// other. Not handled in the spike: said in the log so a stale-data bug can be traced to it.
static void hook_alias(void *user, uint64_t a, uint64_t b, uint64_t size) {
  (void)user;
  KLOG("WARNING: guest 0x%llx and 0x%llx (+0x%llx) share pages; page watches cannot see writes through the other address", (unsigned long long)a,
       (unsigned long long)b, (unsigned long long)size);
}

// ---- host callbacks -----------------------------------------------------------------------------------------------

static void cb_log(void *user, int level, const char *message) {
  (void)user;
  fprintf(stderr, "[kyty %d] %s\n", level, message ? message : "");
}
static int cb_add_event(void *user, int64_t equeue, uint32_t kind, int64_t ident, void *udata) {
  (void)user;
  return p_add_event ? p_add_event(equeue, kind, ident, udata) : (int)0x80020009u;
}
static int cb_delete_event(void *user, int64_t equeue, uint32_t kind, int64_t ident) {
  (void)user;
  return p_delete_event ? p_delete_event(equeue, kind, ident) : (int)0x80020009u;
}
static void cb_deliver_event(void *user, uint32_t kind, int64_t ident, uint64_t data) {
  (void)user;
  if (p_deliver_event) p_deliver_event(kind, ident, data);
}
static int cb_query_memory(void *user, uint64_t address, uint64_t *base, uint64_t *size, uint32_t *flags, const void **alias) {
  (void)user;
  int rc = -1;
  pthread_mutex_lock(&g_mu);
  for (size_t i = 0; i < g_nranges; i++) {
    const KRange *r = &g_ranges[i];
    if (address < r->lo || address >= r->hi) continue;
    if (base) *base = r->lo;
    if (size) *size = r->hi - r->lo;
    if (flags) *flags = r->flags;
    if (alias) *alias = r->alias;
    rc = 0;
    break;
  }
  pthread_mutex_unlock(&g_mu);
  return rc;
}
static const char *cb_app0(void *user) {
  (void)user;
  const char *e = getenv("NPS5_APP0_DIR");
  return e && *e ? e : ps4_vfs_get_app_root();
}
static void cb_publish_pad(void *user, int port, const KytyGpuPadState *state) {
  (void)user;
  if (p_publish_pad && state) p_publish_pad(port, state);
}
static void cb_request_exit(void *user, int code) {
  (void)user;
  KLOG("the library asked to exit (code %d)", code);
  if (p_request_exit) p_request_exit(code);
  else exit(code);
}
static const KytyGpuHostInfo *cb_host_info(void *user) {
  (void)user;
  return &g_info;
}
static int cb_protect_guest(void *user, uint64_t address, uint64_t size, uint32_t watch) {
  (void)user;
  return nps5_kyty_watch_set(address, size, watch);
}
static int cb_run_on_main(void *user, void (*fn)(void *), void *arg, int wait) {
  (void)user;
  if (!fn) return -1;
  if (pthread_main_np()) { // already there (start_graphics is called from the main thread at start-up)
    if (wait) fn(arg);
    else dispatch_async_f(dispatch_get_main_queue(), arg, fn);
    return 0;
  }
  if (wait) dispatch_sync_f(dispatch_get_main_queue(), arg, fn);
  else dispatch_async_f(dispatch_get_main_queue(), arg, fn);
  return 0;
}
static uint64_t cb_call_guest(void *user, uint64_t function, const uint64_t *a) {
  (void)user;
  if (!nps5_hostcall_ctx() || !a) {
    KLOG("call_guest(0x%llx) outside a routed call on a guest thread: refused", (unsigned long long)function);
    return ~0ull;
  }
  return nps5_hostcall_run(a[0], a[1], a[2], a[3], a[4], a[5], function);
}

static int cb_fault_load(void *user, uint64_t *address, uint32_t *size) {
  (void)user;
  return nps5_kyty_watch_fault_load(address, size);
}
static int cb_fault_load_apply(void *user, const void *bytes, uint32_t size) {
  (void)user;
  return nps5_kyty_watch_fault_load_apply(bytes, size);
}

static KytyGpuHostCallbacks g_host = {
    .struct_size = sizeof(KytyGpuHostCallbacks),
    .abi_version = KYTY_GPU_ABI_VERSION,
    .log = cb_log,
    .add_gpu_event = cb_add_event,
    .delete_gpu_event = cb_delete_event,
    .deliver_event = cb_deliver_event,
    .query_memory = cb_query_memory,
    .app0_path = cb_app0,
    .donate_guest_range = NULL,
    .publish_pad = cb_publish_pad,
    .request_exit = cb_request_exit,
    .fetch_pad_output = NULL,
    .publish_pad_motion = NULL,
    .host_info = cb_host_info,
    .protect_guest = cb_protect_guest,
    .run_on_main_thread = cb_run_on_main,
    .call_guest = cb_call_guest,
    .fault_load = cb_fault_load,
    .fault_load_apply = cb_fault_load_apply,
};

// ---- runtime wiring -----------------------------------------------------------------------------------------------

static int host_mprotect(void *host, size_t len, int prot) { return mprotect(host, len, prot); }
static int api_fault(uint64_t guest_page, int is_write) { return g_api->handle_fault ? g_api->handle_fault(guest_page, is_write) : 0; }
// Who faults: a guest thread (it has a guest context) or a thread of the library / the host (the library's own command
// processor and workers touching guest memory through the guest's view trap on the library's own watches).
static _Atomic uint64_t g_faults_guest_thread, g_faults_other_thread, g_resolve_ns;
// ---- alias trim (NPS5_KYTY_ALIAS_TRIM_MS=<ms>, default off) ----------------------------------------------------------
// macOS counts a page in phys_footprint once per mapping that has it in the page table: every guest page the library
// has read or written through its alias view counts TWICE (measured: 1 GiB touched through both views = 2049 MiB),
// although it is one physical page. In a level the library reads gigabytes of texture source data through the alias,
// so the footprint the machine guard and the self limit look at overstates the memory by that amount. The trim drops
// the alias views' page-table entries (protection none, then read-write again: the mapping and the pages stay, the
// next access maps the page again), so only pages used through the alias since the last pass count twice. An access
// that lands in the instant between the two protection calls faults and is simply retried.
// A fault being resolved may use the alias from inside the signal handler (emulated accesses, the library's handler
// reading and writing guest bytes); a second fault there would be fatal. So a pass waits until no resolution is in
// flight and holds new ones at the gate (a few milliseconds) while the views are closed.
static _Atomic uint64_t g_trim_passes, g_trim_retries, g_trim_ns, g_trim_skipped;
static bool g_trim_on;
static _Atomic int g_trim_gate, g_alias_users;
static void alias_user_enter(void) {
  for (;;) {
    atomic_fetch_add_explicit(&g_alias_users, 1, memory_order_seq_cst);
    if (!atomic_load_explicit(&g_trim_gate, memory_order_seq_cst)) return;
    atomic_fetch_sub_explicit(&g_alias_users, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&g_trim_gate, memory_order_acquire)) sched_yield();
  }
}
static void alias_user_leave(void) { atomic_fetch_sub_explicit(&g_alias_users, 1, memory_order_seq_cst); }
static bool in_alias(uint64_t host) { // signal handler: no lock
  for (int attempt = 0; attempt < 64; attempt++) {
    const uint64_t s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
    if (s1 & 1) continue;
    const uint32_t n = atomic_load_explicit(&g_snap_n, memory_order_acquire);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
      const SnapRange r = g_snap[i];
      const uint64_t a = (uint64_t)(uintptr_t)r.alias;
      found = a && host >= a && host - a < r.hi - r.lo;
    }
    // The entry reads above must be complete before the sequence is read again (arm64 may reorder plain loads
    // past an acquire load): a torn entry never passes.
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&g_snap_seq, memory_order_acquire) == s1) return found;
  }
  return false;
}
static void *trim_thread(void *arg) {
  const unsigned ms = (unsigned)(uintptr_t)arg;
  for (;;) {
    usleep(ms * 1000u);
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_mu);
    // Never wait with the gate closed: a resolution in flight may be waiting for a thread that would then stand at the
    // gate (k4hx1 hung that way). Look for an instant without one, for at most 250 ms, else skip this pass.
    bool mine = false;
    while (!mine && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0 < 250000000ull) {
      if (atomic_load_explicit(&g_alias_users, memory_order_seq_cst) == 0) {
        atomic_store_explicit(&g_trim_gate, 1, memory_order_seq_cst);
        mine = atomic_load_explicit(&g_alias_users, memory_order_seq_cst) == 0;
        if (!mine) atomic_store_explicit(&g_trim_gate, 0, memory_order_seq_cst);
      }
      if (!mine) sched_yield();
    }
    if (!mine) { pthread_mutex_unlock(&g_mu); atomic_fetch_add_explicit(&g_trim_skipped, 1, memory_order_relaxed); continue; }
    for (size_t i = 0; i < g_nranges; i++) {
      const KRange *r = &g_ranges[i];
      if (!r->alias) continue;
      const mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)r->alias;
      if (mach_vm_protect(mach_task_self(), at, r->hi - r->lo, FALSE, VM_PROT_NONE) == KERN_SUCCESS)
        mach_vm_protect(mach_task_self(), at, r->hi - r->lo, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    }
    atomic_store_explicit(&g_trim_gate, 0, memory_order_seq_cst);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add_explicit(&g_trim_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_trim_passes, 1, memory_order_relaxed);
  }
  return NULL;
}

static int access_resolve(void *uctx, uint64_t host_addr, int is_write) {
  if (g_trim_on && in_alias(host_addr)) { // an access of the library in the instant a trim pass had its view closed
    static _Thread_local uint64_t last;
    static _Thread_local unsigned again;
    again = last == host_addr ? again + 1 : 0;
    last = host_addr;
    if (again < 100000) {
      atomic_fetch_add_explicit(&g_trim_retries, 1, memory_order_relaxed);
      if (again > 8) sched_yield();
      return 1;
    }
  }
  const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (g_trim_on) alias_user_enter();
  const int ours = nps5_kyty_watch_fault_ctx(uctx, host_addr, is_write);
  if (g_trim_on) alias_user_leave();
  if (ours) {
    atomic_fetch_add_explicit(recomp_get_current_ctx() ? &g_faults_guest_thread : &g_faults_other_thread, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_resolve_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, memory_order_relaxed);
  }
  return ours;
}
static void write_begin(void *host, size_t len) { if (g_trim_on) alias_user_enter(); nps5_kyty_watch_prepare(host, len, 1); if (g_trim_on) alias_user_leave(); }
static void write_end(void *host, size_t len) { (void)host; (void)len; } // the pages stay open and are known written
static void read_prepare(const void *host, size_t len) { if (g_trim_on) alias_user_enter(); nps5_kyty_watch_prepare(host, len, 0); if (g_trim_on) alias_user_leave(); }

// NPS5_KYTY_FAULT_LOG=<n> (diagnostic, default off): the first n handle_fault calls that take longer than 1 ms, with the
// thread's call stack (who touches a GPU-owned page), and the first n instructions next to an access-watched page that
// the emulator does not cover. Written from the signal handler with stdio: for diagnosis runs only.
static _Atomic int g_fault_log_left, g_declined_log_left;
static void on_slow(uint64_t guest, int is_write, unsigned kind, uint64_t ns) {
  if (atomic_fetch_sub(&g_fault_log_left, 1) <= 0) return;
  static const char *const kinds[4] = {"faulting page write-watched", "faulting page access-watched", "neighbour write-watched", "neighbour access-watched"};
  char name[64] = "";
  pthread_getname_np(pthread_self(), name, sizeof name);
  fprintf(stderr, "[kyty-seam] slow fault: %.2f ms, %s of guest 0x%llx (%s), %s thread '%s'; stack:\n", (double)ns / 1e6, is_write ? "store" : "load",
          (unsigned long long)guest, kinds[kind & 3], recomp_get_current_ctx() ? "guest" : "library/host", name);
  void *frames[48];
  const int n = backtrace(frames, 48);
  backtrace_symbols_fd(frames, n, 2);
}
static void on_declined(void *uctx, uint64_t guest, int is_write) {
  if (atomic_fetch_sub(&g_declined_log_left, 1) <= 0) return;
  const ucontext_t *uc = uctx;
  uint32_t insn = 0;
  uint64_t pc = 0;
  if (uc && uc->uc_mcontext) {
    pc = arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    memcpy(&insn, (const void *)(uintptr_t)pc, 4);
  }
  Dl_info info = {0};
  dladdr((const void *)(uintptr_t)pc, &info);
  fprintf(stderr, "[kyty-seam] not emulated: instruction 0x%08x at %s+0x%llx (%s), %s of guest 0x%llx\n", insn, info.dli_sname ? info.dli_sname : "?",
          (unsigned long long)(pc - (uint64_t)(uintptr_t)info.dli_saddr), info.dli_fname ? strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname : "?",
          is_write ? "store" : "load", (unsigned long long)guest);
}

static void print_stats(const char *when, const Nps5KytyWatchStats *p, uint64_t pg, uint64_t po, uint64_t pr, double seconds) {
  Nps5KytyWatchStats s;
  nps5_kyty_watch_stats(&s);
  const uint64_t fg = atomic_load(&g_faults_guest_thread), fo = atomic_load(&g_faults_other_thread), rn = atomic_load(&g_resolve_ns);
  const uint64_t faults = s.faults - p->faults, pages = s.fault_pages - p->fault_pages, ons = s.owner_ns - p->owner_ns;
  KLOG("watch %s: %llu sets, %llu host protects, %llu faults (%llu stores, %llu loads; %llu on guest threads, %llu on library/host threads; %llu raced retries), "
       "%llu tracker pages offered (%llu collateral from 16 KiB pages, %llu left armed), in the library's handle_fault %.1f us/page avg (%llu calls > 100 us, max %.2f ms), "
       "resolver total %.1f us/fault, %llu prepared; false sharing: %llu accesses emulated on the alias, %llu not emulable, %llu straddling into a watched page; %llu loads served by the library%s",
       when, (unsigned long long)(s.sets - p->sets), (unsigned long long)(s.host_protects - p->host_protects), (unsigned long long)faults,
       (unsigned long long)(s.write_faults - p->write_faults), (unsigned long long)(faults - (s.write_faults - p->write_faults)), (unsigned long long)(fg - pg),
       (unsigned long long)(fo - po), (unsigned long long)(s.raced - p->raced), (unsigned long long)pages, (unsigned long long)(s.collateral_pages - p->collateral_pages),
       (unsigned long long)(s.unresolved - p->unresolved), pages ? (double)ons / 1000.0 / (double)pages : 0.0, (unsigned long long)(s.slow_calls - p->slow_calls),
       (double)s.owner_max_ns / 1e6, faults ? (double)(rn - pr) / 1000.0 / (double)faults : 0.0, (unsigned long long)(s.prepares - p->prepares), (unsigned long long)(s.emulated - p->emulated),
       (unsigned long long)(s.emu_declined - p->emu_declined), (unsigned long long)(s.straddles - p->straddles), (unsigned long long)(s.loads_served - p->loads_served), "");
  static const char *const kind[4] = {"faulting page write-watched", "faulting page access-watched", "neighbour write-watched", "neighbour access-watched"};
  for (int i = 0; i < 4; i++) {
    const uint64_t c = s.kind_calls[i] - p->kind_calls[i], ns = s.kind_ns[i] - p->kind_ns[i];
    if (c) KLOG("watch %s:   %-28s %6llu calls, %8.1f us avg, %8.2f ms total", when, kind[i], (unsigned long long)c, (double)ns / 1000.0 / (double)c, (double)ns / 1e6);
  }
  if (seconds > 0 && faults) KLOG("watch %s: %.0f faults/s, %.1f ms/s inside handle_fault", when, (double)faults / seconds, (double)ons / 1e6 / seconds);
}

// NPS5_KYTY_STATS=<n> with n > 1: a line every n seconds with the numbers of that interval (a run that ends by a signal
// never reaches the exit line).
static void *stats_thread(void *arg) {
  const unsigned every = (unsigned)(uintptr_t)arg;
  Nps5KytyWatchStats prev = {0};
  uint64_t pg = 0, po = 0, pr = 0;
  for (;;) {
    sleep(every);
    print_stats("interval", &prev, pg, po, pr, (double)every);
    if (g_trim_on)
      KLOG("alias trim: %llu passes (%llu skipped: no quiet instant), %.1f ms in total, %llu accesses retried", (unsigned long long)atomic_load(&g_trim_passes),
           (unsigned long long)atomic_load(&g_trim_skipped), (double)atomic_load(&g_trim_ns) / 1e6, (unsigned long long)atomic_load(&g_trim_retries));
    nps5_kyty_watch_stats(&prev);
    pg = atomic_load(&g_faults_guest_thread);
    po = atomic_load(&g_faults_other_thread);
    pr = atomic_load(&g_resolve_ns);
  }
  return NULL;
}

static void at_exit(void) {
  if (getenv("NPS5_KYTY_STATS")) {
    const Nps5KytyWatchStats zero = {0};
    print_stats("total", &zero, 0, 0, 0, 0);
  }
  // Not while a crash is being reported (the report ends in exit() on the crashing thread): the library's teardown
  // would run on top of whatever state the fault left (seen: a C++ exception from a mutex inside it, which turned
  // the report into "recursive crash").
  if (g_api && g_api->shutdown && !g_nps5_in_crash_handler && !getenv("NPS5_KYTY_NO_SHUTDOWN")) g_api->shutdown();
}

static void *sym(void *image, const char *name) { return image ? dlsym(image, name) : NULL; }

bool nps5_kyty_startup(GuestContext *ctx, void *gpu_image) {
  const char *e = getenv("NPS5_GPU");
  if (!e || strcmp(e, "kyty") != 0) return false;
  g_trace = getenv("NPS5_KYTY_TRACE") && atoi(getenv("NPS5_KYTY_TRACE")) > 0;
  const char *al = getenv("NPS5_KYTY_ALIAS");
  g_want_alias = !(al && strcmp(al, "0") == 0);

  const char *path = getenv("NPS5_KYTY_GPU_LIB");
  // An explicit path, or the copy next to the binary: never a bare name (that would search the working directory
  // and DYLD paths for a library to load).
  const char *tries[] = {path, "@rpath/kyty_gpu.dylib", "@executable_path/kyty_gpu.dylib", "@executable_path/libkyty_gpu.dylib"};
  for (size_t i = path && *path ? 0 : 1; i < sizeof tries / sizeof *tries && !g_lib; i++) {
    g_lib = dlopen(tries[i], RTLD_NOW | RTLD_LOCAL);
    if (!g_lib && i == 0) { fprintf(stderr, "[kyty-seam] FATAL: NPS5_KYTY_GPU_LIB=%s: %s\n", path, dlerror()); abort(); }
  }
  if (!g_lib) { fprintf(stderr, "[kyty-seam] FATAL: NPS5_GPU=kyty but kyty_gpu.dylib is not loadable (%s); set NPS5_KYTY_GPU_LIB\n", dlerror()); abort(); }
  KytyGpuGetApiFn get = (KytyGpuGetApiFn)dlsym(g_lib, "KytyGpuGetApi");
  if (!get) fatal("kyty_gpu has no KytyGpuGetApi");
  g_api = get(KYTY_GPU_ABI_VERSION);
  if (!g_api) fatal("kyty_gpu refused this host's ABI (KytyGpuGetApi returned null)");
  if ((g_api->abi_version >> 16) != KYTY_GPU_ABI_VERSION_MAJOR || g_api->struct_size < sizeof(KytyGpuApi) || (g_api->abi_version & 0xffff) < 6)
    fatal("kyty_gpu is not an ABI 1.6 library (hosted memory): rebuild it with this kytyGpuApi.h");
  // The watch table is built for one host page size.
  if (getpagesize() != (1 << NPS5_KW_HOST_SHIFT)) fatal("host page size is not the one the watch table was built for");
  if (!g_api->init || !g_api->register_memory || !g_api->unregister_memory) fatal("kyty_gpu lacks init/register_memory/unregister_memory");

  const Nps5Arena *a = &g_nps5_arena;
  g_info = (KytyGpuHostInfo){.struct_size = sizeof g_info,
                             .flags = KYTY_GPU_HOST_HOSTED_MEMORY | KYTY_GPU_HOST_MAIN_QUEUE,
                             .guest_bias = (uint64_t)(uintptr_t)a->bias,
                             .guest_lo = a->guest_lo,
                             .guest_hi = a->guest_hi,
                             .host_page_size = (uint32_t)getpagesize(),
                             .tracker_page_size = 1u << NPS5_KW_TRACK_SHIFT};
  p_add_event = (int (*)(int64_t, uint32_t, int64_t, void *))sym(gpu_image, "Nps5KytyHostAddGpuEvent");
  p_delete_event = (int (*)(int64_t, uint32_t, int64_t))sym(gpu_image, "Nps5KytyHostDeleteGpuEvent");
  p_deliver_event = (void (*)(uint32_t, int64_t, uint64_t))sym(gpu_image, "Nps5KytyHostDeliverEvent");
  p_publish_pad = (void (*)(int, const KytyGpuPadState *))sym(gpu_image, "Nps5KytyHostPublishPad");
  p_start_pad_script = (int (*)(void))sym(gpu_image, "Nps5KytyHostStartPadScript");
  p_request_exit = (void (*)(int))sym(gpu_image, "Nps5KytyHostRequestExit");
  if (!p_add_event || !p_deliver_event)
    KLOG("WARNING: libnps5_gpu has no Nps5KytyHost* exports (AnyPS5 branch nps5-kyty-seam): GPU events will not reach the game");
  if (p_start_pad_script && p_start_pad_script() > 0) g_info.flags |= KYTY_GPU_HOST_SCRIPTED_PAD;

  if (!a->flat) {
    // NPS5_KYTY_EMULATE: 0 report every armed neighbour of a faulting tracker page to the library (the first seam);
    // 1 (default) carry accesses next to an access-watched page out on the alias; 2 never report a neighbour.
    const char *em = getenv("NPS5_KYTY_EMULATE");
    const int flog = getenv("NPS5_KYTY_FAULT_LOG") ? atoi(getenv("NPS5_KYTY_FAULT_LOG")) : 0;
    atomic_store(&g_fault_log_left, flog);
    atomic_store(&g_declined_log_left, flog);
    const Nps5KytyWatchConfig wc = {.guest_lo = a->guest_lo, .guest_hi = a->guest_hi, .bias = (uint64_t)(uintptr_t)a->bias, .host_protect = host_mprotect,
                                    .handle_fault = api_fault, .emulate = em ? atoi(em) : 1, .alias_of = g_want_alias ? alias_of : NULL,
                                    .slow_ns = 1000000, .on_slow = atomic_load(&g_fault_log_left) > 0 ? on_slow : NULL,
                                    .on_declined = atomic_load(&g_declined_log_left) > 0 ? on_declined : NULL};
    if (nps5_kyty_watch_init(&wc) != 0) fatal("cannot reserve the page watch table");
  }

  int rc = g_api->init(&g_host);
  if (rc != KYTY_GPU_OK) { fprintf(stderr, "[kyty-seam] FATAL: kyty_gpu init -> %d\n", rc); abort(); }

  // Routes. A NID the library lacks is fatal only when the game calls it.
  g_entries = calloc(nps5_kyty_route_count ? nps5_kyty_route_count : 1, sizeof *g_entries);
  if (!g_entries) fatal("out of memory");
  unsigned bound = 0;
  const bool host_pointers = g_api->pointer_mode == KYTY_GPU_POINTERS_HOST;
  for (unsigned i = 0; i < nps5_kyty_route_count; i++) {
    const Nps5KytyRoute *r = &nps5_kyty_routes[i];
    void *entry = g_api->nid_entry ? g_api->nid_entry(r->nid) : NULL;
    if (!entry) entry = dlsym(g_lib, r->nid);
    g_entries[i] = entry;
    if (entry) bound++;
    else KLOG("kyty_gpu has no entry for %s (%s, %s): a call will stop the process", r->nid, r->name, r->library);
    // Host-pointer mode: the thunk's own marshalling is what the library wants, so it is called directly. Guest
    // mode (default), and every missing entry: through the adapter.
    *r->slot = host_pointers && entry ? entry : r->raw;
  }
  KLOG("NPS5_GPU=kyty: %s, ABI %u.%u, %u of %u exports bound, pointers %s, bias 0x%llx, host page %u, alias views %s", g_api->build ? g_api->build : "?",
       g_api->abi_version >> 16, g_api->abi_version & 0xffff, bound, nps5_kyty_route_count, host_pointers ? "host" : "guest", (unsigned long long)g_info.guest_bias,
       g_info.host_page_size, g_want_alias ? "on" : "off");

  // Guest memory: the watch owner of the GPU-visible ranges is the library, through the runtime's one resolver chain
  // and one set of host access hooks. AnyPS5's registry and write watch are not installed by the caller.
  if (!a->flat) {
    if (!nps5_diag_add_context_resolver(access_resolve)) fatal("no free fault resolver slot");
    const Nps5HostAccessHooks hooks = {.protect = nps5_kyty_watch_guest_protect, .write_begin = write_begin, .write_end = write_end, .read_prepare = read_prepare};
    nps5_mem_set_host_access_hooks(&hooks);
  }
  g_active = true;
  const Nps5VmHooks h = {.on_map = hook_map, .on_protect = hook_protect, .on_unmap = hook_unmap, .user = NULL, .on_alias = hook_alias};
  if (recomp_vm_hooks_register(&h) < 0) fatal("cannot register the VM hooks");
  atexit(at_exit);
  if (getenv("NPS5_KYTY_STATS") && atoi(getenv("NPS5_KYTY_STATS")) > 1) {
    pthread_t th;
    if (pthread_create(&th, NULL, stats_thread, (void *)(uintptr_t)atoi(getenv("NPS5_KYTY_STATS"))) == 0) pthread_detach(th);
  }

  if (g_want_alias && getenv("NPS5_KYTY_ALIAS_TRIM_MS") && atoi(getenv("NPS5_KYTY_ALIAS_TRIM_MS")) > 0) {
    pthread_t th;
    g_trim_on = true;
    if (pthread_create(&th, NULL, trim_thread, (void *)(uintptr_t)atoi(getenv("NPS5_KYTY_ALIAS_TRIM_MS"))) == 0) pthread_detach(th);
    KLOG("alias trim every %d ms (NPS5_KYTY_ALIAS_TRIM_MS): the alias views are not left counted twice in the footprint", atoi(getenv("NPS5_KYTY_ALIAS_TRIM_MS")));
  }
  if (!a->flat && a->mode == NPS5_ADDR_BASE && !getenv("NPS5_KYTY_NO_DRIVER_DMEM")) {
    // Reported to the library by hook_map (flags_for adds DIRECT for this name).
    const uint64_t lo = a->guest_org + KYTY_DRIVER_DMEM;
    if (recomp_vm_alloc_fixed(ctx, lo, KYTY_DRIVER_DMEM_BYTES, 3, 0, KYTY_DRIVER_DMEM_NAME) == lo)
      KLOG("driver memory: guest 0x%llx +0x%x mapped and registered by the host", (unsigned long long)lo, KYTY_DRIVER_DMEM_BYTES);
    else
      KLOG("driver memory: cannot map guest 0x%llx (the library maps it itself)", (unsigned long long)lo);
  }
  if (!getenv("NPS5_KYTY_LAZY_START") && g_api->start_graphics) {
    rc = g_api->start_graphics();
    if (rc != KYTY_GPU_OK) { fprintf(stderr, "[kyty-seam] FATAL: kyty_gpu start_graphics -> %d\n", rc); abort(); }
  }
  return true;
}

#endif // NPS5_HAVE_ANYPS5_AGC
