// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest memory arena: reservation, commit/decommit, fault classification. See nps5_mem.h.

#include "nps5_mem.h"

#include <errno.h>
#include <stdarg.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// Console layout in base mode; a title's main.c overrides this with its own definition.
__attribute__((weak)) const Nps5MemConfig nps5_mem_config = {
    .abi = NPS5_MEM_ABI,
    .addr_mode = NPS5_ADDR_BASE,
    .host_base = NPS5_BASE_SPAN,
    .guest_lo = 0x400000ULL,
    .guest_hi = 0xFC00000000ULL,
    .image_end = 0x400000ULL,
    .dyn_lo = 0x200000000ULL,
    .pool_lo = 0x1000000000ULL,
    .stack_top = 0x7EEFFC000ULL,
    .stack_len = 0x4000000ULL,
};

Nps5Arena g_nps5_arena;

// vmmap shows the arena as "Memory Tag 240".
#define NPS5_VM_TAG 240

static void errf(char *err, size_t errlen, size_t *pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void errf(char *err, size_t errlen, size_t *pos, const char *fmt, ...) {
  if (!err || *pos >= errlen) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(err + *pos, errlen - *pos, fmt, ap);
  va_end(ap);
  if (n > 0) *pos += (size_t)n < errlen - *pos ? (size_t)n : errlen - *pos - 1;
}

#if defined(__APPLE__)
// Appends the host regions overlapping [lo, lo+len) to err: what the user (or a bug report)
// needs to see when the arena cannot be reserved.
static void list_conflicts(uint64_t lo, uint64_t len, char *err, size_t errlen, size_t *pos) {
  mach_vm_address_t a = lo;
  int shown = 0;
  while (a < lo + len) {
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
      break;
    if (a >= lo + len) break;
    if (shown == 12) {
      errf(err, errlen, pos, "    ... more\n");
      break;
    }
    errf(err, errlen, pos, "    host 0x%llx-0x%llx (%llu MiB) prot %c%c%c tag %u%s\n", (unsigned long long)a,
         (unsigned long long)(a + sz), (unsigned long long)(sz >> 20), info.protection & VM_PROT_READ ? 'r' : '-',
         info.protection & VM_PROT_WRITE ? 'w' : '-', info.protection & VM_PROT_EXECUTE ? 'x' : '-', info.user_tag,
         (a >= NPS5_HOST_CARVEOUT_LO && a < NPS5_HOST_CARVEOUT_HI) ? " (GPU carveout)" : "");
    shown++;
    a += sz;
  }
  if (!shown) errf(err, errlen, pos, "    (no region reported; the range may exceed the process address space)\n");
}

// Non-destructive fixed reservation: VM_FLAGS_FIXED without VM_FLAGS_OVERWRITE fails with
// KERN_NO_SPACE when anything is mapped in the range. Pages start with no access.
static kern_return_t reserve_at(uint64_t lo, uint64_t len) {
  mach_vm_address_t a = lo;
  kern_return_t kr = mach_vm_map(mach_task_self(), &a, len, 0, VM_FLAGS_FIXED | VM_MAKE_TAG(NPS5_VM_TAG),
                                 MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE,
                                 VM_INHERIT_NONE);
  if (kr == KERN_SUCCESS && a != lo) { // cannot happen with VM_FLAGS_FIXED; never keep a stray mapping
    mach_vm_deallocate(mach_task_self(), a, len);
    return KERN_NO_SPACE;
  }
  return kr;
}
#endif

int nps5_mem_reserve(const Nps5MemConfig *cfg, uint64_t flat_size, char *err, size_t errlen) {
  size_t pos = 0;
  if (err && errlen) err[0] = '\0';
  if (g_nps5_arena.host_len) {
    errf(err, errlen, &pos, "guest arena is already reserved");
    return -1;
  }
  if (!cfg || cfg->abi != NPS5_MEM_ABI) {
    errf(err, errlen, &pos, "memory config ABI %u does not match the runtime's %u: rebuild the title",
         cfg ? cfg->abi : 0, NPS5_MEM_ABI);
    return -1;
  }
#if NPS5_ADDR_MODE_IDENTITY
  if (cfg->addr_mode != NPS5_ADDR_IDENTITY) {
    errf(err, errlen, &pos, "runtime was compiled for identity addressing but the title was translated in base mode");
    return -1;
  }
#else
  if (cfg->addr_mode != NPS5_ADDR_BASE) {
    errf(err, errlen, &pos, "title was translated in identity mode but the runtime was compiled without "
                            "-DNPS5_ADDR_MODE_IDENTITY=1");
    return -1;
  }
#endif
  if ((cfg->guest_lo | cfg->guest_hi | cfg->image_end | cfg->stack_top | cfg->stack_len | cfg->host_base) & NPS5_PAGE_MASK) {
    errf(err, errlen, &pos, "memory config is not 16 KiB aligned");
    return -1;
  }
#if defined(__APPLE__)
  Nps5Arena a = {0};
  a.mode = cfg->addr_mode;
  a.guest_lo = cfg->guest_lo;
  a.guest_hi = cfg->guest_hi;
  if (cfg->addr_mode == NPS5_ADDR_IDENTITY) {
    if (flat_size) {
      errf(err, errlen, &pos, "the flat test arena exists in base mode only");
      return -1;
    }
    if (cfg->guest_lo < NPS5_IDENTITY_LO || cfg->guest_hi > NPS5_IDENTITY_HI) {
      errf(err, errlen, &pos, "identity layout 0x%llx-0x%llx is outside the identity arena",
           (unsigned long long)cfg->guest_lo, (unsigned long long)cfg->guest_hi);
      return -1;
    }
    a.host_lo = a.guest_org = NPS5_IDENTITY_LO;
    a.host_len = NPS5_IDENTITY_HI - NPS5_IDENTITY_LO;
    kern_return_t kr = reserve_at(a.host_lo, a.host_len);
    if (kr != KERN_SUCCESS) {
      errf(err, errlen, &pos,
           "cannot reserve the identity arena host 0x%llx-0x%llx (kern_return %d). Identity mode has no "
           "alternative placement; retranslate with -addr-mode base. Host regions in the way:\n",
           (unsigned long long)a.host_lo, (unsigned long long)(a.host_lo + a.host_len), kr);
      list_conflicts(a.host_lo, a.host_len, err, errlen, &pos);
      return -1;
    }
  } else {
    if (cfg->guest_hi > NPS5_BASE_SPAN || !cfg->host_base || (cfg->host_base & (NPS5_BASE_SPAN - 1))) {
      errf(err, errlen, &pos, "base-mode config is invalid (host_base 0x%llx, guest_hi 0x%llx)",
           (unsigned long long)cfg->host_base, (unsigned long long)cfg->guest_hi);
      return -1;
    }
    if (flat_size) {
      flat_size = (flat_size + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
      if (flat_size > cfg->guest_hi) flat_size = cfg->guest_hi;
      a.flat = true;
      a.guest_lo = 0;
      a.guest_hi = flat_size;
    }
    a.host_len = NPS5_BASE_SPAN;
    // The bias is not baked into the translation, so an occupied preferred slot is not fatal:
    // try the following 1 TiB slots up to the end of the user address space (2^47, memory.md §2).
    kern_return_t first_kr = KERN_SUCCESS;
    bool ok = false;
    for (unsigned i = 0; !ok; i++) {
      uint64_t lo = cfg->host_base + (uint64_t)i * NPS5_BASE_SPAN;
      if (lo + NPS5_BASE_SPAN > 0x7F0000000000ULL) break;
      kern_return_t kr = reserve_at(lo, NPS5_BASE_SPAN);
      if (kr == KERN_SUCCESS) {
        a.host_lo = lo;
        ok = true;
      } else if (i == 0) {
        first_kr = kr;
      }
    }
    if (!ok) {
      errf(err, errlen, &pos,
           "cannot reserve 1 TiB of host address space for guest memory at 0x%llx or any following 1 TiB slot "
           "(kern_return %d). Host regions in the preferred slot:\n",
           (unsigned long long)cfg->host_base, first_kr);
      list_conflicts(cfg->host_base, NPS5_BASE_SPAN, err, errlen, &pos);
      return -1;
    }
    a.guest_org = 0;
    a.bias = (uint8_t *)(uintptr_t)a.host_lo;
  }
  g_nps5_arena = a;
  if (a.flat && nps5_mem_commit(0, a.guest_hi) != 0) {
    errf(err, errlen, &pos, "cannot commit the flat test arena: %s", strerror(errno));
    nps5_mem_release();
    return -1;
  }
  return 0;
#else
  (void)flat_size;
  errf(err, errlen, &pos, "guest arena reservation is implemented for macOS only");
  return -1;
#endif
}

static void reset_classification_tables(void);

void nps5_mem_release(void) {
#if defined(__APPLE__)
  if (g_nps5_arena.host_len) mach_vm_deallocate(mach_task_self(), g_nps5_arena.host_lo, g_nps5_arena.host_len);
#endif
  memset(&g_nps5_arena, 0, sizeof(g_nps5_arena));
  reset_classification_tables(); // the next arena must not classify addresses with the old one's images
}

// Host range of guest [addr, addr+len) if it is wholly inside the reservation.
static bool host_range(uint64_t addr, uint64_t len, uint8_t **host) {
  const Nps5Arena *a = &g_nps5_arena;
  if (!a->host_len || addr < a->guest_org) return false;
  uint64_t off = addr - a->guest_org;
  if (off > a->host_len || len > a->host_len - off) return false;
  *host = (uint8_t *)(uintptr_t)(a->host_lo + off);
  return true;
}

// A length so large that rounding [addr, addr+len) up to pages wraps the address space.
static bool len_wraps(uint64_t addr, uint64_t len) { return len > UINT64_MAX - addr - NPS5_PAGE_MASK; }

// ---- host access hooks ----
static Nps5HostAccessHooks g_access_hooks;
static _Atomic int g_have_access_hooks;
__attribute__((weak)) _Thread_local int g_nps5_in_crash_handler; // also defined (weak) in nps5_diag.c: either file links alone

void nps5_mem_set_host_access_hooks(const Nps5HostAccessHooks *hooks) {
  atomic_store_explicit(&g_have_access_hooks, 0, memory_order_release);
  if (!hooks) return;
  g_access_hooks = *hooks;
  atomic_store_explicit(&g_have_access_hooks, 1, memory_order_release);
}

static inline bool access_hooked(const void *p, size_t len) {
  if (!len || !atomic_load_explicit(&g_have_access_hooks, memory_order_acquire) || g_nps5_in_crash_handler) return false;
  const Nps5Arena *a = &g_nps5_arena;
  const uint64_t h = (uint64_t)(uintptr_t)p;
  return a->host_len && h < a->host_lo + a->host_len && h + len > a->host_lo; // overlaps the reservation
}

bool nps5_host_access_active(void) { return atomic_load_explicit(&g_have_access_hooks, memory_order_acquire) && !g_nps5_in_crash_handler; }

void nps5_host_write_begin(void *p, size_t len) {
  if (access_hooked(p, len) && g_access_hooks.write_begin) g_access_hooks.write_begin(p, len);
}
void nps5_host_write_end(void *p, size_t len) {
  if (access_hooked(p, len) && g_access_hooks.write_end) g_access_hooks.write_end(p, len);
}
static _Atomic uint64_t g_write_refaults;
uint64_t nps5_host_write_refault_count(void) { return atomic_load_explicit(&g_write_refaults, memory_order_relaxed); }
bool nps5_host_write_refault(void *p, size_t len, unsigned *tries) {
  if (!access_hooked(p, len) || !g_access_hooks.write_begin || *tries >= NPS5_KIO_RETRIES) return false;
  const unsigned t = ++*tries;
  const uint64_t n = atomic_fetch_add_explicit(&g_write_refaults, 1, memory_order_relaxed) + 1;
  if (n <= 20 || n % 1000 == 0)
    fprintf(stderr, "[kio] kernel write into guest memory host %p +0x%zx failed with EFAULT: a page was closed again after it was opened; opened again, retry %u (%llu so far)\n",
            p, len, t, (unsigned long long)n);
  if (t > 4) sched_yield();
  g_access_hooks.write_begin(p, len);
  return true;
}
void nps5_host_read_prepare(const void *p, size_t len) {
  if (access_hooked(p, len) && g_access_hooks.read_prepare) g_access_hooks.read_prepare(p, len);
}

int nps5_mem_host_protect(void *host, size_t len, int prot) {
  if (atomic_load_explicit(&g_have_access_hooks, memory_order_acquire) && g_access_hooks.protect) {
    const int e = g_access_hooks.protect(host, len, prot);
    if (e == 0) return 0;
    errno = e;
    return -1;
  }
  return mprotect(host, len, prot);
}

int nps5_mem_commit(uint64_t addr, uint64_t len) {
  if (len_wraps(addr, len)) {
    errno = EINVAL;
    return -1;
  }
  uint64_t lo = addr & ~NPS5_PAGE_MASK;
  uint64_t hi = (addr + len + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  uint8_t *h;
  if (hi < lo || !host_range(lo, hi - lo, &h)) {
    errno = EINVAL;
    return -1;
  }
  if (hi == lo) return 0;
  return nps5_mem_host_protect(h, hi - lo, PROT_READ | PROT_WRITE);
}

int nps5_mem_decommit(uint64_t addr, uint64_t len) {
  if (len_wraps(addr, len)) {
    errno = EINVAL;
    return -1;
  }
  uint64_t lo = addr & ~NPS5_PAGE_MASK;
  uint64_t hi = (addr + len + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  uint8_t *h;
  if (hi < lo || !host_range(lo, hi - lo, &h)) {
    errno = EINVAL;
    return -1;
  }
  if (hi == lo) return 0;
  if (g_nps5_arena.flat) { // the test arena stays readable and writable; just drop the pages
    void *p = mmap(h, hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, VM_MAKE_TAG(NPS5_VM_TAG), 0);
    return p == MAP_FAILED ? -1 : 0;
  }
  // Replaces the pages, and any direct-memory alias mapped there, with inaccessible zero-fill,
  // inside our own reservation (checked by host_range). mach_vm_map keeps the maximum protection at
  // read-write like the reservation's (mmap(MAP_FIXED) would make it RWX: guest memory must never
  // become executable on the host, review 1 n1).
  mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)h;
  kern_return_t kr = mach_vm_map(mach_task_self(), &a, hi - lo, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(NPS5_VM_TAG),
                                 MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) {
    errno = ENOMEM;
    return -1;
  }
  // Tell the protect hook (a write watch keeps its own record of the guest protection): the pages are now inaccessible, as
  // they would be after a guest mprotect(PROT_NONE); without this the record can say "read-write" for a decommitted page.
  return nps5_mem_host_protect(h, hi - lo, PROT_NONE);
}

int nps5_mem_protect_inner(uint64_t addr, uint64_t len, int prot) {
  if (len_wraps(addr, len)) {
    errno = EINVAL;
    return -1;
  }
  uint64_t lo = (addr + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  uint64_t hi = (addr + len) & ~NPS5_PAGE_MASK;
  uint8_t *h;
  if (hi <= lo) return 0;
  if (!host_range(lo, hi - lo, &h)) {
    errno = EINVAL;
    return -1;
  }
  if (g_nps5_arena.flat) return 0;
  return nps5_mem_host_protect(h, hi - lo, prot);
}

bool nps5_mem_safe_read(const void *host, void *dst, size_t len) {
  if (!len) return true;
#if defined(__APPLE__)
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)host, len,
                                            (mach_vm_address_t)(uintptr_t)dst, &got);
  if (kr == KERN_SUCCESS && got == len) return true;
  // A page a host library keeps inaccessible to trap reads (host access hooks) is readable to the guest: let the
  // library release it and try once more. Never from the crash handler.
  if (!access_hooked(host, len) || !g_access_hooks.read_prepare) return false;
  g_access_hooks.read_prepare(host, len);
  got = 0;
  kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)host, len, (mach_vm_address_t)(uintptr_t)dst, &got);
  return kr == KERN_SUCCESS && got == len;
#else
  (void)host; (void)dst;
  return false;
#endif
}

