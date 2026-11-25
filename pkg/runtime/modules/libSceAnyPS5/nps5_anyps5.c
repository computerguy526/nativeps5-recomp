// SPDX-License-Identifier: GPL-2.0-or-later
//
// AnyPS5 integration glue (docs/m2/anyps5-integration.md).
//
// 1. Allocation registration. The AGC driver keeps a registry of GPU-visible guest ranges
//    (AgcGuestMemory.h, keyed by GUEST address). The VM layer (nps5_vm.c) notifies observers
//    (Nps5VmHooks): on_map after the pages exist, on_protect before the host mprotect, on_unmap BEFORE the
//    pages are released, so AgcGuestAllocationUnregister can wait for GPU work that still leases the range.
//    This file mirrors the registered ranges so partial unmaps/protects and PROT_NONE <-> accessible
//    transitions map onto the driver's register/unregister/protect calls.
// 2. Driver memory. The driver maps its own 2 MiB page at guest 0xE_E000_0000 with MAP_FIXED inside
//    our reservation; that guest range is claimed in the extent list first so no guest mapping lands on it.

#include "nps5_anyps5_config.h"

#if NPS5_HAVE_ANYPS5_AGC

#include <dlfcn.h>
#include <pthread.h>
#include <math.h>
#include "nps5_framerate_logic.h"
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nps5_diag.h"
#include "nps5_hostcall.h"
#include "nps5_perf.h"
#include "nps5_thunk.h"
#include "nps5_mem.h"
#include "recomp_runtime.h"
#include "ps4_vfs.h"
#include "ps4_kernel_sys.h"
#include "ps4_sfo.h"
#include "nps5_kyty.h"

#define AGC_GUEST_READ 1u
#define AGC_GUEST_WRITE 2u
#define AGC_GUEST_GPU 4u
#define AGC_GUEST_WATCH 8u // AnyPS5 nps5-coherence: put the range under the driver's page write watch
int AgcGuestAllocationRegister(uint64_t guest, uint64_t bytes, uint32_t flags);
int AgcGuestAllocationUnregister(uint64_t guest, uint64_t bytes);
int AgcGuestAllocationProtect(uint64_t guest, uint64_t bytes, uint32_t flags);
const char *AgcGuestAllocationLastError(void);
// libSceAgcDriver: microseconds spent in draw-time pipeline creation and Vulkan device creation (AnyPS5 nps5-gpufu).
uint64_t AgcDriverStallMicros(void);
// libc.prx (AnyPS5): the write watch's own fault entry, the one its libkernel exception delivery calls: records a write to a
// watched page and unprotects it. Returns true when the fault was the watch's.
bool GuestWriteWatchHandleFault_nid_postfix(uintptr_t address);

// ---- Write watch (docs/m3/writewatch.md; AnyPS5 docs/nps5-coherence.md) -------------------------------------------------
// With the watch the driver knows which guest pages the CPU wrote since it last looked (it write-protects them and takes
// the first store as a fault) and can leave GPU results on the GPU until the CPU reads the pages (it makes them
// inaccessible and takes the first access as a fault). Without it every texture and buffer the driver reuses is compared
// byte by byte with a copy. The entry points are newer than the rest of the interface and are looked up by name, so a
// runtime built from this file still runs on AnyPS5 libraries without them (then nothing is watched, as before).
static int (*p_watch_available)(void);                                   // AgcGuestWriteWatchAvailable
static int (*p_allocation_watch)(uint64_t guest, uint64_t bytes, int w); // AgcGuestAllocationWatch
static bool (*p_handle_access)(uintptr_t host, int is_write);            // GuestWriteWatchHandleAccess_nid_postfix
static int (*p_watch_protect)(void *host, size_t bytes, int prot);       // GuestWriteWatchProtect_nid_postfix
static bool (*p_begin_host_write)(void *host, size_t bytes);             // GuestArenaBeginHostWrite_nid_postfix
static void (*p_end_host_write)(void *host, size_t bytes);               // GuestArenaEndHostWrite_nid_postfix
static void (*p_prepare_host_read)(const void *host, size_t bytes);      // GuestArenaPrepareHostRead_nid_no_patch
static bool g_watch; // ranges are registered with AGC_GUEST_WATCH

// libkernel.prx (AnyPS5): Aps5RunOnMainThread runs a function on the calling thread unless told that the host serves the
// main queue. The runtime's main thread does (ps4_host_run_main_loop: CFRunLoopRun), the guest runs on another thread,
// and SDL's video subsystem and windows must live on the main thread.
void Aps5HostServesMainQueue_nid_no_patch(void);
// libc.prx: guest path prefix -> host directory.
void AddPathAlias_nid_no_patch(const char *guest_prefix, const char *host_path);

#define NPS5_AGC_DRIVER_PAGE 0xEE0000000ULL // MacOsDriverMemoryBase (Driver/DriverMemory.hpp)
#define NPS5_AGC_DRIVER_PAGE_BYTES (2u << 20)
#define NPS5_AGC_USER_ZONE 0x200000000ULL

