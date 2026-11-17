// Host harness for the guest VM layer: a real runtime (recomp_runtime.c, the arena, the VM extent
// manager, the generated libkernel thunks) with a fake guest register file. Guest "calls" are made
// by filling ctx registers exactly as System V x86-64 guest code would and invoking the real
// shim_* entry (generated thunk or hand-written shim), so the pointer translation and the
// stack-argument path are exercised as in a translated title.
#ifndef VMH_H
#define VMH_H
#include "mt.h"
#include "nps5_mem.h"
#include "nps5_thunk.h"
#include "recomp_runtime.h"
#include "nps5_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

void shim_mmap(GuestContext *);
void shim_munmap(GuestContext *);
void shim_sceKernelMmap(GuestContext *);
void shim_sceKernelMapFlexibleMemory(GuestContext *);
void shim_sceKernelMapNamedFlexibleMemory(GuestContext *);
void shim_sceKernelReserveVirtualRange(GuestContext *);
void shim_sceKernelMapDirectMemory(GuestContext *);
void shim_sceKernelMprotect(GuestContext *);
void shim_write(GuestContext *);
void shim_writev(GuestContext *);
void shim_sceKernelAllocateDirectMemory(GuestContext *);
void shim_sceKernelReleaseDirectMemory(GuestContext *);
void shim_sceKernelAvailableDirectMemorySize(GuestContext *);
void shim_sceKernelMtypeprotect(GuestContext *);
void shim_sceKernelVirtualQuery(GuestContext *);

#if NPS5_ADDR_MODE_IDENTITY
#define VMH_MODE "identity"
#define ORG NPS5_IDENTITY_LO
const Nps5MemConfig nps5_mem_config = {
    .abi = NPS5_MEM_ABI, .addr_mode = NPS5_ADDR_IDENTITY, .host_base = 0,
    .guest_lo = 0x7000400000ULL, .guest_hi = 0xFC00000000ULL, .image_end = 0x7000400000ULL,
    .dyn_lo = 0x7200000000ULL, .pool_lo = 0x8000000000ULL, .stack_top = 0x77EEFFC000ULL, .stack_len = 0x4000000ULL,
};
#else
#define VMH_MODE "base"
#define ORG 0ULL
#endif
#define G(x) (ORG + (uint64_t)(x))
#define PAGE NPS5_PAGE
#define PROT_RW 3
#define F_ANON 0x1000
#define F_PRIV 0x2
#define F_FIXED 0x10
#define F_EXCL 0x4000
#define F_NOOVR 0x80
#define MAPF (F_ANON | F_PRIV)
#define E_INVAL 0x80020016u
#define E_NOMEM 0x8002000cu

static GuestContext *ctx;
static GuestContext *proc_ctx;
static uint64_t sys_end;       // first address after the system block
static uint64_t stk_lo, stk_top;
static uint64_t arg_slot;      // scratch guest memory used for in/out slots

static sigjmp_buf vmh_jb;
static volatile uint64_t vmh_fault;
static void vmh_on_fault(int s, siginfo_t *si, void *u) {
  (void)s; (void)u;
  vmh_fault = (uint64_t)(uintptr_t)si->si_addr;
  siglongjmp(vmh_jb, 1);
}
static void vmh_install_probe(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER; sa.sa_sigaction = vmh_on_fault;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}
// raw guest->host (host_lo + offset), also for guest 0: what translated code does with [x28 + 0]
static uint8_t *P(uint64_t g) { return (uint8_t *)(uintptr_t)(g_nps5_arena.host_lo + (g - g_nps5_arena.guest_org)); }
static int rd_faults(uint64_t g) {
  if (sigsetjmp(vmh_jb, 1)) return 1;
  volatile uint8_t v = *(volatile uint8_t *)P(g); (void)v; return 0;
}
static int wr_faults(uint64_t g) {
  if (sigsetjmp(vmh_jb, 1)) return 1;
  *(volatile uint8_t *)P(g) = *(volatile uint8_t *)P(g); return 0;
}
static int w1_faults(uint64_t g, uint8_t v) {
  if (sigsetjmp(vmh_jb, 1)) return 1;
  *(volatile uint8_t *)P(g) = v; return 0;
}
static int ok_rw(uint64_t g) { return !rd_faults(g) && !w1_faults(g, *P(g)); }

