// SPDX-License-Identifier: GPL-2.0-or-later
//
// Concurrency tests of the VM extent manager and the crash-handler mapping snapshot
// (docs/m1/memory.md sections 3 and 6): N threads map/unmap/protect/query in disjoint and
// overlapping ranges for a few seconds. Invariants: no lost or phantom mappings (extent list vs
// the pages the host really exposes), no overlaps or gaps in the list, no data corruption in
// memory a thread owns, the lock-free snapshot never returns a torn answer. Uses the guest-address
// API (recomp_vm_*, sceKernelMprotect) directly because the register-file wrappers of vmh.h are
// single-threaded.
#include "vmh.h"
#include "ps4_direct_mem.h"

#define AREA "concurrency"
#define N(s) "conc." VMH_MODE "." s
#ifndef SECS
#define SECS 3
#endif
#define NT 8
static int g_nt = NT;
#define WINPAGES 256

static volatile int g_stop;
static _Atomic uint64_t g_ops, g_bad;
static char g_why[NT + 4][240];
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void bad(int t, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  if (!g_why[t][0]) vsnprintf(g_why[t], sizeof g_why[t], fmt, ap);
  va_end(ap);
  g_bad++;
}
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static uint64_t gmap(uint64_t addr, uint64_t len, int flags) { return recomp_vm_map(ctx, addr, len, 0x10000, PROT_RW, flags, "conc"); }
static int page_mapped_host(uint64_t g) {  // probe a page; only used while no other thread runs
  if (sigsetjmp(vmh_jb, 1)) return 0;
  volatile uint8_t v = *(volatile uint8_t *)P(g); (void)v; return 1;
}

// ---- 1. disjoint windows, per-page model ----
typedef struct { int t; uint64_t base; uint8_t pat[WINPAGES]; /* 0 = unmapped, else expected fill */ } Win;
static Win wins[NT];
static void *disjoint(void *a) {
  Win *w = a; uint64_t s = 0x9e3779b97f4a7c15ULL ^ (uint64_t)w->t * 7919;
  uint8_t gen = 1;
  while (!g_stop) {
    uint64_t i = rnd(&s) % WINPAGES, n = 1 + rnd(&s) % 8; if (i + n > WINPAGES) n = WINPAGES - i;
    uint64_t addr = w->base + i * PAGE, len = n * PAGE;
    int opk = (int)(rnd(&s) % 5);
    if (getenv("CONC_DUMP")) fprintf(stderr, "OP %d page=%llu n=%llu\n", opk, (unsigned long long)i, (unsigned long long)n);
    switch (opk) {
    case 0: case 1: {  // fixed replace
      uint64_t r = gmap(addr, len, MAPF | F_FIXED);
      if (r != addr) { bad(w->t, "fixed map 0x%llx+%llu failed", (unsigned long long)addr, (unsigned long long)n); break; }
      gen = gen == 255 ? 1 : gen + 1;
      for (uint64_t k = 0; k < n; k++) { memset(P(addr + k * PAGE), gen, PAGE); w->pat[i + k] = gen; }
      break; }
    case 2: {  // EXCL on a possibly-mapped range: must succeed iff all free
      int all_free = 1; for (uint64_t k = 0; k < n; k++) if (w->pat[i + k]) all_free = 0;
      uint64_t r = gmap(addr, len, MAPF | F_FIXED | F_EXCL);
      if (all_free != (r == addr) && getenv("CONC_DUMP")) {
        pthread_mutex_lock(&proc_ctx->vm_mutex);
        for (GuestVMExtent *e = proc_ctx->vm_extents; e; e = e->next)
          if (e->addr + e->size + 0x40000 > addr && e->addr < addr + len + 0x40000) fprintf(stderr, "EXT 0x%llx +0x%zx %s %s\n", (unsigned long long)e->addr, e->size, e->is_free ? "free" : "MAPPED", e->name);
        fprintf(stderr, "REQ 0x%llx +0x%llx\n", (unsigned long long)addr, (unsigned long long)len);
        pthread_mutex_unlock(&proc_ctx->vm_mutex);
      }
      if (all_free != (r == addr)) { bad(w->t, "EXCL at 0x%llx: expected %s got 0x%llx", (unsigned long long)addr, all_free ? "success" : "failure", (unsigned long long)r); }
      if (r == addr) { gen = gen == 255 ? 1 : gen + 1; for (uint64_t k = 0; k < n; k++) { memset(P(addr + k * PAGE), gen, PAGE); w->pat[i + k] = gen; } }
      break; }
    case 3:
      if (recomp_vm_free(ctx, addr, len) != 0) bad(w->t, "munmap failed");
      for (uint64_t k = 0; k < n; k++) w->pat[i + k] = 0;
      break;
    case 4: sceKernelMprotect(ctx, (void *)(uintptr_t)addr, len, rnd(&s) & 1 ? 1 : 3); break;
    }
    if (getenv("CONC_DUMP") && g_nt == 1) {
      for (uint64_t k = 0; k < WINPAGES; k++) if ((w->pat[k] != 0) != is_mapped_extent(w->base + k * PAGE)) {
        fprintf(stderr, "DIVERGE after op page=%llu n=%llu page %llu model=%d ext=%d\n", (unsigned long long)i, (unsigned long long)n, (unsigned long long)k, w->pat[k] != 0, is_mapped_extent(w->base + k * PAGE)); g_stop = 1; break; }
    }
    // verify a few pages of the window against the model
    for (int c = 0; c < 4; c++) {
      uint64_t k = rnd(&s) % WINPAGES;
      if (w->pat[k]) {
        volatile uint8_t *p = P(w->base + k * PAGE);
        if (p[0] != w->pat[k] || p[PAGE - 1] != w->pat[k]) { bad(w->t, "page %llu corrupted: 0x%x want 0x%x", (unsigned long long)k, p[0], w->pat[k]); }
      }
    }
    g_ops++;
  }
  return NULL;
}

