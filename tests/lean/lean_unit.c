// Host differential test of the lean HLE entries (docs/m3/leanshim.md): for every lean-marked shim,
// random arguments, the full shim (shim_x(ctx), reads ctx, writes ctx->rax) and the lean entry
// (lean_x(a0..a5, ctx)) must return the same RAX, leave the same guest memory and the same
// context. Build and run with tools/lean_unit.sh (native arm64; no guest code runs).
#include "nps5_lean.h"
#include "nps5_thunk.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- runtime stubs the linked modules need
uint8_t g_nps5_level[NPS5_S_COUNT];
void nps5_log(int s, int l, const char *fmt, ...) { (void)s; (void)l; (void)fmt; }
uint64_t g_recomp_call_model;
recomp_fn_t **g_dispatch_l1[DISPATCH_LEVEL_SIZE];
void ps4_host_quit(int c) { exit(c); }
int ps4_vfs_resolve(const char *g, char *h, size_t n) { strlcpy(h, g, n); return 0; }
static uint64_t bump = 0x10000000;
uint64_t recomp_vm_alloc_named_aligned(GuestContext *c, size_t size, size_t al, int prot, int fl, const char *n) {
  (void)c; (void)prot; (void)fl; (void)n;
  bump = (bump + al - 1) & ~(uint64_t)(al - 1);
  uint64_t r = bump; bump += (size + al - 1) & ~(uint64_t)(al - 1); return r;
}
int recomp_vm_free(GuestContext *c, uint64_t a, size_t s) { (void)c; (void)a; (void)s; return 0; }

// The mutex HLE the generated sync thunks call: a pure function of the mutex address, so the full and
// the lean entry see the same result. Everything else the thunk file references stays unresolved
// (-undefined dynamic_lookup) and is never called.
static int fake_mutex(void *m) { return (int)(((uintptr_t)m >> 3) % 7) - 3; }
int hle_pthread_mutex_lock(GuestContext *c, void *m) { (void)c; return fake_mutex(m); }
int hle_pthread_mutex_trylock(GuestContext *c, void *m) { (void)c; return fake_mutex(m) + 1; }
int hle_pthread_mutex_unlock(GuestContext *c, void *m) { (void)c; return fake_mutex(m) + 2; }
int hle_scePthreadMutexLock(GuestContext *c, void *m) { (void)c; return fake_mutex(m) + 3; }
int hle_scePthreadMutexTrylock(GuestContext *c, void *m) { (void)c; return fake_mutex(m) + 4; }
int hle_scePthreadMutexUnlock(GuestContext *c, void *m) { (void)c; return fake_mutex(m) + 5; }

#define ARENA (512ULL << 20)
#define REGION 0x800000ULL  // scratch region the random arguments point into
#define RSIZE 0x1000
static GuestContext *C;
static int fails, checks;
static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

#define SHIMS(X)                                                                                                      \
  X(memcpy, "PPN") X(memmove, "PPN") X(memset, "PCN") X(strlen, "S") X(strcpy, "PS") X(strncpy, "PSN") X(strcmp, "SS") \
  X(memchr, "PCN") X(memcmp, "PPN") X(bcmp, "PPN") X(bzero, "PN") X(strchr, "SC") X(strrchr, "SC") X(strncmp, "SSN")   \
  X(strcasecmp, "SS") X(strncasecmp, "SSN") X(strcat, "SS") X(strncat, "SSN") X(strnlen, "SN") X(toupper, "C")        \
  X(tolower, "C") X(pthread_mutex_lock, "I") X(pthread_mutex_trylock, "I") X(pthread_mutex_unlock, "I")                \
  X(scePthreadMutexLock, "I") X(scePthreadMutexTrylock, "I") X(scePthreadMutexUnlock, "I")

