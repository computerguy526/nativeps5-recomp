// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest VM extent manager (docs/m1/memory.md "Guest layout", docs/m1/memory-fixes-1.md).
//
// The extent list is the guest's view of its address space above the system block: sorted,
// contiguous, every byte either free or part of a mapping. Granularity is the 16 KiB page (guest
// and host pages are the same size, so no sub-page emulation exists anywhere). A mapping's pages
// are committed in the host arena when it is created and decommitted (PROT_NONE, zero-fill) when
// it is freed; a reservation (NPS5_EXT_RESERVED, sceKernelReserveVirtualRange) holds address
// space without pages until something is mapped into it.
//
// What an extent is is its `kind`, set at creation. The name is for display and for the guest's
// sceKernelSetVirtualRangeName; no decision depends on it.
//
// Locking: vm_mutex guards the list. It is never held while calling out (the direct-memory
// unmap hook, host page operations other than the cheap commit of a fresh range), so the
// direct-memory lock may be held by a caller of this file (docs/m1/memory-fixes-1.md B1).

#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "nps5_mem.h"
#include "ps4_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

GuestContext *g_nps5_vm_root; // the process context; set once by recomp_init_runtime

void ps4_direct_mem_on_unmap(GuestContext *ctx, uint64_t addr, size_t size);

// ---- Mapping notification hooks (recomp_runtime.h Nps5VmHooks) -------------------------------
// Lock order: (direct-memory lock) -> vm_mutex -> g_hooks_lock (read). on_map and on_protect run
// under vm_mutex, so the observer sees changes in the same order as the extent list; on_unmap runs
// with vm_mutex dropped but the extents busy (nobody else can take, split or free them) and before
// any host page is released, so it may block (the AGC driver waits for GPU jobs that lease the range).
#define VM_MAX_HOOKS 4
static pthread_rwlock_t g_hooks_lock = PTHREAD_RWLOCK_INITIALIZER;
static Nps5VmHooks g_hooks[VM_MAX_HOOKS];
static bool g_hook_used[VM_MAX_HOOKS];

static void vm_hook_map(uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  pthread_rwlock_rdlock(&g_hooks_lock);
  for (int i = 0; i < VM_MAX_HOOKS; i++)
    if (g_hook_used[i] && g_hooks[i].on_map) g_hooks[i].on_map(g_hooks[i].user, addr, size, prot, kind, name);
  pthread_rwlock_unlock(&g_hooks_lock);
}
static void vm_hook_unmap(uint64_t addr, uint64_t size) {
  pthread_rwlock_rdlock(&g_hooks_lock);
  for (int i = 0; i < VM_MAX_HOOKS; i++)
    if (g_hook_used[i] && g_hooks[i].on_unmap) g_hooks[i].on_unmap(g_hooks[i].user, addr, size);
  pthread_rwlock_unlock(&g_hooks_lock);
}
static void vm_hook_protect(uint64_t addr, uint64_t size, int prot, int kind, const char *name) {
  pthread_rwlock_rdlock(&g_hooks_lock);
  for (int i = 0; i < VM_MAX_HOOKS; i++)
    if (g_hook_used[i] && g_hooks[i].on_protect) g_hooks[i].on_protect(g_hooks[i].user, addr, size, prot, kind, name);
  pthread_rwlock_unlock(&g_hooks_lock);
}

// Whether mapping an extent of this kind reports on_map from the VM layer itself. A direct mapping does not: its
// host pages are only final once the direct-memory layer has aliased the physical block over the range
// (mach_vm_remap replaces the pages), and an observer that imports the pages at on_map (the AGC driver hands them to
// the GPU) would keep the replaced ones. ps4_direct_mem.c reports it with recomp_vm_hooks_report_map afterwards.
static bool vm_hook_reports_map(int kind) { return kind != NPS5_EXT_RESERVED && kind != NPS5_EXT_DIRECT; }

void recomp_vm_hooks_report_map(GuestContext *ctx, uint64_t addr, uint64_t size) {
  GuestContext *proc = ctx && ctx->process_ctx ? ctx->process_ctx : ctx;
  if (!proc || !size) return;
  const uint64_t end = addr + size;
  pthread_mutex_lock(&proc->vm_mutex);
  for (const GuestVMExtent *e = proc->vm_extents; e && e->addr < end; e = e->next)
    if (e->addr + e->size > addr && !e->is_free && !e->busy && e->kind != NPS5_EXT_RESERVED)
      vm_hook_map(e->addr, e->size, e->prot, e->kind, e->name);
  pthread_mutex_unlock(&proc->vm_mutex);
}

void recomp_vm_hooks_report_alias(uint64_t a, uint64_t b, uint64_t size) {
  if (!size) return;
  pthread_rwlock_rdlock(&g_hooks_lock);
  for (int i = 0; i < VM_MAX_HOOKS; i++)
    if (g_hook_used[i] && g_hooks[i].on_alias) g_hooks[i].on_alias(g_hooks[i].user, a, b, size);
  pthread_rwlock_unlock(&g_hooks_lock);
}

