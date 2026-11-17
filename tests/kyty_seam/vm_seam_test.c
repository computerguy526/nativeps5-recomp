// SPDX-License-Identifier: GPL-2.0-or-later
//
// The NPS5_GPU=kyty seam on the REAL VM layer (core/nps5_vm.c, core/nps5_mem.c, kernel/memory/ps4_direct_mem.c) with
// ASTRO BOT's memory shape: the guest maps read-write direct and flexible memory, the library (the stub kyty_gpu)
// gets it registered and arms page watches on it, and a guest memset over every such range must succeed, before,
// while and after watches, across guest mprotect, unmap and re-map. An address nothing maps must be declined by the
// seam's resolver (it is a crash of the title, never a watch fault and never a livelock).
// Built and run by pkg/runtime/kyty_seam_darwin_test.go (TestKytySeamVM). Prints "VM SEAM OK <checks>".
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "kyty_stub.h"
#include "nps5_diag.h"
#include "nps5_kyty.h"
#include "nps5_kyty_watch.h"
#include "nps5_mem.h"
#include "ps4_direct_mem.h"
#include "recomp_runtime.h"

int sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int, off_t *);
int sceKernelReserveVirtualRange(GuestContext *, void **, size_t, int, size_t);
int sceKernelMapDirectMemory(GuestContext *, void **, size_t, int, int, off_t, size_t);
int sceKernelMapNamedFlexibleMemory(GuestContext *, void **, size_t, int, int, const char *);

static long g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { FIXED = 0x10 };
#define GIB (1ULL << 30)
#define MIB (1ULL << 20)
#define FIXED_2G 0x300000000ULL
#define FIXED_DM 0x1000000000ULL
#define DM_BYTES (3200ULL * MIB)

// ---- the parts of the runtime the seam calls that are not linked here ----
const Nps5KytyRoute nps5_kyty_routes[1] = {{0}};
const unsigned nps5_kyty_route_count = 0;
static Nps5ContextResolver t_resolver;
static GuestContext C;
bool nps5_diag_add_context_resolver(Nps5ContextResolver fn) { t_resolver = fn; return true; }
GuestContext *nps5_hostcall_ctx(void) { return NULL; }
GuestContext *recomp_get_current_ctx(void) { return &C; }
uint64_t nps5_hostcall_run(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t t) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return t; }
const char *ps4_vfs_get_app_root(void) { return "/app0"; }

static volatile KytyStubState *g_stub;
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_expect_crash;
static volatile long g_sigs, g_declined;
static volatile int g_last_sig;

// What nps5_diag's handler does with a memory fault: offer it to the access resolvers; unresolved = a crash.
static void on_fault(int sig, siginfo_t *si, void *uctx) {
  const ucontext_t *uc = uctx;
  const int is_write = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1);
  g_sigs++;
  g_last_sig = sig;
  if (t_resolver && t_resolver(uctx, (uint64_t)(uintptr_t)si->si_addr, is_write)) return;
  g_declined++;
  if (g_expect_crash) siglongjmp(g_jmp, 1);
  static const char m[] = "vm_seam_test: a guest store to mapped read-write memory faulted and the seam declined it\n";
  (void)!write(2, m, sizeof m - 1);
  _exit(3);
}

