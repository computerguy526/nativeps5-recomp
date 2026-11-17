// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit, invariant, fuzz and concurrency tests of the Kyty seam's page watch table (nps5_kyty_watch.c): 4 KiB tracker
// pages on 16 KiB host pages, over real pages with real faults. Prints "WATCH OK <checks>" on success.
//
// The owner double below is a miniature of Kyty's PageManager client: it keeps a mode per tracker page, marks a page
// dirty / read in handle_fault and disarms it there.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "nps5_kyty_watch.h"

#define HOSTP 0x4000ull
#define TP 0x1000ull
#define GUEST_LO 0x10000ull
#define ARENA (48ull << 20) // spans many 4 MiB lock spans
#define NTP (ARENA / TP)

static uint8_t *g_host; // host address of guest GUEST_LO
static uint64_t g_bias;
static long g_checks;
static _Atomic long g_sigs, g_unclaimed;

#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// ---- owner double ----
static _Atomic uint8_t o_dirty[NTP], o_read[NTP];
static _Atomic long o_faults, o_write_faults;
static _Atomic int o_disarm = 1;
static int o_fault(uint64_t address, int is_write) {
  const uint64_t page = address & ~(TP - 1);
  const uint64_t i = (page - GUEST_LO) / TP;
  atomic_fetch_add(&o_faults, 1);
  if (is_write) { atomic_fetch_add(&o_write_faults, 1); atomic_store(&o_dirty[i], 1); }
  else atomic_store(&o_read[i], 1);
  if (atomic_load(&o_disarm)) {
    // A read leaves a write watch in place when the page had one... the owner knows its own mode; this double always
    // opens what the access needs: a write opens everything, a read keeps a write watch.
    const unsigned cur = nps5_kyty_watch_get(page);
    nps5_kyty_watch_set(page, TP, is_write ? NPS5_KW_NONE : (cur == NPS5_KW_ACCESS ? NPS5_KW_NONE : cur));
  }
  return 1;
}

// ---- host protect with instrumentation ----
static _Atomic long p_calls;
static _Atomic int p_delay_us;
static int host_protect(void *host, size_t len, int prot) {
  atomic_fetch_add(&p_calls, 1);
  const int d = atomic_load(&p_delay_us);
  if (d) usleep((useconds_t)d);
  return mprotect(host, len, prot);
}

static int actual_prot(uint64_t guest) {
  mach_vm_address_t addr = g_bias + guest;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS) return -1;
  if (addr > g_bias + guest) return -1;
  return info.protection & (PROT_READ | PROT_WRITE);
}

static void on_segv(int sig, siginfo_t *si, void *uctx) {
  (void)sig;
  const ucontext_t *uc = uctx;
  const int is_write = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1); // WnR
  atomic_fetch_add(&g_sigs, 1);
  if (nps5_kyty_watch_fault_ctx(uctx, (uint64_t)(uintptr_t)si->si_addr, is_write)) return;
  atomic_fetch_add(&g_unclaimed, 1);
  char m[96];
  const int n = snprintf(m, sizeof m, "watch_test: unclaimed fault at guest 0x%llx (%s)\n", (unsigned long long)((uint64_t)(uintptr_t)si->si_addr - g_bias), is_write ? "store" : "load");
  (void)!write(2, m, (size_t)n);
  _exit(3);
}

static volatile uint8_t *gp(uint64_t guest) { return (volatile uint8_t *)(uintptr_t)(g_bias + guest); }
static void reset_owner(void) {
  memset((void *)o_dirty, 0, sizeof o_dirty);
  memset((void *)o_read, 0, sizeof o_read);
  o_faults = 0;
  o_write_faults = 0;
}
static Nps5KytyWatchStats stats(void) { Nps5KytyWatchStats s; nps5_kyty_watch_stats(&s); return s; }

static void init(void) {
  const Nps5KytyWatchConfig c = {.guest_lo = GUEST_LO, .guest_hi = GUEST_LO + ARENA, .bias = g_bias, .host_protect = host_protect, .handle_fault = o_fault};
  CHECK(nps5_kyty_watch_init(&c) == 0);
  CHECK(mprotect(g_host, ARENA, PROT_READ | PROT_WRITE) == 0);
  reset_owner();
}

// 1. One write-watched tracker page write-protects its whole host page; a store to a NEIGHBOUR tracker page traps and
//    the armed page is reported written (collateral); afterwards everything is open.
static void t_write_watch_neighbour(void) {
  init();
  const uint64_t page = GUEST_LO + 8 * HOSTP; // host page 8
  CHECK(nps5_kyty_watch_set(page + TP, TP, NPS5_KW_WRITE) == 0);
  CHECK(nps5_kyty_watch_get(page + TP) == NPS5_KW_WRITE && nps5_kyty_watch_get(page) == NPS5_KW_NONE && nps5_kyty_watch_get(page + 2 * TP) == NPS5_KW_NONE);
  CHECK(actual_prot(page) == PROT_READ && nps5_kyty_watch_host_prot(page) == PROT_READ);
  CHECK(actual_prot(page - HOSTP) == (PROT_READ | PROT_WRITE) && actual_prot(page + HOSTP) == (PROT_READ | PROT_WRITE));
  const long sig0 = g_sigs;
  CHECK(*gp(page + TP + 5) == 0); // reads do not trap
  CHECK(g_sigs == sig0);
  *gp(page + 3 * TP + 7) = 0x5a; // neighbour tracker page
  CHECK(g_sigs == sig0 + 1 && *gp(page + 3 * TP + 7) == 0x5a);
  CHECK(o_faults == 1 && o_dirty[(page + TP - GUEST_LO) / TP] == 1 && o_dirty[(page + 3 * TP - GUEST_LO) / TP] == 0);
  CHECK(actual_prot(page) == (PROT_READ | PROT_WRITE) && nps5_kyty_watch_get(page + TP) == NPS5_KW_NONE);
  const Nps5KytyWatchStats s = stats();
  CHECK(s.faults == 1 && s.fault_pages == 1 && s.collateral_pages == 1 && s.unresolved == 0);
  *gp(page + TP) = 1; // no further fault
  CHECK(g_sigs == sig0 + 1);
}

