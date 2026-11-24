// SPDX-License-Identifier: GPL-2.0-or-later
//
// Flat call model runtime (SPEC 3.3; docs/m1/flatstack.md).
//
// The host stack carries no guest activation in this model, so where a guest address continues
// on the host is a pure function of the address (and the code generation): the dispatch table
// for function entries, the continuation table for return sites, landing pads and other block
// starts, tier 1 for everything else. The shadow stack caches that function for the return
// addresses of live calls. Everything here follows from "the cache may be wrong or empty": a
// RET that misses is resynchronised against the guest stack pointer and continued by address.
#include "nps5_shadow.h"

#include "nps5_diag.h"
#include "recomp_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#endif

__attribute__((weak)) uint64_t g_recomp_call_model = 0;

// ---------------------------------------------------------------- call-site inline caches
// docs/m3/callic.md. The site records live in __DATA,__nps5_ic of every image that holds
// recompiled code (the main binary and tier-1 fragment dylibs): 48 bytes each, slot0/slot1 =
// {guest target, host call entry} written with one STP, read with one LDP (16-byte aligned:
// single-copy atomic with FEAT_LSE2). g_recomp_ic = {flush epoch, enabled}; the strong
// definition is in the trampoline (the weak default keeps the file linkable without it).
__attribute__((weak)) uint64_t g_recomp_ic[3] = {0, 0, 0};

static void ic_site_load(const uint64_t *p, uint64_t *g, uint64_t *h) {
  uint64_t a, b;
  __asm__ volatile("ldp %0, %1, [%2]" : "=r"(a), "=r"(b) : "r"(p) : "memory");
  *g = a; *h = b;
}
static void ic_site_store(uint64_t *p, uint64_t g, uint64_t h) {
  __asm__ volatile("stp %1, %2, [%0]" : : "r"(p), "r"(g), "r"(h) : "memory");
}

// Clears every slot whose host entry is in [lo, hi) (all slots for 0, UINTPTR_MAX). Bump the
// epoch first and fence: a filler that stored before the scan is cleared here, one that stores
// after it sees the new epoch on its re-read and un-publishes its own slot (the trampoline).
static void ic_flush(uintptr_t lo, uintptr_t hi) {
#if defined(__APPLE__)
  __atomic_add_fetch(&g_recomp_ic[0], 1, __ATOMIC_SEQ_CST);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint32_t n = _dyld_image_count();
  for (uint32_t i = 0; i < n; i++) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!mh) continue;
    unsigned long sz = 0;
    uint8_t *d = getsectiondata(mh, "__DATA", "__nps5_ic", &sz);
    if (!d) continue;
    for (unsigned long off = 0; off + 48 <= sz; off += 48) {
      uint64_t *site = (uint64_t *)(d + off);
      for (int k = 0; k < 2; k++) {
        uint64_t g, h;
        ic_site_load(site + 2 * k, &g, &h);
        if (g != ~0ull && h >= lo && h < hi) ic_site_store(site + 2 * k, ~0ull, g_recomp_ic[2]);
      }
    }
  }
#else
  (void)lo; (void)hi;
#endif
}
void recomp_ic_flush_all(void) { ic_flush(0, UINTPTR_MAX); }
void recomp_ic_flush_range(uintptr_t lo, uintptr_t hi) { ic_flush(lo, hi); }

// Test hook: NPS5_IC_FLUSH_STRESS=<usec> flushes every cache that often from a host thread
// (races fills against flushes; the program must not notice).
static void *ic_stress(void *arg) {
  useconds_t us = (useconds_t)(uintptr_t)arg;
  for (;;) { usleep(us); recomp_ic_flush_all(); }
  return NULL;
}

__attribute__((constructor)) static void ic_init(void) {
  const char *st = getenv("NPS5_IC_FLUSH_STRESS");
  if (st && atoi(st) > 0) {
    pthread_t t;
    if (pthread_create(&t, NULL, ic_stress, (void *)(uintptr_t)atoi(st)) == 0) pthread_detach(t);
  }
  const char *e = getenv("NPS5_CALL_IC");
  if (e && e[0] == '0') { g_recomp_ic[1] = 0; return; }
#if defined(__APPLE__)
  int lse2 = 0;
  size_t sz = sizeof lse2;
  if (sysctlbyname("hw.optional.arm.FEAT_LSE2", &lse2, &sz, NULL, 0) != 0 || !lse2) g_recomp_ic[1] = 0;
#endif
}