static void add_ext(uint64_t a, uint64_t s, bool fr, const char *n, int kind) {
  GuestVMExtent *e = calloc(1, sizeof(*e));
  e->addr = a; e->size = s; e->is_free = fr; e->kind = kind;
  if (n) strcpy(e->name, n);
  GuestVMExtent **pp = &C.vm_extents, *prev = NULL;
  while (*pp) { prev = *pp; pp = &(*pp)->next; }
  e->prev = prev;
  *pp = e;
}
static uint8_t *host(uint64_t g) { return g_nps5_arena.bias + g; }
__attribute__((noinline)) static uint32_t count_stub(uint32_t kind) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < g_stub->ncalls; i++) n += g_stub->calls[i].kind == kind;
  return n;
}
static bool registered(uint64_t g) { return g_stub->host->query_memory(g_stub->host->user, g, NULL, NULL, NULL, NULL) == 0; }
// The guest's memset (lean_memset is the host memset on the guest's view), then a read-back of both ends.
__attribute__((noinline)) static bool guest_memset(uint64_t g, int v, uint64_t len) {
  memset(host(g), v, len);
  volatile uint8_t *p = host(g);
  return p[0] == (uint8_t)v && p[len - 1] == (uint8_t)v && p[len / 2] == (uint8_t)v;
}
static int kernel_prot(uint64_t g) {
  mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)host(g);
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS) return -1;
  return addr > (mach_vm_address_t)(uintptr_t)host(g) ? -1 : (int)(info.protection & 3);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  char err[1024];
  if (nps5_mem_reserve(&nps5_mem_config, 0, err, sizeof err)) { printf("reserve: %s\n", err); return 1; }
  C.mem_base = g_nps5_arena.bias;
  C.mem_size = g_nps5_arena.guest_hi;
  C.process_ctx = &C;
  pthread_mutex_init(&C.vm_mutex, NULL);
  const uint64_t sys_end = 0x500000, top = nps5_mem_config.stack_top, len = nps5_mem_config.stack_len;
  add_ext(sys_end, top - len - NPS5_PAGE - sys_end, true, NULL, 0);
  add_ext(top - len - NPS5_PAGE, NPS5_PAGE, false, "stack_guard", NPS5_EXT_STACK_GUARD);
  add_ext(top - len, len, false, "main_stack", NPS5_EXT_MAIN_STACK);
  add_ext(top, g_nps5_arena.guest_hi - top, true, NULL, 0);
  nps5_mem_commit(top - len, len);
  g_nps5_vm_root = &C;
  recomp_vm_note_change();
  uint64_t pp = recomp_vm_map(&C, 0, 0x10000, 0, 3, 0, "param");
  CHECK(pp != (uint64_t)-1);
  uint64_t v;
  uint8_t *h = host(pp);
  v = 0x48; memcpy(h, &v, 8); v = 0x09000000; memcpy(h + 0x10, &v, 8); v = pp + 0x100; memcpy(h + 0x40, &v, 8);
  v = 0x30; memcpy(h + 0x100, &v, 8); v = pp + 0x200; memcpy(h + 0x110, &v, 8); v = 343932928ULL; memcpy(h + 0x200, &v, 8);
  C.proc_param_addr = pp;
  ps4_direct_mem_configure(&C, pp);

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  // A mapping made BEFORE the switch is engaged is replayed to the library.
  void *early = NULL;
  CHECK(sceKernelMapNamedFlexibleMemory(&C, &early, 4 * MIB, 3, 0, "early") == 0 && early);
  const uint64_t g_early = (uint64_t)(uintptr_t)early;

  CHECK(nps5_kyty_startup(&C, NULL) && nps5_kyty_active());
  void *lib = dlopen(getenv("NPS5_KYTY_GPU_LIB"), RTLD_NOW | RTLD_NOLOAD);
  CHECK(lib != NULL);
  g_stub = ((KytyStubState * (*)(void)) dlsym(lib, "KytyStubGetState"))();
  const KytyGpuHostCallbacks *hc = g_stub->host;
  CHECK(g_nps5_arena.mode == NPS5_ADDR_BASE && hc->host_info(hc->user)->guest_bias == (uint64_t)(uintptr_t)g_nps5_arena.bias);
  if (g_early >= 0x200000000ULL) CHECK(registered(g_early));
  CHECK(guest_memset(g_early, 0x5a, 4 * MIB));

  // ASTRO BOT's maps: 2 GiB flexible at the fixed guest address 0x300000000 over a reservation, 3.2 GB of direct memory
  // at the fixed guest address 0x1000000000, both read-write (prot 3). Then what the title does first: clear them.
  void *ra = (void *)(uintptr_t)FIXED_2G;
  CHECK(sceKernelReserveVirtualRange(&C, &ra, 2 * GIB, FIXED, 0x10000) == 0);
  CHECK(!registered(FIXED_2G)); // a reservation has no pages
  void *ma = (void *)(uintptr_t)FIXED_2G;
  CHECK(sceKernelMapNamedFlexibleMemory(&C, &ma, 2 * GIB, 3, FIXED, "astro_2g") == 0 && (uint64_t)(uintptr_t)ma == FIXED_2G);
  CHECK(registered(FIXED_2G) && registered(FIXED_2G + 2 * GIB - 1) && kernel_prot(FIXED_2G) == 3);
  CHECK(guest_memset(FIXED_2G, 0, 64 * MIB) && guest_memset(FIXED_2G + 2 * GIB - 8 * MIB, 0x11, 8 * MIB));
  off_t phys = 0;
  CHECK(sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), DM_BYTES, 0x10000, 0, &phys) == 0);
  void *da = (void *)(uintptr_t)FIXED_DM;
  CHECK(sceKernelMapDirectMemory(&C, &da, DM_BYTES, 3, FIXED, phys, 0x10000) == 0 && (uint64_t)(uintptr_t)da == FIXED_DM);
  CHECK(registered(FIXED_DM) && registered(FIXED_DM + DM_BYTES - 1) && kernel_prot(FIXED_DM) == 3 && kernel_prot(FIXED_DM + DM_BYTES - NPS5_PAGE) == 3);
  CHECK(guest_memset(FIXED_DM, 0, 64 * MIB) && guest_memset(FIXED_DM + DM_BYTES - 8 * MIB, 0x22, 8 * MIB));
  CHECK(g_sigs == 0); // no watch was armed: nothing may trap
  Nps5KytyWatchStats st;
  nps5_kyty_watch_stats(&st);
  CHECK(st.sets == 0 && st.faults == 0 && st.host_protects > 0); // the runtime's commits went through the table

  // The library arms write watches on 1 MiB of each range (as its tracker does once it has read the pages); the guest
  // clears 4 MiB around it. Every armed tracker page is reported, the memset completes, the content is the guest's.
  const uint64_t spots[2] = {FIXED_2G + 16 * MIB, FIXED_DM + 1024 * MIB};
  for (int i = 0; i < 2; i++) {
    const uint64_t g = spots[i];
    CHECK(hc->protect_guest(hc->user, g + MIB, MIB, KYTY_GPU_WATCH_WRITE) == 0 && kernel_prot(g + MIB) == 1 && kernel_prot(g) == 3);
    const uint32_t f0 = count_stub(STUB_FAULT);
    const long s0 = g_sigs;
    CHECK(guest_memset(g, 0x33 + i, 4 * MIB));
    CHECK(count_stub(STUB_FAULT) - f0 == 256 && g_sigs - s0 == 64 && g_declined == 0); // 256 tracker pages, 64 host pages
    CHECK(kernel_prot(g + MIB) == 3 && nps5_kyty_watch_get(g + MIB) == 0);
    // The alias shows what the guest wrote; a write through the alias under an access watch does not trap.
    const void *alias = NULL;
    uint64_t base = 0;
    CHECK(hc->query_memory(hc->user, g, &base, NULL, NULL, &alias) == 0 && alias != NULL);
    uint8_t *al = (uint8_t *)(uintptr_t)alias + (g - base);
    CHECK(al[MIB + 5] == 0x33 + i);
    CHECK(hc->protect_guest(hc->user, g, 64 * 1024, KYTY_GPU_WATCH_ACCESS) == 0 && kernel_prot(g) == 0);
    const long s1 = g_sigs;
    al[100] = 0x77;
    CHECK(g_sigs == s1);
    // The guest's read traps and is let through: its tracker page opens; the 15 others stay armed, so the host page
    // stays closed and the retried load is carried out on the alias.
    CHECK(host(g)[100] == 0x77 && g_sigs > s1 && kernel_prot(g) == 0 && nps5_kyty_watch_get(g) == 0 && nps5_kyty_watch_get(g + 4096) == NPS5_KW_ACCESS);
    CHECK(hc->protect_guest(hc->user, g, 64 * 1024, KYTY_GPU_WATCH_NONE) == 0 && kernel_prot(g) == 3);
    // Guest mprotect to read-only and back with a watch armed in the range: afterwards the guest can clear it again.
    CHECK(hc->protect_guest(hc->user, g + 2 * MIB, 4096, KYTY_GPU_WATCH_WRITE) == 0);
    CHECK(sceKernelMprotect(&C, (void *)(uintptr_t)(g + 2 * MIB), MIB, 1) == 0 && kernel_prot(g + 2 * MIB) == 1 && kernel_prot(g + 2 * MIB + NPS5_PAGE) == 1);
    CHECK(sceKernelMprotect(&C, (void *)(uintptr_t)(g + 2 * MIB), MIB, 3) == 0 && kernel_prot(g + 2 * MIB) == 1 && kernel_prot(g + 2 * MIB + NPS5_PAGE) == 3);
    CHECK(guest_memset(g + 2 * MIB, 0x44, MIB) && kernel_prot(g + 2 * MIB) == 3);
  }

  // False sharing on a 16 KiB host page: the GPU owns one tracker page (access-watched); the guest clears and reads
  // the three others (memset: STP/STR of Q and X registers, DC ZVA; then loads). Nothing is reported to the library,
  // the watched page stays armed, the bytes arrive (seen through the alias and read back through the trap).
  {
    const uint64_t fs = FIXED_2G + 96 * MIB;
    CHECK(guest_memset(fs, 0x01, 4 * NPS5_PAGE));
    CHECK(hc->protect_guest(hc->user, fs + 0x1000, 0x1000, KYTY_GPU_WATCH_ACCESS) == 0 && kernel_prot(fs) == 0);
    const uint32_t f0 = count_stub(STUB_FAULT);
    Nps5KytyWatchStats w0, w1;
    nps5_kyty_watch_stats(&w0);
    memset(host(fs), 0x5c, 0x1000);          // tracker page 0
    memset(host(fs + 0x2000), 0x5d, 0x2000); // tracker pages 2 and 3
    memset(host(fs + 0x2100), 0, 0x400);     // a zeroing memset (bzero path)
    volatile uint8_t *p = host(fs);
    CHECK(p[0] == 0x5c && p[0xfff] == 0x5c && p[0x2000] == 0x5d && p[0x2100] == 0 && p[0x24ff] == 0 && p[0x2500] == 0x5d && p[0x3fff] == 0x5d);
    CHECK(*(volatile uint64_t *)(host(fs) + 0x3ff8) == 0x5d5d5d5d5d5d5d5dull);
    nps5_kyty_watch_stats(&w1);
    CHECK(count_stub(STUB_FAULT) == f0 && kernel_prot(fs) == 0 && nps5_kyty_watch_get(fs + 0x1000) == NPS5_KW_ACCESS);
    CHECK(w1.emulated > w0.emulated && w1.emu_declined == w0.emu_declined);
    const void *alias = NULL;
    uint64_t base = 0;
    CHECK(hc->query_memory(hc->user, fs, &base, NULL, NULL, &alias) == 0 && alias);
    const uint8_t *al = (const uint8_t *)alias + (fs - base);
    CHECK(al[0] == 0x5c && al[0x1000] == 0x01 && al[0x1fff] == 0x01 && al[0x2000] == 0x5d && al[0x2200] == 0);
    // An access that reaches INTO the watched page is the library's: reported (a store), the page opens.
    memset(host(fs + 0xff8), 0x7e, 16);
    CHECK(count_stub(STUB_FAULT) == f0 + 1 && kernel_prot(fs) == 3 && p[0x1007] == 0x7e && p[0x1008] == 0x01);
    printf("false sharing: %llu accesses emulated\n", (unsigned long long)(w1.emulated - w0.emulated));
  }

  // Unmap part of the direct range with a watch armed inside, map it again at the same fixed address, clear it. A
  // stale record of either the watch or the "inaccessible" protection the unmap left would make this trap or crash.
  const uint64_t hole = FIXED_DM + 512 * MIB;
  CHECK(hc->protect_guest(hc->user, hole + 4096, 8192, KYTY_GPU_WATCH_ACCESS) == 0 && kernel_prot(hole) == 0);
  const uint32_t u0 = count_stub(STUB_UNREGISTER);
  CHECK(recomp_vm_free(&C, hole, 16 * MIB) == 0);
  CHECK(count_stub(STUB_UNREGISTER) > u0 && !registered(hole) && nps5_kyty_watch_get(hole + 4096) == 0);
  CHECK(registered(hole - 1) && registered(hole + 16 * MIB) && guest_memset(hole - MIB, 0x55, MIB) && guest_memset(hole + 16 * MIB, 0x56, MIB));
  // The unmapped hole: a store there is a crash of the title. The seam's resolver must decline it at once.
  g_expect_crash = 1;
  const long d0 = g_declined, sg0 = g_sigs;
  if (sigsetjmp(g_jmp, 1) == 0) {
    host(hole)[0x20000] = 1;
    CHECK(!"a store to unmapped guest memory did not fault");
  }
  g_expect_crash = 0;
  CHECK(g_declined == d0 + 1 && g_sigs == sg0 + 1 && g_last_sig == SIGBUS); // macOS reports the reservation's PROT_NONE as SIGBUS
  off_t phys2 = 0;
  CHECK(sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), 16 * MIB, 0x10000, 0, &phys2) == 0);
  void *again = (void *)(uintptr_t)hole;
  CHECK(sceKernelMapDirectMemory(&C, &again, 16 * MIB, 3, FIXED, phys2, 0x10000) == 0 && (uint64_t)(uintptr_t)again == hole);
  CHECK(registered(hole) && kernel_prot(hole) == 3);
  const long s2 = g_sigs;
  CHECK(guest_memset(hole, 0x66, 16 * MIB) && g_sigs == s2);
  // A GPU-only mapping (prot 0x30: no CPU bits) is readable and writable on the host, as without the seam, and registered.
  void *ga = NULL;
  CHECK(sceKernelMapNamedFlexibleMemory(&C, &ga, MIB, 0x30, 0, "gpu_only") == 0 && ga);
  CHECK(kernel_prot((uint64_t)(uintptr_t)ga) == 3 && guest_memset((uint64_t)(uintptr_t)ga, 0x12, MIB));
  // Read-only for the CPU, writable for the GPU (0x21): host read-write (the runtime's rule), the guest record says so.
  CHECK(sceKernelMprotect(&C, ga, MIB, 0x21) == 0 && kernel_prot((uint64_t)(uintptr_t)ga) == 3);
  CHECK(sceKernelMprotect(&C, ga, MIB, 0x11) == 0 && kernel_prot((uint64_t)(uintptr_t)ga) == 1);
  CHECK(sceKernelMprotect(&C, ga, MIB, 3) == 0 && guest_memset((uint64_t)(uintptr_t)ga, 0x13, MIB));
  CHECK(g_declined == d0 + 1);
  printf("VM SEAM OK %ld\n", g_checks);
  _exit(0);
}
