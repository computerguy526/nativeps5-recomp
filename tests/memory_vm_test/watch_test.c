// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host access hooks and access resolvers (docs/m3/writewatch.md), with a small page watch standing in for AnyPS5's:
// it write-protects pages and records the first store, and makes pages inaccessible until another thread has "brought them
// up to date" (read traps). Checks what the runtime owes such a library:
//   threads   stores from many threads to protected pages are all resolved and recorded (is_write is right), no crash
//   trap      a load from a trapped page waits in the handler for the resolving thread and then sees its bytes
//   kernel    read() into a protected page fails with EFAULT bare, works through nps5_kread; write() from a trapped page
//             works through nps5_kwrite; nps5_mem_safe_read/nps5_mem_range_readable accept a trapped page
//   protect   nps5_mem_host_protect goes through the protect hook
//   nested    a fault inside the resolver ends the process at once (no hang, no report loop)      [argv: nested]
//   wild      an unclaimed fault is still a crash                                                [argv: wild]
//   livelock  a resolver that claims a fault without changing the page ends in the crash report, not a spin   [argv: livelock]
//   decommit  nps5_mem_decommit tells the protect hook
// Built by pkg/runtime/mem_vm_darwin_test.go.
#include "nps5_diag.h"
#include "nps5_kio.h"
#include <sys/uio.h>
#include "nps5_mem.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

Nps5Arena g_nps5_arena;
#include <stdint.h>
uint64_t g_recomp_call_model = 0;
int recomp_shadow_handle_fault(void *uctx, uint64_t addr) { (void)uctx; (void)addr; return 0; }

#define PG 16384u
#define NPAGES 256u
static uint8_t *g_base;
enum { ST_OPEN = 0, ST_ARMED = 1, ST_TRAPPED = 2 };
static _Atomic uint8_t g_state[NPAGES];
static _Atomic uint32_t g_written[NPAGES], g_write_faults, g_read_faults, g_wrong_kind, g_protect_calls, g_begin, g_end, g_prepare;
static _Atomic int g_nested_mode, g_reclose, g_rearm_stop;
uint64_t nps5_host_write_refault_count(void);
static _Atomic int g_livelock_mode;
static pthread_mutex_t g_trap_mu = PTHREAD_MUTEX_INITIALIZER; // the "resolver thread" protocol: plain mutex + condvar are
static pthread_cond_t g_trap_cv = PTHREAD_COND_INITIALIZER;   // not async-signal-safe in general; the real watch uses Mach
static int g_trap_req = -1, g_trap_done = -1;                 // semaphores. Good enough here: the handler never holds g_trap_mu at a fault.

static void arm(unsigned p) { atomic_store(&g_state[p], ST_ARMED); mprotect(g_base + p * PG, PG, PROT_READ); }
static void trap(unsigned p) { atomic_store(&g_state[p], ST_TRAPPED); mprotect(g_base + p * PG, PG, PROT_NONE); }
static void open_page(unsigned p) { atomic_store(&g_state[p], ST_OPEN); mprotect(g_base + p * PG, PG, PROT_READ | PROT_WRITE); }

static void *resolver_thread(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_trap_mu);
  for (;;) {
    while (g_trap_req < 0) pthread_cond_wait(&g_trap_cv, &g_trap_mu);
    const int p = g_trap_req;
    g_trap_req = -1;
    if (atomic_load(&g_state[p]) == ST_TRAPPED) {
      mprotect(g_base + p * PG, PG, PROT_READ | PROT_WRITE);
      memset(g_base + p * PG, 0xa5, PG); // the "GPU results"
      atomic_store(&g_state[p], ST_OPEN);
    }
    g_trap_done = p;
    pthread_cond_broadcast(&g_trap_cv);
  }
  return NULL;
}
static void resolve_trap(unsigned p) {
  pthread_mutex_lock(&g_trap_mu);
  while (g_trap_req >= 0) pthread_cond_wait(&g_trap_cv, &g_trap_mu);
  g_trap_req = (int)p;
  g_trap_done = -1;
  pthread_cond_broadcast(&g_trap_cv);
  while (g_trap_done != (int)p && atomic_load(&g_state[p]) == ST_TRAPPED) pthread_cond_wait(&g_trap_cv, &g_trap_mu);
  pthread_mutex_unlock(&g_trap_mu);
}