static bool range_readable_now(const void *host, size_t len);
bool nps5_mem_range_readable(const void *host, size_t len) {
  if (!len) return true;
  if (range_readable_now(host, len)) return true;
  if (!access_hooked(host, len) || !g_access_hooks.read_prepare) return false;
  g_access_hooks.read_prepare(host, len); // a read-trapped page (see nps5_mem_safe_read)
  return range_readable_now(host, len);
}

static bool range_readable_now(const void *host, size_t len) {
#if defined(__APPLE__)
  const mach_vm_address_t start = (mach_vm_address_t)(uintptr_t)host, end = start + len;
  if (end < start) return false;
  mach_vm_address_t cur = start;
  while (cur < end) {
    mach_vm_address_t addr = cur;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS ||
        addr > cur || size == 0 || !(info.protection & VM_PROT_READ))
      return false;
    cur = addr + size;
  }
  return true;
#else
  (void)host;
  return false;
#endif
}

bool nps5_guest_safe_read(uint64_t g, void *dst, size_t len) {
  const Nps5Arena *a = &g_nps5_arena;
  uint8_t *h;
  if (!a->host_len || g < a->guest_org || !host_range(g, len, &h)) return false;
  return nps5_mem_safe_read(h, dst, len);
}

// Host protection for guest pages of loaded images inside [lo, hi): the guest's mprotect of its own
// code or data. Only whole pages that a noted image range covers are touched, so address space
// that is not committed stays inaccessible. Returns how many bytes were changed.
uint64_t nps5_mem_protect_image(uint64_t lo, uint64_t hi, int prot);