static void vmh_init(void) {
  mt_init();
  alarm(240);
  for (int i = 0; i < NPS5_S_COUNT; i++) g_nps5_level[i] = NPS5_L_ERROR + 1;  // silence the debug chatter: it is async and slow
  ctx = recomp_init_runtime(0, NULL, 0, "memtests");
  if (!ctx) { fprintf(stderr, "recomp_init_runtime failed\n"); exit(99); }
  proc_ctx = ctx;
  vmh_install_probe();
  sys_end = nps5_mem_config.image_end + 0x30000;
  stk_top = nps5_mem_config.stack_top; stk_lo = stk_top - nps5_mem_config.stack_len;
  arg_slot = stk_top - 0x8000;        // inside the main stack, far from its end
  ctx->rsp = stk_top - 0x4000;        // [rsp] = return address slot, [rsp+8..] = stack arguments
}
static void setrsp(void) { ctx->rsp = stk_top - 0x4000; *(uint64_t *)P(ctx->rsp) = 0; }
static int guest_errno(void) { return *(int *)P(ctx->fs_base + 0x100); }
static void clear_errno(void) { *(int *)P(ctx->fs_base + 0x100) = 0; }

// ---- guest-ABI wrappers -------------------------------------------------------------------
// sceKernelMmap(addr,len,prot,flags,fd,off,&res): returns the SCE code; *out gets res.
static uint32_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, uint64_t *out) {
  setrsp();
  *(uint64_t *)P(arg_slot) = ~0ULL;
  ctx->rdi = addr; ctx->rsi = len; ctx->rdx = (uint64_t)prot; ctx->rcx = (uint64_t)flags;
  ctx->r8 = (uint64_t)-1; ctx->r9 = 0;
  *(uint64_t *)P(ctx->rsp + 8) = arg_slot;
  shim_sceKernelMmap(ctx);
  if (out) *out = *(uint64_t *)P(arg_slot);
  return (uint32_t)ctx->rax;
}
// POSIX mmap: returns guest address or ~0.
static uint64_t px_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
  setrsp(); clear_errno();
  ctx->rdi = addr; ctx->rsi = len; ctx->rdx = (uint64_t)prot; ctx->rcx = (uint64_t)flags;
  ctx->r8 = (uint64_t)-1; ctx->r9 = 0;
  shim_mmap(ctx);
  return ctx->rax;
}
static int64_t sys_munmap(uint64_t addr, uint64_t len) {
  setrsp(); clear_errno();
  ctx->rdi = addr; ctx->rsi = len;
  shim_munmap(ctx);
  return (int64_t)ctx->rax;
}
static int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
  setrsp();
  ctx->rdi = addr; ctx->rsi = len; ctx->rdx = (uint64_t)prot;
  shim_sceKernelMprotect(ctx);
  return (int32_t)ctx->rax;
}
static uint32_t sys_mtypeprot(uint64_t addr, uint64_t len, int mtype, int prot) {
  setrsp();
  ctx->rdi = addr; ctx->rsi = len; ctx->rdx = (uint64_t)mtype; ctx->rcx = (uint64_t)prot;
  shim_sceKernelMtypeprotect(ctx);
  return (uint32_t)ctx->rax;
}
static uint32_t sys_flex(uint64_t *addr, uint64_t len, int prot, int flags) {
  setrsp();
  *(uint64_t *)P(arg_slot) = *addr;
  ctx->rdi = arg_slot; ctx->rsi = len; ctx->rdx = (uint64_t)prot; ctx->rcx = (uint64_t)flags;
  shim_sceKernelMapFlexibleMemory(ctx);
  *addr = *(uint64_t *)P(arg_slot);
  return (uint32_t)ctx->rax;
}
static uint32_t sys_reserve(uint64_t *addr, uint64_t len, int flags, uint64_t align) {
  setrsp();
  *(uint64_t *)P(arg_slot) = *addr;
  ctx->rdi = arg_slot; ctx->rsi = len; ctx->rdx = (uint64_t)flags; ctx->rcx = align;
  shim_sceKernelReserveVirtualRange(ctx);
  *addr = *(uint64_t *)P(arg_slot);
  return (uint32_t)ctx->rax;
}
static uint32_t sys_dm_alloc(int64_t start, int64_t end, uint64_t len, uint64_t align, int type, int64_t *phys) {
  setrsp();
  *(int64_t *)P(arg_slot + 64) = -1;
  ctx->rdi = (uint64_t)start; ctx->rsi = (uint64_t)end; ctx->rdx = len; ctx->rcx = align; ctx->r8 = (uint64_t)type; ctx->r9 = arg_slot + 64;
  shim_sceKernelAllocateDirectMemory(ctx);
  *phys = *(int64_t *)P(arg_slot + 64);
  return (uint32_t)ctx->rax;
}
static uint32_t sys_dm_map(uint64_t *addr, uint64_t len, int prot, int flags, int64_t phys, uint64_t align) {
  setrsp();
  *(uint64_t *)P(arg_slot) = *addr;
  ctx->rdi = arg_slot; ctx->rsi = len; ctx->rdx = (uint64_t)prot; ctx->rcx = (uint64_t)flags; ctx->r8 = (uint64_t)phys; ctx->r9 = align;
  shim_sceKernelMapDirectMemory(ctx);
  *addr = *(uint64_t *)P(arg_slot);
  return (uint32_t)ctx->rax;
}
static uint32_t sys_dm_release(int64_t phys, uint64_t len) {
  setrsp();
  ctx->rdi = (uint64_t)phys; ctx->rsi = len;
  shim_sceKernelReleaseDirectMemory(ctx);
  return (uint32_t)ctx->rax;
}