int recomp_vm_hooks_register(const Nps5VmHooks *h) {
  GuestContext *root = g_nps5_vm_root;
  if (!h || !root) return -1;
  int id = -1;
  pthread_rwlock_wrlock(&g_hooks_lock);
  for (int i = 0; i < VM_MAX_HOOKS && id < 0; i++)
    if (!g_hook_used[i]) { g_hooks[i] = *h; g_hook_used[i] = true; id = i; }
  pthread_rwlock_unlock(&g_hooks_lock);
  if (id < 0) return -1;
  // Replay what is mapped now, under vm_mutex so that no change slips between the replay and the
  // first live notification (the new hook may also see a change once before and once during the
  // replay: on_map for a range it already has must be accepted).
  if (h->on_map) {
    pthread_mutex_lock(&root->vm_mutex);
    for (const GuestVMExtent *e = root->vm_extents; e; e = e->next)
      if (!e->is_free && !e->busy && e->kind != NPS5_EXT_RESERVED) h->on_map(h->user, e->addr, e->size, e->prot, e->kind, e->name);
    pthread_mutex_unlock(&root->vm_mutex);
  }
  return id;
}

void recomp_vm_hooks_unregister(int id) {
  if (id < 0 || id >= VM_MAX_HOOKS) return;
  pthread_rwlock_wrlock(&g_hooks_lock); // waits for hook calls in flight
  g_hook_used[id] = false;
  memset(&g_hooks[id], 0, sizeof g_hooks[id]);
  pthread_rwlock_unlock(&g_hooks_lock);
}

static inline uint64_t vm_align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// First address >= lo inside free extent e where [addr, addr+size) fits below hi, or 0.
static uint64_t vm_fit(const GuestVMExtent *e, uint64_t lo, uint64_t hi, uint64_t size, uint64_t align) {
  uint64_t start = e->addr > lo ? e->addr : lo;
  uint64_t end = e->addr + e->size < hi ? e->addr + e->size : hi;
  uint64_t cand = vm_align_up(start, align);
  if (cand < start || cand >= end || size > end - cand) return 0;
  return cand;
}

// Carves [addr, addr+size) out of extent e (free, or a reservation) and returns the extent that
// now covers exactly that range, or NULL when bookkeeping memory is unavailable. Caller holds
// vm_mutex.
static GuestVMExtent *vm_carve(GuestContext *proc, GuestVMExtent *e, uint64_t addr, uint64_t size) {
  if (addr > e->addr) {
    GuestVMExtent *prefix = (GuestVMExtent *)calloc(1, sizeof(GuestVMExtent));
    if (!prefix) return NULL;
    *prefix = *e;
    prefix->size = addr - e->addr;
    prefix->next = e;
    if (e->prev) e->prev->next = prefix;
    else proc->vm_extents = prefix;
    e->prev = prefix;
    e->addr = addr;
    e->size -= prefix->size;
  }
  if (e->size > size) {
    GuestVMExtent *suffix = (GuestVMExtent *)calloc(1, sizeof(GuestVMExtent));
    if (!suffix) return NULL; // a harmless split of the prefix remains
    *suffix = *e;
    suffix->addr = e->addr + size;
    suffix->size = e->size - size;
    suffix->prev = e;
    if (e->next) e->next->prev = suffix;
    e->next = suffix;
    e->size = size;
  }
  return e;
}

static void vm_tag(GuestVMExtent *e, int prot, int flags, const char *name, int kind) {
  e->is_free = false;
  e->busy = false;
  e->kind = kind;
  e->prot = prot;
  e->flags = flags;
  e->mtype = 0;
  memset(e->name, 0, sizeof(e->name));
  if (name) strncpy(e->name, name, sizeof(e->name) - 1);
}

static void vm_make_free(GuestVMExtent *e) {
  e->is_free = true;
  e->busy = false;
  e->kind = NPS5_EXT_MAPPED;
  e->prot = 0;
  e->flags = 0;
  e->mtype = 0;
  memset(e->name, 0, sizeof(e->name));
}

// The main stack and its guard page are the runtime's: the guest can neither unmap nor replace them.
static bool vm_pinned(const GuestVMExtent *e) {
  return !e->is_free && (e->kind == NPS5_EXT_MAIN_STACK || e->kind == NPS5_EXT_STACK_GUARD);
}

// Splits the extent (free or not) that strictly contains `at` into [addr, at) and [at, end); both
// halves keep every attribute. Extents being replaced (busy) are left alone. Caller holds vm_mutex.
// False only when memory for the second half is unavailable (nothing is changed then).
static bool vm_split(GuestContext *proc, uint64_t at, bool include_free) {
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if ((e->is_free && !include_free) || e->busy || at <= e->addr || at >= e->addr + e->size) continue;
    GuestVMExtent *tail = (GuestVMExtent *)calloc(1, sizeof(*tail));
    if (!tail) return false;
    *tail = *e;
    tail->addr = at;
    tail->size = e->addr + e->size - at;
    tail->prev = e;
    e->size = at - e->addr;
    e->next = tail;
    if (tail->next) tail->next->prev = tail;
    return true;
  }
  return true; // no extent to split: the boundary already exists (or is in a hole)
}