// ---------------------------------------------------------------- tier-1 default
// recomp_runtime.c (NPS5_LAZY_DISPATCH); the weak default keeps stand-alone builds of this file linking.
__attribute__((weak)) recomp_fn_t recomp_lazy_resolve(uint64_t a) { (void)a; return NULL; }
__attribute__((weak)) uintptr_t recomp_dispatch_miss(GuestContext *ctx, uint64_t guest_rip, uint32_t reason) {
  (void)ctx; (void)reason;
  recomp_fn_t lz = recomp_lazy_resolve(guest_rip);
  if (lz) return (uintptr_t)lz + (recomp_call_model_flat() ? RECOMP_FLAT_JUMP_ENTRY_OFFSET : 0);
  return 0;
}

// ---------------------------------------------------------------- continuation table
// Static part: {guest pc, host address} pairs the emitter writes to __DATA,__nps5_pads. Nested
// model: blocks of functions with an LSDA (landing pads). Flat model: every non-entry block and
// every return site.
extern const uint64_t g_nps5_pads_start[] __asm("section$start$__DATA$__nps5_pads");
extern const uint64_t g_nps5_pads_end[] __asm("section$end$__DATA$__nps5_pads");
// Keeps the section (and so the linker symbols) present when the image has no entries.
__attribute__((used, section("__DATA,__nps5_pads"))) static const uint64_t s_pads_sentinel[2] = {~0ull, 0};

typedef struct { uint64_t guest, host; } ContEnt;
// Compact form (8 B instead of 16, ~49 MB less for a 6 M-entry table): guest address and host offset
// from s_cont_base, both below 4 GiB. Used when every entry fits; s_cont (16 B) is the fallback.
typedef struct { uint32_t guest, host_off; } ContEnt32;
static ContEnt32 *s_cont32;
static uintptr_t s_cont_base;
static ContEnt *s_cont;
static size_t s_cont_n;
static pthread_once_t s_cont_once = PTHREAD_ONCE_INIT;

static int cont_cmp(const void *a, const void *b) {
  uint64_t x = ((const ContEnt *)a)->guest, y = ((const ContEnt *)b)->guest;
  return x < y ? -1 : x > y;
}
static int cont32_cmp(const void *a, const void *b) {
  uint32_t x = ((const ContEnt32 *)a)->guest, y = ((const ContEnt32 *)b)->guest;
  return x < y ? -1 : x > y;
}
static void cont_init(void) {
  size_t n = (size_t)(g_nps5_pads_end - g_nps5_pads_start) / 2, k = 0;
  {
    uint64_t hmin = ~0ull, hmax = 0, gmax = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t g = g_nps5_pads_start[2 * i], h = g_nps5_pads_start[2 * i + 1];
      if (g == ~0ull) continue;
      if (h < hmin) hmin = h;
      if (h > hmax) hmax = h;
      if (g > gmax) gmax = g;
    }
    if (n && hmin <= hmax && hmax - hmin < 0xffffffffull && gmax < 0xffffffffull) {
      ContEnt32 *c = (ContEnt32 *)malloc((n ? n : 1) * sizeof(ContEnt32));
      if (c) {
        for (size_t i = 0; i < n; i++) {
          if (g_nps5_pads_start[2 * i] == ~0ull) continue;
          c[k].guest = (uint32_t)g_nps5_pads_start[2 * i];
          c[k].host_off = (uint32_t)(g_nps5_pads_start[2 * i + 1] - hmin);
          k++;
        }
        qsort(c, k, sizeof(ContEnt32), cont32_cmp);
        s_cont_n = k;
        s_cont_base = (uintptr_t)hmin;
        __atomic_store_n(&s_cont32, c, __ATOMIC_RELEASE);
        return;
      }
    }
  }
  k = 0;
  ContEnt *t = (ContEnt *)malloc((n ? n : 1) * sizeof(ContEnt));
  if (!t) return;
  for (size_t i = 0; i < n; i++) {
    if (g_nps5_pads_start[2 * i] == ~0ull) continue;
    t[k].guest = g_nps5_pads_start[2 * i];
    t[k].host = g_nps5_pads_start[2 * i + 1];
    k++;
  }
  qsort(t, k, sizeof(ContEnt), cont_cmp);
  s_cont_n = k;
  __atomic_store_n(&s_cont, t, __ATOMIC_RELEASE);
}

// Dynamic part: lock-free chained hash, newest node first. A node is never freed or unlinked;
// invalidation zeroes its host address.
typedef struct ContNode {
  uint64_t guest;
  _Atomic uintptr_t host;
  struct ContNode *next;
} ContNode;
#define CONT_BUCKETS 4096
static _Atomic(ContNode *) s_dyn[CONT_BUCKETS];
static _Atomic uint64_t s_dyn_n;
static inline size_t cont_hash(uint64_t g) { return (size_t)((g * 0x9E3779B97F4A7C15ull) >> 52) & (CONT_BUCKETS - 1); }