// ---- 2. all threads on one window, no model; the list must stay consistent ----
static uint64_t g_ow;
static void *overlap(void *a) {
  int t = (int)(intptr_t)a; uint64_t s = 0xdeadbeefULL + (uint64_t)t * 104729;
  while (!g_stop) {
    uint64_t i = rnd(&s) % WINPAGES, n = 1 + rnd(&s) % 16; if (i + n > WINPAGES) n = WINPAGES - i;
    uint64_t addr = g_ow + i * PAGE, len = n * PAGE;
    switch (rnd(&s) % 4) {
    case 0: gmap(addr, len, MAPF | F_FIXED); break;
    case 1: gmap(addr, len, MAPF | F_FIXED | F_EXCL); break;
    case 2: recomp_vm_free(ctx, addr, len); break;
    case 3: sceKernelMprotect(ctx, (void *)(uintptr_t)addr, len, 1 + (int)(rnd(&s) & 2)); break;
    }
    g_ops++;
  }
  return NULL;
}
static void *checker(void *a) {   // list invariants while mutators run
  (void)a; char why[200];
  while (!g_stop) { if (!vm_check_list(why, sizeof why)) { bad(NT, "live list invariant: %s", why); break; } usleep(200); }
  return NULL;
}
// ---- 3. snapshot readers: never torn ----
static void *reader(void *a) {
  int t = (int)(intptr_t)a; uint64_t s = 0x1234567ULL + (uint64_t)t;
  while (!g_stop) {
    uint64_t g = g_ow + (rnd(&s) % (WINPAGES * PAGE));
    Nps5FaultInfo fi; nps5_mem_classify((uint64_t)(uintptr_t)P(g), &fi);
    if (fi.cls == NPS5_FA_MAPPED && !(fi.lo <= g && g < fi.hi && fi.hi - fi.lo <= 0x100000000ULL)) { bad(NT + 1, "torn snapshot entry for 0x%llx: [0x%llx,0x%llx)", (unsigned long long)g, (unsigned long long)fi.lo, (unsigned long long)fi.hi); break; }
    if (fi.cls == NPS5_FA_MAPPED && strcmp(fi.name, "conc") != 0) { bad(NT + 1, "torn name '%.32s' for 0x%llx", fi.name, (unsigned long long)g); break; }
    if (fi.cls != NPS5_FA_MAPPED && fi.cls != NPS5_FA_UNMAPPED) { bad(NT + 1, "class %d for a window address", fi.cls); break; }
    g_ops++;
  }
  return NULL;
}
// ---- 4. unhinted contention: handed-out ranges never overlap ----
// an unhinted mapping is placed in 64 KiB granules (the console's allocation unit): the mapping is
// the length rounded up to it, and a munmap that names fewer bytes frees only those pages
#define RND64K(n) (((n) + 0xFFFFULL) & ~0xFFFFULL)
typedef struct { int t; uint64_t a[64]; uint64_t len[64]; uint8_t id; } Alloc;
static Alloc allocs[NT];
static void *unhinted(void *p) {
  Alloc *al = p; uint64_t s = 0xabcdefULL * (uint64_t)(al->t + 1);
  while (!g_stop) {
    int k = (int)(rnd(&s) % 64);
    if (al->a[k]) {
      for (uint64_t o = 0; o < al->len[k]; o += PAGE * 3) if (*P(al->a[k] + o) != al->id) { bad(al->t, "allocation 0x%llx overwritten by another thread (found 0x%x)", (unsigned long long)al->a[k], *P(al->a[k] + o)); return NULL; }
      recomp_vm_free(ctx, al->a[k], RND64K(al->len[k])); al->a[k] = 0;
    } else {
      uint64_t len = (1 + rnd(&s) % 40) * PAGE;
      uint64_t r = recomp_vm_map(ctx, 0, len, 0x10000, PROT_RW, MAPF, "conc");
      if (r == ~0ULL) { bad(al->t, "unhinted map of 0x%llx failed", (unsigned long long)len); return NULL; }
      memset(P(r), al->id, len); al->a[k] = r; al->len[k] = len;
    }
    g_ops++;
  }
  return NULL;
}
static int cmp_u64pair(const void *a, const void *b) { const uint64_t *x = a, *y = b; return x[0] < y[0] ? -1 : x[0] > y[0]; }