// ---- Classification ----------------------------------------------------------------------

#define MAX_FIXED 4096
typedef struct FixedRange {
  uint64_t lo, hi;
  Nps5FaultClass cls;
  char name[32];
} FixedRange;
static FixedRange g_fixed[MAX_FIXED];
static _Atomic size_t g_fixed_n;
static _Atomic bool g_fixed_truncated;

void nps5_mem_note_range(Nps5FaultClass cls, uint64_t lo, uint64_t hi, const char *name) {
  size_t n = atomic_load_explicit(&g_fixed_n, memory_order_relaxed);
  if (hi <= lo) return;
  if (n >= MAX_FIXED) { // more modules than any title has: say so in the map dump instead of staying silent
    atomic_store_explicit(&g_fixed_truncated, true, memory_order_relaxed);
    return;
  }
  FixedRange *r = &g_fixed[n];
  r->lo = lo;
  r->hi = hi;
  r->cls = cls;
  strncpy(r->name, name ? name : "", sizeof(r->name) - 1);
  atomic_store_explicit(&g_fixed_n, n + 1, memory_order_release); // entry is complete before it is visible
}

// Guest mappings, published under a sequence lock: the writer (VM extent manager, under its
// mutex) bumps the sequence to odd, copies, bumps to even; the crash handler retries a few
// times and gives up rather than block. The entries live in a buffer that only ever grows: a
// larger one is allocated, filled and published inside the writer's odd window, and the old one
// is never freed (the handler may be reading it; the total leak is bounded by the final size), so
// the reader needs neither a lock nor allocation and no number of mappings is dropped.
#define MAP_INIT_CAP 256
#define MAP_MAX_CAP (1u << 20)
typedef struct Nps5MapEntry {
  uint64_t lo, hi;
  char name[16];
  uint8_t kind;
} Nps5MapEntry;
typedef struct MapBuf {
  size_t cap;
  Nps5MapEntry e[];
} MapBuf;
static _Atomic(MapBuf *) g_mapbuf;
static _Atomic size_t g_map_n;
static _Atomic uint64_t g_map_seq;
static _Atomic bool g_map_truncated; // only when memory ran out or MAP_MAX_CAP was exceeded
static size_t g_map_fill;            // writer only

