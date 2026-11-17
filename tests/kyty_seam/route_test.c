// SPDX-License-Identifier: GPL-2.0-or-later
//
// Test host of the NPS5_GPU=kyty seam (pkg/runtime/kyty_seam_darwin_test.go builds and runs it).
// Linked: the generated AnyPS5 thunks (nps5_anyps5_thunks.c), nps5_kyty.c, nps5_kyty_watch.c, a generated fake of the
// AnyPS5 libraries (every export records its call) and the fakes of the runtime below. Loaded: a fake libnps5_gpu and
// the stub kyty_gpu (kyty_stub.c).
//
//   route_test default            switch off: every routable thunk still calls AnyPS5, nothing is loaded or hooked
//   route_test kyty               switch on, guest pointers (default): routes, memory hand-off, watches, callbacks
//   route_test kyty-host          switch on, KYTY_STUB_POINTERS=host
//   route_test missing            a NID the library lacks stops the process when called (expects abort)
//   route_test noentry            nid_entry is null: dlsym fallback
// Prints "ROUTE OK <mode> <checks>".

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "kyty_stub.h"
#include "nps5_anyps5_config.h"
#include "nps5_hostcall.h"
#include "nps5_kyty.h"
#include "nps5_kyty_watch.h"
#include "nps5_mem.h"
#include "recomp_runtime.h"

#define G0 0x200000000ull // guest base of the test arena (the application area starts here)
#define ARENA (64ull << 20)

static long g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// ---- fakes of the runtime ----
Nps5Arena g_nps5_arena;
_Thread_local int g_nps5_in_crash_handler;
static Nps5VmHooks t_hooks;
static int t_nhooks, t_nresolvers, t_naccess;
static Nps5ContextResolver t_resolver;
static Nps5HostAccessHooks t_access;
static GuestContext *t_ctx;
static int t_in_call;
int recomp_vm_hooks_register(const Nps5VmHooks *h) { t_hooks = *h; return t_nhooks++; }
// The host-mapped driver memory (base mode only; the test arena is flat): never reached, the symbol must exist.
uint64_t recomp_vm_alloc_fixed(GuestContext *ctx, uint64_t desired_addr, size_t size, int prot, int flags, const char *name) {
  (void)ctx; (void)desired_addr; (void)size; (void)prot; (void)flags; (void)name;
  return 0;
}
bool nps5_diag_add_context_resolver(Nps5ContextResolver fn) { t_resolver = fn; t_nresolvers++; return true; }
void nps5_mem_set_host_access_hooks(const Nps5HostAccessHooks *h) { if (h) { t_access = *h; t_naccess++; } }
void nps5_anyps5_enter(GuestContext *ctx, const char *name) { (void)name; t_ctx = ctx; t_in_call++; }
void nps5_anyps5_leave(GuestContext *ctx, const char *name) { (void)ctx; (void)name; t_in_call--; }
GuestContext *nps5_hostcall_ctx(void) { return t_in_call > 0 ? t_ctx : NULL; }
GuestContext *recomp_get_current_ctx(void) { return t_ctx; }
uint64_t nps5_hostcall_run(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t target) {
  return target + a0 + a1 + a2 + a3 + a4 + a5;
}
const char *ps4_vfs_get_app_root(void) { return "/fake/app0"; }
int nps5_anyps5_bind_exports(void *handle);

// ---- the generated fake AnyPS5 (fake_anyps5.c in this image, fake_gpu.c in the dylib) ----
typedef struct FakeLast { int id; const char *name; uint64_t a[8]; double f0; uint64_t calls; uint64_t ret; } FakeLast;
extern FakeLast fake_direct;
static FakeLast *fake_gpu; // dlsym(gpu, "fake_gpu_state")
typedef struct FakeHost { int adds, deletes, delivers, pads, exits, script; int64_t eq, ident; uint32_t kind; uint64_t data; void *udata; int code; } FakeHost;
static FakeHost *fake_host;

static uint8_t *g_host;
static uint64_t g_bias;
static GuestContext g_ctx;
static void *g_gpu;
static KytyStubState *g_stub;