// Mirror of what is registered with the driver: sorted, disjoint [lo, hi) ranges with their flags. Only touched
// from the VM hooks (serialised: on_map/on_protect by vm_mutex, on_unmap by the busy extents plus g_mu).
typedef struct {
  uint64_t lo, hi;
  uint32_t flags;
} Range;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Range *g_reg;
static size_t g_nreg, g_capreg;
// Guest ranges whose pages are also mapped at another guest address (direct memory mapped twice): never watched. Sorted
// by nothing, may overlap; trimmed when the range is unmapped. Under g_mu.
static Range *g_alias;
static size_t g_nalias, g_capalias;

static bool aliased(uint64_t lo, uint64_t hi) {
  for (size_t i = 0; i < g_nalias; i++)
    if (g_alias[i].lo < hi && lo < g_alias[i].hi) return true;
  return false;
}
static void alias_add(uint64_t lo, uint64_t hi) {
  if (g_nalias == g_capalias) {
    size_t c = g_capalias ? g_capalias * 2 : 16;
    Range *n = realloc(g_alias, c * sizeof *n);
    if (!n) return;
    g_alias = n;
    g_capalias = c;
  }
  g_alias[g_nalias++] = (Range){lo, hi, 0};
}
// Drops [lo, hi) from the alias list (the range is unmapped; what replaces it starts unaliased).
static void alias_cut(uint64_t lo, uint64_t hi) {
  for (size_t i = 0; i < g_nalias;) {
    Range r = g_alias[i];
    if (r.hi <= lo || r.lo >= hi) { i++; continue; }
    g_alias[i] = g_alias[--g_nalias];
    if (r.lo < lo) alias_add(r.lo, lo);
    if (hi < r.hi) alias_add(hi, r.hi);
  }
}
static int g_hook_id = -1;
static int g_trace = -1;

static int trace_on(void) {
  if (g_trace < 0) {
    const char *t = getenv("NPS5_ANYPS5_TRACE"); // 1: calls with results; 2: also at entry
    g_trace = t ? (atoi(t) > 1 ? 2 : atoi(t) > 0) : 0; // "0" is off
  }
  return g_trace;
}

// Which mappings the GPU may see: committed mappings of the application area (direct and flexible
// memory, mmap). Stacks, the image/system zone and the driver's own page are not. Returns the driver flags, or 0
// when the range is not registered (also for protection NONE: nothing for the GPU to read).
static uint32_t flags_for(uint64_t g, uint64_t size, int prot, int kind, const char *name) {
  if (prot == 0) return 0;
  switch (kind) {
  case NPS5_EXT_MAPPED:
  case NPS5_EXT_FLEXIBLE:
  case NPS5_EXT_DIRECT: break;
  default: return 0;
  }
  if (g < NPS5_AGC_USER_ZONE) return 0;
  if (g < NPS5_AGC_DRIVER_PAGE + NPS5_AGC_DRIVER_PAGE_BYTES && g + size > NPS5_AGC_DRIVER_PAGE) return 0;
  if (name && strncmp(name, "agc_", 4) == 0) return 0;
  uint32_t f = ((prot & 3) ? AGC_GUEST_READ : 0) | ((prot & 2) ? AGC_GUEST_WRITE : 0);
  if ((prot & 0x30) || kind != NPS5_EXT_MAPPED) f |= AGC_GUEST_GPU;
  if ((f & AGC_GUEST_WRITE) && !(f & AGC_GUEST_READ)) f |= AGC_GUEST_READ;
  return f ? f : AGC_GUEST_READ;
}

static int reg_insert(size_t at, uint64_t lo, uint64_t hi, uint32_t flags) {
  if (g_nreg == g_capreg) {
    size_t c = g_capreg ? g_capreg * 2 : 64;
    Range *n = realloc(g_reg, c * sizeof *n);
    if (!n) return -1;
    g_reg = n;
    g_capreg = c;
  }
  memmove(g_reg + at + 1, g_reg + at, (g_nreg - at) * sizeof *g_reg);
  g_reg[at] = (Range){lo, hi, flags};
  g_nreg++;
  return 0;
}

static void register_range(uint64_t lo, uint64_t hi, uint32_t flags) {
  // Watched: what the GPU may see, unless the same pages are mapped at a second address. The mirror keeps the plain flags.
  const uint32_t watch = g_watch && (flags & AGC_GUEST_GPU) && !aliased(lo, hi) ? AGC_GUEST_WATCH : 0;
  if (AgcGuestAllocationRegister(lo, hi - lo, flags | watch) != 0) {
    fprintf(stderr, "[anyps5] AgcGuestAllocationRegister(0x%llx, 0x%llx) failed: %s\n", (unsigned long long)lo, (unsigned long long)(hi - lo),
            AgcGuestAllocationLastError());
    return;
  }
  size_t at = 0;
  while (at < g_nreg && g_reg[at].lo < lo) at++;
  if (reg_insert(at, lo, hi, flags) != 0) AgcGuestAllocationUnregister(lo, 0);
  if (trace_on()) fprintf(stderr, "[anyps5] register 0x%llx +0x%llx flags %u\n", (unsigned long long)lo, (unsigned long long)(hi - lo), flags);
}

