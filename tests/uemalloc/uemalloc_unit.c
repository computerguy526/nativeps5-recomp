// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host unit and stress test of the native UE allocator core (pkg/runtime/modules/libSceUeMalloc). The provider is an
// mmap arena standing in for the guest address space: "guest addresses" are offsets from a base, host = bias + guest.
// Native arm64 only; nothing from a guest is executed. tools/uemalloc_unit.sh builds and runs it.

#include "nps5_uemalloc_core.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define GUEST_BASE 0x200000000ULL // where the arena starts in "guest" space (256 MiB aligned)
#define ARENA_BYTES (48ULL << 30)

static uint8_t *g_host; // host address of GUEST_BASE
static uint8_t *g_bias; // g_host - GUEST_BASE
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pbump;          // next free offset in the arena
static uint64_t g_pmapped;        // live mapped bytes
static uint64_t g_pcap = ~0ULL;   // map() fails when live bytes would exceed this
static int g_pfail;               // map() fails when set
static unsigned long g_nmap, g_nunmap;

static uint64_t prov_map(void *u, size_t size, size_t align) {
  (void)u;
  pthread_mutex_lock(&g_pm);
  uint64_t off = (g_pbump + align - 1) & ~(uint64_t)(align - 1);
  if (g_pfail || off + size > ARENA_BYTES || g_pmapped + size > g_pcap) {
    pthread_mutex_unlock(&g_pm);
    return 0;
  }
  g_pbump = off + size;
  g_pmapped += size;
  g_nmap++;
  pthread_mutex_unlock(&g_pm);
  if (mprotect(g_host + off, size, PROT_READ | PROT_WRITE) != 0) {
    perror("mprotect");
    abort();
  }
  return GUEST_BASE + off;
}

static void prov_unmap(void *u, uint64_t addr, size_t size) {
  (void)u;
  // decommit: later access faults, a re-map gets zero pages (the runtime does the same)
  void *p = mmap(g_bias + addr, size, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    abort();
  }
  pthread_mutex_lock(&g_pm);
  g_pmapped -= size;
  g_nunmap++;
  pthread_mutex_unlock(&g_pm);
}

static int g_fail;
#define CHECK(c, ...)                                                                \
  do {                                                                               \
    if (!(c)) {                                                                      \
      fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                   \
      fprintf(stderr, __VA_ARGS__);                                                  \
      fputc('\n', stderr);                                                           \
      __sync_fetch_and_add(&g_fail, 1);                                              \
    }                                                                                \
  } while (0)

static inline uint8_t *H(uint64_t g) { return g_bias + g; }