void nps5_mem_map_begin(void) {
  atomic_fetch_add_explicit(&g_map_seq, 1, memory_order_acq_rel); // odd: readers retry
  g_map_fill = 0;
  atomic_store_explicit(&g_map_truncated, false, memory_order_relaxed);
}

void nps5_mem_map_add(uint64_t lo, uint64_t hi, const char *name, int kind) {
  MapBuf *b = atomic_load_explicit(&g_mapbuf, memory_order_relaxed);
  // Thread stacks and guards are never merged: the guard of the second of two adjacent stacks
  // is the first page of its own entry (nps5_mem_classify).
  if (b && g_map_fill > 0 && b->e[g_map_fill - 1].hi == lo && name && kind != NPS5_EXT_THREAD_STACK &&
      kind != NPS5_EXT_STACK_GUARD && b->e[g_map_fill - 1].kind == kind &&
      strncmp(b->e[g_map_fill - 1].name, name, sizeof(b->e[0].name) - 1) == 0) {
    b->e[g_map_fill - 1].hi = hi; // adjacent pieces of one mapping
    return;
  }
  if (!b || g_map_fill == b->cap) {
    size_t cap = b ? b->cap * 2 : MAP_INIT_CAP;
    MapBuf *nb = cap <= MAP_MAX_CAP ? (MapBuf *)malloc(sizeof(MapBuf) + cap * sizeof(Nps5MapEntry)) : NULL;
    if (!nb) {
      atomic_store_explicit(&g_map_truncated, true, memory_order_relaxed);
      return;
    }
    nb->cap = cap;
    if (b) memcpy(nb->e, b->e, g_map_fill * sizeof(Nps5MapEntry));
    atomic_store_explicit(&g_mapbuf, nb, memory_order_release); // the old buffer is leaked on purpose
    b = nb;
  }
  Nps5MapEntry *m = &b->e[g_map_fill++];
  m->lo = lo;
  m->hi = hi;
  m->kind = (uint8_t)kind;
  memset(m->name, 0, sizeof(m->name));
  if (name) strncpy(m->name, name, sizeof(m->name) - 1);
}