void recomp_cont_register(uint64_t guest_rip, uintptr_t host_addr) {
  for (ContNode *n = atomic_load(&s_dyn[cont_hash(guest_rip)]); n; n = n->next) {
    if (n->guest == guest_rip) { atomic_store(&n->host, host_addr); return; }
  }
  ContNode *n = (ContNode *)malloc(sizeof *n);
  if (!n) return;
  n->guest = guest_rip;
  atomic_init(&n->host, host_addr);
  _Atomic(ContNode *) *b = &s_dyn[cont_hash(guest_rip)];
  n->next = atomic_load(b);
  while (!atomic_compare_exchange_weak(b, &n->next, n)) {}
  atomic_fetch_add(&s_dyn_n, 1);
}

uintptr_t recomp_cont_table_lookup(uint64_t guest_rip) {
  if (atomic_load_explicit(&s_dyn_n, memory_order_relaxed)) {
    for (ContNode *n = atomic_load(&s_dyn[cont_hash(guest_rip)]); n; n = n->next) {
      if (n->guest == guest_rip) {
        uintptr_t h = atomic_load(&n->host);
        if (h) return h;
        break;
      }
    }
  }
  const ContEnt *t = __atomic_load_n(&s_cont, __ATOMIC_ACQUIRE);
  const ContEnt32 *c = __atomic_load_n(&s_cont32, __ATOMIC_ACQUIRE);
  if (!t && !c) {
    // Not from a signal handler in practice: every path that reaches guest code has been
    // through recomp_shadow_attach or a nested-model EH shim, both of which initialise.
    pthread_once(&s_cont_once, cont_init);
    t = __atomic_load_n(&s_cont, __ATOMIC_ACQUIRE);
    c = __atomic_load_n(&s_cont32, __ATOMIC_ACQUIRE);
    if (!t && !c) return 0;
  }
  if (c) {
    if (guest_rip >= 0xffffffffull) return 0;
    uint32_t g = (uint32_t)guest_rip;
    size_t lo = 0, hi = s_cont_n;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (c[mid].guest < g) lo = mid + 1; else hi = mid;
    }
    return lo < s_cont_n && c[lo].guest == g ? s_cont_base + c[lo].host_off : 0;
  }
  size_t lo = 0, hi = s_cont_n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (t[mid].guest < guest_rip) lo = mid + 1; else hi = mid;
  }
  return lo < s_cont_n && t[lo].guest == guest_rip ? (uintptr_t)t[lo].host : 0;
}

static uintptr_t dispatch_table_lookup(uint64_t target) {
  if (target == 0 || target >= (1ULL << 48)) return 0;
  recomp_fn_t **l2 = g_dispatch_l1[(target >> DISPATCH_L1_SHIFT) & DISPATCH_LEVEL_MASK];
  recomp_fn_t *l3 = l2 ? l2[(target >> DISPATCH_L2_SHIFT) & DISPATCH_LEVEL_MASK] : NULL;
  uintptr_t h = l3 ? (uintptr_t)l3[target & DISPATCH_LEVEL_MASK] : 0;
  return h ? h : (uintptr_t)recomp_lazy_resolve(target);
}

uintptr_t recomp_cont_lookup(uint64_t guest_rip) {
  uintptr_t h = recomp_cont_table_lookup(guest_rip);
  if (h) return h;
  if (!recomp_call_model_flat()) return 0; // a nested function body cannot be entered by address
  h = dispatch_table_lookup(guest_rip);
  return h ? h + RECOMP_FLAT_JUMP_ENTRY_OFFSET : 0;
}

uintptr_t recomp_cont_resolve(GuestContext *ctx, uint64_t guest_rip, uint32_t reason) {
  uintptr_t h = recomp_cont_lookup(guest_rip);
  if (h) return h;
  h = recomp_dispatch_miss(ctx, guest_rip, reason);
  if (h && !recomp_cont_lookup(guest_rip)) {
    NPS5_LOG(NPS5_S_TIER1, NPS5_L_DEBUG, "dispatch miss 0x%llx (reason %u) served at %p, not published",
             (unsigned long long)guest_rip, reason, (void *)h);
  }
  return h;
}