bool recomp_vm_split_extent_at(GuestContext *proc, uint64_t at) { return vm_split(proc, at, false); }

// Search order by zone (memory.md "Guest layout"). The system zone keeps runtime-owned guest
// objects and thread stacks below 0x2_0000_0000, out of the addresses titles hardcode; the user
// zone starts where the console's kernel starts handing out unhinted mappings; pools go to the
// user area above the 64 GiB line as before.
enum { VM_ZONE_SYSTEM, VM_ZONE_USER };

static uint64_t vm_alloc_zone(GuestContext *ctx, size_t size, size_t alignment, int prot, int flags, const char *name,
                              int kind, int zone) {
  if (!ctx || size == 0)
    return (uint64_t)-1;
  if (alignment < NPS5_PAGE)
    alignment = NPS5_PAGE;
  if (alignment > (g_nps5_arena.guest_hi ? g_nps5_arena.guest_hi : (1ULL << 40)))
    return (uint64_t)-1; // no address in the window is aligned that far
  if (alignment & (alignment - 1)) // not a power of two: round up to one
    alignment = (size_t)1 << (64 - __builtin_clzll((unsigned long long)alignment));
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  uint64_t aligned_size = vm_align_up(size, NPS5_PAGE);
  if (aligned_size < size)
    return (uint64_t)-1;

  const uint64_t lo = g_nps5_arena.guest_lo, hi = g_nps5_arena.guest_hi;
  uint64_t dyn = nps5_mem_config.dyn_lo, pool = nps5_mem_config.pool_lo;
  if (dyn < lo || dyn >= hi) dyn = lo; // flat test arena: one zone
  if (pool < dyn || pool >= hi) pool = dyn;
  uint64_t win[3][2];
  int nwin = 0;
  if (zone == VM_ZONE_SYSTEM) {
    win[nwin][0] = lo, win[nwin++][1] = dyn;
    win[nwin][0] = dyn, win[nwin++][1] = hi;
  } else {
    if (kind == NPS5_EXT_POOL)
      win[nwin][0] = pool, win[nwin++][1] = hi;
    win[nwin][0] = dyn, win[nwin++][1] = hi;
    win[nwin][0] = lo, win[nwin++][1] = dyn;
  }

  PS4_LOGF(NPS5_S_VM, NPS5_L_DEBUG, "ps4-vm", "vm alloc (%s): size=0x%zx align=0x%zx flags=0x%x zone=%s\n",
           name ? name : "null", size, alignment, flags, zone == VM_ZONE_SYSTEM ? "system" : "user");

  pthread_mutex_lock(&proc->vm_mutex);
  GuestVMExtent *best = NULL;
  uint64_t addr = 0;
  for (int w = 0; w < nwin && !best; w++) {
    for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
      if (!e->is_free || e->busy) continue;
      if (e->addr >= win[w][1]) break; // sorted: nothing further is inside the window
      if ((addr = vm_fit(e, win[w][0], win[w][1], aligned_size, alignment)) != 0) {
        best = e;
        break;
      }
    }
  }
  if (!best) {
    fprintf(stderr, "[vmm] ERROR: no free guest range for size=0x%zx align=0x%zx name=%s\n", size, alignment,
            name ? name : "(null)");
    for (GuestVMExtent *e = proc->vm_extents; e; e = e->next)
      if (e->is_free)
        fprintf(stderr, "  [free extent] addr=0x%llx size=0x%zx\n", (unsigned long long)e->addr, e->size);
    pthread_mutex_unlock(&proc->vm_mutex);
    return (uint64_t)-1;
  }
  GuestVMExtent *e = vm_carve(proc, best, addr, aligned_size);
  if (!e || (kind != NPS5_EXT_RESERVED && nps5_mem_commit(addr, aligned_size) != 0)) {
    if (e) fprintf(stderr, "[vmm] ERROR: cannot commit guest 0x%llx (+0x%llx): %s\n", (unsigned long long)addr,
                   (unsigned long long)aligned_size, strerror(errno));
    pthread_mutex_unlock(&proc->vm_mutex);
    return (uint64_t)-1; // the range stays free (possibly split)
  }
  vm_tag(e, prot, flags, name, kind);
  recomp_vm_note_change();
  if (vm_hook_reports_map(kind)) vm_hook_map(addr, aligned_size, prot, kind, name);
  pthread_mutex_unlock(&proc->vm_mutex);
  // printed after the unlock: stdout may block, and vm_mutex is also taken by the renderer
  PS4_LOGF(NPS5_S_VM, NPS5_L_DEBUG, "ps4-vm", "vm alloc (%s) -> 0x%llx (+0x%llx)\n", name ? name : "null",
           (unsigned long long)addr, (unsigned long long)aligned_size);
  return addr;
}