static uint64_t rng(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static void fill(uint64_t g, size_t n, uint8_t tag) {
  memset(H(g), tag, n);
}
static int verify(uint64_t g, size_t n, uint8_t tag) {
  const uint8_t *p = H(g);
  for (size_t i = 0; i < n; i++)
    if (p[i] != tag) return 0;
  return 1;
}

// ---- functional tests --------------------------------------------------------------------------

static void test_geometry(void) {
  size_t prev = 0;
  for (size_t n = 0; n <= UEM_MAX_SMALL + 5000; n += (n < 4096 ? 1 : 97)) {
    size_t q = uem_quantize(n, 0);
    CHECK(q >= (n ? n : 1), "quantize(%zu)=%zu is smaller", n, q);
    CHECK(q >= prev, "quantize not monotonic at %zu: %zu < %zu", n, q, prev);
    CHECK(uem_quantize(q, 0) == q, "quantize(%zu)=%zu is not a fixed point (%zu)", n, q, uem_quantize(q, 0));
    if (n <= UEM_MAX_SMALL) CHECK(q % 16 == 0, "small class %zu not a multiple of 16", q);
    CHECK(q - (n ? n : 1) <= (n < 128 ? 16 : n / 4 + 16) || n > UEM_MAX_SMALL, "class waste too large at %zu: %zu", n, q);
    prev = q;
  }
  CHECK(uem_quantize(UEM_MAX_SMALL + 1, 0) % UEM_LARGE_GRANULE == 0, "large not page rounded");
}

static void test_alignment_and_usable(void) {
  static const size_t aligns[] = {0, 1, 3, 8, 16, 17, 32, 48, 64, 128, 256, 4096, 16384, 65536, 131072, 1 << 20};
  for (size_t ai = 0; ai < sizeof(aligns) / sizeof(aligns[0]); ai++) {
    size_t al = aligns[ai];
    size_t eff = al < 16 ? 16 : al;
    if (eff & (eff - 1)) eff = (size_t)1 << (64 - __builtin_clzll(eff - 1));
    uint64_t ptrs[64];
    size_t sizes[64], k = 0;
    for (size_t n = 0; n < 40; n += 3, k++) sizes[k] = n;
    for (size_t n = 100; n < UEM_MAX_SMALL * 3; n = n * 3 + 7, k++) sizes[k] = n;
    for (size_t i = 0; i < k; i++) {
      uint64_t p = uem_alloc(sizes[i], al);
      CHECK(p != 0, "alloc(%zu,%zu) failed", sizes[i], al);
      CHECK((p & (eff - 1)) == 0, "alloc(%zu,%zu)=0x%llx misaligned (want %zu)", sizes[i], al, (unsigned long long)p, eff);
      size_t u = 0;
      CHECK(uem_usable_size(p, &u) == 0, "no usable size for 0x%llx", (unsigned long long)p);
      CHECK(u >= (sizes[i] ? sizes[i] : 1), "usable %zu < request %zu", u, sizes[i]);
      CHECK(u == uem_quantize(sizes[i], al), "usable %zu != quantize %zu for (%zu,%zu)", u, uem_quantize(sizes[i], al), sizes[i], al);
      fill(p, u, (uint8_t)(0x40 + i));
      ptrs[i] = p;
    }
    for (size_t i = 0; i < k; i++) {
      size_t u;
      uem_usable_size(ptrs[i], &u);
      CHECK(verify(ptrs[i], u, (uint8_t)(0x40 + i)), "block %zu (size %zu align %zu) was overwritten", i, sizes[i], al);
    }
    for (size_t i = 0; i < k; i++) CHECK(uem_free(ptrs[i]) == 0, "free failed");
  }
}

static void test_realloc(void) {
  int oom;
  CHECK(uem_realloc(0, 100, 0, &oom) != 0 && !oom, "realloc(NULL) must allocate");
  CHECK(uem_realloc(0, 0, 0, &oom) != 0, "realloc(NULL,0) must allocate (as malloc(0))");
  uint64_t p = uem_alloc(50, 0);
  CHECK(uem_realloc(p, 0, 0, &oom) == 0 && !oom, "realloc(p,0) must free and return 0");
  // grow through every class and into the large range, shrink back; data survives, in-class resize keeps the pointer
  uint64_t q = uem_alloc(16, 0);
  size_t cur = 16;
  fill(q, 16, 0x11);
  size_t steps[] = {17, 40, 100, 200, 1000, 5000, 70000, 131072, 131073, 500000, 3000000, 70000, 900, 40, 16};
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    size_t n = steps[i];
    uint64_t r = uem_realloc(q, n, 0, &oom);
    CHECK(r != 0 && !oom, "realloc to %zu failed", n);
    size_t keep = n < cur ? n : cur;
    CHECK(verify(r, keep, 0x11), "realloc %zu -> %zu lost data", cur, n);
    size_t u;
    uem_usable_size(r, &u);
    CHECK(u >= n, "usable %zu < %zu after realloc", u, n);
    fill(r, n, 0x11);
    q = r;
    cur = n;
  }
  // same class: pointer is kept
  uint64_t a = uem_alloc(100, 0);
  uint64_t b = uem_realloc(a, uem_quantize(100, 0), 0, &oom);
  CHECK(a == b, "growing inside the class moved the block");
  // alignment is preserved/enforced by realloc
  uint64_t c = uem_alloc(64, 16);
  fill(c, 64, 0x22);
  uint64_t d = uem_realloc(c, 64, 4096, &oom);
  CHECK(d && (d & 4095) == 0, "realloc did not honour the new alignment (0x%llx)", (unsigned long long)d);
  CHECK(verify(d, 64, 0x22), "realloc to an alignment lost data");
  uem_free(d);
  uem_free(q);
  uem_free(b);
}