// ---------------------------------------------------------------- registry
// Fixed table of atomics: the fault handler and the crash walker scan it without locks.
#define MAX_SHADOWS 8192
static _Atomic(RecompShadow *) s_shadows[MAX_SHADOWS];
static pthread_mutex_t s_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t s_code_gen = 1;
// Process totals (the per-stack counters die with their stack): slow RETs, of which served by
// the shadow stack after a resynchronisation / returns to a host boundary / by address;
// overflow compactions; stacks created.
static _Atomic uint64_t g_st_slow, g_st_hit, g_st_boundary, g_st_addr, g_st_overflow, g_st_stacks, g_st_miss_calls;

void recomp_shadow_stats(RecompShadowStats *out) {
  out->slow_rets = atomic_load(&g_st_slow);
  out->slow_hits = atomic_load(&g_st_hit);
  out->slow_boundary = atomic_load(&g_st_boundary);
  out->slow_by_address = atomic_load(&g_st_addr);
  out->overflows = atomic_load(&g_st_overflow);
  out->stacks = atomic_load(&g_st_stacks);
  out->dispatch_slow = atomic_load(&g_st_miss_calls);
}

static void stats_atexit(void) {
  RecompShadowStats st;
  recomp_shadow_stats(&st);
  fprintf(stderr, "[nps5-shadow] stacks=%llu slow_rets=%llu (resynced=%llu boundary=%llu by_address=%llu) overflows=%llu dispatch_slow=%llu\n",
          (unsigned long long)st.stacks, (unsigned long long)st.slow_rets, (unsigned long long)st.slow_hits,
          (unsigned long long)st.slow_boundary, (unsigned long long)st.slow_by_address, (unsigned long long)st.overflows,
          (unsigned long long)st.dispatch_slow);
}

uint64_t recomp_code_generation(void) { return atomic_load(&s_code_gen); }

static size_t shadow_entries(void) {
  static size_t n;
  if (!n) {
    const char *e = getenv("NPS5_SHADOW_ENTRIES");
    long v = e ? strtol(e, NULL, 0) : 0;
    n = v >= 8 && v <= (1 << 24) ? (size_t)v : 32768;
  }
  return n;
}

// The registered stack whose mapping (guards included) contains addr.
static RecompShadow *shadow_find(uint64_t addr) {
  for (int i = 0; i < MAX_SHADOWS; i++) {
    RecompShadow *s = atomic_load_explicit(&s_shadows[i], memory_order_acquire);
    if (s && addr >= (uint64_t)(uintptr_t)s->map && addr < (uint64_t)(uintptr_t)s->map + s->map_len) return s;
  }
  return NULL;
}