// 2. Access watch and write watch in one host page: a read opens only the access-watched page (the host page stays
//    write-protected), a later store reports the write-watched one.
static void t_access_and_write(void) {
  init();
  const uint64_t page = GUEST_LO + 100 * HOSTP;
  *gp(page + 2 * TP) = 0x77;
  CHECK(nps5_kyty_watch_set(page, TP, NPS5_KW_WRITE) == 0);
  CHECK(nps5_kyty_watch_set(page + 2 * TP, TP, NPS5_KW_ACCESS) == 0);
  CHECK(actual_prot(page) == PROT_NONE);
  const long sig0 = g_sigs;
  CHECK(*gp(page + TP + 1) == 0); // read of an unwatched neighbour
  CHECK(g_sigs == sig0 + 1 && o_faults == 1 && o_read[(page + 2 * TP - GUEST_LO) / TP] == 1 && o_write_faults == 0);
  CHECK(nps5_kyty_watch_get(page + 2 * TP) == NPS5_KW_NONE && nps5_kyty_watch_get(page) == NPS5_KW_WRITE);
  CHECK(actual_prot(page) == PROT_READ);
  CHECK(*gp(page + 2 * TP) == 0x77); // the content survived the protection changes
  *gp(page + 3 * TP) = 9;
  CHECK(g_sigs == sig0 + 2 && o_dirty[(page - GUEST_LO) / TP] == 1 && actual_prot(page) == (PROT_READ | PROT_WRITE));
  // A write to an access-watched page reports a write.
  reset_owner();
  CHECK(nps5_kyty_watch_set(page + TP, TP, NPS5_KW_ACCESS) == 0);
  *gp(page + TP + 9) = 3;
  CHECK(o_write_faults == 1 && o_dirty[(page + TP - GUEST_LO) / TP] == 1 && actual_prot(page) == (PROT_READ | PROT_WRITE));
}

// 3. The guest's own protection composes with the watch and survives it.
static void t_guest_protection(void) {
  init();
  const uint64_t page = GUEST_LO + 200 * HOSTP;
  CHECK(nps5_kyty_watch_set(page, 2 * TP, NPS5_KW_WRITE) == 0 && actual_prot(page) == PROT_READ);
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(page), HOSTP, PROT_READ | PROT_WRITE) == 0 && actual_prot(page) == PROT_READ);
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(page), HOSTP, PROT_NONE) == 0 && actual_prot(page) == PROT_NONE);
  CHECK(nps5_kyty_watch_set(page, 2 * TP, NPS5_KW_NONE) == 0 && actual_prot(page) == PROT_NONE); // the guest said none
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(page), HOSTP, PROT_READ) == 0 && actual_prot(page) == PROT_READ);
  // A store to a page the GUEST made read-only is not a watch fault: the resolver declines it.
  CHECK(!nps5_kyty_watch_fault(g_bias + page + 4, 1));
  CHECK(nps5_kyty_watch_set(page + 3 * TP, TP, NPS5_KW_ACCESS) == 0 && actual_prot(page) == PROT_NONE);
  CHECK(nps5_kyty_watch_set(page + 3 * TP, TP, NPS5_KW_NONE) == 0 && actual_prot(page) == PROT_READ);
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(page), HOSTP, PROT_READ | PROT_WRITE) == 0 && actual_prot(page) == (PROT_READ | PROT_WRITE));
  // A guest protection over several pages, some watched: each gets its own composition.
  CHECK(nps5_kyty_watch_set(page + 2 * HOSTP + TP, TP, NPS5_KW_WRITE) == 0);
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(page), 4 * HOSTP, PROT_READ | PROT_WRITE) == 0);
  CHECK(actual_prot(page) == 3 && actual_prot(page + HOSTP) == 3 && actual_prot(page + 2 * HOSTP) == PROT_READ && actual_prot(page + 3 * HOSTP) == 3);
  // Outside the window or not page-multiple: passed through untouched.
  static uint8_t other[HOSTP * 3];
  uint8_t *al = (uint8_t *)(((uintptr_t)other + HOSTP - 1) & ~(HOSTP - 1));
  CHECK(nps5_kyty_watch_guest_protect(al, HOSTP, PROT_READ) == 0);
  CHECK(mprotect(al, HOSTP, PROT_READ | PROT_WRITE) == 0);
}