static void test_large_cache(void) {
  UemStats s0, s1;
  uem_stats(&s0);
  unsigned long m0 = g_nmap;
  uint64_t p = uem_alloc(300000, 0);
  fill(p, 300000, 0x33);
  uem_free(p);
  uint64_t q = uem_alloc(300000, 0);
  CHECK(p == q, "a freed large block of the same size was not reused (cache)");
  CHECK(g_nmap - m0 == 1, "cache miss mapped %lu times", g_nmap - m0);
  uem_free(q);
  uem_stats(&s1);
  CHECK(s1.large_cached_bytes > 0, "freed large block not cached");
  // a huge block is returned to the provider
  unsigned long u0 = g_nunmap;
  uint64_t h = uem_alloc(64ULL << 20, 0);
  CHECK(h != 0, "64 MiB alloc failed");
  uem_free(h);
  CHECK(g_nunmap == u0 + 1, "huge block was not unmapped");
  // large alignment
  uint64_t a = uem_alloc(200000, 1 << 20);
  CHECK(a && (a & ((1 << 20) - 1)) == 0, "large aligned alloc misaligned");
  uem_free(a);
}

static void test_oom(void) {
  // map() failure: small path, large path, realloc keeps the old block
  uint64_t keep = uem_alloc(100, 0);
  fill(keep, 100, 0x44);
  g_pfail = 1;
  uint64_t big = uem_alloc(1ULL << 30, 0);
  CHECK(big == 0, "large alloc must fail when the provider has nothing");
  int oom = 0;
  uint64_t r = uem_realloc(keep, 40ULL << 20, 0, &oom);
  CHECK(r == 0 && oom, "realloc OOM not reported");
  CHECK(verify(keep, 100, 0x44), "failed realloc damaged the original block");
  g_pfail = 0;
  CHECK(uem_alloc(~0ULL, 0) == 0, "absurd size must fail");
  CHECK(uem_alloc(1ULL << 50, 0) == 0, "2^50 must fail");
  CHECK(uem_free(0xdeadbeef000ULL) == -1, "free of a foreign pointer must be refused");
  CHECK(uem_free(0) == 0, "free(NULL) is a no-op");
  size_t u;
  CHECK(uem_usable_size(0xdeadbeef000ULL, &u) == -1, "usable size of a foreign pointer");
  uem_free(keep);
  // capped provider: allocate until it fails, free everything, allocate again
  g_pcap = g_pmapped + (4ULL << 20);
  uint64_t v[64];
  int n = 0;
  while (n < 64 && (v[n] = uem_alloc(900000, 0)) != 0) n++;
  CHECK(n > 0 && n < 64, "capped provider: %d blocks", n);
  for (int i = 0; i < n; i++) uem_free(v[i]);
  uint64_t again = uem_alloc(900000, 0);
  CHECK(again != 0, "memory was not reusable after the OOM");
  uem_free(again);
  g_pcap = ~0ULL;
}

// ---- stress -----------------------------------------------------------------------------------

#define MAXLIVE 4096
typedef struct Blk { uint64_t g; size_t n; uint8_t tag; } Blk;

typedef struct Mailbox {
  pthread_mutex_t m;
  Blk q[4096];
  int n;
} Mailbox;