// Runtime-owned guest memory (thread stacks, HLE objects the guest reads): system zone. The
// kind is derived from the name here and only here: these callers are the runtime's own.
uint64_t recomp_vm_alloc_named_aligned(GuestContext *ctx, size_t size,
                                       size_t alignment, int prot, int flags,
                                       const char *name) {
  const int kind = name && strcmp(name, "thread_stack") == 0 ? NPS5_EXT_THREAD_STACK : NPS5_EXT_MAPPED;
  return vm_alloc_zone(ctx, size, alignment, prot, flags, name, kind, VM_ZONE_SYSTEM);
}

// A mapping the guest asked for without naming an address (mmap(NULL), flexible and direct
// memory, virtual range reservations): user zone, lowest free address from 0x2_0000_0000.
uint64_t recomp_vm_map_guest(GuestContext *ctx, size_t size, size_t alignment, int prot, int flags,
                             const char *name) {
  return vm_alloc_zone(ctx, size, alignment, prot, flags, name, NPS5_EXT_MAPPED, VM_ZONE_USER);
}

uint64_t recomp_vm_alloc_named(GuestContext *ctx, size_t size, int prot,
                               int flags, const char *name) {
  return recomp_vm_alloc_named_aligned(ctx, size, NPS5_PAGE, prot, flags, name);
}

uint64_t recomp_vm_alloc(GuestContext *ctx, size_t size) {
  return recomp_vm_alloc_named(ctx, size, PROT_READ | PROT_WRITE, 0, "anon");
}

// ---- Placing a mapping at a chosen address: one critical section ----------------------------
// PLACE_REPLACE   MAP_FIXED: whatever the guest has mapped in the range is unmapped first
// PLACE_EXCL      MAP_FIXED|MAP_EXCL/NO_OVERWRITE: only free space and reservations
// PLACE_FREE_ONLY a hint: only free space (a reservation is a mapping; a hint never splits it)
// The check, the removal of the old extents and the tagging of the new one happen under a single
// vm_mutex hold, so no other thread can take any part of the range in between (an unhinted mmap
// on another thread used to be able to; FreeBSD does the replace under the map lock too).
// What the old mappings still need (detaching direct-memory aliases, giving back their host pages)
// is done after the lock is dropped, while the new extent is marked busy so no other thread
// changes it; the unmap hook may take the direct-memory lock, which is held around callers of
// this file, never the other way round.
enum { PLACE_REPLACE, PLACE_EXCL, PLACE_FREE_ONLY };

typedef struct VmRange {
  uint64_t addr, size;
} VmRange;

static uint64_t vm_place(GuestContext *proc, uint64_t start, uint64_t span, int prot, int flags, const char *name,
                         int kind, int mode, int *err) {
  const uint64_t end = start + span;
  VmRange *old = NULL;
  size_t nold = 0, k = 0;
  int e_ = ENOMEM;

  pthread_mutex_lock(&proc->vm_mutex);
  uint64_t covered = 0;
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (e->addr >= end || e->addr + e->size <= start) continue;
    covered += (e->addr + e->size < end ? e->addr + e->size : end) - (e->addr > start ? e->addr : start);
    if (e->busy) goto fail;
    if (mode == PLACE_REPLACE) {
      if (vm_pinned(e)) { e_ = EINVAL; goto fail; }
    } else if (mode == PLACE_EXCL) {
      if (!e->is_free && e->kind != NPS5_EXT_RESERVED) goto fail;
    } else if (!e->is_free) {
      goto fail;
    }
  }
  if (covered != span) // partly outside the extent list: an image or the system block
    goto fail;
  if (!vm_split(proc, start, true) || !vm_split(proc, end, true))
    goto fail;
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next)
    if (e->addr >= start && e->addr + e->size <= end && !e->is_free && e->kind != NPS5_EXT_RESERVED) nold++;
  if (nold && !(old = (VmRange *)malloc(nold * sizeof(*old))))
    goto fail;
  GuestVMExtent *f = NULL;
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (e->addr < start || e->addr + e->size > end) continue;
    if (!e->is_free && e->kind != NPS5_EXT_RESERVED) old[k++] = (VmRange){e->addr, e->size};
    if (!f) f = e;
  }
  // The extents inside the range tile it exactly: fold them into one, tagged with the new owner.
  for (GuestVMExtent *n = f->next; n && n->addr < end;) {
    f->next = n->next;
    if (n->next) n->next->prev = f;
    free(n);
    n = f->next;
  }
  f->addr = start;
  f->size = span;
  vm_tag(f, prot, flags, name, kind);

  if (nold == 0) {
    if (kind != NPS5_EXT_RESERVED && nps5_mem_commit(start, span) != 0) {
      vm_make_free(f);
      recomp_vm_note_change();
      goto fail_noold;
    }
    recomp_vm_note_change();
    if (vm_hook_reports_map(kind)) vm_hook_map(start, span, prot, kind, name);
    pthread_mutex_unlock(&proc->vm_mutex);
    return start;
  }
  f->busy = true;
  recomp_vm_note_change();
  pthread_mutex_unlock(&proc->vm_mutex);

  bool ok = true;
  for (size_t i = 0; i < nold; i++) {
    vm_hook_unmap(old[i].addr, old[i].size); // before the pages go
    ps4_direct_mem_on_unmap(proc, old[i].addr, old[i].size);
    // Back to inaccessible zero-fill: nothing of the old mapping survives into the new one.
    if (nps5_mem_decommit(old[i].addr, old[i].size) != 0)
      fprintf(stderr, "[vmm] WARNING: decommit of guest 0x%llx (+0x%llx) failed: %s\n",
              (unsigned long long)old[i].addr, (unsigned long long)old[i].size, strerror(errno));
  }
  if (kind != NPS5_EXT_RESERVED && nps5_mem_commit(start, span) != 0) {
    fprintf(stderr, "[vmm] ERROR: cannot commit guest 0x%llx (+0x%llx): %s\n", (unsigned long long)start,
            (unsigned long long)span, strerror(errno));
    nps5_mem_decommit(start, span);
    ok = false;
  }
  free(old);
  pthread_mutex_lock(&proc->vm_mutex);
  if (ok) f->busy = false;
  else vm_make_free(f);
  recomp_vm_note_change();
  if (ok && vm_hook_reports_map(kind)) vm_hook_map(start, span, prot, kind, name);
  pthread_mutex_unlock(&proc->vm_mutex);
  if (!ok) {
    if (err) *err = ENOMEM;
    return (uint64_t)-1;
  }
  return start;

