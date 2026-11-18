// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host side of the Unreal Engine allocator replacement (-hle-malloc ue5-binned3, docs/m3/function-hle.md).
//
// The translator binds the guest entry points of FMallocBinned3 (its constructor, and the functions its
// vtable points at for Malloc, Realloc, Free, QuantizeSize, GetAllocationSize, Trim and the TLS-cache hooks)
// to the shims below. A shim is an ordinary HLE function: it reads the System V arguments from the
// GuestContext, writes rax and returns through SHIM_RETURN. The object the engine keeps in GMalloc stays
// where it was (static storage in the guest image); the shim for its constructor only stores the vtable
// pointer. Everything the object used to hold lives in nps5_uemalloc_core.c instead.

#include "nps5_diag.h"
#include "nps5_funcrepl.h"
#include "nps5_mem.h"
#include "nps5_thunk.h"
#include "nps5_uemalloc_core.h"
#include "recomp_runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

__attribute__((weak)) const Nps5ReplParam nps5_repl_params[] = {{NULL, 0}};
__attribute__((weak)) const Nps5ReplWrap nps5_repl_wraps[] = {{NULL, 0, NULL}};

void *nps5_repl_wrap_body(const char *shim, uint64_t *guest) {
  for (const Nps5ReplWrap *w = nps5_repl_wraps; w->shim; w++) {
    if (!strcmp(w->shim, shim)) {
      if (guest) *guest = w->guest;
      return (void *)w->body;
    }
  }
  return NULL;
}

bool nps5_repl_param(const char *key, uint64_t *out) {
  for (const Nps5ReplParam *p = nps5_repl_params; p->key; p++) {
    if (!strcmp(p->key, key)) {
      *out = p->value;
      return true;
    }
  }
  return false;
}

static uint64_t prov_map(void *user, size_t size, size_t align) {
  (void)user;
  int err = 0;
  uint64_t a = recomp_vm_map_ex(g_nps5_vm_root, 0, size, align, PROT_READ | PROT_WRITE, 0, "ue5_malloc", NPS5_EXT_MAPPED, &err);
  return a == (uint64_t)-1 ? 0 : a;
}

static void prov_unmap(void *user, uint64_t addr, size_t size) {
  (void)user;
  recomp_vm_free(g_nps5_vm_root, addr, size);
}

static _Atomic int g_state; // 0 = not started, 1 = ready, -1 = failed
static int g_log_stats;

static void ue_stats_at_exit(void) {
  UemStats s;
  uem_stats(&s);
  fprintf(stderr,
          "[ue5-malloc] allocs=%llu (small %llu, tcache hits %llu, large %llu) frees=%llu unknown_frees=%llu slab=%llu MiB "
          "large_live=%llu KiB large_cached=%llu KiB\n",
          (unsigned long long)s.allocs, (unsigned long long)s.small_allocs, (unsigned long long)s.tcache_hits,
          (unsigned long long)s.large_allocs, (unsigned long long)s.frees, (unsigned long long)s.unknown_frees,
          (unsigned long long)(s.slab_bytes >> 20), (unsigned long long)(s.large_live_bytes >> 10),
          (unsigned long long)(s.large_cached_bytes >> 10));
}

// The allocator is brought up by the constructor shim, which the engine runs before its first allocation. A
// call that arrives first anyway (a build where the constructor was not replaced is refused by the
// translator, so this is only reachable through a damaged profile) starts it as well.
static int ue_ready(void) {
  int st = atomic_load_explicit(&g_state, memory_order_acquire);
  if (st) return st > 0;
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&lk);
  st = atomic_load_explicit(&g_state, memory_order_relaxed);
  if (!st) {
    UemProvider p = {prov_map, prov_unmap, NULL, g_nps5_arena.bias};
    st = uem_init(&p) == 0 ? 1 : -1;
    if (st > 0) {
      const char *e = getenv("NPS5_UEMALLOC_POISON");
      if (e && *e == '1') uem_set_poison(1);
      e = getenv("NPS5_UEMALLOC_STATS");
      if (e && *e == '1') {
        g_log_stats = 1;
        atexit(ue_stats_at_exit);
      }
      NPS5_LOG(NPS5_S_HLE, NPS5_L_INFO, "ue5-malloc: native allocator active (replaces FMallocBinned3)");
    } else {
      NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "ue5-malloc: allocator setup failed");
    }
    atomic_store_explicit(&g_state, st, memory_order_release);
  }
  pthread_mutex_unlock(&lk);
  return st > 0;
}

static __attribute__((noreturn, cold)) void ue_oom(const char *what, uint64_t size, uint64_t align) {
  UemStats s;
  uem_stats(&s);
  fprintf(stderr, "[ue5-malloc] FATAL out of memory in %s: size=0x%llx align=0x%llx (slab %llu MiB, large live %llu MiB)\n", what,
          (unsigned long long)size, (unsigned long long)align, (unsigned long long)(s.slab_bytes >> 20),
          (unsigned long long)(s.large_live_bytes >> 20));
  abort();
}

// FMallocBinned3::FMallocBinned3(): rdi = this. The vtable is the one the guest image already holds.
void shim_ue5_malloc_ctor(GuestContext *ctx) {
  uint64_t vt;
  if (!nps5_repl_param("ue5m.vtable", &vt)) {
    fprintf(stderr, "[ue5-malloc] FATAL: the translation did not record the FMallocBinned3 vtable (ue5m.vtable)\n");
    abort();
  }
  if (!ue_ready()) ue_oom("setup", 0, 0);
  uint64_t *self = (uint64_t *)NPS5_G2H(ctx, ctx->rdi);
  if (!self) {
    fprintf(stderr, "[ue5-malloc] FATAL: FMallocBinned3 constructed at a null address\n");
    abort();
  }
  *self = vt;
  ctx->rax = ctx->rdi;
  SHIM_RETURN();
}