// A signature of the extent list: count, sum of mapped bytes, folded boundaries. Equal before and
// after a failed call = the call changed nothing.
typedef struct { uint64_t n, mapped, h; } Sig;
static Sig vm_sig(void) {
  Sig s = {0, 0, 1469598103934665603ULL};
  pthread_mutex_lock(&proc_ctx->vm_mutex);
  for (GuestVMExtent *e = proc_ctx->vm_extents; e; e = e->next) {
    s.n++;
    if (!e->is_free) s.mapped += e->size;
    s.h = (s.h ^ e->addr) * 1099511628211ULL; s.h = (s.h ^ e->size) * 1099511628211ULL; s.h = (s.h ^ (uint64_t)e->is_free) * 1099511628211ULL;
  }
  pthread_mutex_unlock(&proc_ctx->vm_mutex);
  return s;
}
static int sig_eq(Sig a, Sig b) { return a.n == b.n && a.mapped == b.mapped && a.h == b.h; }

// Extent list invariants: sorted, contiguous, no overlap, no zero-size extent, names terminated.
static int vm_check_list(char *why, size_t cap) {
  pthread_mutex_lock(&proc_ctx->vm_mutex);
  int ok = 1;
  GuestVMExtent *prev = NULL;
  for (GuestVMExtent *e = proc_ctx->vm_extents; e && ok; prev = e, e = e->next) {
    if (e->size == 0) { snprintf(why, cap, "zero-size extent at 0x%llx", (unsigned long long)e->addr); ok = 0; }
    else if ((e->addr | e->size) & NPS5_PAGE_MASK) { snprintf(why, cap, "unaligned extent 0x%llx+0x%zx", (unsigned long long)e->addr, e->size); ok = 0; }
    else if (e->prev != prev) { snprintf(why, cap, "broken prev link at 0x%llx", (unsigned long long)e->addr); ok = 0; }
    else if (prev && prev->addr + prev->size > e->addr) { snprintf(why, cap, "overlap 0x%llx+0x%zx vs 0x%llx", (unsigned long long)prev->addr, prev->size, (unsigned long long)e->addr); ok = 0; }
    else if (prev && prev->addr + prev->size < e->addr) { snprintf(why, cap, "gap before 0x%llx", (unsigned long long)e->addr); ok = 0; }
    else if (prev && prev->is_free && e->is_free) { snprintf(why, cap, "uncoalesced free extents at 0x%llx", (unsigned long long)e->addr); ok = 0; }
  }
  pthread_mutex_unlock(&proc_ctx->vm_mutex);
  return ok;
}
static int is_mapped_extent(uint64_t g) {
  pthread_mutex_lock(&proc_ctx->vm_mutex);
  int r = 0;
  for (GuestVMExtent *e = proc_ctx->vm_extents; e; e = e->next)
    if (g >= e->addr && g < e->addr + e->size) { r = !e->is_free; break; }
  pthread_mutex_unlock(&proc_ctx->vm_mutex);
  return r;
}
static int extent_prot(uint64_t g) {
  pthread_mutex_lock(&proc_ctx->vm_mutex);
  int r = -1;
  for (GuestVMExtent *e = proc_ctx->vm_extents; e; e = e->next)
    if (g >= e->addr && g < e->addr + e->size) { r = e->is_free ? -2 : e->prot; break; }
  pthread_mutex_unlock(&proc_ctx->vm_mutex);
  return r;
}
static void fill(uint64_t g, size_t n, uint8_t v) { memset(P(g), v, n); }
static int is_fill(uint64_t g, size_t n, uint8_t v) {  // 0 also when any probed byte is unreadable
  if (sigsetjmp(vmh_jb, 1)) return 0;
  for (size_t i = 0; i < n; i += 4093) if (*(volatile uint8_t *)P(g + i) != v) return 0;
  return *(volatile uint8_t *)P(g + n - 1) == v;
}
// Maps [g, g+len) fixed, fails the test case on error.
static int map_fixed(uint64_t g, uint64_t len) {
  uint64_t r = 0;
  uint32_t rc = sys_mmap(g, len, PROT_RW, MAPF | F_FIXED, &r);
  return rc == 0 && r == g;
}
// Runs fn on a helper thread with a watchdog; returns a wait(2)-style status: exit code << 8 when fn
// returned, SIGSEGV when it faulted (the probe handler catches it), SIGALRM when it did not finish
// within 8 s (a deadlock; the stuck thread is abandoned). A forked child cannot be used: the arena
// is VM_INHERIT_NONE (a guest never forks), so a child has no guest memory at all.
typedef struct { int (*fn)(void *); void *arg; volatile int code; volatile int state; } InChild;
static void *in_child_main(void *p) {
  InChild *c = (InChild *)p;
  if (sigsetjmp(vmh_jb, 1)) { c->state = 2; return NULL; }
  c->code = c->fn(c->arg);
  c->state = 1;
  return NULL;
}
static int in_child(int (*fn)(void *), void *arg) {
  static InChild c;
  c.fn = fn; c.arg = arg; c.code = -1; c.state = 0;
  pthread_t th;
  if (pthread_create(&th, NULL, in_child_main, &c) != 0) return 0xff00;
  for (int i = 0; i < 800 && !c.state; i++) usleep(10000);
  if (!c.state) { pthread_detach(th); return SIGALRM; }
  pthread_join(th, NULL);
  return c.state == 2 ? SIGSEGV : (c.code & 0xff) << 8;
}
#endif