fail:
  free(old);
fail_noold:
  pthread_mutex_unlock(&proc->vm_mutex);
  if (err) *err = e_;
  return (uint64_t)-1;
}

// Maps exactly [desired_addr, desired_addr+size) rounded up to pages, only where nothing is
// mapped (free space or a reservation); anything else in the way fails (returns -1) and nothing
// changes. A misaligned address fails: it is never widened over memory the caller did not name.
// The address may be anywhere in the guest window: this is how a title gets memory at an
// address it hardcodes (0x2_0000_0000, 0x30_0000_0000, ...).
uint64_t recomp_vm_alloc_fixed(GuestContext *ctx, uint64_t desired_addr,
                               size_t size, int prot, int flags,
                               const char *name) {
  if (!ctx || size == 0 || (desired_addr & NPS5_PAGE_MASK))
    return (uint64_t)-1;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  uint64_t span = vm_align_up(size, NPS5_PAGE);
  PS4_LOGF(NPS5_S_VM, NPS5_L_DEBUG, "ps4-vm", "vm alloc fixed (%s): desired=0x%llx size=0x%zx\n",
           name ? name : "null", (unsigned long long)desired_addr, size);
  if (span < size || !nps5_guest_range_ok(desired_addr, span))
    return (uint64_t)-1;
  return vm_place(proc, desired_addr, span, prot, flags, name, NPS5_EXT_MAPPED, PLACE_EXCL, NULL);
}

// MAP_FIXED without MAP_EXCL: whatever guest mappings lie in the range are unmapped first, as
// FreeBSD does. The range must be inside the guest window and must not touch memory that is
// not the guest's to replace (images, the system block, the main stack and its guard); then
// the call fails and nothing is unmapped. Atomic: see vm_place.
uint64_t recomp_vm_map_fixed_replace(GuestContext *ctx, uint64_t addr, size_t size, int prot, int flags,
                                     const char *name) {
  if (!ctx || size == 0 || (addr & NPS5_PAGE_MASK))
    return (uint64_t)-1;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  const uint64_t span = vm_align_up(size, NPS5_PAGE);
  if (span < size || !nps5_guest_range_ok(addr, span))
    return (uint64_t)-1;
  return vm_place(proc, addr, span, prot, flags, name, NPS5_EXT_MAPPED, PLACE_REPLACE, NULL);
}

