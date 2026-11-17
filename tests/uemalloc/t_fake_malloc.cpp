// Synthetic stand-in for an engine allocator object, to test the function-replacement facility end to end
// (tools/uemalloc_test.sh): an abstract allocator interface with a vtable (the layout of UE's FMalloc: virtual
// destructors, Malloc, TryMalloc, Realloc, TryRealloc, Free, QuantizeSize, GetAllocationSize), a concrete
// allocator whose constructor and virtual functions are the "original" implementation, and a global pointer the
// rest of the program uses, as the engine uses GMalloc. The original implementation is libc malloc with a header
// and counts its calls; once the translator has replaced it with the host allocator the counter stays 0.
#include "../ps5/src/ps5test.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#define NOINL __attribute__((noinline))

struct FakeExec {
  virtual ~FakeExec() {}
  virtual bool Exec(void *) { return false; }
};

struct FakeMalloc : FakeExec {
  virtual void *Malloc(size_t n, uint32_t a) = 0;
  virtual void *TryMalloc(size_t n, uint32_t a) { return Malloc(n, a); }  // base-class default, stays guest code
  virtual void *Realloc(void *p, size_t n, uint32_t a) = 0;
  virtual void *TryRealloc(void *p, size_t n, uint32_t a) { return Realloc(p, n, a); }
  virtual void Free(void *p) = 0;
  virtual size_t QuantizeSize(size_t n, uint32_t a) { return n; }
  virtual bool GetAllocationSize(void *p, size_t &out) { return false; }
  virtual void Trim(bool) {}
};

// ---- the "original" allocator: libc malloc with a header, counts its calls -----------------------
static unsigned long g_orig_calls;
static inline void count_call() { __atomic_fetch_add(&g_orig_calls, 1, __ATOMIC_RELAXED); }

struct Hdr { void *raw; size_t size; };

struct FakeBinned : FakeMalloc {
  unsigned long magic;
  NOINL FakeBinned();
  ~FakeBinned() override;
  void *Malloc(size_t n, uint32_t a) override;
  void *Realloc(void *p, size_t n, uint32_t a) override;
  void Free(void *p) override;
  size_t QuantizeSize(size_t n, uint32_t a) override;
  bool GetAllocationSize(void *p, size_t &out) override;
  void Trim(bool) override;
};

NOINL FakeBinned::FakeBinned() { count_call(); magic = 0x0123456789abcdefULL; }
FakeBinned::~FakeBinned() { count_call(); }
static size_t q16(size_t n) { return ((n ? n : 1) + 15) & ~(size_t)15; }
void *FakeBinned::Malloc(size_t n, uint32_t a) {
  count_call();
  size_t al = a < 16 ? 16 : a, sz = q16(n);
  void *raw = malloc(sz + al + sizeof(Hdr));
  if (!raw) return nullptr;
  uintptr_t p = ((uintptr_t)raw + sizeof(Hdr) + al - 1) & ~(uintptr_t)(al - 1);
  ((Hdr *)p)[-1] = {raw, sz};
  return (void *)p;
}
void *FakeBinned::Realloc(void *p, size_t n, uint32_t a) {
  count_call();
  if (!p) return Malloc(n, a);
  if (n == 0) { Free(p); return nullptr; }
  size_t old = ((Hdr *)p)[-1].size;
  void *q = Malloc(n, a);
  if (q) { memcpy(q, p, n < old ? n : old); Free(p); }
  return q;
}
void FakeBinned::Free(void *p) { count_call(); if (p) free(((Hdr *)p)[-1].raw); }
size_t FakeBinned::QuantizeSize(size_t n, uint32_t a) { count_call(); return q16(n); }
bool FakeBinned::GetAllocationSize(void *p, size_t &out) { count_call(); out = ((Hdr *)p)[-1].size; return true; }
void FakeBinned::Trim(bool) { count_call(); }

// ---- the program: uses the allocator only through the global pointer -------------------------
alignas(64) static unsigned char g_obj_storage[sizeof(FakeBinned)];
static FakeMalloc *volatile GMalloc;

static NOINL FakeMalloc *gm() { FakeMalloc *m = GMalloc; __asm__ volatile("" : "+r"(m)); return m; }

static int g_fail;
#define EXPECT(c, what) do { if (!(c)) { t_str("FAIL: " what "\n"); g_fail++; } } while (0)

static T_NOINLINE int verify(const unsigned char *p, size_t n, unsigned char v) {
  for (size_t i = 0; i < n; i++) if (p[i] != v) return 0;
  return 1;
}

static T_NOINLINE void test_basic() {
  FakeMalloc *m = gm();
  static const uint32_t aligns[] = {0, 8, 16, 32, 64, 256, 4096, 65536};
  static const size_t sizes[] = {0, 1, 15, 16, 17, 100, 1000, 4096, 40000, 131072, 131073, 700000};
  for (uint32_t a : aligns) for (size_t n : sizes) {
    void *p = m->Malloc(n, a);
    EXPECT(p != nullptr, "Malloc returned null");
    uint32_t eff = a < 16 ? 16 : a;
    EXPECT(((uintptr_t)p & (eff - 1)) == 0, "Malloc result misaligned");
    size_t u = 0;
    EXPECT(m->GetAllocationSize(p, u), "GetAllocationSize failed for a live block");
    EXPECT(u >= (n ? n : 1), "GetAllocationSize smaller than the request");
    EXPECT(m->QuantizeSize(n, a) == u, "QuantizeSize differs from GetAllocationSize");
    memset(p, 0x5a, n ? n : 1);
    EXPECT(verify((unsigned char *)p, n ? n : 1, 0x5a), "block does not hold data");
    void *t = m->TryMalloc(n, a);          // base-class default (guest code) -> native Malloc
    EXPECT(t != nullptr && t != p, "TryMalloc");
    m->Free(t);
    m->Free(p);
  }
  m->Free(nullptr);
  size_t q = m->QuantizeSize(100, 0);
  EXPECT(q >= 100 && q <= 128, "QuantizeSize(100) out of range");
}