// Removes [lo, hi) from the registry and the mirror (splitting entries that stick out on either side).
static void unregister_span(uint64_t lo, uint64_t hi) {
  for (size_t i = 0; i < g_nreg;) {
    Range r = g_reg[i];
    if (r.hi <= lo || r.lo >= hi) { i++; continue; }
    const uint64_t a = r.lo > lo ? r.lo : lo, b = r.hi < hi ? r.hi : hi;
    AgcGuestAllocationUnregister(a, a == r.lo && b == r.hi ? 0 : b - a);
    if (trace_on()) fprintf(stderr, "[anyps5] unregister 0x%llx +0x%llx\n", (unsigned long long)a, (unsigned long long)(b - a));
    memmove(g_reg + i, g_reg + i + 1, (g_nreg - i - 1) * sizeof *g_reg);
    g_nreg--;
    if (r.lo < a && reg_insert(i++, r.lo, a, r.flags) != 0) continue;
    if (b < r.hi && reg_insert(i++, b, r.hi, r.flags) != 0) continue;
  }
}

static void hook_map(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  (void)user;
  const uint32_t f = flags_for(addr, size, prot, kind, name);
  pthread_mutex_lock(&g_mu);
  for (size_t i = 0; i < g_nreg; i++) // a replayed range already registered
    if (g_reg[i].lo == addr && g_reg[i].hi == addr + size && g_reg[i].flags == f) { pthread_mutex_unlock(&g_mu); return; }
  unregister_span(addr, addr + size);
  if (f) register_range(addr, addr + size, f);
  pthread_mutex_unlock(&g_mu);
}

static void hook_unmap(void *user, uint64_t addr, uint64_t size) {
  (void)user;
  pthread_mutex_lock(&g_mu);
  unregister_span(addr, addr + size);
  alias_cut(addr, addr + size);
  pthread_mutex_unlock(&g_mu);
}

// Two guest ranges share their pages: neither can be watched (a store through one does not fault on the other). `a` is the
// new mapping (reported before its on_map), `b` an older one that may be registered and watched.
static void hook_alias(void *user, uint64_t a, uint64_t b, uint64_t size) {
  (void)user;
  if (!g_watch) return;
  pthread_mutex_lock(&g_mu);
  alias_add(a, a + size);
  alias_add(b, b + size);
  const uint64_t r[2] = {a, b};
  for (int k = 0; k < 2; k++)
    for (size_t i = 0; i < g_nreg; i++) { // the registered parts only
      const uint64_t lo = g_reg[i].lo > r[k] ? g_reg[i].lo : r[k], hi = g_reg[i].hi < r[k] + size ? g_reg[i].hi : r[k] + size;
      if (lo >= hi) continue;
      if (p_allocation_watch(lo, hi - lo, 0) != 0)
        fprintf(stderr, "[anyps5] AgcGuestAllocationWatch(0x%llx, 0x%llx, 0) failed: %s\n", (unsigned long long)lo, (unsigned long long)(hi - lo), AgcGuestAllocationLastError());
      if (trace_on()) fprintf(stderr, "[anyps5] unwatch 0x%llx +0x%llx (aliased)\n", (unsigned long long)lo, (unsigned long long)(hi - lo));
    }
  pthread_mutex_unlock(&g_mu);
}

// Sets the flags of [a, b), which lies inside one mirror entry (splitting it).
static void set_flags(uint64_t a, uint64_t b, uint32_t f) {
  for (size_t i = 0; i < g_nreg; i++) {
    const Range r = g_reg[i];
    if (r.lo > a || r.hi < b) continue;
    memmove(g_reg + i, g_reg + i + 1, (g_nreg - i - 1) * sizeof *g_reg);
    g_nreg--;
    if (r.lo < a) reg_insert(i++, r.lo, a, r.flags);
    reg_insert(i++, a, b, f);
    if (b < r.hi) reg_insert(i, b, r.hi, r.flags);
    return;
  }
}

static void hook_protect(void *user, uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  (void)user;
  const uint32_t f = flags_for(addr, size, prot, kind, name);
  const uint64_t hi = addr + size;
  pthread_mutex_lock(&g_mu);
  if (!f) { // no longer visible to the GPU
    unregister_span(addr, hi);
    pthread_mutex_unlock(&g_mu);
    return;
  }
  // Tracked parts change flags; untracked parts (the range was PROT_NONE) are registered now.
  Range *parts = NULL;
  size_t np = 0;
  for (size_t i = 0; i < g_nreg; i++)
    if (g_reg[i].hi > addr && g_reg[i].lo < hi) np++;
  if (np && !(parts = malloc(np * sizeof *parts))) { pthread_mutex_unlock(&g_mu); return; }
  np = 0;
  for (size_t i = 0; i < g_nreg; i++)
    if (g_reg[i].hi > addr && g_reg[i].lo < hi)
      parts[np++] = (Range){g_reg[i].lo > addr ? g_reg[i].lo : addr, g_reg[i].hi < hi ? g_reg[i].hi : hi, g_reg[i].flags};
  uint64_t cur = addr;
  for (size_t i = 0; i < np; i++) {
    if (parts[i].lo > cur) register_range(cur, parts[i].lo, f);
    if (parts[i].flags != f) {
      if (AgcGuestAllocationProtect(parts[i].lo, parts[i].hi - parts[i].lo, f) == 0) {
        set_flags(parts[i].lo, parts[i].hi, f);
        if (trace_on()) fprintf(stderr, "[anyps5] protect 0x%llx +0x%llx flags %u\n", (unsigned long long)parts[i].lo, (unsigned long long)(parts[i].hi - parts[i].lo), f);
      } else
        fprintf(stderr, "[anyps5] AgcGuestAllocationProtect(0x%llx, 0x%llx) failed: %s\n", (unsigned long long)parts[i].lo,
                (unsigned long long)(parts[i].hi - parts[i].lo), AgcGuestAllocationLastError());
    }
    cur = parts[i].hi;
  }
  if (cur < hi) register_range(cur, hi, f);
  free(parts);
  pthread_mutex_unlock(&g_mu);
}