static void on_segv(int sig, siginfo_t *si, void *uctx) {
  (void)sig;
  const ucontext_t *uc = uctx;
  const int is_write = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1);
  if (t_resolver && t_resolver(uctx, (uint64_t)(uintptr_t)si->si_addr, is_write)) return;
  static const char m[] = "route_test: unclaimed fault\n";
  (void)!write(2, m, sizeof m - 1);
  _exit(3);
}

static int actual_prot(uint64_t guest) {
  mach_vm_address_t addr = g_bias + guest;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS) return -1;
  return addr > g_bias + guest ? -1 : (int)(info.protection & 3);
}

typedef void (*Shim)(GuestContext *);
static Shim shim_of(const char *name) {
  char buf[200];
  size_t n = strlen(name);
  if (n > 12 && strcmp(name + n - 12, "_nid_postfix") == 0) n -= 12;
  snprintf(buf, sizeof buf, "shim_aps5_%.*s", (int)n, name);
  Shim s = (Shim)dlsym(RTLD_MAIN_ONLY, buf);
  if (!s) { fprintf(stderr, "no shim %s\n", buf); exit(1); }
  return s;
}
// Guest call: six register arguments, then stack arguments.
static uint64_t call(Shim s, const uint64_t *regs6, const uint64_t *stack, int nstack) {
  GuestContext *c = &g_ctx;
  c->rdi = regs6[0]; c->rsi = regs6[1]; c->rdx = regs6[2]; c->rcx = regs6[3]; c->r8 = regs6[4]; c->r9 = regs6[5];
  c->rsp = G0 + (1 << 20);
  uint64_t *sp = (uint64_t *)(uintptr_t)(g_bias + c->rsp);
  sp[0] = 0xdeadbeef; // return address
  for (int i = 0; i < nstack; i++) sp[1 + i] = stack[i];
  c->rax = 0xAAAAAAAAAAAAAAAAull;
  s(c);
  CHECK(c->rsp == G0 + (1 << 20) + 8); // SHIM_RETURN popped the return address
  return c->rax;
}
__attribute__((noinline)) static const KytyStubCall *last_stub(uint32_t kind) {
  const volatile KytyStubState *st = g_stub;
  for (uint32_t i = st->ncalls; i > 0; i--)
    if (st->calls[i - 1].kind == kind) return (const KytyStubCall *)&st->calls[i - 1];
  return NULL;
}
// The stub records from inside the fault handler too: read its state as volatile and never inline, so a count taken
// after a trapping load or store is not one the compiler remembered from before it.
__attribute__((noinline)) static uint32_t count_stub(uint32_t kind) {
  const volatile KytyStubState *st = g_stub;
  uint32_t n = 0;
  for (uint32_t i = 0; i < st->ncalls; i++) n += st->calls[i].kind == kind;
  return n;
}
static void *anyps5_symbol(const Nps5KytyRoute *r) {
  void *p = dlsym(RTLD_MAIN_ONLY, r->name);
  return p ? p : dlsym(g_gpu, r->name);
}

static const uint64_t REGS[6] = {G0 + 0x100000, 0x22, 0x33, G0 + 0x4000, 0x55, 0x66};

// The default path: the switch is off.
static void test_default(void) {
  CHECK(!nps5_kyty_startup(&g_ctx, g_gpu) && !nps5_kyty_active());
  CHECK(t_nhooks == 0 && t_nresolvers == 0 && t_naccess == 0 && !nps5_kyty_watch_ready());
  CHECK(nps5_kyty_route_count == 84);
  for (unsigned i = 0; i < nps5_kyty_route_count; i++) {
    const Nps5KytyRoute *r = &nps5_kyty_routes[i];
    CHECK(*r->slot != NULL && *r->slot == anyps5_symbol(r)); // the pointer is the AnyPS5 function, bound at load
    CHECK(*r->slot != r->raw);
    FakeLast *f = dlsym(RTLD_MAIN_ONLY, r->name) ? &fake_direct : fake_gpu;
    const uint64_t before = f->calls;
    call(shim_of(r->name), REGS, NULL, 0);
    CHECK(f->calls == before + 1 && strcmp(f->name, r->name) == 0);
  }
  // Marshalling is what it was: pointers translated to host, the guest's stack arguments in place, results back.
  const uint64_t stack[6] = {0x77, G0 + 0x8000, 0x99, 0xa1, 0xa2, 0xa3};
  fake_direct.ret = g_bias + G0 + 0x100040;
  const uint64_t rax = call(shim_of("sceAgcDcbDmaData"), REGS, stack, 6);
  CHECK(strcmp(fake_direct.name, "sceAgcDcbDmaData") == 0 && fake_direct.a[0] == g_bias + G0 + 0x100000 && fake_direct.a[1] == 0x22 && fake_direct.a[4] == 0x55 &&
        fake_direct.a[6] == 0x77 && fake_direct.a[7] == G0 + 0x8000);
  CHECK(rax == G0 + 0x100040);
}

