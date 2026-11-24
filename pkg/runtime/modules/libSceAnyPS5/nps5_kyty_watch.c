// SPDX-License-Identifier: GPL-2.0-or-later
//
// Page watch table of the Kyty GPU seam: 4 KiB tracker pages on 16 KiB host pages. See nps5_kyty_watch.h.

#include "nps5_kyty_watch.h"
#include "nps5_kyty_emu.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define HOST_PAGE (1ull << NPS5_KW_HOST_SHIFT)
#define TRACK_PAGE (1ull << NPS5_KW_TRACK_SHIFT)
#define SUBS 4u // tracker pages per host page
#define LOCKS 64u
#define LOCK_SPAN_SHIFT 22 // one lock per 4 MiB of guest address space (256 host pages)

// g_watch[host page]: 2 bits per tracker page (NPS5_KW_*). g_gprot[host page]: bit 7 = recorded, bits 0-1 = the
// guest's PROT_READ|PROT_WRITE; 0 = never recorded = read-write (the runtime commits guest pages read-write).
static _Atomic(uint8_t) *g_watch;
static uint8_t *g_gprot;
static uint64_t g_pages; // host pages covered
static Nps5KytyWatchConfig g_cfg;
static atomic_flag g_locks[LOCKS];
static _Atomic uint64_t s_sets, s_protects, s_faults, s_fault_pages, s_collateral, s_unresolved, s_prepares;
static _Atomic uint64_t s_write_faults, s_raced, s_owner_ns, s_owner_max_ns, s_slow_calls;

static _Atomic uint64_t s_kind_calls[4], s_kind_ns[4], s_emulated, s_emu_declined, s_loads_served, s_straddles;
// The fault being offered to the owner on this thread (its faulting tracker page only): see nps5_kyty_watch_fault_load.
static _Thread_local void *t_uctx;
static _Thread_local uint64_t t_fault_host;
static _Thread_local int t_applied;
static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static inline void lock_for(uint64_t guest) {
  atomic_flag *l = &g_locks[(guest >> LOCK_SPAN_SHIFT) % LOCKS];
  while (atomic_flag_test_and_set_explicit(l, memory_order_acquire)) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#endif
  }
}
static inline void unlock_for(uint64_t guest) { atomic_flag_clear_explicit(&g_locks[(guest >> LOCK_SPAN_SHIFT) % LOCKS], memory_order_release); }

static inline int guest_prot(uint64_t page) {
  const uint8_t g = g_gprot[page];
  return (g & 0x80) ? (g & 3) : (PROT_READ | PROT_WRITE);
}
// Host protection of a host page from its watch byte and the guest's protection.
static inline int effective(uint8_t w, int gprot) {
  if (w == 0) return gprot;
  if (w & 0xAA) return PROT_NONE;   // some tracker page is watched for access
  return gprot & ~PROT_WRITE;       // some tracker page is watched for writes
}
static inline bool in_window(uint64_t guest, uint64_t size) {
  return g_watch && guest >= g_cfg.guest_lo && guest <= g_cfg.guest_hi && size <= g_cfg.guest_hi - guest;
}