// One guest mapping request, as mmap and the sceKernelMap* calls make it. `size` is the length the
// guest asked for; rounding is decided here, once, for every entry point:
//   MAP_FIXED       exactly that address (16 KiB aligned, not 0), length rounded to pages only,
//                   replacing the guest's own mappings (never the runtime's, see vm_pinned);
//                   with MAP_EXCL/NO_OVERWRITE only if nothing is mapped there
//   addr != 0       a hint: that address if it is free, else anywhere (a hint is advisory; it
//                   never splits a reservation)
//   addr == 0       user zone, lowest free address
// The placed length of an unfixed request is rounded to `alignment` (the allocation granule of the
// calling API). A fixed request that cannot be honoured fails; it is never moved somewhere else,
// because a title that names an address goes on to use that address.
uint64_t recomp_vm_map_ex(GuestContext *ctx, uint64_t addr, size_t size, size_t alignment, int prot, int flags,
                          const char *name, int kind, int *err) {
  enum { G_MAP_FIXED = 0x10, G_MAP_NO_OVERWRITE = 0x80, G_MAP_EXCL = 0x4000 };
  int e_ = ENOMEM;
  uint64_t got = (uint64_t)-1;
  if (!ctx || size == 0) {
    e_ = EINVAL;
    goto done;
  }
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  if (flags & G_MAP_FIXED) {
    // Guest pages are 16 KiB (as the host's): an unaligned or null fixed address is the caller's
    // error, as on the console. Widening it silently would map memory the title did not ask for.
    const uint64_t span = vm_align_up(size, NPS5_PAGE);
    if (addr == 0 || (addr & NPS5_PAGE_MASK) || span < size || !nps5_guest_range_ok(addr, span)) {
      e_ = EINVAL;
    } else {
      got = vm_place(proc, addr, span, prot, flags, name, kind,
                     (flags & (G_MAP_NO_OVERWRITE | G_MAP_EXCL)) ? PLACE_EXCL : PLACE_REPLACE, &e_);
    }
    if (got == (uint64_t)-1) {
      Nps5FaultInfo fi;
      nps5_mem_classify((uint64_t)(uintptr_t)g_nps5_arena.bias + addr, &fi);
      NPS5_LOG(NPS5_S_MEM, NPS5_L_WARN,
               "fixed guest mapping refused (%s): addr=0x%llx size=0x%zx flags=0x%x name=%s (address is: %s%s%s)",
               e_ == EINVAL ? "invalid" : "unavailable", (unsigned long long)addr, size, flags, name ? name : "",
               nps5_fault_class_name(fi.cls), fi.name[0] ? " " : "", fi.name);
    }
    goto done;
  }
  size_t gran = alignment < NPS5_PAGE ? NPS5_PAGE : alignment;
  if (gran & (gran - 1)) { // not a power of two: round up to one (as vm_alloc_zone places it)
    if (gran > (1ULL << 62)) goto done;
    gran = (size_t)1 << (64 - __builtin_clzll((unsigned long long)gran));
  }
  const uint64_t span = vm_align_up(size, gran);
  if (span < size) goto done;
  if (addr != 0 && !(addr & NPS5_PAGE_MASK) && nps5_guest_range_ok(addr, span)) {
    got = vm_place(proc, addr, span, prot, flags, name, kind, PLACE_FREE_ONLY, NULL);
    if (got != (uint64_t)-1) goto done;
  }
  got = vm_alloc_zone(ctx, span, alignment, prot, flags, name, kind, VM_ZONE_USER);
done:
  if (err) *err = got == (uint64_t)-1 ? e_ : 0;
  return got;
}

uint64_t recomp_vm_map(GuestContext *ctx, uint64_t addr, size_t size, size_t alignment, int prot, int flags,
                       const char *name) {
  return recomp_vm_map_ex(ctx, addr, size, alignment, prot, flags, name, NPS5_EXT_MAPPED, NULL);
}