static void test_all_routes_reach_kyty(bool host_pointers) {
  CHECK(nps5_kyty_active() && t_nhooks == 1 && t_nresolvers == 1 && t_naccess == 1 && nps5_kyty_watch_ready());
  CHECK(g_stub && g_stub->host && g_stub->calls[0].kind == STUB_INIT);
  CHECK(count_stub(STUB_NID_ENTRY) == 84 && count_stub(STUB_START) == 1 && g_stub->in_main_thread == 1);
  const KytyGpuHostInfo *info = g_stub->host->host_info(g_stub->host->user);
  CHECK(info->struct_size == sizeof *info && info->guest_bias == g_bias && info->host_page_size == 16384 && info->tracker_page_size == 4096 &&
        (info->flags & KYTY_GPU_HOST_HOSTED_MEMORY) && (info->flags & KYTY_GPU_HOST_MAIN_QUEUE) && info->guest_hi == G0 + ARENA);
  CHECK(g_stub->host->struct_size == sizeof(KytyGpuHostCallbacks) && g_stub->host->abi_version == KYTY_GPU_ABI_VERSION && g_stub->host->donate_guest_range == NULL);
  const uint64_t direct0 = fake_direct.calls, gpu0 = fake_gpu->calls;
  for (unsigned i = 0; i < nps5_kyty_route_count; i++) {
    const Nps5KytyRoute *r = &nps5_kyty_routes[i];
    CHECK(*r->slot != anyps5_symbol(r));
    CHECK(host_pointers ? *r->slot == nps5_kyty_entry(i) : *r->slot == r->raw);
    const uint32_t before = count_stub(STUB_EXPORT);
    g_stub->next_result = 0;
    call(shim_of(r->name), REGS, NULL, 0);
    const KytyStubCall *c = last_stub(STUB_EXPORT);
    CHECK(count_stub(STUB_EXPORT) == before + 1 && c && strcmp(c->nid, r->nid) == 0);
  }
  CHECK(fake_direct.calls == direct0 && fake_gpu->calls == gpu0); // AnyPS5's graphics exports were never entered
  // An export that is not routed still goes to AnyPS5 (same library, same thunk file).
  call(shim_of("sceAgcAcbAcquireMemGetSize"), REGS, NULL, 0);
  CHECK(fake_direct.calls == direct0 + 1 && strcmp(fake_direct.name, "sceAgcAcbAcquireMemGetSize") == 0);
  call(shim_of("scePadInit_nid_postfix"), REGS, NULL, 0);
  CHECK(fake_gpu->calls == gpu0 + 1 && strcmp(fake_gpu->name, "scePadInit_nid_postfix") == 0);
}

