// SPDX-License-Identifier: GPL-2.0-or-later
//
// See nps5_uemalloc_core.h and docs/m3/function-hle.md.

#include "nps5_uemalloc_core.h"

#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define UNIT_SHIFT 16
#define UNIT (1ULL << UNIT_SHIFT)
#define SLAB_SHIFT 28
#define SLAB_BYTES (1ULL << SLAB_SHIFT)
#define UNITS_PER_SLAB (SLAB_BYTES >> UNIT_SHIFT)
#define GUEST_BITS 40
#define SLAB_OF_N (1u << (GUEST_BITS - SLAB_SHIFT))
#define MAX_SLABS 1023u // slab ids are 1-based in a uint16_t table; 255.75 GiB of spans
#define MAX_SPANS (1u << 22)
#define NCLS 48
#define MAX_SPAN_UNITS 16 // 1 MiB: the span of the largest class holds 8 blocks
#define BATCH_MAX 64
#define LARGE_CACHE_N 48
#define LARGE_CACHE_MAX_BLOCK (16ULL << 20)
#define LARGE_CACHE_MAX_BYTES (96ULL << 20)

typedef struct Span {
  uint64_t base;      // guest address (64 KiB aligned)
  uint64_t freelist;  // guest address of the first returned block, 0 = none
  uint32_t nblocks;   // blocks the span holds
  uint32_t bump;      // blocks [bump, nblocks) were never handed out
  uint32_t nfree;     // blocks on the free list
  uint32_t used;      // blocks handed out (also those sitting in thread caches)
  uint32_t prev, next; // class partial list / free-span list (span ids, 0 = none)
  uint16_t cls;
  uint16_t units;
  uint8_t in_partial;
  uint8_t pad[7];
} Span;

typedef struct Slab {
  uint64_t base;
  uint32_t *pm; // unit -> span id (0 = none)
  uint32_t bump; // next never used unit
} Slab;

typedef struct Class {
  os_unfair_lock lock;
  uint32_t partial; // head of the list of spans that can still hand out a block
  uint32_t nspans;
  char pad[48];
} __attribute__((aligned(64))) Class;

typedef struct TCache {
  uint64_t head[NCLS];
  uint32_t count[NCLS];
  uint64_t allocs, frees, hits;
  struct TCache *next, *prev;
} TCache;

typedef struct LargeEnt {
  uint64_t addr; // 0 empty, 1 deleted
  uint64_t size;
} LargeEnt;

typedef struct CacheEnt {
  uint64_t addr, size;
} CacheEnt;

static UemProvider g_prov;
static uint8_t *g_bias;
static _Atomic int g_ready;
static os_unfair_lock g_init_lock = OS_UNFAIR_LOCK_INIT;

static uint32_t g_csize[NCLS];
static uint16_t g_cunits[NCLS];
static uint32_t g_cblocks[NCLS];
static uint8_t g_batch[NCLS];
static uint8_t g_high[NCLS];
static uint8_t g_size_cls[(UEM_MAX_SMALL >> 4) + 1];
static Class g_cls[NCLS];

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT; // slabs, span ids, free span lists
static Slab g_slabs[MAX_SLABS];
static uint32_t g_nslabs;
static _Atomic uint16_t g_slab_of[SLAB_OF_N];
static Span *g_spans; // [MAX_SPANS], id 0 unused
static uint32_t g_nspans;
static uint32_t g_free_spans[MAX_SPAN_UNITS + 1]; // by unit count
static uint32_t g_cur_slab;                       // slab spans are cut from (0 = none)
static uint64_t g_slab_bytes;

static os_unfair_lock g_large_lock = OS_UNFAIR_LOCK_INIT;
static LargeEnt *g_large;
static size_t g_large_cap, g_large_n, g_large_dead;
static CacheEnt g_lcache[LARGE_CACHE_N];
static uint64_t g_lcache_bytes, g_large_live, g_large_allocs;