static bool resolve(uint64_t a, int is_write) {
  if (a < (uint64_t)(uintptr_t)g_base || a >= (uint64_t)(uintptr_t)g_base + (uint64_t)NPAGES * PG) return false;
  const unsigned p = (unsigned)((a - (uint64_t)(uintptr_t)g_base) / PG);
  if (atomic_load(&g_nested_mode)) *(volatile int *)16 = 1; // a fault inside the resolver
  if (atomic_load(&g_livelock_mode)) return true; // claims it, changes nothing
  const uint8_t st = atomic_load(&g_state[p]);
  if (st == ST_TRAPPED) {
    atomic_fetch_add(is_write ? &g_write_faults : &g_read_faults, 1);
    resolve_trap(p);
    return true; // the access is retried; a store to a page that is armed by then faults once more
  }
  if (st == ST_ARMED) {
    if (!is_write) { atomic_fetch_add(&g_wrong_kind, 1); return false; }
    atomic_fetch_add(&g_write_faults, 1);
    atomic_fetch_add(&g_written[p], 1);
    atomic_store(&g_state[p], ST_OPEN);
    return mprotect(g_base + p * PG, PG, PROT_READ | PROT_WRITE) == 0;
  }
  return true; // another thread opened it between the fault and now: retry
}

static _Atomic int g_last_prot = -1;
static int hook_protect(void *h, size_t n, int prot) { atomic_fetch_add(&g_protect_calls, 1); atomic_store(&g_last_prot, prot); return mprotect(h, n, prot) == 0 ? 0 : errno; }
static void hook_begin(void *h, size_t n) {
  atomic_fetch_add(&g_begin, 1);
  for (uintptr_t a = (uintptr_t)h & ~(uintptr_t)(PG - 1); a < (uintptr_t)h + n; a += PG) {
    const unsigned p = (unsigned)((a - (uintptr_t)g_base) / PG);
    if (atomic_load(&g_state[p]) == ST_TRAPPED) resolve_trap(p);
    atomic_fetch_add(&g_written[p], 1);
    open_page(p);
  }
  // the library arming the last page of the range again before the kernel copies (its own threads do, at any time)
  if (atomic_load(&g_reclose) > 0 && atomic_fetch_sub(&g_reclose, 1) > 0) arm((unsigned)(((uintptr_t)h + n - 1 - (uintptr_t)g_base) / PG));
}
static void hook_end(void *h, size_t n) { (void)h; (void)n; atomic_fetch_add(&g_end, 1); }
static void hook_prepare(const void *h, size_t n) {
  atomic_fetch_add(&g_prepare, 1);
  for (uintptr_t a = (uintptr_t)h & ~(uintptr_t)(PG - 1); a < (uintptr_t)h + n; a += PG) {
    const unsigned p = (unsigned)((a - (uintptr_t)g_base) / PG);
    if (atomic_load(&g_state[p]) == ST_TRAPPED) resolve_trap(p);
  }
}

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s (line %d)\n", msg, __LINE__); exit(1); } } while (0)