static T_NOINLINE void test_realloc() {
  FakeMalloc *m = gm();
  unsigned char *p = (unsigned char *)m->Realloc(nullptr, 24, 0);
  EXPECT(p != nullptr, "Realloc(NULL) must allocate");
  memset(p, 0x37, 24);
  static const size_t steps[] = {100, 1000, 20000, 300000, 3000, 200, 24};
  size_t cur = 24;
  for (size_t n : steps) {
    p = (unsigned char *)m->TryRealloc(p, n, 0);
    EXPECT(p != nullptr, "Realloc returned null");
    size_t keep = n < cur ? n : cur;
    EXPECT(verify(p, keep, 0x37), "Realloc lost data");
    memset(p, 0x37, n);
    cur = n;
  }
  EXPECT(m->Realloc(p, 0, 0) == nullptr, "Realloc(p,0) must free and return NULL");
}

struct TArg { int id; int fails; unsigned long ops; };
static void *worker(void *a) {
  TArg *t = (TArg *)a;
  FakeMalloc *m = gm();
  uint64_t s = 0x1234567 + t->id * 7919;
  enum { N = 256 };
  struct B { unsigned char *p; size_t n; unsigned char tag; } live[N];
  memset(live, 0, sizeof live);
  for (int it = 0; it < 30000; it++) {
    int i = (int)(t_lcg(&s) % N);
    uint64_t r = t_lcg(&s);
    size_t n = (r % 10 < 8) ? (r >> 8) % 600 : (r >> 8) % 90000;
    uint32_t al = (r >> 4) % 16 == 0 ? 64 : 0;
    if (live[i].p) {
      if (!verify(live[i].p, live[i].n, live[i].tag)) { t->fails++; }
      if (r & 1) {
        unsigned char *q = (unsigned char *)m->Realloc(live[i].p, n, al);
        if (n == 0) { live[i].p = nullptr; continue; }
        size_t keep = n < live[i].n ? n : live[i].n;
        if (!q || !verify(q, keep, live[i].tag)) t->fails++;
        if (q) { memset(q, live[i].tag, n); live[i].p = q; live[i].n = n; }
      } else {
        m->Free(live[i].p);
        live[i].p = nullptr;
      }
    } else {
      unsigned char *q = (unsigned char *)m->Malloc(n ? n : 1, al);
      if (!q || (al && ((uintptr_t)q & (al - 1)))) t->fails++;
      if (q) { live[i] = {q, n ? n : 1, (unsigned char)(1 + t->id * 40 + (it & 31))}; memset(q, live[i].tag, live[i].n); }
    }
    t->ops++;
  }
  for (int i = 0; i < N; i++) if (live[i].p) { if (!verify(live[i].p, live[i].n, live[i].tag)) t->fails++; m->Free(live[i].p); }
  return nullptr;
}

static T_NOINLINE void test_threads() {
  pthread_t th[4];
  TArg a[4] = {};
  for (int i = 0; i < 4; i++) { a[i].id = i; pthread_create(&th[i], nullptr, worker, &a[i]); }
  unsigned long ops = 0;
  int fails = 0;
  for (int i = 0; i < 4; i++) { pthread_join(th[i], nullptr); ops += a[i].ops; fails += a[i].fails; }
  t_dec("thread_ops", (long long)ops);
  EXPECT(fails == 0, "threaded alloc/free/realloc corrupted a block");
}

extern "C" uint64_t sceKernelGetProcessTime(void);

/* Cost of one Malloc + Free pair through the global pointer (tools/uembench.sh compares the bridges). */
static T_NOINLINE void bench_pair() {
  FakeMalloc *m = gm();
  const int N = 2000000;
  unsigned long sink = 0;
  uint64_t t0 = sceKernelGetProcessTime();
  for (int i = 0; i < N; i++) { unsigned char *p = (unsigned char *)m->Malloc(64, 16); p[0] = (unsigned char)i; sink += p[0]; m->Free(p); }
  uint64_t us = sceKernelGetProcessTime() - t0;
  t_dec("bench_malloc_free_pair_ns_x100", (long long)(us * 100000ULL / N));
  EXPECT(sink != 1, "bench");
}

int main() {
  // The constructor and the global pointer, as the engine's GCreateMalloc does it.
  GMalloc = new (g_obj_storage) FakeBinned();
  test_basic();
  test_realloc();
  test_threads();
  bench_pair();
  gm()->Trim(true);
  t_str(g_orig_calls == 0 ? "replaced=yes\n" : "replaced=no\n");
  t_str(g_fail ? "result=FAIL\n" : "result=PASS\n");
  return g_fail ? 1 : 0;
}