static Mailbox g_mail[16];
static int g_nthreads, g_iters;
static atomic_ulong g_ops;

static size_t pick_size(uint64_t *rs) {
  uint64_t r = rng(rs);
  switch (r % 100) {
  case 0 ... 59: return (r >> 8) % 256;
  case 60 ... 84: return (r >> 8) % 4096;
  case 85 ... 94: return (r >> 8) % 60000;
  case 95 ... 98: return UEM_MAX_SMALL - 200 + (r >> 8) % 600;
  default: return 140000 + (r >> 8) % 3000000;
  }
}
static size_t pick_align(uint64_t *rs) {
  uint64_t r = rng(rs) % 100;
  if (r < 85) return 0;
  if (r < 93) return 32;
  if (r < 97) return 64;
  if (r < 99) return 4096;
  return 65536;
}

static void check_aligned(uint64_t p, size_t al) {
  size_t eff = al < 16 ? 16 : al;
  CHECK((p & (eff - 1)) == 0, "stress: 0x%llx misaligned for %zu", (unsigned long long)p, al);
}

static void *stress_thread(void *arg) {
  int id = (int)(intptr_t)arg;
  uint64_t rs = 0x9E3779B97F4A7C15ULL * (id + 1) + 12345;
  Blk *live = calloc(MAXLIVE, sizeof(Blk));
  int nlive = 0;
  uint8_t tagc = (uint8_t)(id * 16 + 1);
  for (int it = 0; it < g_iters; it++) {
    // take blocks other threads gave us (cross-thread free)
    if ((it & 63) == 0) {
      Mailbox *mb = &g_mail[id];
      pthread_mutex_lock(&mb->m);
      while (mb->n > 0) {
        Blk b = mb->q[--mb->n];
        pthread_mutex_unlock(&mb->m);
        size_t u = 0;
        CHECK(uem_usable_size(b.g, &u) == 0, "stress: foreign block vanished");
        CHECK(verify(b.g, b.n, b.tag), "stress: handed-off block corrupted (n=%zu)", b.n);
        CHECK(uem_free(b.g) == 0, "stress: cross-thread free failed");
        pthread_mutex_lock(&mb->m);
      }
      pthread_mutex_unlock(&mb->m);
    }
    uint64_t r = rng(&rs);
    unsigned op = r % 100;
    if (nlive < 8 || (op < 48 && nlive < MAXLIVE)) {
      size_t n = pick_size(&rs), al = pick_align(&rs);
      uint64_t p = uem_alloc(n, al);
      if (!p) { CHECK(0, "stress: alloc(%zu) failed", n); continue; }
      check_aligned(p, al);
      size_t u;
      uem_usable_size(p, &u);
      CHECK(u >= (n ? n : 1), "stress: usable too small");
      uint8_t tag = tagc++;
      if (!tag) tag = tagc++;
      fill(p, n ? n : 1, tag); // only the requested part: the tail may be touched by nobody
      live[nlive++] = (Blk){p, n ? n : 1, tag};
    } else if (op < 75) { // free
      int i = (int)((r >> 20) % nlive);
      Blk b = live[i];
      CHECK(verify(b.g, b.n, b.tag), "stress: live block corrupted before free (n=%zu)", b.n);
      live[i] = live[--nlive];
      if (g_nthreads > 1 && ((r >> 40) & 7) == 0) { // hand it to another thread to free
        int to = (int)((r >> 44) % g_nthreads);
        if (to != id) {
          Mailbox *mb = &g_mail[to];
          pthread_mutex_lock(&mb->m);
          if (mb->n < 4096) { mb->q[mb->n++] = b; pthread_mutex_unlock(&mb->m); continue; }
          pthread_mutex_unlock(&mb->m);
        }
      }
      CHECK(uem_free(b.g) == 0, "stress: free failed");
    } else if (op < 95) { // realloc
      int i = (int)((r >> 20) % nlive);
      Blk b = live[i];
      CHECK(verify(b.g, b.n, b.tag), "stress: live block corrupted before realloc");
      size_t n = pick_size(&rs), al = pick_align(&rs);
      int oom;
      uint64_t p = uem_realloc(b.g, n, al, &oom);
      if (n == 0) { CHECK(p == 0, "stress: realloc(p,0)"); live[i] = live[--nlive]; continue; }
      if (!p) { CHECK(0, "stress: realloc(%zu) failed", n); continue; }
      check_aligned(p, al);
      size_t keep = b.n < n ? b.n : n;
      CHECK(verify(p, keep, b.tag), "stress: realloc lost data (%zu -> %zu)", b.n, n);
      fill(p, n, b.tag);
      live[i] = (Blk){p, n, b.tag};
    } else { // quantize / usable size consistency on a live block
      int i = (int)((r >> 20) % nlive);
      size_t u;
      CHECK(uem_usable_size(live[i].g, &u) == 0 && u >= live[i].n, "stress: usable size");
    }
    atomic_fetch_add(&g_ops, 1);
  }
  for (int i = 0; i < nlive; i++) {
    CHECK(verify(live[i].g, live[i].n, live[i].tag), "stress: live block corrupted at exit (n=%zu)", live[i].n);
    uem_free(live[i].g);
  }
  free(live);
  return NULL; // thread exit flushes the cache (pthread key destructor)
}