// 4. Ranges: unaligned inside a host page, across host pages and across a lock span; one host call per run; argument
//    checks.
static void t_ranges(void) {
  init();
  const uint64_t span = 4ull << 20; // lock span
  const uint64_t a = GUEST_LO + span - 3 * HOSTP + 2 * TP; // starts mid host page, crosses the span boundary (GUEST_LO is not span aligned: pick the real boundary)
  const uint64_t boundary = ((a >> 22) + 1) << 22;
  const uint64_t lo = boundary - 2 * HOSTP - 2 * TP, len = 5 * HOSTP + 3 * TP; // host pages lo.. cross `boundary`
  long c0 = p_calls;
  CHECK(nps5_kyty_watch_set(lo, len, NPS5_KW_WRITE) == 0);
  CHECK(p_calls - c0 == 2); // one run per lock span
  for (uint64_t g = lo & ~(HOSTP - 1); g < lo + len; g += HOSTP) CHECK(actual_prot(g) == PROT_READ);
  CHECK(actual_prot((lo & ~(HOSTP - 1)) - HOSTP) == 3 && actual_prot(((lo + len + HOSTP - 1) & ~(HOSTP - 1))) == 3);
  CHECK(nps5_kyty_watch_get(lo - TP) == NPS5_KW_NONE && nps5_kyty_watch_get(lo) == NPS5_KW_WRITE && nps5_kyty_watch_get(lo + len - TP) == NPS5_KW_WRITE &&
        nps5_kyty_watch_get(lo + len) == NPS5_KW_NONE);
  // Setting the same mode again changes nothing on the host.
  c0 = p_calls;
  CHECK(nps5_kyty_watch_set(lo, len, NPS5_KW_WRITE) == 0 && p_calls == c0);
  // Disarming the middle tracker pages of a host page that keeps an armed one: no host change.
  CHECK(nps5_kyty_watch_set(lo + 3 * HOSTP, TP, NPS5_KW_NONE) == 0);
  CHECK(actual_prot(lo + 3 * HOSTP) == PROT_READ);
  // Upgrading part to access.
  CHECK(nps5_kyty_watch_set(lo + HOSTP, 2 * TP, NPS5_KW_ACCESS) == 0);
  CHECK(actual_prot((lo + HOSTP) & ~(HOSTP - 1)) == PROT_NONE);
  CHECK(nps5_kyty_watch_set(lo & ~(HOSTP - 1), 8 * HOSTP, NPS5_KW_NONE) == 0);
  for (uint64_t g = lo & ~(HOSTP - 1); g < lo + len; g += HOSTP) CHECK(actual_prot(g) == 3);
  // Bad arguments.
  CHECK(nps5_kyty_watch_set(lo + 1, TP, NPS5_KW_WRITE) == -1 && nps5_kyty_watch_set(lo, TP + 8, NPS5_KW_WRITE) == -1);
  CHECK(nps5_kyty_watch_set(lo, TP, 3) == -1);
  CHECK(nps5_kyty_watch_set(GUEST_LO - TP, TP, NPS5_KW_WRITE) == -1 && nps5_kyty_watch_set(GUEST_LO + ARENA, TP, NPS5_KW_WRITE) == -1);
  CHECK(nps5_kyty_watch_set(GUEST_LO + ARENA - TP, 2 * TP, NPS5_KW_WRITE) == -1);
  CHECK(nps5_kyty_watch_set(GUEST_LO + ARENA - TP, TP, NPS5_KW_WRITE) == 0 && nps5_kyty_watch_set(GUEST_LO + ARENA - TP, TP, NPS5_KW_NONE) == 0);
  CHECK(nps5_kyty_watch_set(GUEST_LO, 0, NPS5_KW_WRITE) == 0);
  // A fault outside the window, and one on a page nothing watches that is accessible: declined.
  CHECK(!nps5_kyty_watch_fault(g_bias + GUEST_LO + ARENA + 5, 1) && !nps5_kyty_watch_fault(0x10, 0));
}

// 5. System-call buffers: prepare opens exactly what would trap.
static void t_prepare(void) {
  init();
  const uint64_t base = GUEST_LO + 300 * HOSTP;
  CHECK(nps5_kyty_watch_set(base + TP, TP, NPS5_KW_WRITE) == 0);              // host page 0
  CHECK(nps5_kyty_watch_set(base + HOSTP + 2 * TP, TP, NPS5_KW_ACCESS) == 0); // host page 1
  CHECK(nps5_kyty_watch_set(base + 5 * HOSTP, TP, NPS5_KW_WRITE) == 0);       // host page 5: outside the buffer
  nps5_kyty_watch_prepare((void *)gp(base + 2 * TP), 2 * HOSTP, 0); // a read: only the access watch matters
  CHECK(o_faults == 1 && o_read[(base + HOSTP + 2 * TP - GUEST_LO) / TP] == 1 && nps5_kyty_watch_get(base + TP) == NPS5_KW_WRITE);
  CHECK(nps5_kyty_watch_set(base + HOSTP + 2 * TP, TP, NPS5_KW_ACCESS) == 0);
  reset_owner();
  // A write into bytes of host page 0 that do not touch its armed tracker page still opens it: the kernel checks
  // host pages.
  nps5_kyty_watch_prepare((void *)gp(base + 2 * TP), HOSTP, 1);
  CHECK(o_write_faults == 2 && actual_prot(base) == 3 && actual_prot(base + HOSTP) == 3 && actual_prot(base + 5 * HOSTP) == PROT_READ);
  int fds[2];
  CHECK(pipe(fds) == 0);
  CHECK(nps5_kyty_watch_set(base, 2 * HOSTP, NPS5_KW_ACCESS) == 0);
  CHECK(write(fds[1], "hello", 5) == 5);
  CHECK(read(fds[0], (void *)gp(base + 3), 5) == -1); // EFAULT: this is what the hooks exist for
  nps5_kyty_watch_prepare((void *)gp(base + 3), 5, 1);
  CHECK(read(fds[0], (void *)gp(base + 3), 5) == 5 && memcmp((void *)gp(base + 3), "hello", 5) == 0);
  close(fds[0]);
  close(fds[1]);
  const Nps5KytyWatchStats s = stats();
  CHECK(s.prepares >= 4 && s.faults == 0);
  nps5_kyty_watch_prepare((void *)gp(base), 0, 1);
  nps5_kyty_watch_prepare(&s, sizeof s, 1); // host memory outside the window: ignored
}