// Installs the VM hooks (core/nps5_vm.c): everything mapped now is registered by the replay, later maps, protection
// changes and unmaps follow. The unmap hook runs before the host pages are released.
// The driver's write watch (libc.prx GuestWriteWatch) write-protects the guest pages it watches and resolves the first
// write in a SIGSEGV/SIGBUS handler. nps5_diag's crash handler is installed above it, so it must offer such faults to the
// watch before it reports a crash (nps5_diag_add_fault_resolver); the watch's own sigaction handler stays as the fallback.
static bool write_watch_resolve(uint64_t host_addr) { return GuestWriteWatchHandleFault_nid_postfix((uintptr_t)host_addr); }
// The same with read traps (pages the driver made inaccessible until their GPU results are stored).
static bool watch_access_resolve(uint64_t host_addr, int is_write) { return p_handle_access((uintptr_t)host_addr, is_write); }
static void watch_write_begin(void *host, size_t len) { (void)p_begin_host_write(host, len); }
static void watch_write_end(void *host, size_t len) { p_end_host_write(host, len); }
static void watch_read_prepare(const void *host, size_t len) { p_prepare_host_read(host, len); }

// Decides whether guest memory is watched and wires the runtime side of it; before the VM hooks register anything.
// NPS5_AGC_WRITE_WATCH=0 turns it off (the driver reads the same variable).
static void nps5_anyps5_setup_watch(void) {
  const char *e = getenv("NPS5_AGC_WRITE_WATCH");
  if (e && strcmp(e, "0") == 0) return;
  p_watch_available = (int (*)(void))dlsym(RTLD_DEFAULT, "AgcGuestWriteWatchAvailable");
  p_allocation_watch = (int (*)(uint64_t, uint64_t, int))dlsym(RTLD_DEFAULT, "AgcGuestAllocationWatch");
  p_handle_access = (bool (*)(uintptr_t, int))dlsym(RTLD_DEFAULT, "GuestWriteWatchHandleAccess_nid_postfix");
  p_watch_protect = (int (*)(void *, size_t, int))dlsym(RTLD_DEFAULT, "GuestWriteWatchProtect_nid_postfix");
  p_begin_host_write = (bool (*)(void *, size_t))dlsym(RTLD_DEFAULT, "GuestArenaBeginHostWrite_nid_postfix");
  p_end_host_write = (void (*)(void *, size_t))dlsym(RTLD_DEFAULT, "GuestArenaEndHostWrite_nid_postfix");
  p_prepare_host_read = (void (*)(const void *, size_t))dlsym(RTLD_DEFAULT, "GuestArenaPrepareHostRead_nid_no_patch");
  if (!p_watch_available || !p_allocation_watch || !p_handle_access || !p_watch_protect || !p_begin_host_write || !p_end_host_write ||
      !p_prepare_host_read) {
    if (trace_on()) fprintf(stderr, "[anyps5] write watch: not offered by these AnyPS5 libraries; guest memory is compared\n");
    return;
  }
  if (!p_watch_available()) return;
  if (g_nps5_arena.flat) return; // the test arena has no host protections to track
  if (!nps5_diag_add_access_resolver(watch_access_resolve)) {
    fprintf(stderr, "[ps4-recomp] WARNING: no free fault resolver slot; the AGC write watch stays off\n");
    return;
  }
  const Nps5HostAccessHooks hooks = {.protect = p_watch_protect, .write_begin = watch_write_begin, .write_end = watch_write_end,
                                     .read_prepare = watch_read_prepare};
  nps5_mem_set_host_access_hooks(&hooks);
  g_watch = true;
  if (trace_on()) fprintf(stderr, "[anyps5] write watch: on for GPU-visible guest memory\n");
}