static void test_marshalling(bool host_pointers) {
  const uint64_t ptr = host_pointers ? g_bias + G0 + 0x100000 : G0 + 0x100000;
  // Twelve arguments, six on the guest's stack, narrow ones packed on the arm64 stack by the callee's prototype.
  const uint64_t regs[6] = {G0 + 0x100000, 0x101, 0x202, 0x303, 0x123456789abcull, 0x505};
  const uint64_t stack[6] = {0x606, 0xfedcba987654ull, 0x80000001ull, 0x1, 0x0, 0x7};
  uint64_t rax = call(shim_of("sceAgcDcbDmaData"), regs, stack, 6);
  const KytyStubCall *c = last_stub(STUB_EXPORT);
  CHECK(strcmp(c->nid, "WmAc2MEj6Io") == 0 && c->a[0] == ptr && c->a[1] == 0x01 && c->a[2] == 0x02 && c->a[3] == 0x03 && c->a[4] == 0x123456789abcull &&
        c->a[5] == 0x05 && c->a[6] == 0x06 && c->a[7] == 0xfedcba987654ull && c->a[8] == 0x80000001ull && c->a[9] == 1 && c->a[10] == 0 && c->a[11] == 7);
  CHECK(rax == G0 + 0x100040); // the cursor comes back as a guest address in both modes
  const uint64_t nullregs[6] = {0, 1, 2, 3, 4, 5};
  rax = call(shim_of("sceAgcDcbDmaData"), nullregs, stack, 6);
  CHECK(rax == 0 && last_stub(STUB_EXPORT)->a[0] == 0);
  // A pointer past the guest window: guest mode hands the value on untouched (the library sees what the game
  // passed); host mode gets NULL, as AnyPS5 does.
  const uint64_t wild[6] = {G0 + ARENA + 0x1000, 0, 0, 0, 0, 0};
  call(shim_of("sceAgcDcbDmaData"), wild, stack, 6);
  CHECK(last_stub(STUB_EXPORT)->a[0] == (host_pointers ? 0 : G0 + ARENA + 0x1000));
  // A float argument and a negative int result.
  g_ctx.xmm[0].f32[0] = 2.2f;
  rax = call(shim_of("sceVideoOutColorSettingsSetGamma_"), REGS, NULL, 0);
  c = last_stub(STUB_EXPORT);
  CHECK(strcmp(c->nid, "DYhhWbJSeRg") == 0 && c->f[0] == (double)2.2f && c->a[0] == ptr && rax == (uint64_t)(int64_t)-7);
  // An unsigned 32-bit result.
  const uint64_t r16[6] = {16, 0, 0, 0, 0, 0};
  CHECK(call(shim_of("sceAgcCbNopGetSize"), r16, NULL, 0) == 64);
  // A guest-address parameter (udata of AddEqEvent) is never translated; generic entries see the raw registers.
  g_stub->next_result = 0;
  const uint64_t ev[6] = {0x1234, 5, G0 + 0x7000, 0, 0, 0};
  call(shim_of("sceAgcDriverAddEqEvent"), ev, NULL, 0);
  c = last_stub(STUB_EXPORT);
  CHECK(strcmp(c->nid, "w2rJhmD+dsE") == 0 && c->a[0] == 0x1234 && c->a[1] == 5 && c->a[2] == G0 + 0x7000);
}