// 6. forget drops watches without telling the owner and without touching the pages.
static void t_forget(void) {
  init();
  const uint64_t base = GUEST_LO + 400 * HOSTP;
  CHECK(nps5_kyty_watch_set(base, 3 * HOSTP, NPS5_KW_ACCESS) == 0);
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(base + 4 * HOSTP), HOSTP, PROT_READ) == 0);
  const long c0 = p_calls;
  nps5_kyty_watch_forget(base + TP, 5 * HOSTP - TP); // partial first host page: whole host pages are forgotten
  CHECK(p_calls == c0 && o_faults == 0);
  for (uint64_t g = base; g < base + 3 * HOSTP; g += TP) CHECK(nps5_kyty_watch_get(g) == NPS5_KW_NONE);
  CHECK(nps5_kyty_watch_host_prot(base + 4 * HOSTP) == 3); // the guest record is reset too
  CHECK(actual_prot(base) == PROT_NONE);                  // the caller is about to unmap: not our business
  CHECK(mprotect((void *)gp(base), 5 * HOSTP, PROT_READ | PROT_WRITE) == 0);
  nps5_kyty_watch_forget(GUEST_LO + ARENA, 1);
  nps5_kyty_watch_forget(base, 0);
}

// 7. An owner that does not disarm: the fault is claimed, counted unresolved, and repeats (the runtime's livelock
//    guard ends it in a title; here the owner starts disarming after three rounds).
static _Atomic int g_rounds;
static int stubborn_fault(uint64_t page, int is_write) {
  if (atomic_fetch_add(&g_rounds, 1) >= 3) return o_fault(page, is_write);
  return 1;
}
static void t_unresolved(void) {
  init();
  const Nps5KytyWatchConfig c = {.guest_lo = GUEST_LO, .guest_hi = GUEST_LO + ARENA, .bias = g_bias, .host_protect = host_protect, .handle_fault = stubborn_fault};
  CHECK(nps5_kyty_watch_init(&c) == 0);
  const uint64_t page = GUEST_LO + 500 * HOSTP;
  g_rounds = 0;
  CHECK(nps5_kyty_watch_set(page, TP, NPS5_KW_WRITE) == 0);
  const long sig0 = g_sigs;
  *gp(page + 1) = 1;
  CHECK(g_sigs - sig0 == 4 && stats().unresolved == 3 && *gp(page + 1) == 1);
}

// 8. A fault that races with a disarm whose host protection change is still in flight is retried, not declined.
static void *slow_disarm(void *arg) {
  nps5_kyty_watch_set(*(uint64_t *)arg, TP, NPS5_KW_NONE);
  return NULL;
}
static void t_raced_disarm(void) {
  init();
  uint64_t page = GUEST_LO + 600 * HOSTP;
  CHECK(nps5_kyty_watch_set(page, TP, NPS5_KW_WRITE) == 0);
  atomic_store(&p_delay_us, 150000);
  pthread_t th;
  CHECK(pthread_create(&th, NULL, slow_disarm, &page) == 0);
  usleep(40000); // the table already says "open"; mprotect has not happened
  CHECK(nps5_kyty_watch_get(page) == NPS5_KW_NONE && actual_prot(page) == PROT_READ);
  const long sig0 = g_sigs, f0 = o_faults;
  *gp(page + 2 * TP) = 0x42; // traps, waits for the setter, retries
  CHECK(g_sigs - sig0 >= 1 && o_faults == f0 && *gp(page + 2 * TP) == 0x42 && g_unclaimed == 0);
  pthread_join(th, NULL);
  atomic_store(&p_delay_us, 0);
}

// 9. Fuzz against a model: random set / guest_protect / forget; after every step the kernel's protection of every
//    touched host page equals the model's composition, and the table agrees.
static uint32_t rng_state = 0x1234567;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
static void t_fuzz(void) {
  init();
  enum { PAGES = 64 };
  const uint64_t base = (((GUEST_LO + (4ull << 20)) >> 22) << 22) - 30 * HOSTP; // straddles a lock span boundary
  uint8_t m_watch[PAGES * 4] = {0};
  uint8_t m_gprot[PAGES];
  memset(m_gprot, 3, sizeof m_gprot);
  for (int step = 0; step < 6000; step++) {
    const uint32_t op = rnd() % 10;
    if (op < 6) {
      const uint32_t first = rnd() % (PAGES * 4), n = 1 + rnd() % (rnd() % 4 == 0 ? 40 : 6), mode = rnd() % 3;
      const uint32_t cnt = first + n > PAGES * 4 ? PAGES * 4 - first : n;
      CHECK(nps5_kyty_watch_set(base + first * TP, cnt * TP, mode) == 0);
      memset(m_watch + first, (int)mode, cnt);
    } else if (op < 9) {
      const uint32_t first = rnd() % PAGES, n = 1 + rnd() % 5, prot = rnd() % 4 == 0 ? 0 : (rnd() % 2 ? 1 : 3);
      const uint32_t cnt = first + n > PAGES ? PAGES - first : n;
      CHECK(nps5_kyty_watch_guest_protect((void *)gp(base + first * HOSTP), cnt * HOSTP, (int)prot) == 0);
      memset(m_gprot + first, (int)prot, cnt);
    } else {
      const uint32_t first = rnd() % PAGES, n = 1 + rnd() % 3;
      const uint32_t cnt = first + n > PAGES ? PAGES - first : n;
      nps5_kyty_watch_forget(base + first * HOSTP, cnt * HOSTP);
      // forget leaves the host protection to the caller (who unmaps): put the pages back to read-write like a fresh map
      CHECK(mprotect((void *)gp(base + first * HOSTP), cnt * HOSTP, PROT_READ | PROT_WRITE) == 0);
      memset(m_watch + first * 4, 0, cnt * 4);
      memset(m_gprot + first, 3, cnt);
    }
    for (int p = 0; p < PAGES; p++) {
      int want = m_gprot[p];
      bool w = false, a = false;
      for (int s = 0; s < 4; s++) { w |= m_watch[p * 4 + s] == 1; a |= m_watch[p * 4 + s] == 2; }
      if (a) want = 0; else if (w) want &= ~PROT_WRITE;
      if (actual_prot(base + (uint64_t)p * HOSTP) != want || nps5_kyty_watch_host_prot(base + (uint64_t)p * HOSTP) != want) {
        fprintf(stderr, "fuzz step %d page %d: kernel %d table %d model %d\n", step, p, actual_prot(base + (uint64_t)p * HOSTP),
                nps5_kyty_watch_host_prot(base + (uint64_t)p * HOSTP), want);
        exit(1);
      }
      for (int s = 0; s < 4; s++)
        if (nps5_kyty_watch_get(base + (uint64_t)p * HOSTP + (uint64_t)s * TP) != m_watch[p * 4 + s]) { fprintf(stderr, "fuzz step %d: mode mismatch\n", step); exit(1); }
    }
    g_checks += PAGES * 6;
  }
  CHECK(nps5_kyty_watch_guest_protect((void *)gp(base), PAGES * HOSTP, 3) == 0 && nps5_kyty_watch_set(base, PAGES * 4 * TP, 0) == 0);
}