static os_unfair_lock g_tc_lock = OS_UNFAIR_LOCK_INIT; // registry of thread caches
static TCache *g_tc_list;
static uint64_t g_dead_allocs, g_dead_frees, g_dead_hits;
static pthread_key_t g_key;
static __thread TCache *t_tc;
static _Atomic uint64_t g_unknown_frees;
static _Atomic int g_poison;

static inline uint64_t ld64(uint64_t g) { return *(uint64_t *)(g_bias + g); }
static inline void st64(uint64_t g, uint64_t v) { *(uint64_t *)(g_bias + g) = v; }
static inline Span *span_at(uint32_t id) { return id ? &g_spans[id] : NULL; }

// ---- size classes ------------------------------------------------------------------------------

static void build_classes(void) {
  uint32_t n = 0;
  for (uint32_t s = 16; s <= 128; s += 16) g_csize[n++] = s;
  for (uint32_t b = 128; b < UEM_MAX_SMALL; b *= 2)
    for (uint32_t k = 1; k <= 4; k++) g_csize[n++] = b + b / 4 * k;
  // n == NCLS, g_csize[NCLS-1] == UEM_MAX_SMALL
  uint32_t c = 0;
  for (uint32_t i = 0; i <= UEM_MAX_SMALL >> 4; i++) {
    uint32_t bytes = i << 4;
    while (g_csize[c] < bytes) c++;
    g_size_cls[i] = (uint8_t)c;
  }
  for (uint32_t i = 0; i < NCLS; i++) {
    uint64_t span = (uint64_t)g_csize[i] * 8;
    if (span < UNIT) span = UNIT;
    span = (span + UNIT - 1) & ~(UNIT - 1);
    g_cunits[i] = (uint16_t)(span >> UNIT_SHIFT);
    g_cblocks[i] = (uint32_t)(span / g_csize[i]);
    uint32_t batch = 16384 / g_csize[i];
    if (batch < 2) batch = 2;
    if (batch > BATCH_MAX) batch = BATCH_MAX;
    g_batch[i] = (uint8_t)batch;
    g_high[i] = (uint8_t)(batch * 2 > 255 ? 255 : batch * 2);
  }
}

static inline size_t norm_align(size_t a) {
  if (a <= UEM_MIN_ALIGN) return UEM_MIN_ALIGN;
  if (a & (a - 1)) {
    if (a > (1ULL << 40)) return 0;
    a = (size_t)1 << (64 - __builtin_clzll((unsigned long long)(a - 1)));
  }
  return a;
}

// Class of a request, or -1 when it needs its own mapping.
static inline int pick_class(size_t size, size_t align) {
  if (size > UEM_MAX_SMALL || align > UEM_MAX_SMALL_ALIGN) return -1;
  int c = g_size_cls[(size + 15) >> 4];
  if (align > UEM_MIN_ALIGN) {
    while (c < NCLS && (g_csize[c] & (align - 1))) c++;
    if (c == NCLS) return -1;
  }
  return c;
}

static inline uint64_t large_round(size_t size, size_t align) {
  uint64_t g = align > UEM_LARGE_GRANULE ? align : UEM_LARGE_GRANULE;
  (void)g;
  return ((uint64_t)size + UEM_LARGE_GRANULE - 1) & ~(uint64_t)(UEM_LARGE_GRANULE - 1);
}

// ---- spans and slabs ---------------------------------------------------------------------------

static Span *span_lookup(uint64_t g) {
  if (g >> GUEST_BITS) return NULL;
  uint16_t si = atomic_load_explicit(&g_slab_of[g >> SLAB_SHIFT], memory_order_relaxed);
  if (!si) return NULL;
  uint32_t id = __atomic_load_n(&g_slabs[si - 1].pm[(g >> UNIT_SHIFT) & (UNITS_PER_SLAB - 1)], __ATOMIC_RELAXED);
  return span_at(id);
}