// AnyPS5 stamps the title's VideoOut flip/vblank status (processTimeCounter, processTime, tsc) and AMPR timestamps with the
// process-time and TSC exports of its libkernel. Those must be the clocks the title itself reads (docs/m3/clock.md: 2.13 GHz
// ticks, GPU-stall credit excluded), not AnyPS5's own 1 GHz cntvct clock: UE's frame pacer derives its sleep from a vblank
// stamp and its own clock reads, and two clocks at different rates made it sleep for hundreds of ms (docs/m3/stall.md).
typedef struct {
  uint64_t (*process_time)(void);
  uint64_t (*process_time_counter)(void);
  uint64_t (*counter_frequency)(void);
  uint64_t (*read_tsc)(void);
  uint64_t (*tsc_frequency)(void);
} Nps5AnyPS5GuestClock;
static const Nps5AnyPS5GuestClock g_anyps5_guest_clock = {sceKernelGetProcessTime, sceKernelGetProcessTimeCounter, sceKernelGetProcessTimeCounterFrequency,
                                                           sceKernelReadTsc, sceKernelGetTscFrequency};

static void nps5_anyps5_install_guest_clock(void) {
  if (getenv("NPS5_ANYPS5_OWN_CLOCK")) return; // A/B switch: AnyPS5 keeps stamping with its own clock
  void *k = dlopen("@rpath/libkernel.prx", RTLD_NOW | RTLD_NOLOAD);
  void (*set)(const Nps5AnyPS5GuestClock *) = k ? (void (*)(const Nps5AnyPS5GuestClock *))dlsym(k, "KernelSetGuestClock_nid_postfix") : NULL;
  if (!set) {
    fprintf(stderr, "[anyps5] WARNING: AnyPS5 libkernel has no KernelSetGuestClock; flip/vblank stamps use its own clock\n");
    return;
  }
  set(&g_anyps5_guest_clock);
}

static void nps5_anyps5_install_hooks(void) {
  Nps5VmHooks h = {.on_map = hook_map, .on_protect = hook_protect, .on_unmap = hook_unmap, .user = NULL, .on_alias = hook_alias};
  g_hook_id = recomp_vm_hooks_register(&h);
  if (g_hook_id < 0) fprintf(stderr, "[ps4-recomp] WARNING: cannot register the AGC allocation hooks\n");
}

void nps5_anyps5_leave(GuestContext *ctx, const char *name) {
  nps5_hostcall_leave(ctx);
  if (trace_on()) { // the clock credit grows while shaders compile: log every further 250 ms of it
    static uint64_t shown;
    const uint64_t c = AgcDriverStallMicros();
    if (c >= shown + 250000) {
      shown = c;
      fprintf(stderr, "[anyps5] host GPU setup time excluded from the guest clock so far: %.0f ms (process time %llu us)\n", c / 1000.0,
              (unsigned long long)sceKernelGetProcessTime());
    }
  }
  if (trace_on()) fprintf(stderr, "[anyps5] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) -> 0x%llx\n", name, (unsigned long long)ctx->rdi, (unsigned long long)ctx->rsi,
                          (unsigned long long)ctx->rdx, (unsigned long long)ctx->rcx, (unsigned long long)ctx->rax);
}

// Real-time game speed (opt-in, per title). Astro Bot advances a fixed 1/60 s per frame from one global block
// {f64 rate (60.0), f32 1/rate, pad, u64 period in us (16666)}; below 60 fps the game runs in slow motion. With
// NPS5_FRAMERATE_BLOCK=<guest addr hex> (profile game.tsv key framerate_block; Astro's is 93d4040 of 01.007; the old
// NPS5_ASTRO_REALTIME=1 is a deprecated alias of it, see nps5_framerate_cfg.h), every
// AGC flip packet (one per game frame) measures the wall-clock frame time, smooths it, and rewrites the three fields
// consistently, so game time follows real time. The block layout is verified before use; if the expected values are not
// there the feature switches itself off with a log line. Clamp 20..60 fps. NPS5_FRAMERATE_MIN / _MAX override the clamp.
// Not covered: cloth (ClothSimulation dt) keeps seeing the real dt.
static struct {
  int state; // 0 = not read yet, 1 = active, -1 = off
  uint64_t addr, last_ns, last_log_ns;
  double ema, cur, lo, hi;
  uint32_t frames, tries, rewrites;
  Nps5Fr l;
  int no_freeze_av, no_freeze_load, last_frozen;
  Nps5FrLoad load;
} g_fr;

// Freeze sources (see nps5_framerate_logic.h). Weak refs: the video shim and the VFS counters live in other modules.
extern int nps5_avplayer_playing_count(void) __attribute__((weak_import));
extern _Atomic int nps5_vfs_phase __attribute__((weak_import));
extern _Atomic unsigned long long nps5_vfs_bytes_read __attribute__((weak_import));
// Loading = a new levels/<name> phase appeared in the last 5 s, or the VFS read more than 6 MB within a second (+2 s);
// the VFS part is capped per run of busy seconds (NPS5_FRAMERATE_LOAD_MAX_MS, default 8000, 0 = no cap), after that it
// counts as streaming until 5 quiet seconds (nps5_fr_load_step). Returns 0, or 1 video, 2 phase, 3 VFS volume.
static int nps5_framerate_frozen(uint64_t now) {
  if (!g_fr.no_freeze_av && nps5_avplayer_playing_count && nps5_avplayer_playing_count() > 0) return 1;
  if (g_fr.no_freeze_load) return 0;
  if (&nps5_vfs_bytes_read && &nps5_vfs_phase) {
      const int r = nps5_fr_load_step(&g_fr.load, now, atomic_load_explicit(&nps5_vfs_phase, memory_order_relaxed),
                                      atomic_load_explicit(&nps5_vfs_bytes_read, memory_order_relaxed));
      return r ? r + 1 : 0;
  }
  return 0;
}