bool recomp_vm_query(GuestContext *ctx, uint64_t addr, GuestVMExtent *out) {
  if (!ctx || !out)
    return false;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  bool found = false;
  pthread_mutex_lock(&proc->vm_mutex);
  for (const GuestVMExtent *curr = proc->vm_extents; curr; curr = curr->next) {
    if (addr >= curr->addr && addr - curr->addr < curr->size) {
      *out = *curr; // a copy: the node can be freed by another thread once the lock is dropped
      out->next = out->prev = NULL;
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&proc->vm_mutex);
  return found;
}

// ---- Provably unmapped guest ranges, for readers that must not take vm_mutex per query ----
// The GPU renderer treats a T# table entry whose T# starts in such a range as unbound. A range is
// provably unmapped when it is inside a free extent or inside a reservation that nothing was
// mapped into (NPS5_EXT_RESERVED: sceKernelReserveVirtualRange; mapping into it splits it and
// retags the mapped part). Everything else is mapped as far as this classification goes: module
// images and the other memory the runtime places outside the extent windows have no extent
// at all, and the protection of an extent (sceKernelMprotect may set PROT_NONE on a whole extent
// for one guard page) says nothing about whether memory is there.
// g_vm_generation changes, with vm_mutex held, whenever an extent becomes free or mapped or is
// renamed, so a reader can keep a copy of the ranges and refresh it only when it changed.
static _Atomic uint64_t g_vm_generation = 1;

// Also republishes the mapping snapshot the crash handler classifies fault addresses with
// (nps5_mem_classify): the caller holds vm_mutex, so the list is stable while it is copied.
void recomp_vm_note_change(void) {
  atomic_fetch_add_explicit(&g_vm_generation, 1, memory_order_release);
  GuestContext *cur = g_nps5_vm_root;
  if (!cur) return;
  nps5_mem_map_begin();
  for (const GuestVMExtent *e = cur->vm_extents; e; e = e->next)
    if (!e->is_free) nps5_mem_map_add(e->addr, e->addr + e->size, e->name, e->kind);
  nps5_mem_map_end();
}

uint64_t recomp_vm_generation(void) {
  return atomic_load_explicit(&g_vm_generation, memory_order_acquire);
}

static bool vm_extent_unmapped(const GuestVMExtent *e) {
  return e->is_free || e->kind == NPS5_EXT_RESERVED;
}

size_t recomp_vm_unmapped_ranges(GuestContext *ctx, uint64_t (*out)[2], size_t max, uint64_t *gen) {
  if (!ctx)
    return 0;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  size_t n = 0;
  uint64_t cur_start = 0, cur_end = 0;
  bool open = false;
  pthread_mutex_lock(&proc->vm_mutex);
  if (gen)
    *gen = atomic_load_explicit(&g_vm_generation, memory_order_acquire);
  for (const GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (!vm_extent_unmapped(e) || e->size == 0)
      continue;
    if (open && e->addr == cur_end) {
      cur_end = e->addr + e->size; // adjacent: merge
      continue;
    }
    if (open) {
      if (n < max) {
        out[n][0] = cur_start;
        out[n][1] = cur_end;
      }
      n++;
    }
    cur_start = e->addr;
    cur_end = e->addr + e->size;
    open = true;
  }
  if (open) {
    if (n < max) {
      out[n][0] = cur_start;
      out[n][1] = cur_end;
    }
    n++;
  }
  pthread_mutex_unlock(&proc->vm_mutex);
  return n;
}

// Merges free extents that touch, for the extents overlapping [start, end] plus one neighbour
// on each side. Caller holds vm_mutex.
static void vm_coalesce_free(GuestContext *proc, uint64_t start, uint64_t end) {
  GuestVMExtent *e = proc->vm_extents;
  while (e && e->addr + e->size < start)
    e = e->next;
  if (e && e->prev)
    e = e->prev;
  while (e && e->next && e->addr <= end) {
    GuestVMExtent *n = e->next;
    if (e->is_free && n->is_free && !e->busy && !n->busy && e->addr + e->size == n->addr) {
      e->size += n->size;
      e->next = n->next;
      if (n->next)
        n->next->prev = e;
      free(n);
    } else {
      e = n;
    }
  }
}

// munmap over [addr, addr+size) (page-widened). Every mapped extent in the range is freed,
// also when the range spans several extents (a mapping that was split, e.g. by
// sceKernelMtypeprotect, is several) or crosses a hole; the parts of extents outside the range
// stay as they were with all their attributes; unmapped parts are skipped. A range that touches
// the main stack or its guard page is refused (-1, errno EINVAL) without changing anything, the
// same rule MAP_FIXED applies. The change is made in phases and no range is ever freed by
// address twice:
//  1. (locked) split the extents at both ends, note the mapped ranges inside;
//  2. (unlocked, the ranges are still allocated so nobody can take them) detach direct-memory
//     aliases and release the host pages, once per mapped range;
//  3. (locked) mark the ranges free and merge neighbours.
// Returns 0, or -1 for bad arguments or when memory for the bookkeeping is unavailable (the
// extents are then unchanged apart from harmless splits).
int recomp_vm_free(GuestContext *ctx, uint64_t addr, size_t size) {
  if (!ctx || addr == 0 || size == 0)
    return -1;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  // [addr, addr+size) widened to pages; a length that wraps the address space is an error, not a
  // short range (munmap(addr, 2^64-0x100) used to free one page and succeed).
  if (size > UINT64_MAX - addr - NPS5_PAGE_MASK)
    return -1;
  const uint64_t start = addr & ~NPS5_PAGE_MASK;
  const uint64_t end = (addr + size + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  if (end <= start)
    return -1;

  pthread_mutex_lock(&proc->vm_mutex);
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (e->addr < end && e->addr + e->size > start && vm_pinned(e)) {
      pthread_mutex_unlock(&proc->vm_mutex);
      errno = EINVAL;
      return -1;
    }
  }
  if (!recomp_vm_split_extent_at(proc, start) || !recomp_vm_split_extent_at(proc, end)) {
    pthread_mutex_unlock(&proc->vm_mutex);
    return -1;
  }
  size_t n = 0;
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next)
    if (!e->is_free && !e->busy && e->addr >= start && e->addr + e->size <= end)
      n++;
  if (n == 0) {
    pthread_mutex_unlock(&proc->vm_mutex);
    return 0;
  }
  GuestVMExtent **mine = (GuestVMExtent **)malloc(n * sizeof(*mine));
  if (!mine) {
    pthread_mutex_unlock(&proc->vm_mutex);
    return -1;
  }
  size_t k = 0;
  // The extents this call frees are marked busy before the lock is dropped: a busy extent is not
  // split, merged, replaced or freed by anyone else (vm_split, vm_coalesce_free, vm_place and
  // this function all skip or refuse it), so the pointers stay valid and the phase-3 free below
  // touches exactly these extents. A concurrent overlapping munmap finds them busy and leaves
  // them to this call; a concurrent MAP_FIXED over them fails (ENOMEM) instead of racing the
  // release of the host pages (two calls used to collect the same extents and the slower one
  // freed the mapping a MAP_FIXED had created in the meantime).
  for (GuestVMExtent *e = proc->vm_extents; e && k < n; e = e->next)
    if (!e->is_free && !e->busy && e->addr >= start && e->addr + e->size <= end) {
      e->busy = true;
      mine[k++] = e;
    }
  recomp_vm_note_change();
  pthread_mutex_unlock(&proc->vm_mutex);

  // Per-extent addresses are copied now: the extents themselves cannot change while busy.
  for (size_t i = 0; i < k; i++) {
    if (mine[i]->kind != NPS5_EXT_RESERVED) vm_hook_unmap(mine[i]->addr, mine[i]->size); // before the pages go
    ps4_direct_mem_on_unmap(proc, mine[i]->addr, mine[i]->size);
    // Back to inaccessible zero-fill: a later access is a classified fault, not stale data.
    if (nps5_mem_decommit(mine[i]->addr, mine[i]->size) != 0)
      fprintf(stderr, "[vmm] WARNING: decommit of guest 0x%llx (+0x%llx) failed: %s\n",
              (unsigned long long)mine[i]->addr, (unsigned long long)mine[i]->size, strerror(errno));
  }

  pthread_mutex_lock(&proc->vm_mutex);
  for (size_t i = 0; i < k; i++)
    vm_make_free(mine[i]); // clears busy
  vm_coalesce_free(proc, start, end);
  recomp_vm_note_change();
  pthread_mutex_unlock(&proc->vm_mutex);
  free(mine);
  return 0;
}