typedef struct { unsigned first, count; int rounds; } Job;
static void *writer(void *arg) {
  const Job *j = arg;
  for (int r = 1; r <= j->rounds; r++)
    for (unsigned i = 0; i < j->count; i++) g_base[(size_t)(j->first + i) * PG + 8] = (uint8_t)r;
  return NULL;
}
static void *rearmer(void *arg) { // a library thread that keeps closing one page
  while (!atomic_load(&g_rearm_stop)) { arm((unsigned)(uintptr_t)arg); usleep(20); }
  return NULL;
}
static void *reader(void *arg) { return (void *)(uintptr_t)(*(volatile uint8_t *)arg); }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(40);
  g_base = mmap(NULL, (size_t)NPAGES * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(g_base != MAP_FAILED, "mmap");
  g_nps5_arena.host_lo = (uint64_t)(uintptr_t)g_base; // the "guest reservation": the hooks only see pointers inside it
  g_nps5_arena.host_len = (uint64_t)NPAGES * PG;
  nps5_diag_install_crash_handlers();
  CHECK(nps5_diag_add_access_resolver(resolve), "add resolver");
  const Nps5HostAccessHooks hooks = {.protect = hook_protect, .write_begin = hook_begin, .write_end = hook_end, .read_prepare = hook_prepare};
  nps5_mem_set_host_access_hooks(&hooks);
  pthread_t rt;
  pthread_create(&rt, NULL, resolver_thread, NULL);

  if (argc > 1 && !strcmp(argv[1], "wild")) { *(volatile int *)8 = 1; puts("FAIL: wild write survived"); return 1; }
  if (argc > 1 && !strcmp(argv[1], "livelock")) { // a resolver that claims a fault but leaves the page protected
    nps5_diag_set_livelock_limit(1000);
    atomic_store(&g_livelock_mode, 1);
    arm(1);
    g_base[PG] = 1;
    puts("FAIL: a claimed-but-unresolved fault spun forever");
    return 1;
  }
  if (argc > 1 && !strcmp(argv[1], "nested")) {
    atomic_store(&g_nested_mode, 1);
    arm(0);
    g_base[0] = 1;
    puts("FAIL: fault in the resolver survived");
    return 1;
  }

  // threads: 8 writers, the main thread re-arms pages under them
  enum { T = 8, PER = 24, ROUNDS = 200 };
  pthread_t th[T];
  Job jobs[T];
  for (unsigned p = 0; p < T * PER; p++) arm(p);
  for (int t = 0; t < T; t++) { jobs[t] = (Job){(unsigned)t * PER, PER, ROUNDS}; pthread_create(&th[t], NULL, writer, &jobs[t]); }
  for (int k = 0; k < 400; k++) { arm((unsigned)(k * 7) % (T * PER)); if (k % 16 == 0) sched_yield(); }
  for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
  for (unsigned p = 0; p < T * PER; p++) {
    CHECK(atomic_load(&g_written[p]) >= 1, "a protected page was written without a record");
    if (atomic_load(&g_state[p]) == ST_ARMED) open_page(p);
    CHECK(g_base[(size_t)p * PG + 8] == (uint8_t)ROUNDS, "a store was lost");
  }
  CHECK(atomic_load(&g_wrong_kind) == 0, "a store was reported as a load");
  CHECK(nps5_diag_resolved_faults() >= T * PER, "resolved fault count");

  // trap: loads on other threads wait for the resolver thread
  const unsigned tp = 200;
  trap(tp); trap(tp + 1);
  pthread_t r1, r2;
  pthread_create(&r1, NULL, reader, g_base + (size_t)tp * PG + 100);
  pthread_create(&r2, NULL, reader, g_base + (size_t)tp * PG + 200);
  void *v1, *v2;
  pthread_join(r1, &v1); pthread_join(r2, &v2);
  CHECK((uintptr_t)v1 == 0xa5 && (uintptr_t)v2 == 0xa5, "a trapped load did not see the resolved bytes");
  g_base[(size_t)(tp + 1) * PG + 3] = 0x11; // a store to a trapped page: resolved first, then it lands
  CHECK(g_base[(size_t)(tp + 1) * PG + 3] == 0x11 && g_base[(size_t)(tp + 1) * PG + 4] == 0xa5, "a store to a trapped page");
  CHECK(atomic_load(&g_read_faults) >= 1, "no load fault was seen as a load");

  // kernel: read into a protected page, write from a trapped page
  int fds[2];
  CHECK(pipe(fds) == 0, "pipe");
  CHECK(write(fds[1], "abcdefgh", 8) == 8, "fill");
  const unsigned kp = 210;
  arm(kp);
  errno = 0;
  CHECK(read(fds[0], g_base + (size_t)kp * PG, 8) == -1 && errno == EFAULT, "bare read into a protected page must fail (the premise)");
  const uint32_t before = atomic_load(&g_written[kp]);
  CHECK(nps5_kread(fds[0], g_base + (size_t)kp * PG, 8) == 8, "nps5_kread into a protected page");
  CHECK(!memcmp(g_base + (size_t)kp * PG, "abcdefgh", 8) && atomic_load(&g_written[kp]) > before, "the kernel write was not recorded");
  CHECK(atomic_load(&g_begin) == atomic_load(&g_end) && atomic_load(&g_begin) >= 1, "write bracket not balanced");
  trap(kp + 1);
  CHECK(nps5_kwrite(fds[1], g_base + (size_t)(kp + 1) * PG, 4) == 4, "nps5_kwrite from a trapped page");
  uint8_t back[4];
  CHECK(read(fds[0], back, 4) == 4 && back[0] == 0xa5 && back[3] == 0xa5, "the kernel read stale bytes");
  char local[8]; // outside the reservation: no hook
  const uint32_t b0 = atomic_load(&g_begin);
  CHECK(write(fds[1], "12345678", 8) == 8 && nps5_kread(fds[0], local, 8) == 8 && atomic_load(&g_begin) == b0, "a host buffer went through the hooks");
  trap(kp + 2);
  uint8_t one = 0;
  CHECK(nps5_mem_safe_read(g_base + (size_t)(kp + 2) * PG + 9, &one, 1) && one == 0xa5, "safe read of a trapped page");
  trap(kp + 3);
  CHECK(nps5_mem_range_readable(g_base + (size_t)(kp + 3) * PG, PG), "range_readable of a trapped page");
  CHECK(!nps5_mem_safe_read((void *)(uintptr_t)64, &one, 1), "safe read of unmapped memory");

  // kernel, page closed again between write_begin and the copy (a watch re-armed by another thread): the call is
  // repeated, the title never sees EFAULT or a short read for a valid buffer, and a sequential read keeps its position.
  {
    char path[] = "/tmp/nps5_watch_refault.XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    static uint8_t pat[3 * PG];
    for (size_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 131 + (i >> 9));
    CHECK(write(fd, pat, sizeof pat) == (ssize_t)sizeof pat, "fill file");
    const unsigned rp = 240;
    uint8_t *dst = g_base + (size_t)rp * PG;
    uint64_t r0 = nps5_host_write_refault_count();
    atomic_store(&g_reclose, 3);
    memset(dst, 0, 2 * PG);
    CHECK(nps5_kpread(fd, dst + 100, PG, 5) == (ssize_t)PG && !memcmp(dst + 100, pat + 5, PG), "nps5_kpread into a page closed again 3 times");
    CHECK(nps5_host_write_refault_count() == r0 + 3 && atomic_load(&g_reclose) == 0, "refault count (pread)");
    // sequential: the first page of the destination is open, the second closed again: the kernel may copy part before it
    // fails, which moves the file position; the retry must start from the original position
    CHECK(lseek(fd, 7, SEEK_SET) == 7, "seek");
    memset(dst, 0, 2 * PG);
    r0 = nps5_host_write_refault_count();
    atomic_store(&g_reclose, 2);
    CHECK(nps5_kread(fd, dst + PG - 4096, 8192) == 8192 && !memcmp(dst + PG - 4096, pat + 7, 8192), "nps5_kread across a page closed again");
    CHECK(lseek(fd, 0, SEEK_CUR) == 7 + 8192 && nps5_host_write_refault_count() == r0 + 2, "position after a repeated sequential read");
    struct iovec iov[2] = {{dst + 16, 1000}, {dst + PG + 16, 3000}};
    atomic_store(&g_reclose, 1);
    CHECK(nps5_kpreadv(fd, iov, 2, 11) == 4000 && !memcmp(dst + 16, pat + 11, 1000) && !memcmp(dst + PG + 16, pat + 1011, 3000), "nps5_kpreadv with a page closed again");
    // readv (sequential, position restored) and fread (stdio reads a large request straight into the buffer)
    CHECK(lseek(fd, 13, SEEK_SET) == 13, "seek (readv)");
    memset(dst, 0, 2 * PG);
    r0 = nps5_host_write_refault_count();
    atomic_store(&g_reclose, 2);
    CHECK(nps5_kreadv(fd, iov, 2) == 4000 && !memcmp(dst + 16, pat + 13, 1000) && !memcmp(dst + PG + 16, pat + 1013, 3000), "nps5_kreadv with a page closed again");
    CHECK(lseek(fd, 0, SEEK_CUR) == 13 + 4000 && nps5_host_write_refault_count() > r0, "position after a repeated readv");
    atomic_store(&g_reclose, 0);
    {
      FILE *fp = fdopen(dup(fd), "rb");
      CHECK(fp && fseeko(fp, 9, SEEK_SET) == 0, "fdopen");
      memset(dst, 0, 2 * PG);
      r0 = nps5_host_write_refault_count();
      atomic_store(&g_reclose, 2);
      CHECK(nps5_kfread(dst + PG - 8192, 1, 2 * 8192, fp) == 2 * 8192 && !memcmp(dst + PG - 8192, pat + 9, 2 * 8192), "nps5_kfread across a page closed again");
      CHECK(ftello(fp) == 9 + 2 * 8192 && !ferror(fp), "stream position and error flag after a repeated fread");
      printf("refault: fread repeated %llu times\n", (unsigned long long)(nps5_host_write_refault_count() - r0));
      atomic_store(&g_reclose, 0);
      fclose(fp);
    }
    // a page that is closed again every time is given up after NPS5_KIO_RETRIES: EFAULT, as for a bad pointer
    atomic_store(&g_reclose, 1 << 20);
    errno = 0;
    CHECK(nps5_kpread(fd, dst, 64, 0) == -1 && errno == EFAULT, "bounded retries");
    atomic_store(&g_reclose, 0);
    // outside the reservation nothing is retried
    r0 = nps5_host_write_refault_count();
    errno = 0;
    CHECK(nps5_kpread(fd, (void *)(uintptr_t)64, 8, 0) == -1 && errno == EFAULT && nps5_host_write_refault_count() == r0, "bad host pointer");
    // stress: a thread closes the page all the time while reads go into it
    pthread_t ra;
    atomic_store(&g_rearm_stop, 0);
    pthread_create(&ra, NULL, rearmer, (void *)(uintptr_t)(rp + 1));
    int bad = 0;
    for (int k = 0; k < 4000; k++) {
      const size_t off = (size_t)(k * 37) % PG, len = 1 + (size_t)(k * 101) % 6000;
      if (k & 1) { if (nps5_kpread(fd, dst + PG + 500, len, (off_t)off) != (ssize_t)len) bad++; }
      else { lseek(fd, (off_t)off, SEEK_SET); if (nps5_kread(fd, dst + PG + 500, len) != (ssize_t)len) bad++; }
      atomic_store(&g_state[rp + 1], ST_OPEN); mprotect(dst + PG, PG, PROT_READ | PROT_WRITE); // look at it without a fault
      if (memcmp(dst + PG + 500, pat + off, len)) bad++;
    }
    atomic_store(&g_rearm_stop, 1);
    pthread_join(ra, NULL);
    open_page(rp + 1);
    CHECK(bad == 0, "a read into a page another thread keeps closing failed or was short");
    printf("refault: %llu kernel writes repeated\n", (unsigned long long)nps5_host_write_refault_count());
    close(fd);
    unlink(path);
  }

  // decommit: the protect hook hears of it (PROT_NONE), so a watch's record of the guest protection cannot go stale
  const uint32_t pd = atomic_load(&g_protect_calls);
  CHECK(nps5_mem_decommit(230 * (uint64_t)PG, PG) == 0 && atomic_load(&g_protect_calls) == pd + 1 && atomic_load(&g_last_prot) == PROT_NONE, "decommit through the protect hook");
  CHECK(nps5_mem_commit(230 * (uint64_t)PG, PG) == 0 && g_base[230 * (size_t)PG] == 0, "commit after decommit");
  // protect: through the hook
  const uint32_t pc = atomic_load(&g_protect_calls);
  CHECK(nps5_mem_host_protect(g_base + 220 * (size_t)PG, PG, PROT_READ) == 0 && atomic_load(&g_protect_calls) == pc + 1, "protect hook");
  nps5_mem_set_host_access_hooks(NULL);
  CHECK(nps5_mem_host_protect(g_base + 220 * (size_t)PG, PG, PROT_READ | PROT_WRITE) == 0 && atomic_load(&g_protect_calls) == pc + 1, "protect without hooks");
  puts("WATCH OK");
  return 0;
}