#include "nps5_framerate_cfg.h"
static void nps5_framerate_init(void) {
  int alias = 0;
  g_fr.addr = nps5_framerate_cfg(getenv("NPS5_FRAMERATE_BLOCK"), getenv("NPS5_ASTRO_REALTIME"), &alias);
  if (alias) fprintf(stderr, "[framerate] NPS5_ASTRO_REALTIME is deprecated: use NPS5_FRAMERATE_BLOCK=<guest addr hex> (profile key framerate_block); using 0x%llx\n", (unsigned long long)g_fr.addr);
  g_fr.state = g_fr.addr ? 1 : -1;
  if (!nps5_framerate_clamp(getenv("NPS5_FRAMERATE_MIN"), getenv("NPS5_FRAMERATE_MAX"), &g_fr.lo, &g_fr.hi))
    fprintf(stderr, "[framerate] NPS5_FRAMERATE_MIN/MAX not within 1 <= min <= max <= 240: using 20..60\n");
  nps5_fr_defaults(&g_fr.l, g_fr.lo, g_fr.hi);
  // NPS5_FRAMERATE_LEGACY=1 old behaviour; _NO_FREEZE_AV / _NO_FREEZE_LOAD=1 keep one freeze off; _EMA (0.02), _HYST_PCT (5),
  // _HYST_MS (1000), _WARM (60 frames without a rewrite after a freeze) tune the smoothing (nps5_fr_tune checks the ranges).
  if (getenv("NPS5_FRAMERATE_LEGACY") && atoi(getenv("NPS5_FRAMERATE_LEGACY")) > 0) g_fr.l.legacy = 1;
  g_fr.no_freeze_av = getenv("NPS5_FRAMERATE_NO_FREEZE_AV") && atoi(getenv("NPS5_FRAMERATE_NO_FREEZE_AV")) > 0;
  g_fr.no_freeze_load = getenv("NPS5_FRAMERATE_NO_FREEZE_LOAD") && atoi(getenv("NPS5_FRAMERATE_NO_FREEZE_LOAD")) > 0;
  double load_ms = 8000.0;
  if (nps5_fr_tune_one(getenv("NPS5_FRAMERATE_LOAD_MAX_MS"), 0.0, 600000.0, 0, &load_ms))
      fprintf(stderr,
              "[framerate] NPS5_FRAMERATE_LOAD_MAX_MS (0..600000, 0 = no cap) not a number in range: using 8000\n");
  nps5_fr_load_init(&g_fr.load, (uint64_t)(load_ms * 1e6));
  if (nps5_fr_tune(&g_fr.l, getenv("NPS5_FRAMERATE_EMA"), getenv("NPS5_FRAMERATE_HYST_PCT"), getenv("NPS5_FRAMERATE_HYST_MS"), getenv("NPS5_FRAMERATE_WARM")))
    fprintf(stderr, "[framerate] NPS5_FRAMERATE_EMA (0 < v <= 1) / _HYST_PCT (0..50) / _HYST_MS (0..60000) / _WARM (0..36000): a value outside its range is ignored, the default stays\n");
}