// Host protection for a guest protection value. Guest page protections are enforced on the host
// pages for what the CPU can do; a page the GPU may read or write stays readable (and writable for
// GPU_WRITE) because the in-process renderer reads guest memory through the same host pointers.
// PROT_EXEC counts as readable: guest code is data to the host (translated code runs elsewhere).
// Orbis bits: READ 1, WRITE 2, EXEC 4, GPU_READ 0x10, GPU_WRITE 0x20.
static int vm_host_prot(int prot) {
  int hp = 0;
  if (prot & (0x1 | 0x4 | 0x10 | 0x20)) hp |= PROT_READ;
  if (prot & (0x2 | 0x20)) hp |= PROT_WRITE;
  return hp;
}

int recomp_vm_protect(GuestContext *ctx, uint64_t addr, uint64_t size, int prot, int mtype) {
  if (!ctx) return EINVAL;
  if (size == 0) return 0;
  GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
  if (size > UINT64_MAX - addr - NPS5_PAGE_MASK) return EINVAL; // the rounded range would wrap
  const uint64_t start = addr & ~NPS5_PAGE_MASK;
  const uint64_t end = (addr + size + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;
  if (end <= start || !nps5_guest_range_ok(start, end - start)) return EINVAL;

  int rc = 0;
  pthread_mutex_lock(&proc->vm_mutex);
  bool covered = false;
  uint64_t first = UINT64_MAX;
  for (const GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (e->addr < first) first = e->addr;
    if (!e->is_free && e->addr < end && e->addr + e->size > start) covered = true;
  }
  if (!covered) {
    // The images and the system block sit below the extent list and have none. The guest may
    // change the protection of its own images (the loader makes code read-only; a title that
    // patches its code asks for write first): that goes to the host pages of the noted image
    // ranges only. Anything else below the list has no protection to change and is accepted.
    if (end > first) rc = EINVAL; // nothing mapped in the range
    else nps5_mem_protect_image(start, end, vm_host_prot(prot));
    pthread_mutex_unlock(&proc->vm_mutex);
    return rc;
  }
  if (!vm_split(proc, start, false) || !vm_split(proc, end, false)) {
    pthread_mutex_unlock(&proc->vm_mutex);
    return ENOMEM;
  }
  for (GuestVMExtent *e = proc->vm_extents; e; e = e->next) {
    if (e->is_free || e->busy || e->addr < start || e->addr + e->size > end) continue;
    e->prot = prot;
    if (mtype >= 0) e->mtype = mtype;
    // Reservations hold address space only (no pages); runtime-owned extents keep their pages.
    if (e->kind == NPS5_EXT_RESERVED || vm_pinned(e) || e->kind == NPS5_EXT_THREAD_STACK) continue;
    // Under vm_mutex: an unmap cannot slip in between and have its decommit undone by this call.
    // Before the host protection changes. An observer that tracks the pages' protection itself (the AGC write watch)
    // learns the new value from the protect call below, which goes through the host access hooks (nps5_mem.h).
    vm_hook_protect(e->addr, e->size, prot, e->kind, e->name);
    if (nps5_mem_protect_inner(e->addr, e->size, vm_host_prot(prot)) != 0)
      fprintf(stderr, "[vmm] WARNING: host protect of guest 0x%llx (+0x%zx) failed: %s\n", (unsigned long long)e->addr,
              e->size, strerror(errno));
  }
  pthread_mutex_unlock(&proc->vm_mutex); // no snapshot republish: protection is not part of it
  return rc;
}