static void test_memory(void) {
  const KytyGpuHostCallbacks *h = g_stub->host;
  const uint64_t base = G0 + (16 << 20), size = 1 << 20;
  uint32_t reg0 = count_stub(STUB_REGISTER);
  // Not GPU-visible: stacks, the system zone, the AGC driver's own ranges, inaccessible mappings.
  t_hooks.on_map(NULL, base, size, 3, NPS5_EXT_THREAD_STACK, "stack");
  t_hooks.on_map(NULL, 0x10000000, size, 3, NPS5_EXT_MAPPED, "low");
  t_hooks.on_map(NULL, base, size, 3, NPS5_EXT_MAPPED, "agc_scratch");
  t_hooks.on_map(NULL, base, size, 0, NPS5_EXT_DIRECT, "none");
  CHECK(count_stub(STUB_REGISTER) == reg0 && h->query_memory(h->user, base, NULL, NULL, NULL, NULL) < 0);
  // A direct mapping.
  t_hooks.on_map(NULL, base, size, 0x33, NPS5_EXT_DIRECT, "gpu heap");
  const KytyStubCall *c = last_stub(STUB_REGISTER);
  CHECK(count_stub(STUB_REGISTER) == reg0 + 1 && c->a[0] == base && c->a[1] == size);
  CHECK(c->a[2] == (KYTY_GPU_MEMORY_DIRECT | KYTY_GPU_MEMORY_GPU_READ | KYTY_GPU_MEMORY_GPU_WRITE | KYTY_GPU_MEMORY_CPU_READ | KYTY_GPU_MEMORY_CPU_WRITE));
  uint8_t *alias = (uint8_t *)(uintptr_t)c->a[3];
  volatile uint8_t *guest = (volatile uint8_t *)(uintptr_t)(g_bias + base);
  CHECK(alias != NULL && alias != (uint8_t *)guest);
  guest[0x5008] = 0x11;
  alias[0x9000] = 0x22;
  CHECK(alias[0x5008] == 0x11 && guest[0x9000] == 0x22); // one set of pages, two views
  uint64_t qb = 0, qs = 0;
  uint32_t qf = 0;
  const void *qa = NULL;
  CHECK(h->query_memory(h->user, base + 0x8123, &qb, &qs, &qf, &qa) == 0 && qb == base && qs == size && qa == alias && (qf & KYTY_GPU_MEMORY_DIRECT));
  CHECK(h->query_memory(h->user, base + size, &qb, &qs, &qf, &qa) < 0);
  // A replayed map of the same range is not reported twice.
  t_hooks.on_map(NULL, base, size, 0x33, NPS5_EXT_DIRECT, "gpu heap");
  CHECK(count_stub(STUB_REGISTER) == reg0 + 1);

  // The library arms a write watch on one 4 KiB tracker page: the 16 KiB host page of the guest view is read-only,
  // the alias stays writable, a store through the guest view reaches handle_fault and then completes.
  CHECK(h->protect_guest(h->user, base + 0x5000, 0x1000, KYTY_GPU_WATCH_WRITE) == 0);
  CHECK(actual_prot(base + 0x4000) == PROT_READ && actual_prot(base) == 3 && actual_prot(base + 0x8000) == 3);
  uint32_t f0 = count_stub(STUB_FAULT);
  alias[0x5010] = 0x33; // the GPU side writes behind the watch
  CHECK(count_stub(STUB_FAULT) == f0 && guest[0x5010] == 0x33);
  guest[0x4004] = 0x44; // the CPU writes a neighbour tracker page of the same host page
  c = last_stub(STUB_FAULT);
  CHECK(count_stub(STUB_FAULT) == f0 + 1 && c->a[0] == base + 0x5000 && c->a[1] == 1 && guest[0x4004] == 0x44 && actual_prot(base + 0x4000) == 3);
  // Read trap.
  CHECK(h->protect_guest(h->user, base + 0x10000, 0x2000, KYTY_GPU_WATCH_ACCESS) == 0 && actual_prot(base + 0x10000) == 0);
  f0 = count_stub(STUB_FAULT);
  // A load from an UNWATCHED tracker page of that host page: carried out on the alias, the library is not told and
  // its pages stay armed (false sharing on the 16 KiB page).
  alias[0x13000] = 0x5e;
  CHECK(guest[0x13000] == 0x5e);
  guest[0x12008] = 0x6f; // and a store
  CHECK(count_stub(STUB_FAULT) == f0 && actual_prot(base + 0x10000) == 0 && alias[0x12008] == 0x6f);
  // A load from a watched page itself: reported, that page opens, its watched neighbour stays armed.
  CHECK(guest[0x10004] == 0);
  CHECK(count_stub(STUB_FAULT) == f0 + 1 && last_stub(STUB_FAULT)->a[0] == base + 0x10000 && last_stub(STUB_FAULT)->a[1] == 0 && actual_prot(base + 0x10000) == 0);
  CHECK(last_stub(STUB_FAULT)->a[2] == base + 0x10004); // the faulting byte, not just its page
  // The library serves a load itself (ABI 1.7: the page's published copy instead of a GPU drain): the page stays armed.
  ((KytyStubState *)g_stub)->serve_loads = 1;
  const uint32_t sv0 = count_stub(STUB_SERVED);
  CHECK(*(volatile uint64_t *)(guest + 0x11008) == 0xA7A6A5A4A3A2A1A0ull);
  CHECK(*(volatile uint32_t *)(guest + 0x11010) == 0xA3A2A1A0u && *(volatile uint8_t *)(guest + 0x11fff) == 0xA0);
  CHECK(count_stub(STUB_SERVED) == sv0 + 3 && last_stub(STUB_SERVED)->a[0] == base + 0x11fff && last_stub(STUB_SERVED)->a[1] == 1);
  CHECK(nps5_kyty_watch_get(base + 0x11000) == NPS5_KW_ACCESS && actual_prot(base + 0x10000) == 0);
  ((KytyStubState *)g_stub)->serve_loads = 0;
  guest[0x11000] = 1; // a store to it: reported as a store, opened
  CHECK(last_stub(STUB_FAULT)->a[0] == base + 0x11000 && last_stub(STUB_FAULT)->a[1] == 1 && actual_prot(base + 0x10000) == 3);
  Nps5KytyWatchStats wst;
  nps5_kyty_watch_stats(&wst);
  CHECK(wst.emulated >= 2 && wst.emu_declined == 0 && wst.loads_served == 3);
  CHECK(h->protect_guest(h->user, base + 0x800, 0x1000, KYTY_GPU_WATCH_WRITE) < 0); // not tracker-page aligned
  // System-call buffers and the runtime's own protection changes go through the host access hooks.
  CHECK(h->protect_guest(h->user, base + 0x20000, 0x1000, KYTY_GPU_WATCH_WRITE) == 0);
  f0 = count_stub(STUB_FAULT);
  t_access.read_prepare((void *)(guest + 0x20000), 64);
  CHECK(count_stub(STUB_FAULT) == f0);
  t_access.write_begin((void *)(guest + 0x21000), 64);
  t_access.write_end((void *)(guest + 0x21000), 64);
  CHECK(count_stub(STUB_FAULT) == f0 + 1 && actual_prot(base + 0x20000) == 3);
  CHECK(h->protect_guest(h->user, base + 0x24000, 0x1000, KYTY_GPU_WATCH_WRITE) == 0);
  CHECK(t_access.protect((void *)(guest + 0x24000), 0x4000, PROT_READ | PROT_WRITE) == 0 && actual_prot(base + 0x24000) == PROT_READ); // the watch stays
  CHECK(t_access.protect((void *)(guest + 0x28000), 0x4000, PROT_READ) == 0 && actual_prot(base + 0x28000) == PROT_READ);

  // The guest changes the protection of a part: the library is told, the alias stays.
  uint32_t p0 = count_stub(STUB_PROTECT);
  t_hooks.on_protect(NULL, base + 0x40000, 0x8000, 1, NPS5_EXT_DIRECT, "gpu heap");
  c = last_stub(STUB_PROTECT);
  CHECK(count_stub(STUB_PROTECT) == p0 + 1 && c->a[0] == base + 0x40000 && c->a[1] == 0x8000 && !(c->a[2] & KYTY_GPU_MEMORY_CPU_WRITE) && (c->a[2] & KYTY_GPU_MEMORY_CPU_READ));
  CHECK(h->query_memory(h->user, base, NULL, NULL, NULL, &qa) == 0 && qa == alias);

  // Unmapping the middle: unregister first (watches inside are dropped), the outer parts come back with new aliases.
  uint32_t u0 = count_stub(STUB_UNREGISTER);
  reg0 = count_stub(STUB_REGISTER);
  CHECK(nps5_kyty_watch_get(base + 0x24000) == NPS5_KW_WRITE);
  t_hooks.on_unmap(NULL, base + 0x20000, 0x10000);
  c = last_stub(STUB_UNREGISTER);
  CHECK(count_stub(STUB_UNREGISTER) == u0 + 1 && c->a[0] == base + 0x20000 && c->a[1] == 0x10000);
  CHECK(nps5_kyty_watch_get(base + 0x24000) == NPS5_KW_NONE);
  CHECK(count_stub(STUB_REGISTER) == reg0 + 2);
  CHECK(h->query_memory(h->user, base + 0x20000, NULL, NULL, NULL, NULL) < 0);
  CHECK(h->query_memory(h->user, base + 0x1000, &qb, &qs, &qf, &qa) == 0 && qb == base && qs == 0x20000 && qa != NULL);
  CHECK(((const uint8_t *)qa)[0x5008] == 0x11);
  CHECK(h->query_memory(h->user, base + 0x30000, &qb, &qs, &qf, &qa) == 0 && qb == base + 0x30000 && qs == size - 0x30000 && qa != NULL);
  // Protection to none: no longer GPU-visible.
  u0 = count_stub(STUB_UNREGISTER);
  t_hooks.on_protect(NULL, base, 0x20000, 0, NPS5_EXT_DIRECT, "gpu heap");
  CHECK(count_stub(STUB_UNREGISTER) == u0 + 1 && h->query_memory(h->user, base, NULL, NULL, NULL, NULL) < 0);
  // And back: registered afresh.
  reg0 = count_stub(STUB_REGISTER);
  t_hooks.on_protect(NULL, base, 0x20000, 3, NPS5_EXT_DIRECT, "gpu heap");
  CHECK(count_stub(STUB_REGISTER) == reg0 + 1 && h->query_memory(h->user, base, NULL, NULL, NULL, NULL) == 0);
  // Unmapping something never registered tells the library nothing.
  u0 = count_stub(STUB_UNREGISTER);
  t_hooks.on_unmap(NULL, G0 + (40 << 20), 0x10000);
  CHECK(count_stub(STUB_UNREGISTER) == u0);
  t_hooks.on_unmap(NULL, base, size);
  CHECK(h->query_memory(h->user, base + 0x30000, NULL, NULL, NULL, NULL) < 0);
  // Flexible and plain mmap memory.
  t_hooks.on_map(NULL, base, 0x8000, 3, NPS5_EXT_FLEXIBLE, "flex");
  CHECK((last_stub(STUB_REGISTER)->a[2] & KYTY_GPU_MEMORY_FLEXIBLE) && (last_stub(STUB_REGISTER)->a[2] & KYTY_GPU_MEMORY_GPU_WRITE));
  t_hooks.on_map(NULL, base + 0x8000, 0x8000, 1, NPS5_EXT_MAPPED, "ro");
  CHECK(last_stub(STUB_REGISTER)->a[2] == (KYTY_GPU_MEMORY_GPU_READ | KYTY_GPU_MEMORY_CPU_READ));
  t_hooks.on_unmap(NULL, base, 0x10000);
}