void recomp_shadow_attach(GuestContext *ctx) {
  pthread_once(&s_cont_once, cont_init);
  if (ctx->shadow) { // a context that was released back to "no stack" keeps none; defensive
    ctx->shadow_sp = (uint64_t)(uintptr_t)ctx->shadow->bottom;
    return;
  }
  size_t page = (size_t)getpagesize();
  size_t bytes = (shadow_entries() + 1) * sizeof(RecompShadowEntry);
  bytes = (bytes + page - 1) & ~(page - 1);
  RecompShadow *s = (RecompShadow *)calloc(1, sizeof *s);
  uint8_t *m = s ? (uint8_t *)mmap(NULL, bytes + 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
  if (m == MAP_FAILED || mprotect(m, page, PROT_NONE) != 0 || mprotect(m + page + bytes, page, PROT_NONE) != 0) {
    fprintf(stderr, "\nFATAL: cannot map a shadow return stack (%zu bytes): %s\n", bytes, strerror(errno));
    abort();
  }
  s->map = m;
  s->map_len = bytes + 2 * page;
  s->lo = (RecompShadowEntry *)(m + page);
  s->bottom = (RecompShadowEntry *)(m + page + bytes) - 1;
  s->bottom->guest_ra = RECOMP_SHADOW_RA_BOTTOM;
  s->bottom->guest_rsp = ~0ull;
  s->ctx = ctx;
  s->code_gen = recomp_code_generation();
  pthread_mutex_lock(&s_reg_mu);
  int slot = -1;
  for (int i = 0; i < MAX_SHADOWS && slot < 0; i++)
    if (!atomic_load(&s_shadows[i])) slot = i;
  if (slot >= 0) atomic_store(&s_shadows[slot], s);
  pthread_mutex_unlock(&s_reg_mu);
  if (slot < 0) {
    fprintf(stderr, "\nFATAL: more than %d live shadow return stacks\n", MAX_SHADOWS);
    abort();
  }
  ctx->shadow = s;
  ctx->shadow_sp = (uint64_t)(uintptr_t)s->bottom;
  if (atomic_fetch_add(&g_st_stacks, 1) == 0 && getenv("NPS5_SHADOW_STATS")) atexit(stats_atexit);
}

void recomp_shadow_release(GuestContext *ctx) {
  if (!ctx || !ctx->shadow) return;
  RecompShadow *s = ctx->shadow;
  ctx->shadow = NULL;
  ctx->shadow_sp = 0;
  pthread_mutex_lock(&s_reg_mu);
  for (int i = 0; i < MAX_SHADOWS; i++)
    if (atomic_load(&s_shadows[i]) == s) atomic_store(&s_shadows[i], NULL);
  pthread_mutex_unlock(&s_reg_mu);
  munmap(s->map, s->map_len);
  free(s);
}

// ---------------------------------------------------------------- overflow
#if defined(__APPLE__) && defined(__arm64__)
static uint64_t *uc_reg(void *ucontext, unsigned n) {
  arm_thread_state64_t *ss = &((ucontext_t *)ucontext)->uc_mcontext->__ss;
  if (n < 29) return (uint64_t *)&ss->__x[n];
  if (n == 29) return (uint64_t *)&ss->__fp;
  if (n == 30) return (uint64_t *)&ss->__lr;
  return NULL;
}

// Keeps the newest half of a full stack at its high end; returns the new stack pointer.
static RecompShadowEntry *shadow_compact(RecompShadow *s, RecompShadowEntry *sp) {
  size_t used = (size_t)(s->bottom - sp), keep = used / 2;
  RecompShadowEntry *nsp = s->bottom - keep;
  memmove(nsp, sp, keep * sizeof *sp);
  atomic_fetch_add(&s->epoch, 1);
  atomic_fetch_add(&s->overflows, 1);
  atomic_fetch_add(&g_st_overflow, 1);
  return nsp;
}

// NPS5_SHADOW_FAULT_FAST=1 (default off, read once): the shadow-push test first, the 8,192-slot
// registry scan second. Every guest write fault of every thread reaches this handler; nearly none
// is a shadow push, so the scan was pure waste. Same result either order (both must hold).
static int g_fault_fast = -1;
static _Atomic uint64_t g_st_fault_scans;
void recomp_shadow_test_set_fault_fast(int on) { g_fault_fast = on ? 1 : 0; }
uint64_t recomp_shadow_fault_scans(void) { return atomic_load(&g_st_fault_scans); }

int recomp_shadow_handle_fault(void *ucontext, uint64_t fault_addr) {
  if (!ucontext || !recomp_call_model_flat()) return 0;
  if (g_fault_fast < 0) {
      const char *e = getenv("NPS5_SHADOW_FAULT_FAST");
      g_fault_fast = (e && e[0] == '1') ? 1 : 0;
  }
  arm_thread_state64_t *ss = &((ucontext_t *)ucontext)->uc_mcontext->__ss;
  uint64_t pc = ss->__pc;
  // A data abort at pc means pc was fetched, so it is readable; an instruction-fetch fault has
  // pc == fault address (not readable), a misaligned pc is no instruction: both take the old order.
  if (g_fault_fast == 1 && pc != fault_addr && (pc & 3) == 0 &&
      (*(const uint32_t *)(uintptr_t)pc & 0xffff8000u) != 0xa9be0000u)
      return 0;
  atomic_fetch_add_explicit(&g_st_fault_scans, 1, memory_order_relaxed);
  RecompShadow *s = shadow_find(fault_addr);
  if (!s || fault_addr >= (uint64_t)(uintptr_t)s->lo) return 0; // only the low guard
  uint32_t insn = *(const uint32_t *)(uintptr_t)ss->__pc;
  // stp Xt1, Xt2, [Xn, #-32]!  (the push of a call entry, of recomp_enter_guest and of the
  // indirect-call miss path)
  if ((insn & 0xffff8000u) != 0xa9be0000u) return 0;
  uint64_t *base = uc_reg(ucontext, (insn >> 5) & 31);
  if (!base || *base != (uint64_t)(uintptr_t)s->lo) return 0;
  *base = (uint64_t)(uintptr_t)shadow_compact(s, s->lo);
  return 1;
}

void recomp_shadow_sync_from_signal(GuestContext *ctx, void *ucontext) {
  if (!ctx || !ucontext || !recomp_call_model_flat() || !ctx->shadow) return;
  arm_thread_state64_t *ss = &((ucontext_t *)ucontext)->uc_mcontext->__ss;
  if (!recomp_pc_in_guest_code(ss->__pc)) return; // host code: ctx->shadow_sp is current
  RecompShadow *s = ctx->shadow;
  RecompShadowEntry *sp = (RecompShadowEntry *)(uintptr_t)ss->__fp;
  if (sp < s->lo || sp > s->bottom) return; // another context's stack is live in x29
  if ((size_t)(sp - s->lo) < RECOMP_SHADOW_REDZONE + 4) {
    // No room for the handler's entries: compact in place, as the overflow fault would.
    sp = shadow_compact(s, sp);
    ss->__fp = (uint64_t)(uintptr_t)sp;
  }
  ctx->shadow_sp = (uint64_t)(uintptr_t)(sp - RECOMP_SHADOW_REDZONE);
}
#else
int recomp_shadow_handle_fault(void *ucontext, uint64_t fault_addr) { (void)ucontext; (void)fault_addr; return 0; }
void recomp_shadow_test_set_fault_fast(int on) { (void)on; }
uint64_t recomp_shadow_fault_scans(void) { return 0; }
void recomp_shadow_sync_from_signal(GuestContext *ctx, void *ucontext) { (void)ctx; (void)ucontext; }
#endif

// ---------------------------------------------------------------- invalidation
void recomp_shadow_invalidate(uintptr_t host_lo, uintptr_t host_hi) {
  atomic_fetch_add(&s_code_gen, 1);
  ic_flush(host_lo, host_hi);
  pthread_mutex_lock(&s_reg_mu);
  for (int i = 0; i < MAX_SHADOWS; i++) {
    RecompShadow *s = atomic_load(&s_shadows[i]);
    if (!s) continue;
    // The owner's stack pointer is not known here (it may be in a register): poison every slot
    // of the mapping that names the range. Dead slots are rewritten before they are read.
    int hit = 0;
    for (RecompShadowEntry *e = s->lo; e < s->bottom; e++) {
      uint64_t c = __atomic_load_n(&e->host_cont, __ATOMIC_RELAXED);
      if (c >= host_lo && c < host_hi) {
        __atomic_store_n(&e->guest_ra, RECOMP_SHADOW_RA_POISON, __ATOMIC_RELAXED);
        hit = 1;
      }
    }
    if (hit) atomic_fetch_add(&s->epoch, 1);
  }
  pthread_mutex_unlock(&s_reg_mu);
  for (size_t b = 0; b < CONT_BUCKETS; b++) {
    for (ContNode *n = atomic_load(&s_dyn[b]); n; n = n->next) {
      uintptr_t h = atomic_load(&n->host);
      if (h >= host_lo && h < host_hi) atomic_store(&n->host, 0);
    }
  }
}

// ---------------------------------------------------------------- slow paths
// Lenter_guest_ret of the flat trampoline; this weak stand-in serves nested builds and host
// tests, which never reach it.
__attribute__((weak)) void recomp_enter_guest_ret(void) { abort(); }

uintptr_t recomp_shadow_boundary_cont(void) { return (uintptr_t)(void (*)(void))recomp_enter_guest_ret; }

RecompShadowEntry *recomp_shadow_trim(GuestContext *ctx, uint64_t rsp) {
  RecompShadow *s = ctx->shadow;
  if (!s) return NULL;
  RecompShadowEntry *e = (RecompShadowEntry *)(uintptr_t)ctx->shadow_sp, *last = NULL;
  if (e < s->lo || e > s->bottom) e = s->bottom; // corrupt pointer: empty cache
  while (e < s->bottom && e->guest_rsp < rsp) last = e++;
  ctx->shadow_sp = (uint64_t)(uintptr_t)e;
  return last;
}

static size_t chain_text(uint64_t top, uint64_t rsp, char *out, size_t cap);

__attribute__((noreturn)) static void flat_fatal(GuestContext *ctx, uint64_t rip, uint32_t reason, const char *what) {
  char chain[400];
  chain_text(ctx->shadow_sp, ctx->rsp, chain, sizeof chain);
  uint64_t ret_addr = 0;
  nps5_guest_safe_read(ctx->rsp, &ret_addr, sizeof ret_addr);
  if (reason == RECOMP_MISS_CALL || reason == RECOMP_MISS_JUMP) {
    // Same wording as recomp_dispatch (the nested model's report of the same condition).
    fprintf(stderr, "\nFATAL: Unresolved indirect jump/call to 0x%llx (return addr=0x%llx)\n",
            (unsigned long long)rip, (unsigned long long)ret_addr);
  } else {
    fprintf(stderr, "\nFATAL: %s 0x%llx (guest rsp 0x%llx)\n", what, (unsigned long long)rip, (unsigned long long)ctx->rsp);
  }
  fprintf(stderr, "Guest call chain: %s\n", chain[0] ? chain : "(unavailable)");
  nps5_log(NPS5_S_DISPATCH, NPS5_L_ERROR, "%s 0x%llx (reason %u, guest rsp 0x%llx); guest call chain: %s", what,
           (unsigned long long)rip, reason, (unsigned long long)ctx->rsp, chain[0] ? chain : "(unavailable)");
  nps5_diag_set_note("%s 0x%llx | guest chain: %.250s", what, (unsigned long long)rip, chain);
  recomp_dump_guest_context(ctx);
  fflush(stderr);
  abort();
}

// NPS5_TRACE_SLOWDISPATCH=<N> (diagnosis): every N-th slow indirect call/jump/resume of a thread (the ones counted in
// dispatch_slow) is tallied by (target, caller return address, host thread name); the top rows go to stderr every
// 2^16 samples and at exit. Finds hot loops that leave the inline cache (e.g. a spin loop through a virtual call).
#define SD_ROWS 512
typedef struct { uint64_t rip, ra; uint32_t reason; uint64_t n; char th[24]; } SdRow;
static SdRow s_sd[SD_ROWS];
static int s_sd_n;
static uint64_t s_sd_total;
static pthread_mutex_t s_sd_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_sd_every = -1;
static _Thread_local uint64_t t_sd_count;

static void sd_print_locked(const char *why) {
  SdRow tmp[SD_ROWS];
  int n = s_sd_n;
  memcpy(tmp, s_sd, sizeof(SdRow) * (size_t)n);
  for (int i = 1; i < n; i++) { // insertion sort, descending by n
    SdRow r = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j].n < r.n) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = r;
  }
  fprintf(stderr, "[slowdispatch] %s: %llu samples (1 per %d), %d sites\n", why, (unsigned long long)s_sd_total, s_sd_every, n);
  for (int i = 0; i < n && i < 16; i++)
    fprintf(stderr, "[slowdispatch]   %8llu  target 0x%llx  reason %u  caller_ra 0x%llx  thread '%s'\n", (unsigned long long)tmp[i].n,
            (unsigned long long)tmp[i].rip, tmp[i].reason, (unsigned long long)tmp[i].ra, tmp[i].th);
  fflush(stderr);
}
static void sd_atexit(void) {
  pthread_mutex_lock(&s_sd_mu);
  if (s_sd_total) sd_print_locked("exit");
  pthread_mutex_unlock(&s_sd_mu);
}
static void sd_sample(GuestContext *ctx, uint64_t rip, uint32_t reason) {
  if (s_sd_every < 0) {
    const char *e = getenv("NPS5_TRACE_SLOWDISPATCH");
    int v = e ? atoi(e) : 0;
    if (v > 0) atexit(sd_atexit);
    s_sd_every = v > 0 ? v : 0;
  }
  if (!s_sd_every || (++t_sd_count % (uint64_t)s_sd_every)) return;
  uint64_t ra = 0;
  RecompShadow *s = ctx->shadow;
  RecompShadowEntry *top = (RecompShadowEntry *)(uintptr_t)ctx->shadow_sp;
  if (s && top >= s->lo && top < s->bottom) ra = top->guest_ra;
  char th[24] = {0};
  pthread_getname_np(pthread_self(), th, sizeof th);
  pthread_mutex_lock(&s_sd_mu);
  s_sd_total++;
  int i = 0;
  for (; i < s_sd_n; i++)
    if (s_sd[i].rip == rip && s_sd[i].ra == ra && strcmp(s_sd[i].th, th) == 0) break;
  if (i == s_sd_n && s_sd_n < SD_ROWS) { s_sd[i] = (SdRow){rip, ra, reason, 0, {0}}; memcpy(s_sd[i].th, th, sizeof th); s_sd_n++; }
  if (i < s_sd_n) s_sd[i].n++;
  if ((s_sd_total & 0xffff) == 0) sd_print_locked("periodic");
  pthread_mutex_unlock(&s_sd_mu);
}