static void nps5_framerate_tick(GuestContext *ctx) {
  if (g_fr.state < 0) return;
  if (g_fr.state == 0) { nps5_framerate_init(); if (g_fr.state < 0) return; }
  const int blk_ok = RECOMP_MEM_OK(ctx) && g_fr.addr != 0 && g_fr.addr < ctx->mem_size && ctx->mem_size - g_fr.addr >= 24;
  uint8_t *b = blk_ok ? (uint8_t *)NPS5_G2H(ctx, g_fr.addr) : NULL;
  const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (!b) { g_fr.state = -1; fprintf(stderr, "[framerate] block 0x%llx not mappable: feature off\n", (unsigned long long)g_fr.addr); return; }
  if (g_fr.frames == 0) {
    double d; float f; uint64_t us;
    memcpy(&d, b, 8); memcpy(&f, b + 8, 4); memcpy(&us, b + 16, 8);
    if (d != 60.0 || f != (float)(1.0 / 60.0) || (us != 16666 && us != 16667)) {
      if (++g_fr.tries < 300) return; // the title may not have initialised its data yet
      g_fr.state = -1;
      fprintf(stderr, "[framerate] block 0x%llx has unexpected values (f64 %g, f32 %g, u64 %llu): feature off\n", (unsigned long long)g_fr.addr, d, (double)f,
              (unsigned long long)us);
      return;
    }
    g_fr.cur = 60.0; g_fr.ema = 60.0; g_fr.l.cur = 60.0; g_fr.l.ema = 60.0; g_fr.l.last_ns = now;
    fprintf(stderr, "[framerate] block 0x%llx verified before: f64 %g, f32 %.9g, u64 %llu; real-time speed on, clamp %.0f..%.0f fps\n", (unsigned long long)g_fr.addr, d,
            (double)f, (unsigned long long)us, g_fr.lo, g_fr.hi);
    g_fr.last_ns = now; g_fr.last_log_ns = now;
    g_fr.frames = 1;
    return;
  }
  g_fr.frames++;
  const int frozen = nps5_framerate_frozen(now);
  g_fr.last_frozen = frozen;
  double want = 0, inst = 1.0 / (((double)(now - g_fr.last_ns) * 1e-9) + 1e-9);
  const int rw = nps5_fr_step(&g_fr.l, now, frozen, &want);
  g_fr.last_ns = now;
  g_fr.ema = g_fr.l.ema;
  if (rw) {
    double d = want; float f = (float)(1.0 / want); uint64_t us = (uint64_t)(1e6 / want);
    memcpy(b, &d, 8); memcpy(b + 8, &f, 4); memcpy(b + 16, &us, 8);
    g_fr.cur = want; g_fr.rewrites++;
  }
  if (now - g_fr.last_log_ns >= 5000000000ull) {
    double d; float f; uint64_t us;
    memcpy(&d, b, 8); memcpy(&f, b + 8, 4); memcpy(&us, b + 16, 8);
    static const char *const fz[] = {"-", "video", "phase", "vfs"};
    fprintf(stderr,
            "[framerate] frame %u measured %.1f fps (ema %.1f) block now f64 %.3f f32 %.9g u64 %llu (%u rewrites) "
            "frozen %s%s\n",
            g_fr.frames, inst, g_fr.ema, d, (double)f, (unsigned long long)us, g_fr.rewrites, fz[g_fr.last_frozen & 3],
            g_fr.load.streaming ? " streaming" : "");
    g_fr.last_log_ns = now;
  }
}

void nps5_rscale_flip(void); // core/nps5_rscale.c
int nps5_tick_in_root(void)
    __attribute__((weak)); // nps5_physsub.c: this thread is inside the F2 update root (dtscan section 4.3 gate)
void nps5_tick_agc_note(const char *name) __attribute__((weak)); // records the sceAgc* call made inside the root
void nps5_tick_flip_bt(GuestContext *ctx)
    __attribute__((weak)); // NPS5_PHYS_FLIP_BT=1: one guest backtrace at the 600th flip
void nps5_anyps5_enter(GuestContext *ctx, const char *name) {
    if (nps5_tick_in_root && nps5_tick_in_root()) nps5_tick_agc_note(name);
    if (name[3] == 'A' && !strcmp(name, "sceAgcDcbSetFlip")) {
        if (g_fr.state >= 0) nps5_framerate_tick(ctx);
        if (nps5_tick_flip_bt) nps5_tick_flip_bt(ctx);
        nps5_rscale_flip(); // core/nps5_rscale.c: render-scale controller (no-op unless the profile's render_scale is live)
    }
  nps5_hostcall_enter(ctx); // AnyPS5 calls guest callbacks (Agc command buffer out-of-space) by pointer
  if (trace_on() >= 2) fprintf(stderr, "[anyps5] > %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)\n", name, (unsigned long long)ctx->rdi, (unsigned long long)ctx->rsi,
                               (unsigned long long)ctx->rdx, (unsigned long long)ctx->rcx, (unsigned long long)ctx->r8, (unsigned long long)ctx->r9);
}

// Per-title GPU caches (docs/nps5-pipeline-cache.md in AnyPS5): the shader disk cache, the VkPipelineCache and journal, and
// under KosmicKrisp the Metal pipeline archive (KK_PIPELINE_ARCHIVE) and the prebuilt Metal libraries (KK_METALLIB_DIR)
// all live below ANYPS5_SHADER_CACHE_DIR/<source version>/ (the driver derives the KK paths, keyed by device, driver
// build and OS build, and calls setenv with overwrite 0, so a value set here or by the user wins). Where that directory
// is, in order: ANYPS5_SHADER_CACHE_DIR; NPS5_GPU_CACHE_DIR; the title's own directory
// ~/Library/Application Support/nativeps5/<titleId>/gpu-cache when the title id is known (NPS5_TITLE_ID, else the SFO), so a
// title keeps its caches wherever it is launched from; else $PS4RECOMP_CACHE_DIR/anyps5.
// APS5_KK_COLLECT_MSL=1 by default: every Metal library the cache misses is written as MSL next to the archive, so
// tools/nps5_kk_prewarm.sh can build prebuilt libraries (which also make a cold OS Metal cache cost nothing).
static void nps5_anyps5_setup_gpu_cache(void) {
  if (!getenv("ANYPS5_SHADER_CACHE_DIR")) {
    char dir[1100] = {0};
    const char *explicit_dir = getenv("NPS5_GPU_CACHE_DIR");
    if (explicit_dir && *explicit_dir) {
      snprintf(dir, sizeof dir, "%s", explicit_dir);
    } else {
      char tid[64] = {0};
      const char *e = getenv("NPS5_TITLE_ID");
      if (e && *e) snprintf(tid, sizeof tid, "%s", e);
      else {
        Ps4SfoContext sfo = {0};
        if (ps4_sfo_find_and_load(&sfo)) {
          ps4_sfo_get_string(&sfo, "TITLE_ID", tid, sizeof tid);
          ps4_sfo_free(&sfo);
        }
      }
      const char *home = getenv("HOME");
      int ok = tid[0] && home && *home;
      for (char *c = tid; ok && *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '-' || *c == '_')) ok = 0;
      if (ok) snprintf(dir, sizeof dir, "%s/Library/Application Support/nativeps5/%s/gpu-cache", home, tid);
      else if (getenv("PS4RECOMP_CACHE_DIR")) snprintf(dir, sizeof dir, "%s/anyps5", getenv("PS4RECOMP_CACHE_DIR"));
    }
    if (dir[0]) setenv("ANYPS5_SHADER_CACHE_DIR", dir, 0);
  }
  setenv("APS5_KK_COLLECT_MSL", "1", 0);
}