void nps5_mem_map_end(void) {
  atomic_store_explicit(&g_map_n, g_map_fill, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_map_seq, 1, memory_order_acq_rel);
}

static void reset_classification_tables(void) {
  atomic_store_explicit(&g_fixed_n, 0, memory_order_release);
  atomic_store_explicit(&g_fixed_truncated, false, memory_order_relaxed);
  nps5_mem_map_begin(); // an empty snapshot; the buffer is kept for the next arena
  nps5_mem_map_end();
}

uint64_t nps5_mem_protect_image(uint64_t lo, uint64_t hi, int prot) {
  uint64_t done = 0;
  size_t nf = atomic_load_explicit(&g_fixed_n, memory_order_acquire);
  for (size_t i = 0; i < nf; i++) {
    if (g_fixed[i].cls != NPS5_FA_IMAGE || g_fixed[i].lo >= hi || g_fixed[i].hi <= lo) continue;
    const uint64_t a = g_fixed[i].lo > lo ? g_fixed[i].lo : lo, b = g_fixed[i].hi < hi ? g_fixed[i].hi : hi;
    if (nps5_mem_protect_inner(a, b - a, prot) == 0) done += b - a;
  }
  return done;
}

const char *nps5_fault_class_name(Nps5FaultClass c) {
  switch (c) {
  case NPS5_FA_NOT_GUEST: return "not guest memory";
  case NPS5_FA_NULL_GUARD: return "guest null guard (null or near-null guest pointer)";
  case NPS5_FA_IMAGE: return "guest image";
  case NPS5_FA_SYSTEM: return "guest system block (main TLS/TCB/args)";
  case NPS5_FA_STACK_GUARD: return "guest stack guard page (stack overflow)";
  case NPS5_FA_STACK: return "guest main stack";
  case NPS5_FA_UNMAPPED: return "unmapped guest address";
  case NPS5_FA_MAPPED: return "guest mapping";
  case NPS5_FA_BEYOND: return "beyond the guest address space";
  }
  return "?";
}