static void run(void *(*fn)(void *), void **args, int n, void *(*extra)(void *), int nextra) {
  pthread_t th[NT + 4]; int k = 0;
  g_stop = 0; g_ops = 0; g_bad = 0; memset(g_why, 0, sizeof g_why);
  for (int i = 0; i < n; i++) pthread_create(&th[k++], NULL, fn, args[i]);
  for (int i = 0; i < nextra; i++) pthread_create(&th[k++], NULL, extra, (void *)(intptr_t)i);
  double t0 = now(); while (now() - t0 < SECS) usleep(20000);
  g_stop = 1; for (int i = 0; i < k; i++) pthread_join(th[i], NULL);
}
static const char *first_why(void) { for (int i = 0; i < NT + 4; i++) if (g_why[i][0]) return g_why[i]; return ""; }

int main(void) {
  vmh_init();
  if (getenv("CONC_THREADS")) g_nt = atoi(getenv("CONC_THREADS"));
  char why[200];
  // 1
  mt_begin(N("disjoint_windows_model_check"), AREA);
  void *args[NT];
  for (int i = 0; i < NT; i++) { wins[i].t = i; wins[i].base = G(0x8000000000ULL) + (uint64_t)i * 0x10000000ULL; memset(wins[i].pat, 0, sizeof wins[i].pat); args[i] = &wins[i]; }
  run(disjoint, args, g_nt, checker, 1);
  MT_EXPECT(g_bad == 0, "%s (ops=%llu)", first_why(), (unsigned long long)g_ops);
  MT_EXPECT(vm_check_list(why, sizeof why), "final list: %s", why);
  int lost = 0, phantom = 0, corrupt = 0;
  for (int i = 0; i < NT; i++) for (int k = 0; k < WINPAGES; k++) {
    uint64_t g = wins[i].base + (uint64_t)k * PAGE;
    int ext = is_mapped_extent(g), host = page_mapped_host(g);
    if (wins[i].pat[k] && !ext) lost++;
    if (!wins[i].pat[k] && ext) phantom++;
    if (ext != host) corrupt++;
    if (wins[i].pat[k] && host && *P(g) != wins[i].pat[k]) corrupt++;
  }
  MT_EXPECT(lost == 0 && phantom == 0 && corrupt == 0, "lost=%d phantom=%d extent/host/data mismatch=%d", lost, phantom, corrupt);
  mt_end();
  for (int i = 0; i < NT; i++) recomp_vm_free(ctx, wins[i].base, WINPAGES * PAGE);

  // 2 + 3
  mt_begin(N("overlapping_window_and_snapshot_readers"), AREA);
  g_ow = G(0x8000000000ULL) + 0x40000000ULL;
  void *oargs[NT]; for (int i = 0; i < NT; i++) oargs[i] = (void *)(intptr_t)i;
  pthread_t tr[3]; (void)tr;
  // readers run as the "extra" threads together with the checker via a combined function
  run(overlap, oargs, NT, reader, 3);
  char torn[240]; strcpy(torn, g_why[NT + 1]); g_why[NT + 1][0] = 0;
  MT_EXPECT(!g_why[0][0] && !g_why[1][0] && !g_why[2][0] && !g_why[3][0] && !g_why[4][0] && !g_why[5][0] && !g_why[6][0] && !g_why[7][0] && !g_why[NT][0], "%s (ops=%llu)", first_why(), (unsigned long long)g_ops);
  MT_EXPECT(!torn[0], "nps5_mem_classify's sequence-lock reader accepts a half-written snapshot entry (no acquire fence between the data reads and the closing sequence load); probabilistic, seen about once per 10^8 queries" " [" "%s" "]", torn);
  MT_EXPECT(vm_check_list(why, sizeof why), "final list: %s", why);
  int ph = 0, mis = 0, snap = 0;
  for (int k = 0; k < WINPAGES; k++) {
    uint64_t g = g_ow + (uint64_t)k * PAGE;
    int ext = is_mapped_extent(g), host = page_mapped_host(g);
    if (ext && !host) mis++;       // extent says mapped, host says inaccessible: a decommit won a race it should not have
    if (!ext && host) ph++;        // extent says free, host still accessible: a leaked commit
    Nps5FaultInfo fi; nps5_mem_classify((uint64_t)(uintptr_t)P(g), &fi);
    if ((fi.cls == NPS5_FA_MAPPED) != ext) snap++;
  }
  MT_EXPECT(mis == 0 && ph == 0, "overlapping concurrent munmap/MAP_FIXED lose mappings: two recomp_vm_free calls collect the same extents, decommit outside vm_mutex, and the slower one marks a mapping that another thread has since created as free (the file comment says no range is freed twice, but nothing prevents it)" " [" "%d pages are mapped in the extent list but inaccessible on the host; %d pages are free in the list but still accessible" "]", mis, ph);
  MT_EXPECT(snap == 0, "%d pages where the crash snapshot disagrees with the extent list", snap);
  mt_end();
  recomp_vm_free(ctx, g_ow, WINPAGES * PAGE);

  // 4
  mt_begin(N("unhinted_first_fit_contention"), AREA);
  Sig s0 = vm_sig();
  for (int i = 0; i < NT; i++) { memset(&allocs[i], 0, sizeof allocs[i]); allocs[i].t = i; allocs[i].id = (uint8_t)(0x40 + i); args[i] = &allocs[i]; }
  run(unhinted, args, NT, checker, 1);
  MT_EXPECT(g_bad == 0, "%s (ops=%llu)", first_why(), (unsigned long long)g_ops);
  uint64_t rng[NT * 64][2]; int nr = 0;
  for (int i = 0; i < NT; i++) for (int k = 0; k < 64; k++) if (allocs[i].a[k]) { rng[nr][0] = allocs[i].a[k]; rng[nr][1] = RND64K(allocs[i].len[k]); nr++; }
  qsort(rng, (size_t)nr, sizeof rng[0], cmp_u64pair);
  int ov = 0; for (int i = 1; i < nr; i++) if (rng[i - 1][0] + rng[i - 1][1] > rng[i][0]) ov++;
  MT_EXPECT(ov == 0, "%d overlapping live allocations", ov);
  for (int i = 0; i < NT; i++) for (int k = 0; k < 64; k++) if (allocs[i].a[k]) recomp_vm_free(ctx, allocs[i].a[k], RND64K(allocs[i].len[k]));
  MT_EXPECT(sig_eq(s0, vm_sig()), "extent list after freeing everything differs from the start (leak or failed coalescing)");
  MT_EXPECT(vm_check_list(why, sizeof why), "final list: %s", why);
  mt_end();
  return mt_status();
}