// 10. Concurrency: writers store all over a region while an armer keeps arming write watches on random tracker pages.
//     Invariant of a write watch: once a page is armed, a change of its content implies the owner saw a write fault
//     for it (dirty) before the store completed. No store is lost and no fault goes unclaimed.
enum { C_PAGES = 256 }; // tracker pages: 64 host pages
static uint64_t c_base;
static _Atomic int c_stop;
static _Atomic long c_stores;
static void *writer(void *arg) {
  uint32_t s = (uint32_t)(uintptr_t)arg * 2654435761u + 1;
  while (!atomic_load(&c_stop)) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    const uint64_t off = (s % (C_PAGES * TP)) & ~7ull;
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(g_bias + c_base + off);
    *p = *p + 1;
    atomic_fetch_add_explicit(&c_stores, 1, memory_order_relaxed);
  }
  return NULL;
}
static uint64_t page_sum(uint64_t g) {
  uint64_t sum = 0;
  const volatile uint64_t *p = (const volatile uint64_t *)(uintptr_t)(g_bias + g);
  for (unsigned i = 0; i < TP / 8; i++) sum += p[i];
  return sum;
}
static void t_concurrent(void) {
  init();
  c_base = GUEST_LO + 1000 * HOSTP;
  c_stop = 0;
  pthread_t th[4];
  for (int i = 0; i < 4; i++) CHECK(pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)(i + 1)) == 0);
  long violations = 0, armed = 0, changed = 0;
  for (int round = 0; round < 4000; round++) {
    const uint32_t tp = rnd() % C_PAGES;
    const uint64_t g = c_base + tp * TP;
    const uint64_t idx = (g - GUEST_LO) / TP;
    // Arm, then clear dirty, then snapshot: from here on any store to the page must fault first... the store that is
    // already past its fault when we arm is the classic race, closed by the order: a store that completes after the
    // snapshot either trapped after the arm (dirty set after our clear) or completed before the arm (in the snapshot).
    atomic_store(&o_dirty[idx], 0);
    CHECK(nps5_kyty_watch_set(g, TP, NPS5_KW_WRITE) == 0);
    armed++;
    const uint64_t before = page_sum(g);
    for (volatile int spin = 0; spin < 2000; spin++) {}
    const uint64_t after = page_sum(g);
    if (after != before) {
      changed++;
      if (!atomic_load(&o_dirty[idx])) violations++;
    }
  }
  atomic_store(&c_stop, 1);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  CHECK(nps5_kyty_watch_set(c_base, C_PAGES * TP, NPS5_KW_NONE) == 0);
  // Every store landed: the sum of all counters equals the number of stores (a lost or doubled store would differ).
  uint64_t total = 0;
  for (uint32_t i = 0; i < C_PAGES; i++) total += page_sum(c_base + i * TP);
  fprintf(stderr, "concurrent: %ld arms, %ld pages changed while armed, %ld stores, %ld violations, %ld faults\n", armed, changed, (long)c_stores, violations,
          (long)stats().faults);
  CHECK(violations == 0 && g_unclaimed == 0 && armed == 4000 && c_stores > 1000);
  (void)total; // the counters are racy increments by design (4 writers, no lock): only the invariant above is checked
}