// FMalloc::Malloc(this, size_t Count, uint32 Alignment): rsi = Count, edx = Alignment.
void shim_ue5_malloc_malloc(GuestContext *ctx) {
  if (__builtin_expect(atomic_load_explicit(&g_state, memory_order_relaxed) <= 0, 0) && !ue_ready()) ue_oom("malloc", ctx->rsi, (uint32_t)ctx->rdx);
  uint64_t p = uem_alloc(ctx->rsi, (uint32_t)ctx->rdx);
  if (__builtin_expect(!p, 0)) ue_oom("Malloc", ctx->rsi, (uint32_t)ctx->rdx);
  ctx->rax = p;
  SHIM_RETURN();
}

// FMalloc::Realloc(this, void *Original, size_t Count, uint32 Alignment): rsi, rdx, ecx.
void shim_ue5_malloc_realloc(GuestContext *ctx) {
  if (__builtin_expect(atomic_load_explicit(&g_state, memory_order_relaxed) <= 0, 0) && !ue_ready()) ue_oom("realloc", ctx->rdx, (uint32_t)ctx->rcx);
  int oom;
  uint64_t p = uem_realloc(ctx->rsi, ctx->rdx, (uint32_t)ctx->rcx, &oom);
  if (__builtin_expect(oom, 0)) ue_oom("Realloc", ctx->rdx, (uint32_t)ctx->rcx);
  ctx->rax = p;
  SHIM_RETURN();
}

// FMalloc::Free(this, void *Original): rsi.
void shim_ue5_malloc_free(GuestContext *ctx) {
  if (ctx->rsi && uem_free(ctx->rsi) != 0) {
    static _Atomic int once;
    if (!atomic_exchange(&once, 1))
      NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "ue5-malloc: Free(0x%llx) of a pointer the native allocator does not own (ignored; first report only)",
               (unsigned long long)ctx->rsi);
  }
  SHIM_RETURN();
}

// FMalloc::QuantizeSize(this, size_t Count, uint32 Alignment): rsi, edx; returns size_t in rax.
void shim_ue5_malloc_quantize(GuestContext *ctx) {
  ctx->rax = uem_quantize(ctx->rsi, (uint32_t)ctx->rdx);
  SHIM_RETURN();
}

// FMalloc::GetAllocationSize(this, void *Original, SIZE_T &SizeOut): rsi, rdx; returns bool in al.
void shim_ue5_malloc_getsize(GuestContext *ctx) {
  size_t sz;
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, ctx->rdx);
  if (ctx->rsi && out && uem_usable_size(ctx->rsi, &sz) == 0) {
    *out = sz;
    ctx->rax = 1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// Trim(bool), Setup/MarkUsed/MarkUnused/ClearAndDisable TLS caches, ~FMallocBinned3: the per-thread caches are
// the allocator's own business and process teardown frees nothing.
void shim_ue5_malloc_nop(GuestContext *ctx) {
  SHIM_RETURN();
}

// ---- lean entries of the hot shims (docs/m3/leanshim.md): the same bodies in the lean ABI, a0 = this.
// They run the allocator on the stored guest context without the full bridge. None of them calls guest code;
// the out-of-memory paths abort.
#include "nps5_lean.h"

NPS5_LEAN_FN(ue5_malloc_malloc) {
  if (__builtin_expect(atomic_load_explicit(&g_state, memory_order_relaxed) <= 0, 0) && !ue_ready()) ue_oom("malloc", a1, (uint32_t)a2);
  uint64_t p = uem_alloc(a1, (uint32_t)a2);
  if (__builtin_expect(!p, 0)) ue_oom("Malloc", a1, (uint32_t)a2);
  return p;
}

NPS5_LEAN_FN(ue5_malloc_realloc) {
  if (__builtin_expect(atomic_load_explicit(&g_state, memory_order_relaxed) <= 0, 0) && !ue_ready()) ue_oom("realloc", a2, (uint32_t)a3);
  int oom;
  uint64_t p = uem_realloc(a1, a2, (uint32_t)a3, &oom);
  if (__builtin_expect(oom, 0)) ue_oom("Realloc", a2, (uint32_t)a3);
  return p;
}

NPS5_LEAN_FN(ue5_malloc_free) {
  if (a1 && uem_free(a1) != 0) {
    static _Atomic int once;
    if (!atomic_exchange(&once, 1))
      NPS5_LOG(NPS5_S_HLE, NPS5_L_ERROR, "ue5-malloc: Free(0x%llx) of a pointer the native allocator does not own (ignored; first report only)",
               (unsigned long long)a1);
  }
  return ctx->rax;
}

NPS5_LEAN_FN(ue5_malloc_quantize) { return uem_quantize(a1, (uint32_t)a2); }

NPS5_LEAN_FN(ue5_malloc_getsize) {
  size_t sz;
  uint64_t *out = (uint64_t *)NPS5_G2H(ctx, a2);
  if (a1 && out && uem_usable_size(a1, &sz) == 0) {
    *out = sz;
    return 1;
  }
  return 0;
}

NPS5_LEAN_FN(ue5_malloc_nop) { return ctx->rax; }