#define DECL(n, k) void shim_##n(GuestContext *); uint64_t lean_##n(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
SHIMS(DECL)
void shim_malloc(GuestContext *); void shim_free(GuestContext *); void shim_calloc(GuestContext *);
void shim_realloc(GuestContext *); void shim_aligned_alloc(GuestContext *); void shim_memalign(GuestContext *);
void shim_malloc_usable_size(GuestContext *);
uint64_t lean_malloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_free(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_calloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_realloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_aligned_alloc(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_memalign(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
uint64_t lean_malloc_usable_size(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);

typedef void (*full_t)(GuestContext *);
typedef uint64_t (*lean_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, GuestContext *);
static void bad(const char *name, const char *what, uint64_t a, uint64_t b) {
  printf("FAIL %s: %s differs (full 0x%llx, lean 0x%llx)\n", name, what, (unsigned long long)a, (unsigned long long)b);
  fails++;
}

// Fills the scratch region with string-ish data: printable letters, some NULs, case mixes.
static void fill_region(void) {
  uint8_t *p = C->mem_base + REGION;
  for (int i = 0; i < RSIZE * 2; i++) {
    uint64_t r = rnd();
    p[i] = (r % 11 == 0) ? 0 : (uint8_t)(r % 5 == 0 ? (r >> 8) : 'A' + (r >> 8) % 52 + ((r >> 8) % 52 > 25 ? 6 : 0));
  }
}

static uint64_t gen_arg(char k) {
  switch (k) {
  case 'P': case 'S': return REGION + rnd() % 0x400;
  case 'N': return rnd() % 7 == 0 ? rnd() % 4 : rnd() % 200;
  case 'C': return rnd() % 3 == 0 ? rnd() : (uint64_t)(int64_t)((int)(rnd() % 400) - 100);
  default: return REGION + (rnd() % 0x100) * 8;
  }
}

static void diff_one(const char *name, full_t full, lean_t lean, const char *kinds) {
  uint64_t a[6];
  for (int i = 0; i < 6; i++) a[i] = kinds[i] && i < (int)strlen(kinds) ? gen_arg(kinds[i]) : rnd();
  static uint8_t snap[RSIZE * 2], mf[RSIZE * 2];
  fill_region();
  memcpy(snap, C->mem_base + REGION, sizeof snap);
  C->rax = rnd(); C->rcx = rnd(); C->rdx = rnd(); C->rbx = rnd(); C->rbp = rnd();
  C->rdi = a[0]; C->rsi = a[1]; C->rdx = a[2]; C->rcx = a[3]; C->r8 = a[4]; C->r9 = a[5];
  C->r10 = rnd(); C->r11 = rnd(); C->r12 = rnd(); C->r13 = rnd(); C->r14 = rnd(); C->r15 = rnd();
  GuestContext before = *C;
  full(C);
  GuestContext after_full = *C;
  memcpy(mf, C->mem_base + REGION, sizeof mf);
  memcpy(C->mem_base + REGION, snap, sizeof snap);
  *C = before;
  uint64_t rax = lean(a[0], a[1], a[2], a[3], a[4], a[5], C);
  checks++;
  if (rax != after_full.rax) bad(name, "RAX", after_full.rax, rax);
  if (memcmp(mf, C->mem_base + REGION, sizeof mf)) bad(name, "guest memory", 0, 1);
  // the lean entry must not touch the context at all (the bridge reads RAX from the return value) ...
  GuestContext chk = before;
  if (memcmp(C, &chk, sizeof chk)) bad(name, "context (the lean entry wrote to it)", 0, 1);
  // ... and the full shim changes only RAX (and pops the return address)
  chk = after_full; chk.rax = before.rax; chk.rsp = before.rsp;
  if (memcmp(&chk, &before, sizeof chk)) bad(name, "context (the full shim wrote more than rax/rsp)", 0, 1);
}

static void heap_tests(void) {
  // allocate with one entry point and release/resize with the other: they share one heap
  for (int round = 0; round < 3000; round++) {
    int full_alloc = rnd() & 1, full_free = rnd() & 1;
    uint64_t sz = rnd() % 5 == 0 ? rnd() % 100000 : rnd() % 300;
    uint64_t p, p2;
    C->rdi = sz;
    if (full_alloc) { shim_malloc(C); p = C->rax; } else p = lean_malloc(sz, 0, 0, 0, 0, 0, C);
    checks++;
    if (!p || p % 16) { bad("malloc", "result (null or misaligned)", 1, p); continue; }
    memset(C->mem_base + p, 0x5a, sz > 64 ? 64 : sz);
    C->rdi = p; shim_malloc_usable_size(C); uint64_t uf = C->rax;
    uint64_t ul = lean_malloc_usable_size(p, 0, 0, 0, 0, 0, C);
    if (uf != ul || uf < sz) bad("malloc_usable_size", "size", uf, ul);
    uint64_t nsz = rnd() % 1000;
    C->rdi = p; C->rsi = nsz;
    if (rnd() & 1) { shim_realloc(C); p2 = C->rax; } else p2 = lean_realloc(p, nsz, 0, 0, 0, 0, C);
    if (nsz && !p2) { bad("realloc", "null", 1, 0); continue; }
    if (nsz && C->mem_base[p2] != 0x5a && sz) bad("realloc", "content", 0x5a, C->mem_base[p2]);
    if (!nsz) continue;
    C->rdi = p2;
    if (full_free) { shim_free(C); } else lean_free(p2, 0, 0, 0, 0, 0, C);
  }
  for (int round = 0; round < 500; round++) {
    uint64_t n = rnd() % 20, sz = rnd() % 64 + 1, p;
    C->rdi = n; C->rsi = sz;
    if (rnd() & 1) { shim_calloc(C); p = C->rax; } else p = lean_calloc(n, sz, 0, 0, 0, 0, C);
    checks++;
    if (!p) { bad("calloc", "null", 1, 0); continue; }
    for (uint64_t i = 0; i < n * sz; i++) if (C->mem_base[p + i]) { bad("calloc", "not zeroed", 0, 1); break; }
    uint64_t al = 1ULL << (4 + rnd() % 9);
    C->rdi = al; C->rsi = sz;
    uint64_t q;
    if (rnd() & 1) { shim_aligned_alloc(C); q = C->rax; } else q = lean_aligned_alloc(al, sz, 0, 0, 0, 0, C);
    if (!q || q % al) bad("aligned_alloc", "alignment", al, q);
    C->rdi = al; C->rsi = sz;
    if (rnd() & 1) { shim_memalign(C); q = C->rax; } else q = lean_memalign(al, sz, 0, 0, 0, 0, C);
    if (!q || q % al) bad("memalign", "alignment", al, q);
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int iters = argc > 1 ? atoi(argv[1]) : 3000;
  C = calloc(1, sizeof *C);
  C->mem_base = calloc(1, ARENA); C->mem_size = ARENA;
  C->fs_base = 0x200000;
  C->rsp = 0x3000000;
#define RUN(n, k) for (int i = 0; i < iters; i++) diff_one(#n, shim_##n, lean_##n, k);
  SHIMS(RUN)
  heap_tests();
  if (fails) { printf("lean_unit: %d FAILED of %d checks\n", fails, checks); return 1; }
  printf("lean_unit: PASS (%d checks)\n", checks);
  return 0;
}