uintptr_t recomp_flat_continue(GuestContext *ctx, uint64_t rip, uint32_t reason) {
  RecompShadow *s = ctx->shadow;
  uintptr_t host = 0;
  if (reason == RECOMP_MISS_RET) {
    // The guest returned to `rip`; ctx->rsp is the stack pointer after the RET. Entries whose
    // return-address slot is now below rsp belong to activations that are gone (unwound by
    // guest code, a discarded cache, a shim that ran guest code on a copy of the context);
    // the entry of this very return, if the cache still has it, is the oldest of them.
    if (s) atomic_fetch_add_explicit(&s->slow_rets, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_st_slow, 1, memory_order_relaxed);
    RecompBoundary *b = (RecompBoundary *)(uintptr_t)ctx->host_boundary;
    RecompShadowEntry *last = recomp_shadow_trim(ctx, ctx->rsp);
    if (b && ctx->rsp > b->guest_rsp) {
      // Above the stack of the innermost host -> guest entry: the only legal way there is the
      // return of the entered function.
      if (rip != b->guest_ra) {
        char what[200];
        snprintf(what, sizeof what,
                 "guest returned across a host boundary (HLE callback or thread entry; entered with rsp 0x%llx, return address 0x%llx) to",
                 (unsigned long long)b->guest_rsp, (unsigned long long)b->guest_ra);
        flat_fatal(ctx, rip, reason, what);
      }
      if (s) atomic_fetch_add_explicit(&s->slow_boundary, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_st_boundary, 1, memory_order_relaxed);
      return recomp_shadow_boundary_cont();
    }
    if (last && last->guest_ra == rip && last->host_cont) {
      if (s) atomic_fetch_add_explicit(&s->slow_hits, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_st_hit, 1, memory_order_relaxed);
      return (uintptr_t)last->host_cont;
    }
  }
  atomic_fetch_add_explicit(reason == RECOMP_MISS_RET ? &g_st_addr : &g_st_miss_calls, 1, memory_order_relaxed);
  if (reason != RECOMP_MISS_RET && s_sd_every != 0) sd_sample(ctx, rip, reason);
  if (reason != RECOMP_MISS_RET) {
    // The same indirect target 2^24 times in a row with no other slow dispatch between: almost
    // always a continuation that dispatches to itself (a tail edge to an address the translation has
    // no function for). Said once per thread; the run goes on.
    static _Thread_local uint64_t t_last_rip, t_same;
    if (rip == t_last_rip) {
      if (++t_same == (1u << 24)) {
        char th[32] = {0};
        pthread_getname_np(pthread_self(), th, sizeof th);
        fprintf(stderr, "[nps5] dispatch self-loop? thread '%s' dispatched to guest 0x%llx (reason %u) %u times in a row\n", th,
                (unsigned long long)rip, reason, 1u << 24);
      }
    } else {
      t_last_rip = rip;
      t_same = 0;
    }
  }
  host = recomp_cont_resolve(ctx, rip, reason);
  if (!host) {
    flat_fatal(ctx, rip, reason,
               reason == RECOMP_MISS_RET ? "guest RET to an address with no host continuation (not a return site, block or function entry):"
               : reason == RECOMP_MISS_RESUME ? "non-local resume at a guest address with no host continuation:"
               : "unresolved indirect jump/call to");
  }
  return host;
}