void shim_aps5_sceVideoOutSubmitFlip(GuestContext *ctx);
void shim_aps5_sceAgcDcbSetFlip(GuestContext *ctx);

void nps5_anyps5_startup(GuestContext *ctx) {
  // Frame boundaries of the performance trace: a CPU-buffer flip, or the flip packet of a command buffer (AGC titles
  // flip from the DCB and never call sceVideoOutSubmitFlip).
  nps5_perf_register_flip_fn((void *)shim_aps5_sceVideoOutSubmitFlip);
  nps5_perf_register_flip_fn((void *)shim_aps5_sceAgcDcbSetFlip);
  const Nps5Arena *a = &g_nps5_arena;
  if (!a->flat && a->mode == NPS5_ADDR_BASE) {
    const uint64_t lo = a->guest_org + NPS5_AGC_DRIVER_PAGE;
    // Address space only: the driver maps its page here itself (MAP_FIXED inside the reservation).
    if (recomp_vm_alloc_fixed(ctx, lo, NPS5_AGC_DRIVER_PAGE_BYTES, 0, 0, "agc_driver_page") == lo)
      nps5_mem_decommit(lo, NPS5_AGC_DRIVER_PAGE_BYTES);
    else
      fprintf(stderr, "[ps4-recomp] WARNING: cannot claim the AGC driver page at guest 0x%llx\n", (unsigned long long)lo);
  }
  // VideoOut, Pad and UserService live in libnps5_gpu.dylib (their names would collide with the runtime's own
  // HLE of the same names at link time): bind the thunks to its exports.
  void *gpu = dlopen("@rpath/libnps5_gpu.dylib", RTLD_NOW | RTLD_LOCAL);
  if (!gpu) {
    fprintf(stderr, "[ps4-recomp] FATAL: cannot load libnps5_gpu.dylib: %s\n", dlerror());
    abort();
  }
  if (nps5_anyps5_bind_exports(gpu)) fprintf(stderr, "[ps4-recomp] WARNING: some AnyPS5 exports are missing (see above)\n");
  if (nps5_anyps5_bind_kernel()) fprintf(stderr, "[ps4-recomp] WARNING: AnyPS5 libkernel equeue exports are missing\n");
  Aps5HostServesMainQueue_nid_no_patch();
  nps5_anyps5_install_guest_clock();
  // NPS5_GPU=kyty (off by default): KytyPS5's GPU stack takes the graphics exports and is the one owner of the
  // GPU-visible guest ranges (its page watch, through the same resolver chain and host access hooks). AnyPS5's
  // allocation registry and write watch are then not installed at all; its driver is never started because none of
  // its graphics exports is called. Everything else (libkernel, pad, user service, libc) stays AnyPS5.
  if (!nps5_kyty_startup(ctx, gpu)) {
    nps5_anyps5_setup_watch();
    nps5_anyps5_install_hooks();
    if (!g_watch) nps5_diag_add_fault_resolver(write_watch_resolve);
  }
  // AnyPS5's libc maps guest paths onto the host: /app0 is the application root of the translated title.
  const char *app = ps4_vfs_get_app_root();
  if (app && *app) AddPathAlias_nid_no_patch("/app0", app);
  nps5_anyps5_setup_gpu_cache();
  // A cold first launch builds shaders and Metal pipelines for seconds while the title waits for its first flip with a
  // time-limit of its own (Calculator3D: 5 s of sceKernelGetProcessTime). That build time is host-only work: the
  // title's process-time clocks exclude it (NPS5_NO_STALL_CREDIT=1 turns that off).
  if (!getenv("NPS5_NO_STALL_CREDIT")) nps5_set_guest_clock_credit(AgcDriverStallMicros);
  // Vulkan loader for SDL (docs/nps5-video-pad.md); the ICD (KosmicKrisp) comes from the environment.
  if (!getenv("SDL_VULKAN_LIBRARY") && access("/opt/homebrew/lib/libvulkan.dylib", R_OK) == 0)
    setenv("SDL_VULKAN_LIBRARY", "/opt/homebrew/lib/libvulkan.dylib", 0);
}

#endif // NPS5_HAVE_ANYPS5_AGC