static int slab_new(void) { // g_lock held
  if (g_nslabs >= MAX_SLABS) return -1;
  uint64_t base = g_prov.map(g_prov.user, SLAB_BYTES, SLAB_BYTES);
  if (!base || (base & (SLAB_BYTES - 1)) || (base >> GUEST_BITS) || ((base + SLAB_BYTES) >> GUEST_BITS)) {
    if (base) g_prov.unmap(g_prov.user, base, SLAB_BYTES);
    return -1;
  }
  uint32_t *pm = (uint32_t *)mmap(NULL, UNITS_PER_SLAB * sizeof(uint32_t), PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE, -1, 0);
  if (pm == MAP_FAILED) {
    g_prov.unmap(g_prov.user, base, SLAB_BYTES);
    return -1;
  }
  Slab *s = &g_slabs[g_nslabs];
  s->base = base;
  s->pm = pm;
  s->bump = 0;
  g_nslabs++;
  atomic_store_explicit(&g_slab_of[base >> SLAB_SHIFT], (uint16_t)g_nslabs, memory_order_release);
  g_cur_slab = g_nslabs;
  g_slab_bytes += SLAB_BYTES;
  return 0;
}

static void span_map(Span *s, uint32_t id, int set) { // g_lock held
  uint16_t si = atomic_load_explicit(&g_slab_of[s->base >> SLAB_SHIFT], memory_order_relaxed);
  uint32_t u0 = (uint32_t)((s->base >> UNIT_SHIFT) & (UNITS_PER_SLAB - 1));
  for (uint32_t u = 0; u < s->units; u++) __atomic_store_n(&g_slabs[si - 1].pm[u0 + u], set ? id : 0, __ATOMIC_RELEASE);
}

// A span of the geometry of class c, or NULL. Class lock held by the caller.
static Span *span_new(unsigned c, uint32_t *id_out) {
  unsigned units = g_cunits[c];
  os_unfair_lock_lock(&g_lock);
  uint32_t id = g_free_spans[units];
  Span *s;
  if (id) {
    s = &g_spans[id];
    g_free_spans[units] = s->next;
  } else {
    if (g_nspans + 1 >= MAX_SPANS) goto fail;
    uint64_t need = (uint64_t)units;
    for (;;) {
      if (g_cur_slab) {
        Slab *sl = &g_slabs[g_cur_slab - 1];
        if (sl->bump + need <= UNITS_PER_SLAB) break;
      }
      if (slab_new() != 0) goto fail;
    }
    Slab *sl = &g_slabs[g_cur_slab - 1];
    id = ++g_nspans;
    s = &g_spans[id];
    memset(s, 0, sizeof(*s));
    s->base = sl->base + ((uint64_t)sl->bump << UNIT_SHIFT);
    sl->bump += (uint32_t)need;
    s->units = (uint16_t)units;
  }
  s->cls = (uint16_t)c;
  s->nblocks = g_cblocks[c];
  s->bump = s->nfree = s->used = 0;
  s->freelist = 0;
  s->in_partial = 0;
  s->prev = s->next = 0;
  span_map(s, id, 1);
  os_unfair_lock_unlock(&g_lock);
  *id_out = id;
  return s;
fail:
  os_unfair_lock_unlock(&g_lock);
  return NULL;
}

static void span_release(Span *s, uint32_t id) { // class lock held, span not on the partial list
  os_unfair_lock_lock(&g_lock);
  span_map(s, id, 0);
  s->next = g_free_spans[s->units];
  g_free_spans[s->units] = id;
  os_unfair_lock_unlock(&g_lock);
}

static void partial_push(Class *cl, Span *s, uint32_t id) {
  s->prev = 0;
  s->next = cl->partial;
  if (cl->partial) g_spans[cl->partial].prev = id;
  cl->partial = id;
  s->in_partial = 1;
}

static void partial_remove(Class *cl, Span *s) {
  if (s->prev) g_spans[s->prev].next = s->next; else cl->partial = s->next;
  if (s->next) g_spans[s->next].prev = s->prev;
  s->prev = s->next = 0;
  s->in_partial = 0;
}

// ---- thread caches -----------------------------------------------------------------------------