static void run_stress(int nthreads, int iters) {
  g_nthreads = nthreads;
  g_iters = iters;
  for (int i = 0; i < 16; i++) { pthread_mutex_init(&g_mail[i].m, NULL); g_mail[i].n = 0; }
  pthread_t th[16];
  for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, stress_thread, (void *)(intptr_t)i);
  for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  // blocks still in mailboxes
  for (int i = 0; i < nthreads; i++)
    while (g_mail[i].n > 0) {
      Blk b = g_mail[i].q[--g_mail[i].n];
      CHECK(verify(b.g, b.n, b.tag), "stress: mailbox block corrupted");
      uem_free(b.g);
    }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 150000;
  g_host = mmap(NULL, ARENA_BYTES + (256ULL << 20), PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (g_host == MAP_FAILED) { perror("reserve"); return 2; }
  g_host = (uint8_t *)(((uintptr_t)g_host + (256ULL << 20) - 1) & ~((256ULL << 20) - 1));
  g_bias = g_host - GUEST_BASE;
  UemProvider p = {prov_map, prov_unmap, NULL, g_bias};
  if (uem_init(&p) != 0) { fprintf(stderr, "uem_init failed\n"); return 2; }
  uem_set_poison(1);

  test_geometry();
  test_alignment_and_usable();
  test_realloc();
  test_large_cache();
  test_oom();
  printf("functional tests done, failures so far: %d\n", g_fail);

  uem_set_poison(0);
  run_stress(1, iters);
  run_stress(4, iters);
  run_stress(8, iters);
  uem_set_poison(1);
  run_stress(8, iters / 2);
  UemStats s;
  uem_stats(&s);
  printf("stress: %lu ops; allocs=%llu small=%llu tcache hits=%llu large=%llu frees=%llu unknown=%llu slab=%llu MiB large_live=%llu large_cached=%llu KiB\n",
         (unsigned long)g_ops, (unsigned long long)s.allocs, (unsigned long long)s.small_allocs, (unsigned long long)s.tcache_hits,
         (unsigned long long)s.large_allocs, (unsigned long long)s.frees, (unsigned long long)s.unknown_frees,
         (unsigned long long)(s.slab_bytes >> 20), (unsigned long long)s.large_live_bytes, (unsigned long long)(s.large_cached_bytes >> 10));
  CHECK(s.large_live_bytes == 0, "stress leaked %llu bytes of large blocks", (unsigned long long)s.large_live_bytes);
  printf(g_fail ? "FAIL (%d)\n" : "PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