static void main_fn(void *arg) { *(int *)arg = pthread_main_np() ? 7 : -1; }
static void *other_thread(void *arg) {
  const KytyGpuHostCallbacks *h = g_stub->host;
  *(int *)arg = h->run_on_main_thread(h->user, main_fn, arg, 0); // async: runs when the main queue is served
  return NULL;
}
static void test_callbacks(void) {
  const KytyGpuHostCallbacks *h = g_stub->host;
  CHECK(strcmp(h->app0_path(h->user), "/fake/app0") == 0);
  // Events and exit go to the AnyPS5 side helpers of libnps5_gpu.
  CHECK(h->add_gpu_event(h->user, 0x77, KYTY_GPU_EVENT_EOP_INTERRUPT, 3, (void *)0x1234) == 0);
  CHECK(fake_host->adds == 1 && fake_host->eq == 0x77 && fake_host->kind == KYTY_GPU_EVENT_EOP_INTERRUPT && fake_host->ident == 3 && fake_host->udata == (void *)0x1234);
  h->deliver_event(h->user, KYTY_GPU_EVENT_FLIP, 1, 42);
  CHECK(fake_host->delivers == 1 && fake_host->kind == KYTY_GPU_EVENT_FLIP && fake_host->data == 42);
  CHECK(h->delete_gpu_event(h->user, 0x77, KYTY_GPU_EVENT_EOP_INTERRUPT, 3) == 0 && fake_host->deletes == 1);
  KytyGpuPadState pad = {.buttons = 0x4000, .connected = 1};
  h->publish_pad(h->user, 0, &pad);
  CHECK(fake_host->pads == 1);
  h->request_exit(h->user, 5);
  CHECK(fake_host->exits == 1 && fake_host->code == 5);
  CHECK(fake_host->script == 1 && (h->host_info(h->user)->flags & KYTY_GPU_HOST_SCRIPTED_PAD));
  // Guest callbacks: only on a guest thread inside a routed call.
  const uint64_t args[6] = {1, 2, 3, 4, 5, 6};
  CHECK(h->call_guest(h->user, 0x400000, args) == ~0ull);
  t_ctx = &g_ctx;
  t_in_call = 1;
  CHECK(h->call_guest(h->user, 0x400000, args) == 0x400000 + 21);
  t_in_call = 0;
  // Main thread: inline when already there, queued from another thread.
  int v = 0;
  CHECK(h->run_on_main_thread(h->user, main_fn, &v, 1) == 0 && v == 7);
  CHECK(h->run_on_main_thread(h->user, NULL, NULL, 1) < 0);
  int rc = -5;
  pthread_t th;
  CHECK(pthread_create(&th, NULL, other_thread, &rc) == 0);
  pthread_join(th, NULL); // rc now holds the return of run_on_main_thread (0); the queued call overwrites it with 7 on the main queue
  CHECK(rc == 0);
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "default";
  // The arena above the guest addresses, so host = guest + bias with a positive bias (as in a title: 2^40).
  mach_vm_address_t at = 0x20000000000ull;
  CHECK(mach_vm_map(mach_task_self(), &at, ARENA, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, 0, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) == KERN_SUCCESS);
  g_host = (uint8_t *)(uintptr_t)at;
  CHECK((uint64_t)at > G0);
  g_bias = (uint64_t)at - G0;
  g_nps5_arena = (Nps5Arena){.bias = (uint8_t *)(uintptr_t)g_bias, .host_lo = at, .host_len = ARENA, .guest_org = 0, .guest_lo = 0x400000, .guest_hi = G0 + ARENA, .mode = NPS5_ADDR_BASE, .flat = false};
  g_ctx.mem_base = (uint8_t *)(uintptr_t)g_bias;
  g_ctx.mem_size = G0 + ARENA;
  t_ctx = &g_ctx;
  struct sigaction sa = {0};
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  g_gpu = dlopen(getenv("FAKE_GPU_LIB"), RTLD_NOW | RTLD_LOCAL);
  CHECK(g_gpu != NULL);
  CHECK(nps5_anyps5_bind_exports(g_gpu) == 0);
  fake_gpu = dlsym(g_gpu, "fake_gpu_state");
  fake_host = dlsym(g_gpu, "fake_host_state");
  CHECK(fake_gpu && fake_host);

  if (strcmp(mode, "default") == 0) {
    test_default();
    // Nothing of the library was loaded.
    CHECK(getenv("NPS5_KYTY_GPU_LIB") == NULL || dlopen(getenv("NPS5_KYTY_GPU_LIB"), RTLD_NOW | RTLD_NOLOAD) == NULL);
  } else {
    CHECK(nps5_kyty_startup(&g_ctx, g_gpu));
    void *lib = dlopen(getenv("NPS5_KYTY_GPU_LIB"), RTLD_NOW | RTLD_NOLOAD);
    CHECK(lib != NULL);
    g_stub = ((KytyStubState * (*)(void)) dlsym(lib, "KytyStubGetState"))();
    if (strcmp(mode, "kyty") == 0 || strcmp(mode, "kyty-host") == 0) {
      const bool hostp = strcmp(mode, "kyty-host") == 0;
      test_all_routes_reach_kyty(hostp);
      test_marshalling(hostp);
      test_memory();
      test_callbacks();
    } else if (strcmp(mode, "missing") == 0) {
      const uint64_t r16[6] = {16, 0, 0, 0, 0, 0};
      CHECK(call(shim_of("sceAgcCbNopGetSize"), r16, NULL, 0) == 64); // the others work
      fprintf(stderr, "calling the missing export\n");
      call(shim_of("sceAgcDcbDrawIndexAuto"), REGS, NULL, 0); // must abort
      fprintf(stderr, "FAIL: the missing export returned\n");
      return 1;
    } else if (strcmp(mode, "noentry") == 0) {
      const uint64_t r16[6] = {16, 0, 0, 0, 0, 0};
      CHECK(count_stub(STUB_NID_ENTRY) == 0);
      CHECK(call(shim_of("sceAgcCbNopGetSize"), r16, NULL, 0) == 64); // found by dlsym of the NID
    } else {
      fprintf(stderr, "unknown mode %s\n", mode);
      return 2;
    }
  }
  printf("ROUTE OK %s %ld\n", mode, g_checks);
  fflush(stdout);
  _exit(0); // skip atexit (the stub's shutdown is covered by its own record in kyty mode below)
}