static void tc_flush_class(TCache *t, unsigned c, uint32_t keep);

static void tc_destroy(void *p) {
  TCache *t = (TCache *)p;
  if (!t) return;
  for (unsigned c = 0; c < NCLS; c++) tc_flush_class(t, c, 0);
  os_unfair_lock_lock(&g_tc_lock);
  if (t->prev) t->prev->next = t->next; else g_tc_list = t->next;
  if (t->next) t->next->prev = t->prev;
  g_dead_allocs += t->allocs;
  g_dead_frees += t->frees;
  g_dead_hits += t->hits;
  os_unfair_lock_unlock(&g_tc_lock);
  if (t_tc == t) t_tc = NULL;
  free(t);
}

static TCache *tc_get(void) {
  TCache *t = t_tc;
  if (__builtin_expect(t != NULL, 1)) return t;
  t = (TCache *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  os_unfair_lock_lock(&g_tc_lock);
  t->next = g_tc_list;
  if (g_tc_list) g_tc_list->prev = t;
  g_tc_list = t;
  os_unfair_lock_unlock(&g_tc_lock);
  t_tc = t;
  pthread_setspecific(g_key, t);
  return t;
}

// Takes up to batch blocks of class c from the central list into the cache bin. Returns how many.
static unsigned tc_refill(TCache *t, unsigned c) {
  Class *cl = &g_cls[c];
  const unsigned want = g_batch[c];
  const uint32_t size = g_csize[c];
  uint64_t head = 0;
  unsigned n = 0;
  os_unfair_lock_lock(&cl->lock);
  while (n < want) {
    Span *s = span_at(cl->partial);
    if (!s) {
      uint32_t id;
      s = span_new(c, &id);
      if (!s) break;
      cl->nspans++;
      partial_push(cl, s, id);
    }
    while (n < want) {
      uint64_t b;
      if (s->freelist) {
        b = s->freelist;
        s->freelist = ld64(b);
        s->nfree--;
      } else if (s->bump < s->nblocks) {
        b = s->base + (uint64_t)s->bump++ * size;
      } else {
        break;
      }
      st64(b, head);
      head = b;
      n++;
      s->used++;
    }
    if (!s->freelist && s->bump >= s->nblocks) partial_remove(cl, s);
  }
  os_unfair_lock_unlock(&cl->lock);
  t->head[c] = head;
  t->count[c] = n;
  return n;
}

// Gives blocks back until at most keep remain in the bin.
static void tc_flush_class(TCache *t, unsigned c, uint32_t keep) {
  if (t->count[c] <= keep) return;
  Class *cl = &g_cls[c];
  uint64_t head = t->head[c];
  uint32_t n = t->count[c];
  // detach the part to give back, keep the first `keep` blocks in the bin
  uint64_t give = head;
  uint64_t last_kept = 0;
  for (uint32_t i = 0; i < keep; i++) {
    last_kept = give;
    give = ld64(give);
  }
  if (keep == 0) t->head[c] = 0; else st64(last_kept, 0);
  t->count[c] = keep;
  (void)n;
  os_unfair_lock_lock(&cl->lock);
  while (give) {
    uint64_t b = give;
    give = ld64(b);
    Span *s = span_lookup(b);
    uint32_t id = (uint32_t)(s - g_spans);
    st64(b, s->freelist);
    s->freelist = b;
    s->nfree++;
    s->used--;
    if (!s->in_partial) {
      partial_push(cl, s, id);
    } else if (s->used == 0 && cl->nspans > 1) {
      partial_remove(cl, s);
      cl->nspans--;
      span_release(s, id);
    }
  }
  os_unfair_lock_unlock(&cl->lock);
}

// ---- large blocks -------------------------------------------------------------------------------

static inline size_t large_hash(uint64_t a, size_t cap) {
  return (size_t)(((a >> 14) * 0x9E3779B97F4A7C15ULL) >> 20) & (cap - 1);
}

static int large_rehash(size_t newcap) { // lock held
  LargeEnt *nt = (LargeEnt *)mmap(NULL, newcap * sizeof(LargeEnt), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (nt == MAP_FAILED) return -1;
  for (size_t i = 0; i < g_large_cap; i++) {
    if (g_large[i].addr > 1) {
      size_t h = large_hash(g_large[i].addr, newcap);
      while (nt[h].addr) h = (h + 1) & (newcap - 1);
      nt[h] = g_large[i];
    }
  }
  if (g_large) munmap(g_large, g_large_cap * sizeof(LargeEnt));
  g_large = nt;
  g_large_cap = newcap;
  g_large_dead = 0;
  return 0;
}

static int large_insert(uint64_t addr, uint64_t size) { // lock held
  if ((g_large_n + g_large_dead + 1) * 2 > g_large_cap) {
    size_t nc = g_large_cap ? g_large_cap : 1024;
    if ((g_large_n + 1) * 2 > nc) nc *= 2;
    if (large_rehash(nc) != 0) return -1;
  }
  size_t h = large_hash(addr, g_large_cap);
  while (g_large[h].addr > 1) h = (h + 1) & (g_large_cap - 1);
  if (g_large[h].addr == 1) g_large_dead--;
  g_large[h].addr = addr;
  g_large[h].size = size;
  g_large_n++;
  return 0;
}

static LargeEnt *large_find(uint64_t addr) { // lock held
  if (!g_large_cap) return NULL;
  size_t h = large_hash(addr, g_large_cap);
  for (size_t i = 0; i < g_large_cap; i++) {
    LargeEnt *e = &g_large[(h + i) & (g_large_cap - 1)];
    if (e->addr == addr) return e;
    if (e->addr == 0) return NULL;
  }
  return NULL;
}

static void large_erase(LargeEnt *e) { // lock held
  e->addr = 1;
  g_large_n--;
  g_large_dead++;
}

static uint64_t large_alloc(size_t size, size_t align) {
  uint64_t len = large_round(size, align);
  if (len < size) return 0;
  os_unfair_lock_lock(&g_large_lock);
  // a recently freed block of about the right size?
  int best = -1;
  for (int i = 0; i < LARGE_CACHE_N; i++) {
    CacheEnt *c = &g_lcache[i];
    if (c->addr && c->size >= len && c->size - len <= len / 8 && !(c->addr & (align - 1)) &&
        (best < 0 || c->size < g_lcache[best].size))
      best = i;
  }
  if (best >= 0) {
    CacheEnt c = g_lcache[best];
    g_lcache[best].addr = 0;
    g_lcache_bytes -= c.size;
    if (large_insert(c.addr, c.size) == 0) {
      g_large_live += c.size;
      g_large_allocs++;
      os_unfair_lock_unlock(&g_large_lock);
      return c.addr;
    }
    g_prov.unmap(g_prov.user, c.addr, c.size);
  }
  os_unfair_lock_unlock(&g_large_lock);
  size_t al = align > UEM_LARGE_GRANULE ? align : UEM_LARGE_GRANULE;
  uint64_t a = g_prov.map(g_prov.user, len, al);
  if (!a) {
    // give the cache back to the system and try once more
    os_unfair_lock_lock(&g_large_lock);
    for (int i = 0; i < LARGE_CACHE_N; i++) {
      if (g_lcache[i].addr) {
        g_prov.unmap(g_prov.user, g_lcache[i].addr, g_lcache[i].size);
        g_lcache_bytes -= g_lcache[i].size;
        g_lcache[i].addr = 0;
      }
    }
    os_unfair_lock_unlock(&g_large_lock);
    a = g_prov.map(g_prov.user, len, al);
    if (!a) return 0;
  }
  os_unfair_lock_lock(&g_large_lock);
  if (large_insert(a, len) != 0) {
    os_unfair_lock_unlock(&g_large_lock);
    g_prov.unmap(g_prov.user, a, len);
    return 0;
  }
  g_large_live += len;
  g_large_allocs++;
  os_unfair_lock_unlock(&g_large_lock);
  return a;
}

// 0 = freed, -1 = not a large block.
static int large_free(uint64_t p) {
  os_unfair_lock_lock(&g_large_lock);
  LargeEnt *e = large_find(p);
  if (!e) {
    os_unfair_lock_unlock(&g_large_lock);
    return -1;
  }
  uint64_t size = e->size;
  large_erase(e);
  g_large_live -= size;
  uint64_t evict_a = 0, evict_s = 0;
  if (size <= LARGE_CACHE_MAX_BLOCK) {
    int slot = -1;
    for (int i = 0; i < LARGE_CACHE_N; i++)
      if (!g_lcache[i].addr) { slot = i; break; }
    if (slot < 0 || g_lcache_bytes + size > LARGE_CACHE_MAX_BYTES) {
      // make room: drop the largest cached block
      int big = -1;
      for (int i = 0; i < LARGE_CACHE_N; i++)
        if (g_lcache[i].addr && (big < 0 || g_lcache[i].size > g_lcache[big].size)) big = i;
      if (big >= 0 && g_lcache[big].size >= size) {
        evict_a = g_lcache[big].addr;
        evict_s = g_lcache[big].size;
        g_lcache_bytes -= evict_s;
        g_lcache[big].addr = 0;
        slot = big;
      } else {
        slot = -1;
      }
    }
    if (slot >= 0 && g_lcache_bytes + size <= LARGE_CACHE_MAX_BYTES) {
      g_lcache[slot].addr = p;
      g_lcache[slot].size = size;
      g_lcache_bytes += size;
      size = 0;
    }
  }
  os_unfair_lock_unlock(&g_large_lock);
  if (evict_a) g_prov.unmap(g_prov.user, evict_a, evict_s);
  if (size) g_prov.unmap(g_prov.user, p, size);
  return 0;
}

static int large_size(uint64_t p, uint64_t *out) {
  os_unfair_lock_lock(&g_large_lock);
  LargeEnt *e = large_find(p);
  if (e) *out = e->size;
  os_unfair_lock_unlock(&g_large_lock);
  return e ? 0 : -1;
}

// ---- public API --------------------------------------------------------------------------------

int uem_init(const UemProvider *p) {
  if (atomic_load_explicit(&g_ready, memory_order_acquire)) return 0;
  os_unfair_lock_lock(&g_init_lock);
  if (atomic_load_explicit(&g_ready, memory_order_relaxed)) {
    os_unfair_lock_unlock(&g_init_lock);
    return 0;
  }
  int rc = -1;
  g_spans = (Span *)mmap(NULL, (size_t)MAX_SPANS * sizeof(Span), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (g_spans == MAP_FAILED) {
    g_spans = NULL;
    goto out;
  }
  g_prov = *p;
  g_bias = p->bias;
  build_classes();
  if (pthread_key_create(&g_key, tc_destroy) != 0) goto out;
  rc = 0;
  atomic_store_explicit(&g_ready, 1, memory_order_release);
out:
  os_unfair_lock_unlock(&g_init_lock);
  return rc;
}

uint64_t uem_alloc(size_t size, size_t align) {
  align = norm_align(align);
  if (!align) return 0;
  int c = pick_class(size, align);
  if (c < 0) {
    if (size > (1ULL << 46)) return 0;
    return large_alloc(size ? size : 1, align);
  }
  TCache *t = tc_get();
  if (__builtin_expect(!t, 0)) return 0;
  t->allocs++;
  uint64_t b = t->head[c];
  if (__builtin_expect(b != 0, 1)) {
    t->head[c] = ld64(b);
    t->count[c]--;
    t->hits++;
    return b;
  }
  if (!tc_refill(t, (unsigned)c)) return 0;
  b = t->head[c];
  t->head[c] = ld64(b);
  t->count[c]--;
  return b;
}

int uem_free(uint64_t p) {
  if (!p) return 0;
  Span *s = span_lookup(p);
  if (__builtin_expect(s != NULL, 1)) {
    unsigned c = s->cls;
    TCache *t = tc_get();
    if (__builtin_expect(!t, 0)) return -1;
    t->frees++;
    if (__builtin_expect(atomic_load_explicit(&g_poison, memory_order_relaxed), 0) && g_csize[c] > 8)
      memset(g_bias + p + 8, 0xDD, g_csize[c] - 8);
    st64(p, t->head[c]);
    t->head[c] = p;
    if (__builtin_expect(++t->count[c] > g_high[c], 0)) tc_flush_class(t, c, g_high[c] - g_batch[c]);
    return 0;
  }
  if (large_free(p) == 0) return 0;
  atomic_fetch_add_explicit(&g_unknown_frees, 1, memory_order_relaxed);
  return -1;
}

int uem_usable_size(uint64_t p, size_t *out) {
  Span *s = span_lookup(p);
  if (s) {
    *out = g_csize[s->cls];
    return 0;
  }
  uint64_t sz;
  if (large_size(p, &sz) == 0) {
    *out = (size_t)sz;
    return 0;
  }
  return -1;
}

size_t uem_quantize(size_t size, size_t align) {
  align = norm_align(align);
  if (!align) return size;
  int c = pick_class(size, align);
  if (c >= 0) return g_csize[c];
  if (size > (1ULL << 46)) return size;
  return (size_t)large_round(size ? size : 1, align);
}

uint64_t uem_realloc(uint64_t p, size_t size, size_t align, int *oom) {
  *oom = 0;
  if (!p) {
    uint64_t r = uem_alloc(size, align);
    if (!r) *oom = 1;
    return r;
  }
  if (size == 0) {
    uem_free(p);
    return 0;
  }
  size_t al = norm_align(align);
  if (!al) {
    *oom = 1;
    return 0;
  }
  size_t cap;
  if (uem_usable_size(p, &cap) != 0) { // not ours: nothing sensible to copy from
    *oom = 1;
    return 0;
  }
  int keep = 0;
  Span *s = span_lookup(p);
  if (s) {
    int c = pick_class(size, al);
    keep = c == (int)s->cls && !(p & (al - 1));
  } else {
    keep = size <= cap && cap - size <= cap / 4 && !(p & (al - 1)) && pick_class(size, al) < 0;
  }
  if (keep) return p;
  uint64_t n = uem_alloc(size, al);
  if (!n) {
    *oom = 1;
    return 0;
  }
  memcpy(g_bias + n, g_bias + p, size < cap ? size : cap);
  uem_free(p);
  return n;
}

void uem_thread_flush(void) {
  TCache *t = t_tc;
  if (!t) return;
  for (unsigned c = 0; c < NCLS; c++) tc_flush_class(t, c, 0);
}

void uem_set_poison(int on) { atomic_store_explicit(&g_poison, on, memory_order_relaxed); }

void uem_stats(UemStats *o) {
  memset(o, 0, sizeof(*o));
  os_unfair_lock_lock(&g_tc_lock);
  uint64_t a = g_dead_allocs, f = g_dead_frees, h = g_dead_hits;
  for (TCache *t = g_tc_list; t; t = t->next) {
    a += t->allocs;
    f += t->frees;
    h += t->hits;
  }
  os_unfair_lock_unlock(&g_tc_lock);
  os_unfair_lock_lock(&g_large_lock);
  o->large_allocs = g_large_allocs;
  o->large_live_bytes = g_large_live;
  o->large_cached_bytes = g_lcache_bytes;
  os_unfair_lock_unlock(&g_large_lock);
  os_unfair_lock_lock(&g_lock);
  o->slab_bytes = g_slab_bytes;
  os_unfair_lock_unlock(&g_lock);
  o->small_allocs = a;
  o->tcache_hits = h;
  o->allocs = a + o->large_allocs;
  o->frees = f;
  o->unknown_frees = atomic_load_explicit(&g_unknown_frees, memory_order_relaxed);
}