void nps5_mem_classify(uint64_t host, Nps5FaultInfo *out) {
  const Nps5Arena *a = &g_nps5_arena;
  memset(out, 0, sizeof(*out));
  if (!a->host_len || host < a->host_lo || host - a->host_lo >= a->host_len) {
    out->cls = NPS5_FA_NOT_GUEST;
    return;
  }
  uint64_t g = a->guest_org + (host - a->host_lo);
  out->guest = g;
  if (g < a->guest_lo) {
    out->cls = NPS5_FA_NULL_GUARD;
    out->lo = a->guest_org;
    out->hi = a->guest_lo;
    return;
  }
  if (g >= a->guest_hi) {
    out->cls = NPS5_FA_BEYOND;
    out->lo = a->guest_hi;
    out->hi = a->guest_org + a->host_len;
    return;
  }
  size_t nf = atomic_load_explicit(&g_fixed_n, memory_order_acquire);
  for (size_t i = 0; i < nf; i++) {
    if (g >= g_fixed[i].lo && g < g_fixed[i].hi) {
      out->cls = g_fixed[i].cls;
      out->lo = g_fixed[i].lo;
      out->hi = g_fixed[i].hi;
      memcpy(out->name, g_fixed[i].name, sizeof(out->name));
      return;
    }
  }
  out->cls = NPS5_FA_UNMAPPED;
  for (int attempt = 0; attempt < 4; attempt++) {
    uint64_t s0 = atomic_load_explicit(&g_map_seq, memory_order_acquire);
    if (s0 & 1) continue;
    const MapBuf *b = atomic_load_explicit(&g_mapbuf, memory_order_acquire);
    size_t n = atomic_load_explicit(&g_map_n, memory_order_relaxed);
    if (!b) n = 0;
    else if (n > b->cap) n = b->cap; // n and b are republished together; never read past this buffer
    Nps5FaultInfo hit = *out;
    hit.truncated = atomic_load_explicit(&g_map_truncated, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
      if (g >= b->e[i].lo && g < b->e[i].hi) {
        hit.cls = NPS5_FA_MAPPED;
        hit.truncated = false;
        hit.lo = b->e[i].lo;
        hit.hi = b->e[i].hi;
        memcpy(hit.name, b->e[i].name, sizeof(b->e[i].name));
        hit.name[sizeof(b->e[i].name) - 1] = '\0';
        // The main stack's guard is its own extent; a thread stack's is the first page of
        // its "thread_stack" mapping (recomp_create_thread_context).
        const int kind = b->e[i].kind;
        if (kind == NPS5_EXT_STACK_GUARD || (kind == NPS5_EXT_THREAD_STACK && g < hit.lo + NPS5_PAGE)) {
          hit.cls = NPS5_FA_STACK_GUARD;
          if (kind == NPS5_EXT_THREAD_STACK) hit.hi = hit.lo + NPS5_PAGE;
        }
        break;
      }
    }
    // The data loads above must not be reordered after the sequence re-check.
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&g_map_seq, memory_order_acquire) == s0) {
      *out = hit;
      return;
    }
  }
}