// ---------------------------------------------------------------- diagnostics
size_t recomp_shadow_chain(uint64_t top, uint64_t rsp, uint64_t *conts, size_t max) {
  RecompShadow *s = shadow_find(top);
  if (!s || (top & 7)) return 0;
  RecompShadowEntry *e = (RecompShadowEntry *)(uintptr_t)top;
  if (e < s->lo || e > s->bottom || ((uintptr_t)e - (uintptr_t)s->lo) % sizeof *e) return 0;
  uintptr_t bc = recomp_shadow_boundary_cont();
  size_t n = 0;
  for (; e < s->bottom && n < max; e++) {
    if (e->guest_rsp < rsp) continue; // popped by the guest without a RET through it
    rsp = e->guest_rsp;
    if (e->guest_ra == RECOMP_SHADOW_RA_POISON) continue;
    conts[n++] = e->host_cont == bc && bc ? RECOMP_CHAIN_BOUNDARY : e->host_cont;
  }
  return n;
}

// "f <- g <- h" by function name (needs the pc map), for log lines and notes. Not signal-safe.
static size_t chain_text(uint64_t top, uint64_t rsp, char *out, size_t cap) {
  uint64_t c[24];
  size_t n = recomp_shadow_chain(top, rsp, c, 24), len = 0, shown = 0;
  out[0] = 0;
  for (size_t i = 0; i < n; i++) {
    char tmp[128];
    uint64_t fn = 0;
    const char *nm = NULL;
    if (c[i] == RECOMP_CHAIN_BOUNDARY) snprintf(tmp, sizeof tmp, "<host boundary>");
    else if (!nps5_diag_host_pc_to_guest(c[i] - 4, NULL, &fn, &nm)) continue;
    else if (nm) snprintf(tmp, sizeof tmp, "%s", nm);
    else snprintf(tmp, sizeof tmp, "fn_0x%llx", (unsigned long long)fn);
    if (len + strlen(tmp) + 6 >= cap) break;
    len += (size_t)snprintf(out + len, cap - len, "%s%s", shown ? " <- " : "", tmp);
    shown++;
  }
  return shown;
}