int nps5_kyty_watch_init(const Nps5KytyWatchConfig *cfg) {
  if (!cfg || !cfg->host_protect || cfg->guest_hi <= cfg->guest_lo) return -1;
  nps5_kyty_watch_shutdown();
  g_cfg = *cfg;
  g_pages = (cfg->guest_hi + HOST_PAGE - 1) >> NPS5_KW_HOST_SHIFT;
  // Anonymous zero-fill: only the parts that are ever watched get pages (1 byte per 16 KiB: 64 MiB of address space
  // for a 1 TiB guest window, 64 KiB of memory per GiB actually watched).
  void *a = mmap(NULL, g_pages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  void *b = mmap(NULL, g_pages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (a == MAP_FAILED || b == MAP_FAILED) {
    if (a != MAP_FAILED) munmap(a, g_pages);
    if (b != MAP_FAILED) munmap(b, g_pages);
    return -1;
  }
  g_gprot = b;
  g_watch = a;
  return 0;
}

bool nps5_kyty_watch_ready(void) { return g_watch != NULL; }

void nps5_kyty_watch_shutdown(void) {
  if (g_watch) munmap((void *)g_watch, g_pages);
  if (g_gprot) munmap(g_gprot, g_pages);
  g_watch = NULL;
  g_gprot = NULL;
  g_pages = 0;
  s_sets = s_protects = s_faults = s_fault_pages = s_collateral = s_unresolved = s_prepares = 0;
  s_write_faults = s_raced = s_owner_ns = s_owner_max_ns = s_slow_calls = 0;
  for (int i = 0; i < 4; i++) s_kind_calls[i] = s_kind_ns[i] = 0;
  s_emulated = s_emu_declined = s_loads_served = s_straddles = 0;
}

// Applies host protection `prot` to host pages [first, end) (one call).
static int apply(uint64_t first, uint64_t end, int prot) {
  atomic_fetch_add_explicit(&s_protects, 1, memory_order_relaxed);
  return g_cfg.host_protect((void *)(uintptr_t)(g_cfg.bias + (first << NPS5_KW_HOST_SHIFT)), (size_t)((end - first) << NPS5_KW_HOST_SHIFT), prot);
}

int nps5_kyty_watch_set(uint64_t guest, uint64_t size, unsigned watch) {
  if (watch > NPS5_KW_ACCESS || ((guest | size) & (TRACK_PAGE - 1)) || !in_window(guest, size)) return -1;
  atomic_fetch_add_explicit(&s_sets, 1, memory_order_relaxed);
  int rc = 0;
  const uint64_t end = guest + size;
  for (uint64_t cur = guest; cur < end;) {
    // One lock span at a time; inside it, runs of host pages whose protection changes to the same value are applied
    // with one call.
    uint64_t span_end = ((cur >> LOCK_SPAN_SHIFT) + 1) << LOCK_SPAN_SHIFT;
    if (span_end > end) span_end = end;
    lock_for(cur);
    uint64_t run_first = 0, run_end = 0;
    int run_prot = -1;
    for (uint64_t page = cur >> NPS5_KW_HOST_SHIFT; (page << NPS5_KW_HOST_SHIFT) < span_end; page++) {
      const uint64_t base = page << NPS5_KW_HOST_SHIFT;
      uint8_t w = atomic_load_explicit(&g_watch[page], memory_order_relaxed);
      const uint8_t old = w;
      for (unsigned s = 0; s < SUBS; s++) {
        const uint64_t sub = base + (uint64_t)s * TRACK_PAGE;
        if (sub >= cur && sub < span_end) w = (uint8_t)((w & ~(3u << (2 * s))) | (watch << (2 * s)));
      }
      if (w == old) continue;
      const int gp = guest_prot(page), before = effective(old, gp), after = effective(w, gp);
      atomic_store_explicit(&g_watch[page], w, memory_order_release);
      if (before == after) continue;
      if (run_prot == after && run_end == page) {
        run_end = page + 1;
        continue;
      }
      if (run_prot >= 0 && apply(run_first, run_end, run_prot) != 0) rc = -1;
      run_first = page;
      run_end = page + 1;
      run_prot = after;
    }
    if (run_prot >= 0 && apply(run_first, run_end, run_prot) != 0) rc = -1;
    unlock_for(cur);
    cur = span_end;
  }
  return rc;
}

int nps5_kyty_watch_guest_protect(void *host, size_t len, int prot) {
  const uint64_t h = (uint64_t)(uintptr_t)host;
  if (!g_watch || h < g_cfg.bias || !in_window(h - g_cfg.bias, len) || ((h | len) & (HOST_PAGE - 1))) {
    // Not ours to compose (outside the window, or not whole host pages): plain protection.
    return g_cfg.host_protect && g_cfg.host_protect(host, len, prot) != 0 ? (errno ? errno : EINVAL) : 0;
  }
  const uint64_t guest = h - g_cfg.bias, end = guest + len;
  const int gp = prot & (PROT_READ | PROT_WRITE);
  int rc = 0;
  for (uint64_t cur = guest; cur < end;) {
    uint64_t span_end = ((cur >> LOCK_SPAN_SHIFT) + 1) << LOCK_SPAN_SHIFT;
    if (span_end > end) span_end = end;
    lock_for(cur);
    // The request is applied to every page (the runtime relies on it also for pages whose record is unchanged, e.g.
    // freshly committed ones); only the watched pages get a protection of their own.
    uint64_t run_first = cur >> NPS5_KW_HOST_SHIFT;
    int run_prot = -1;
    const uint64_t last = span_end >> NPS5_KW_HOST_SHIFT;
    for (uint64_t page = run_first; page < last; page++) {
      g_gprot[page] = (uint8_t)(0x80 | gp);
      const int eff = effective(atomic_load_explicit(&g_watch[page], memory_order_relaxed), gp);
      if (eff != run_prot) {
        if (run_prot >= 0 && apply(run_first, page, run_prot) != 0) rc = errno ? errno : EINVAL;
        run_first = page;
        run_prot = eff;
      }
    }
    if (run_prot >= 0 && apply(run_first, last, run_prot) != 0) rc = errno ? errno : EINVAL;
    unlock_for(cur);
    cur = span_end;
  }
  return rc;
}

void nps5_kyty_watch_forget(uint64_t guest, uint64_t size) {
  if (!in_window(guest, size) || size == 0) return;
  const uint64_t first = guest >> NPS5_KW_HOST_SHIFT, last = (guest + size + HOST_PAGE - 1) >> NPS5_KW_HOST_SHIFT;
  for (uint64_t page = first; page < last; page++) {
    if (atomic_load_explicit(&g_watch[page], memory_order_relaxed) == 0 && g_gprot[page] == 0) continue; // untouched: do not commit the array
    lock_for(page << NPS5_KW_HOST_SHIFT);
    atomic_store_explicit(&g_watch[page], 0, memory_order_release);
    g_gprot[page] = 0;
    unlock_for(page << NPS5_KW_HOST_SHIFT);
  }
}

static inline bool traps(unsigned mode, int is_write) { return mode == NPS5_KW_ACCESS || (mode == NPS5_KW_WRITE && is_write); }

static inline bool byte_traps(uint8_t w, int is_write) { return (w & 0xAA) || (is_write && w); }

// What the kernel allows on the host page now: 1 the access, 0 not, -1 nothing mapped / unknown.
static int kernel_allows(uint64_t host, int is_write) {
#if defined(__APPLE__)
  mach_vm_address_t addr = (mach_vm_address_t)host;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS) return -1;
  if (addr > host) return -1;
  return (info.protection & (is_write ? VM_PROT_WRITE : VM_PROT_READ)) != 0;
#else
  (void)host; (void)is_write;
  return -1;
#endif
}

// A fault on a page with no armed tracker page. Either it is not a watch fault at all, or the access raced with a
// disarm: the table already says "open" while the host protection change is still in flight (or went through just after
// the access trapped), possibly followed by a new arm. Wait for a setter that holds the lock, then: armed again ->
// retry (the next fault is an ordinary watch fault); the kernel allows the access now -> retry; otherwise it is not
// ours. One Mach call, only on this rare path (which otherwise ends in a crash report).
static bool raced_disarm(uint64_t guest, uint64_t host, int is_write) {
  const uint64_t page = guest >> NPS5_KW_HOST_SHIFT;
  for (int attempt = 0; attempt < 3; attempt++) {
    lock_for(guest);
    unlock_for(guest);
    if (byte_traps(atomic_load_explicit(&g_watch[page], memory_order_acquire), is_write)) return true;
    const int allowed = kernel_allows(host, is_write);
    if (allowed == 1) return true;
    if (allowed < 0) return false;
    // Not allowed and (a moment ago) not armed: an arm may have slipped in between the two looks.
    if (byte_traps(atomic_load_explicit(&g_watch[page], memory_order_acquire), is_write)) return true;
    if ((guest_prot(page) & (is_write ? PROT_WRITE : PROT_READ)) == 0) return false; // the guest's own protection forbids it
  }
  return false;
}

// One call of the owner for tracker page `s` of host page `page`, timed. kind: see Nps5KytyWatchStats.kind_calls.
// `at`: the guest address reported: the faulting byte for the page that faulted (the owner can tell which bytes of the
// page the access touches), 0 for the first byte of the page (a neighbour, a prepared buffer).
static void call_owner(uint64_t page, unsigned s, int is_write, unsigned kind, uint64_t at) {
  const uint64_t t0 = now_ns();
  g_cfg.handle_fault(at ? at : (page << NPS5_KW_HOST_SHIFT) + (uint64_t)s * TRACK_PAGE, is_write);
  const uint64_t dt = now_ns() - t0;
  atomic_fetch_add_explicit(&s_owner_ns, dt, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_kind_calls[kind], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_kind_ns[kind], dt, memory_order_relaxed);
  if (dt > 100000) atomic_fetch_add_explicit(&s_slow_calls, 1, memory_order_relaxed);
  if (g_cfg.on_slow && dt > g_cfg.slow_ns) g_cfg.on_slow(at ? at : (page << NPS5_KW_HOST_SHIFT) + (uint64_t)s * TRACK_PAGE, is_write, kind, dt);
  if (dt > atomic_load_explicit(&s_owner_max_ns, memory_order_relaxed)) atomic_store_explicit(&s_owner_max_ns, dt, memory_order_relaxed);
}

// Offers the armed tracker pages of host page `page` that trap the access to the owner; `first_sub` (0..3) goes first
// when it is one of them. only_mode != 0 restricts it to pages in that watch mode; skip_sub (0..3, or 4 for none) is
// left out. Returns the number offered.
static unsigned offer(uint64_t page, int is_write, unsigned first_sub, bool faulting_first, unsigned only_mode, unsigned skip_sub, uint64_t fault_guest) {
  unsigned n = 0;
  for (unsigned i = 0; i < SUBS; i++) {
    const unsigned s = (first_sub + i) % SUBS;
    if (s == skip_sub) continue;
    const unsigned mode = (atomic_load_explicit(&g_watch[page], memory_order_acquire) >> (2 * s)) & 3u;
    if (!traps(mode, is_write) || (only_mode && mode != only_mode)) continue;
    n++;
    const bool neighbour = !(faulting_first && s == first_sub);
    if (neighbour) atomic_fetch_add_explicit(&s_collateral, 1, memory_order_relaxed);
    call_owner(page, s, is_write, (neighbour ? 2u : 0u) + (mode == NPS5_KW_ACCESS ? 1u : 0u), neighbour ? 0 : fault_guest);
  }
  return n;
}

// The emulator's question (signal handler): may host [addr, addr+size) be accessed on the alias? Yes when every tracker
// page it touches is one the access would not trap on and the guest's own protection allows it.
typedef struct { bool is_write; } RedirectCtx;
static void *emu_redirect(void *user, const Nps5A64Access *a) {
  (void)user;
  if (a->addr < g_cfg.bias || a->size == 0) return NULL;
  const uint64_t guest = a->addr - g_cfg.bias;
  if (guest < g_cfg.guest_lo || guest >= g_cfg.guest_hi || a->size > g_cfg.guest_hi - guest) return NULL;
  for (uint64_t t = guest >> NPS5_KW_TRACK_SHIFT; t <= (guest + a->size - 1) >> NPS5_KW_TRACK_SHIFT; t++) {
    const uint64_t page = t >> (NPS5_KW_HOST_SHIFT - NPS5_KW_TRACK_SHIFT);
    const unsigned mode = (atomic_load_explicit(&g_watch[page], memory_order_acquire) >> (2 * (t & (SUBS - 1)))) & 3u;
    if (traps(mode, a->is_store)) return NULL;
    if ((guest_prot(page) & (a->is_store ? PROT_WRITE : PROT_READ)) == 0) return NULL;
  }
  return g_cfg.alias_of(guest, a->size);
}

int nps5_kyty_watch_fault_ctx(void *ucontext, uint64_t host, int is_write) {
  if (!g_watch || !g_cfg.handle_fault || host < g_cfg.bias) return 0;
  const uint64_t guest = host - g_cfg.bias;
  if (guest < g_cfg.guest_lo || guest >= g_cfg.guest_hi) return 0;
  const uint64_t page = guest >> NPS5_KW_HOST_SHIFT;
  const uint8_t w = atomic_load_explicit(&g_watch[page], memory_order_acquire);
  // Was it a watch that stopped the access? A write also traps on a write-watched page, a read only on an
  // access-watched one. No armed page: the fault is the guest's own protection (or a race with a disarm that already
  // went through, which the runtime's retry settles).
  if (!byte_traps(w, is_write)) {
    const bool retry = raced_disarm(guest, host, is_write);
    if (retry) atomic_fetch_add_explicit(&s_raced, 1, memory_order_relaxed);
    return retry ? 1 : 0;
  }
  atomic_fetch_add_explicit(&s_faults, 1, memory_order_relaxed);
  if (is_write) atomic_fetch_add_explicit(&s_write_faults, 1, memory_order_relaxed);
  const unsigned f = (unsigned)((guest >> NPS5_KW_TRACK_SHIFT) & (SUBS - 1));
  const unsigned fmode = (w >> (2 * f)) & 3u;
  const int emulate = g_cfg.alias_of ? g_cfg.emulate : 0;
  unsigned n = 0;
  if (emulate == 0) {
    // Every armed tracker page of the host page that traps the access, the faulting one first.
    n = offer(page, is_write, f, traps(fmode, is_write), 0, SUBS, guest);
  } else if (traps(fmode, is_write)) {
    // The faulting tracker page is armed: the owner handles it. With emulate == 1 the write-watched neighbours of a
    // store are opened with it (as if written: one host page, one fault). Access-watched neighbours are left alone:
    // when they still close the host page, the retry traps again and takes the branch below.
    t_uctx = is_write ? NULL : ucontext;
    t_fault_host = host;
    t_applied = 0;
    call_owner(page, f, is_write, fmode == NPS5_KW_ACCESS ? 1u : 0u, guest);
    t_uctx = NULL;
    n = 1;
    if (t_applied) { // the owner completed the load itself: the page stays armed, the thread continues behind it
      atomic_fetch_add_explicit(&s_fault_pages, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&s_loads_served, 1, memory_order_relaxed);
      return 2;
    }
    if (is_write && emulate == 1) n += offer(page, 1, f, false, NPS5_KW_WRITE, f, 0);
  } else if (emulate == 1 && !(w & 0xAA)) {
    // A store to an unwatched tracker page whose neighbours are only write-watched: report them written. That opens
    // the host page for good; the owner re-reads pages that did not change.
    n = offer(page, is_write, f, false, 0, f, 0);
  } else {
    // An access to an unwatched tracker page (or a load from a write-watched one) of a host page that an
    // access-watched neighbour keeps closed: the neighbour's bytes belong to the GPU and must stay trapped. Carry the
    // instruction out on the alias and continue behind it.
    if (ucontext && nps5_a64_emulate_access(ucontext, host, emu_redirect, NULL)) {
      atomic_fetch_add_explicit(&s_emulated, 1, memory_order_relaxed);
      return 2;
    }
    // Not carried out. When the instruction is one the emulator knows and its bytes reach into an armed tracker page
    // (an access that straddles the unwatched page that faulted and a watched one, e.g. 8 bytes at ...ffc), that
    // page is what the access needs: it is offered as the page that faulted, with the load available for the owner
    // to complete (nps5_kyty_watch_fault_load). The watched neighbours the access does not touch stay closed.
    Nps5A64Access a;
    if (ucontext && nps5_a64_fault_access(ucontext, &a) && a.size != 0 && a.addr >= g_cfg.bias && host >= a.addr && host - a.addr < a.size &&
        a.addr - g_cfg.bias >= g_cfg.guest_lo && a.addr - g_cfg.bias < g_cfg.guest_hi && a.size <= g_cfg.guest_hi - (a.addr - g_cfg.bias)) {
      const uint64_t ga = a.addr - g_cfg.bias;
      for (uint64_t t = ga >> NPS5_KW_TRACK_SHIFT; t <= (ga + a.size - 1) >> NPS5_KW_TRACK_SHIFT; t++) {
        const uint64_t tpage = t >> (NPS5_KW_HOST_SHIFT - NPS5_KW_TRACK_SHIFT);
        const unsigned ts = (unsigned)(t & (SUBS - 1));
        const unsigned mode = (atomic_load_explicit(&g_watch[tpage], memory_order_acquire) >> (2 * ts)) & 3u;
        if (!traps(mode, is_write)) continue;
        const uint64_t first = t << NPS5_KW_TRACK_SHIFT;
        atomic_fetch_add_explicit(&s_straddles, 1, memory_order_relaxed);
        t_uctx = is_write ? NULL : ucontext;
        t_fault_host = host;
        t_applied = 0;
        call_owner(tpage, ts, is_write, mode == NPS5_KW_ACCESS ? 1u : 0u, first > ga ? first : ga);
        t_uctx = NULL;
        atomic_fetch_add_explicit(&s_fault_pages, 1, memory_order_relaxed);
        if (t_applied) {
          atomic_fetch_add_explicit(&s_loads_served, 1, memory_order_relaxed);
          return 2;
        }
        if (traps((atomic_load_explicit(&g_watch[tpage], memory_order_acquire) >> (2 * ts)) & 3u, is_write)) atomic_fetch_add_explicit(&s_unresolved, 1, memory_order_relaxed);
        return 1; // the owner opened it (or will be asked again): the retry is emulated, or runs on open pages
      }
    }
    atomic_fetch_add_explicit(&s_emu_declined, 1, memory_order_relaxed);
    if (g_cfg.on_declined) g_cfg.on_declined(ucontext, guest, is_write);
    n = offer(page, is_write, f, false, 0, f, 0); // not an instruction the emulator covers: the old way
  }
  atomic_fetch_add_explicit(&s_fault_pages, n, memory_order_relaxed);
  const uint8_t after = atomic_load_explicit(&g_watch[page], memory_order_acquire);
  if (emulate == 0 || n == 0) {
      // The owner left a page armed (or another thread re-armed one meanwhile). Retrying is right for the race; a
      // page the owner never opens is caught by the runtime's livelock guard.
      if (byte_traps(after, is_write)) atomic_fetch_add_explicit(&s_unresolved, 1, memory_order_relaxed);
  } else if (traps((after >> (2 * f)) & 3u, is_write)) {
      atomic_fetch_add_explicit(&s_unresolved, 1, memory_order_relaxed); // the faulting page itself stayed armed
  }
  // The owner changed the page's watch state: progress (see Nps5KytyWatchConfig.progress_verdict).
  return g_cfg.progress_verdict && n != 0 && after != w ? 3 : 1;
}

int nps5_kyty_watch_fault_load(uint64_t *guest, uint32_t *size) {
  Nps5A64Access a;
  if (!t_uctx || t_applied || !guest || !size || !nps5_a64_fault_access(t_uctx, &a)) return 0;
  if (a.is_store || a.size > 16 || t_fault_host < a.addr || t_fault_host - a.addr >= a.size || a.addr < g_cfg.bias) return 0;
  *guest = a.addr - g_cfg.bias;
  *size = a.size;
  return 1;
}

typedef struct { const void *bytes; uint32_t size; } ServedLoad;
static void *served_redirect(void *user, const Nps5A64Access *a) {
  const ServedLoad *sl = (const ServedLoad *)user;
  return !a->is_store && a->size == sl->size ? (void *)(uintptr_t)sl->bytes : NULL;
}

int nps5_kyty_watch_fault_load_apply(const void *bytes, uint32_t size) {
  if (!t_uctx || t_applied || !bytes || size == 0 || size > 16) return 0;
  ServedLoad sl = {bytes, size};
  if (!nps5_a64_emulate_access(t_uctx, t_fault_host, served_redirect, &sl)) return 0;
  t_applied = 1;
  return 1;
}

bool nps5_kyty_watch_fault(uint64_t host, int is_write) { return nps5_kyty_watch_fault_ctx(NULL, host, is_write) != 0; }

void nps5_kyty_watch_prepare(const void *host, size_t len, int is_write) {
  const uint64_t h = (uint64_t)(uintptr_t)host;
  if (!g_watch || !g_cfg.handle_fault || len == 0 || h < g_cfg.bias) return;
  const uint64_t guest = h - g_cfg.bias;
  if (guest < g_cfg.guest_lo || guest >= g_cfg.guest_hi || len > g_cfg.guest_hi - guest) return;
  const uint64_t first = guest >> NPS5_KW_HOST_SHIFT, last = (guest + len - 1) >> NPS5_KW_HOST_SHIFT;
  for (uint64_t page = first; page <= last; page++) {
    if (atomic_load_explicit(&g_watch[page], memory_order_relaxed) == 0) continue;
    // The whole host page must open: the kernel checks the host page, not the tracker page.
    unsigned n = 0;
    for (unsigned sub = 0; sub < SUBS; sub++) {
      const unsigned mode = (atomic_load_explicit(&g_watch[page], memory_order_acquire) >> (2 * sub)) & 3u;
      if (!traps(mode, is_write)) continue;
      n++;
      call_owner(page, sub, is_write, mode == NPS5_KW_ACCESS ? 1u : 0u, 0);
    }
    atomic_fetch_add_explicit(&s_prepares, n, memory_order_relaxed);
  }
}

unsigned nps5_kyty_watch_get(uint64_t guest) {
  if (!in_window(guest, 1)) return 0;
  return (atomic_load_explicit(&g_watch[guest >> NPS5_KW_HOST_SHIFT], memory_order_acquire) >> (2 * ((guest >> NPS5_KW_TRACK_SHIFT) & (SUBS - 1)))) & 3u;
}

int nps5_kyty_watch_host_prot(uint64_t guest) {
  if (!in_window(guest, 1)) return PROT_READ | PROT_WRITE;
  const uint64_t page = guest >> NPS5_KW_HOST_SHIFT;
  return effective(atomic_load_explicit(&g_watch[page], memory_order_acquire), guest_prot(page));
}

void nps5_kyty_watch_stats(Nps5KytyWatchStats *out) {
  if (!out) return;
  out->sets = atomic_load(&s_sets);
  out->host_protects = atomic_load(&s_protects);
  out->faults = atomic_load(&s_faults);
  out->fault_pages = atomic_load(&s_fault_pages);
  out->collateral_pages = atomic_load(&s_collateral);
  out->unresolved = atomic_load(&s_unresolved);
  out->prepares = atomic_load(&s_prepares);
  out->write_faults = atomic_load(&s_write_faults);
  out->raced = atomic_load(&s_raced);
  out->owner_ns = atomic_load(&s_owner_ns);
  out->owner_max_ns = atomic_load(&s_owner_max_ns);
  out->slow_calls = atomic_load(&s_slow_calls);
  out->emulated = atomic_load(&s_emulated);
  out->emu_declined = atomic_load(&s_emu_declined);
  out->loads_served = atomic_load(&s_loads_served);
  out->straddles = atomic_load(&s_straddles);
  for (int i = 0; i < 4; i++) {
    out->kind_calls[i] = atomic_load(&s_kind_calls[i]);
    out->kind_ns[i] = atomic_load(&s_kind_ns[i]);
  }
}