void nps5_mem_dump_map(int fd) {
  const Nps5Arena *a = &g_nps5_arena;
  char line[192];
  int n = snprintf(line, sizeof(line),
                   "guest arena: mode %s, host 0x%llx-0x%llx, bias 0x%llx, guest 0x%llx-0x%llx%s\n",
                   a->mode == NPS5_ADDR_IDENTITY ? "identity" : "base", (unsigned long long)a->host_lo,
                   (unsigned long long)(a->host_lo + a->host_len), (unsigned long long)(uintptr_t)a->bias,
                   (unsigned long long)a->guest_lo, (unsigned long long)a->guest_hi, a->flat ? " (flat test arena)" : "");
  if (n > 0) (void)!write(fd, line, (size_t)n);
  size_t nf = atomic_load_explicit(&g_fixed_n, memory_order_acquire);
  for (size_t i = 0; i < nf; i++) {
    n = snprintf(line, sizeof(line), "  0x%012llx-0x%012llx %-12s %s\n", (unsigned long long)g_fixed[i].lo,
                 (unsigned long long)g_fixed[i].hi, g_fixed[i].cls == NPS5_FA_IMAGE ? "image" : g_fixed[i].cls == NPS5_FA_STACK ? "stack" : "fixed",
                 g_fixed[i].name);
    if (n > 0) (void)!write(fd, line, (size_t)n);
  }
  const MapBuf *mb = atomic_load_explicit(&g_mapbuf, memory_order_acquire);
  size_t nm = atomic_load_explicit(&g_map_n, memory_order_relaxed);
  if (!mb) nm = 0;
  else if (nm > mb->cap) nm = mb->cap;
  for (size_t i = 0; i < nm; i++) {
    n = snprintf(line, sizeof(line), "  0x%012llx-0x%012llx %-12s %.16s\n", (unsigned long long)mb->e[i].lo,
                 (unsigned long long)mb->e[i].hi, "mapping", mb->e[i].name);
    if (n > 0) (void)!write(fd, line, (size_t)n);
  }
  if (atomic_load_explicit(&g_fixed_truncated, memory_order_relaxed)) {
    static const char more[] = "  ... more fixed ranges than the table holds\n";
    (void)!write(fd, more, sizeof(more) - 1);
  }
  if (atomic_load_explicit(&g_map_truncated, memory_order_relaxed)) {
    static const char more[] = "  ... more mappings than the snapshot holds\n";
    (void)!write(fd, more, sizeof(more) - 1);
  }
}

// Checked pointer translation failed (nps5_thunk.h, NPS5_CHECKED_PTR): say which pointer and stop.
void nps5_bad_guest_pointer(uint64_t g) {
  char line[160];
  int n = snprintf(line, sizeof(line),
                   "FATAL: guest pointer 0x%llx is outside the guest window (end 0x%llx): refusing to translate it\n",
                   (unsigned long long)g, (unsigned long long)g_nps5_arena.guest_hi);
  if (n > 0) (void)!write(2, line, (size_t)n);
  abort();
}