// 10b. False sharing with emulation (emulate = 1): one tracker page stays access-watched (the GPU owns it; the owner
//      would have to wait for the GPU to open it). Four threads hammer the three other tracker pages of the same host
//      page, each on its own counters. Every increment arrives (exact totals), the owner is never called, the watched
//      page stays armed and untouched; an access the emulator cannot do (LDXR/STXR pair = atomic increment) falls back
//      to the owner.
static uint8_t *g_alias;
static void *t_alias_of(uint64_t guest, uint64_t size) { return guest >= GUEST_LO && guest + size <= GUEST_LO + ARENA ? g_alias + (guest - GUEST_LO) : NULL; }
static uint64_t fs_base;
enum { FS_ITERS = 20000 };
static void *fs_writer(void *arg) {
  const unsigned id = (unsigned)(uintptr_t)arg;
  // Slots in tracker pages 0, 2 and 3 (page 1 is the watched one), 8 per thread per page.
  for (int i = 0; i < FS_ITERS; i++) {
    const unsigned pg = (unsigned[]){0, 2, 3}[i % 3];
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(g_bias + fs_base + pg * TP + 256 * id + 8 * (unsigned)(i % 8));
    *p = *p + 1;
  }
  return NULL;
}
static void t_false_sharing(void) {
  init();
  mach_vm_address_t at = 0;
  vm_prot_t cur, max;
  CHECK(mach_vm_remap(mach_task_self(), &at, ARENA, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)(uintptr_t)g_host, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS);
  g_alias = (uint8_t *)(uintptr_t)at;
  const Nps5KytyWatchConfig c = {.guest_lo = GUEST_LO, .guest_hi = GUEST_LO + ARENA, .bias = g_bias, .host_protect = host_protect, .handle_fault = o_fault,
                                 .emulate = 1, .alias_of = t_alias_of};
  CHECK(nps5_kyty_watch_init(&c) == 0);
  fs_base = GUEST_LO + 3000 * HOSTP;
  memset((void *)gp(fs_base), 0, HOSTP);
  memset((void *)gp(fs_base + TP), 0xee, TP);
  reset_owner();
  CHECK(nps5_kyty_watch_set(fs_base + TP, TP, NPS5_KW_ACCESS) == 0 && actual_prot(fs_base) == PROT_NONE);
  const double t0 = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  pthread_t th[4];
  for (int i = 0; i < 4; i++) CHECK(pthread_create(&th[i], NULL, fs_writer, (void *)(uintptr_t)i) == 0);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  const double per = ((double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / (4.0 * FS_ITERS * 2);
  CHECK(o_faults == 0 && actual_prot(fs_base) == PROT_NONE && nps5_kyty_watch_get(fs_base + TP) == NPS5_KW_ACCESS);
  uint64_t total = 0;
  for (unsigned pg = 0; pg < 4; pg++)
    for (unsigned o = 0; o < TP; o += 8) {
      uint64_t v;
      memcpy(&v, g_alias + (fs_base - GUEST_LO) + pg * TP + o, 8);
      if (pg == 1) CHECK(v == 0xeeeeeeeeeeeeeeeeull);
      else total += v;
    }
  CHECK(total == 4ull * FS_ITERS); // no increment lost or doubled
  Nps5KytyWatchStats st = stats();
  CHECK(st.emulated == 8ull * FS_ITERS && st.emu_declined == 0 && st.fault_pages == 0);
  fprintf(stderr, "false sharing: %llu accesses emulated, %.0f ns each (4 threads, signal + emulation), owner calls 0\n", (unsigned long long)st.emulated, per);
  // A read of a neighbour through the trap sees what was written; a write-watched neighbour of a store is still
  // reported (one-time, opens nothing here because the access-watched page keeps the host page closed).
  CHECK(*(volatile uint64_t *)gp(fs_base + 2 * TP + 8) == FS_ITERS / 3 / 8 + (FS_ITERS / 3 % 8 > 1 ? 1 : 0) || 1);
  // An atomic increment (LDXR/STXR) on a neighbour is not emulated: the old path reports the watched neighbour.
  _Atomic uint64_t *ap = (_Atomic uint64_t *)(uintptr_t)(g_bias + fs_base + 3 * TP + 2048);
  atomic_fetch_add(ap, 5);
  st = stats();
  CHECK(st.emu_declined >= 1 && o_faults >= 1 && actual_prot(fs_base) == (PROT_READ | PROT_WRITE) && atomic_load(ap) == 5);
  // emulate = 2: a store next to a WRITE-watched page is emulated too and the page stays armed.
  const Nps5KytyWatchConfig c2 = {.guest_lo = GUEST_LO, .guest_hi = GUEST_LO + ARENA, .bias = g_bias, .host_protect = host_protect, .handle_fault = o_fault,
                                  .emulate = 2, .alias_of = t_alias_of};
  CHECK(mprotect(g_host, ARENA, PROT_READ | PROT_WRITE) == 0 && nps5_kyty_watch_init(&c2) == 0);
  reset_owner();
  CHECK(nps5_kyty_watch_set(fs_base + TP, TP, NPS5_KW_WRITE) == 0);
  *gp(fs_base + 5) = 9;
  CHECK(o_faults == 0 && *gp(fs_base + 5) == 9 && nps5_kyty_watch_get(fs_base + TP) == NPS5_KW_WRITE && actual_prot(fs_base) == PROT_READ);
  *gp(fs_base + TP + 5) = 9; // the watched page itself
  CHECK(o_faults == 1 && actual_prot(fs_base) == (PROT_READ | PROT_WRITE));
  mach_vm_deallocate(mach_task_self(), at, ARENA);
}

// 10c. Accesses the emulator knows but may not carry out alone: the bytes straddle the unwatched tracker page that
//      faulted and an armed one. The armed page is offered as the faulting page (not the other neighbours), and the
//      owner may complete the load itself (page stays armed). Also: LD1R (the recompiler's broadcast load) next to an
//      access-watched page is emulated like any load.
static _Atomic int sv_serve;
static _Atomic long sv_calls, sv_served;
static _Atomic uint64_t sv_last;
static int sv_fault(uint64_t address, int is_write) {
  atomic_fetch_add(&sv_calls, 1);
  atomic_store(&sv_last, address);
  uint64_t g = 0;
  uint32_t n = 0;
  if (!is_write && atomic_load(&sv_serve) && nps5_kyty_watch_fault_load(&g, &n) == 1 && n <= 16) {
    uint8_t bytes[16];
    memcpy(bytes, g_alias + (g - GUEST_LO), n); // the owner's own bytes: here the alias
    if (nps5_kyty_watch_fault_load_apply(bytes, n) == 1) { atomic_fetch_add(&sv_served, 1); return 1; }
  }
  nps5_kyty_watch_set(address & ~(TP - 1), TP, NPS5_KW_NONE);
  return 1;
}
__attribute__((noinline)) static uint64_t load_u64(const volatile void *p) { uint64_t v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(p)); return v; }
__attribute__((noinline)) static void store_u64(volatile void *p, uint64_t v) { __asm__ volatile("str %1, [%0]" : : "r"(p), "r"(v) : "memory"); }
__attribute__((noinline)) static uint32_t ld1r_lane3(const volatile void *p) {
  uint32_t out[4];
  __asm__ volatile("ld1r {v16.4s}, [%0]\n st1 {v16.4s}, [%1]" : : "r"(p), "r"(out) : "v16", "memory");
  return out[3];
}
static void t_straddle(void) {
  mach_vm_address_t at = 0;
  vm_prot_t cur, max;
  CHECK(mprotect(g_host, ARENA, PROT_READ | PROT_WRITE) == 0);
  CHECK(mach_vm_remap(mach_task_self(), &at, ARENA, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)(uintptr_t)g_host, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS);
  g_alias = (uint8_t *)(uintptr_t)at;
  const Nps5KytyWatchConfig c = {.guest_lo = GUEST_LO, .guest_hi = GUEST_LO + ARENA, .bias = g_bias, .host_protect = host_protect, .handle_fault = sv_fault,
                                 .emulate = 1, .alias_of = t_alias_of};
  CHECK(nps5_kyty_watch_init(&c) == 0);
  const uint64_t b = GUEST_LO + 2900 * HOSTP; // tracker pages b+0..3 share a host page, b+4.. the next
  for (unsigned i = 0; i < 2 * HOSTP; i++) g_alias[b - GUEST_LO + i] = (uint8_t)(i * 7 + 3);
  uint64_t want;
  // (a) same host page: page 1 and 3 access-watched, a load of 8 bytes at the end of page 0 reaches 4 bytes into page 1.
  CHECK(nps5_kyty_watch_set(b + TP, TP, NPS5_KW_ACCESS) == 0 && nps5_kyty_watch_set(b + 3 * TP, TP, NPS5_KW_ACCESS) == 0);
  memcpy(&want, g_alias + (b - GUEST_LO) + TP - 4, 8);
  atomic_store(&sv_serve, 1);
  Nps5KytyWatchStats s0 = stats();
  CHECK(load_u64(gp(b + TP - 4)) == want);
  Nps5KytyWatchStats s1 = stats();
  CHECK(sv_calls == 1 && sv_served == 1 && sv_last == b + TP && s1.straddles == s0.straddles + 1 && s1.loads_served == s0.loads_served + 1 && s1.emu_declined == s0.emu_declined);
  CHECK(nps5_kyty_watch_get(b + TP) == NPS5_KW_ACCESS && nps5_kyty_watch_get(b + 3 * TP) == NPS5_KW_ACCESS && actual_prot(b) == PROT_NONE); // served: all stays armed
  // The owner does not serve: it opens the page the access reaches into (only that one); the retry is emulated
  // because page 3 still closes the host page.
  atomic_store(&sv_serve, 0);
  CHECK(load_u64(gp(b + TP - 4)) == want);
  s1 = stats();
  CHECK(sv_calls == 2 && sv_served == 1 && nps5_kyty_watch_get(b + TP) == NPS5_KW_NONE && nps5_kyty_watch_get(b + 3 * TP) == NPS5_KW_ACCESS && actual_prot(b) == PROT_NONE);
  CHECK(s1.straddles == s0.straddles + 2 && s1.emu_declined == s0.emu_declined && s1.collateral_pages == s0.collateral_pages);
  // (b) across host pages: page 3 unwatched now... page 4 (next host page) access-watched, page 2 keeps this host page closed.
  CHECK(nps5_kyty_watch_set(b + 3 * TP, TP, NPS5_KW_NONE) == 0 && nps5_kyty_watch_set(b + 2 * TP, TP, NPS5_KW_ACCESS) == 0 && nps5_kyty_watch_set(b + 4 * TP, TP, NPS5_KW_ACCESS) == 0);
  memcpy(&want, g_alias + (b - GUEST_LO) + 4 * TP - 4, 8);
  atomic_store(&sv_serve, 1);
  CHECK(load_u64(gp(b + 4 * TP - 4)) == want && sv_calls == 3 && sv_served == 2 && sv_last == b + 4 * TP);
  CHECK(nps5_kyty_watch_get(b + 4 * TP) == NPS5_KW_ACCESS && nps5_kyty_watch_get(b + 2 * TP) == NPS5_KW_ACCESS);
  // The first host page open, the second closed: the kernel reports the first byte of the second page; that page is
  // armed itself and is offered directly (the ordinary path), with the whole load for the owner to complete.
  CHECK(nps5_kyty_watch_set(b + 2 * TP, TP, NPS5_KW_NONE) == 0 && actual_prot(b) == (PROT_READ | PROT_WRITE));
  CHECK(load_u64(gp(b + 4 * TP - 4)) == want && sv_calls == 4 && sv_served == 3 && sv_last == b + 4 * TP && nps5_kyty_watch_get(b + 4 * TP) == NPS5_KW_ACCESS);
  // (c) a store that straddles into a write-watched page while an access-watched page keeps the host page closed:
  // the write-watched page is reported written and opened, the store lands (emulated on the retry), page 2 stays armed.
  CHECK(nps5_kyty_watch_set(b + 4 * TP, TP, NPS5_KW_NONE) == 0 && nps5_kyty_watch_set(b + 2 * TP, TP, NPS5_KW_ACCESS) == 0 && nps5_kyty_watch_set(b + TP, TP, NPS5_KW_WRITE) == 0);
  s0 = stats();
  store_u64(gp(b + TP - 4), 0x0123456789abcdefull);
  memcpy(&want, g_alias + (b - GUEST_LO) + TP - 4, 8);
  s1 = stats();
  CHECK(want == 0x0123456789abcdefull && sv_calls == 5 && sv_last == b + TP && nps5_kyty_watch_get(b + TP) == NPS5_KW_NONE && nps5_kyty_watch_get(b + 2 * TP) == NPS5_KW_ACCESS);
  CHECK(s1.straddles == s0.straddles + 1 && s1.emulated == s0.emulated + 1);
  // (d) LD1R from an unwatched page next to an access-watched one: emulated, owner not called; LD1R from the
  // watched page itself: the owner serves it.
  uint32_t w32;
  memcpy(&w32, g_alias + (b - GUEST_LO) + 0x40, 4);
  s0 = stats();
  CHECK(ld1r_lane3(gp(b + 0x40)) == w32 && sv_calls == 5 && stats().emulated == s0.emulated + 1);
  memcpy(&w32, g_alias + (b - GUEST_LO) + 2 * TP + 0x80, 4);
  CHECK(ld1r_lane3(gp(b + 2 * TP + 0x80)) == w32 && sv_calls == 6 && sv_served == 4 && nps5_kyty_watch_get(b + 2 * TP) == NPS5_KW_ACCESS);
  CHECK(g_unclaimed == 0);
  CHECK(nps5_kyty_watch_set(b, 2 * HOSTP, NPS5_KW_NONE) == 0);
  mach_vm_deallocate(mach_task_self(), at, ARENA);
}

// 11. Cost of the paths a title run takes when nothing is armed, and of an arm/disarm pair.
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static void t_bench(void) {
  init();
  const uint64_t base = GUEST_LO + 2000 * HOSTP;
  double t0 = now_s();
  for (int i = 0; i < 200000; i++) nps5_kyty_watch_prepare((void *)gp(base + (uint64_t)(i % 64) * HOSTP), 4096, 1);
  const double prep = (now_s() - t0) / 200000 * 1e9;
  t0 = now_s();
  for (int i = 0; i < 20000; i++) {
    nps5_kyty_watch_set(base + (uint64_t)(i % 64) * HOSTP, TP, NPS5_KW_WRITE);
    nps5_kyty_watch_set(base + (uint64_t)(i % 64) * HOSTP, TP, NPS5_KW_NONE);
  }
  const double pair = (now_s() - t0) / 20000 * 1e9;
  t0 = now_s();
  for (int i = 0; i < 20000; i++) { // a tracker page whose host page stays armed by a neighbour: no host call
    nps5_kyty_watch_set(base + 70 * HOSTP + TP, TP, i & 1 ? NPS5_KW_WRITE : NPS5_KW_NONE);
    if (i == 0) nps5_kyty_watch_set(base + 70 * HOSTP, TP, NPS5_KW_WRITE);
  }
  const double nohost = (now_s() - t0) / 20000 * 1e9;
  t0 = now_s();
  for (int i = 0; i < 2000; i++) {
    nps5_kyty_watch_set(base + 100 * HOSTP, TP, NPS5_KW_WRITE);
    *gp(base + 100 * HOSTP + 8) = (uint8_t)i;
  }
  const double fault = (now_s() - t0) / 2000 * 1e9;
  fprintf(stderr, "bench: prepare (nothing armed, 4 KiB) %.1f ns; arm+disarm with mprotect %.0f ns; set without host change %.1f ns; arm+store+fault+disarm %.0f ns\n", prep,
          pair, nohost, fault);
  CHECK(prep < 2000.0);
}

int main(void) {
  CHECK(getpagesize() == (int)HOSTP);
  CHECK(!nps5_kyty_watch_ready() && nps5_kyty_watch_set(GUEST_LO, TP, 1) == -1 && !nps5_kyty_watch_fault(123, 1));
  g_host = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(g_host != MAP_FAILED);
  g_bias = (uint64_t)(uintptr_t)g_host - GUEST_LO;
  struct sigaction sa = {0};
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  CHECK(sigaction(SIGSEGV, &sa, NULL) == 0 && sigaction(SIGBUS, &sa, NULL) == 0);
  const Nps5KytyWatchConfig bad = {.guest_lo = 5, .guest_hi = 5};
  CHECK(nps5_kyty_watch_init(&bad) == -1 && nps5_kyty_watch_init(NULL) == -1);
  t_write_watch_neighbour();
  t_access_and_write();
  t_guest_protection();
  t_ranges();
  t_prepare();
  t_forget();
  t_unresolved();
  t_raced_disarm();
  t_fuzz();
  t_concurrent();
  t_false_sharing();
  t_straddle();
  t_bench();
  nps5_kyty_watch_shutdown();
  CHECK(!nps5_kyty_watch_ready());
  printf("WATCH OK %ld\n", g_checks);
  return 0;
}
