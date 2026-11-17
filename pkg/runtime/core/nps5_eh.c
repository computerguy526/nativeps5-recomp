// Guest non-local control transfers in the nested host-stack call model:
// setjmp/longjmp, the C++ unwinder's context install (landing pads), and abort diagnostics.
// Design, invariants and the migration path to the flat-stack model: docs/m1/setjmp-eh.md.
//
// Host frame layout (emitter, asm_emitter.go): every recompiled guest function pushes
//   [sp+0] x30 (host return address)   [sp+8] x26 (guest rsp on entry = address of the guest return address)
// and the shim bridge (_recomp_call_shim) pushes a 48-byte frame
//   [x29,x30][shim, ctx][guest FPCR]
// below the guest frame that called the shim. So from a shim the guest frames above it are
// contiguous 16-byte records with strictly increasing guest rsp, until a host boundary
// (a recomp_enter_guest frame, i.e. an HLE->guest callback) is reached.
//
// Flat call model (g_recomp_call_model; docs/m1/flatstack.md): no guest function has a host
// frame. The target of a non-local transfer is found from the guest state alone - the guest rip
// through the continuation table, the frames to drop through the guest rsp (recomp_shadow_trim) -
// and the host boundary rule is checked against the recomp_enter_guest record
// (GuestContext.host_boundary). The *_flat functions below are that model; the rest of the file
// is the nested one.
#include "recomp_runtime.h"
#include "nps5_diag.h"
#include "nps5_shadow.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- landing pad table
// {guest pc, host address} pairs emitted by the lifter; the table lives in nps5_shadow.c (it is
// the continuation table of the flat model's slow paths as well).
static uintptr_t pad_lookup(uint64_t guest) { return recomp_cont_table_lookup(guest); }

// ---------------------------------------------------------------- host frame access
typedef struct {
  uintptr_t bridge;      // _recomp_call_shim frame (sp after its prologue)
  uintptr_t bridge_lr;   // host return address into the guest function that called the shim
  uint64_t guest_fpcr;   // guest FPCR of that call
  uintptr_t first;       // host sp of the calling guest function's frame
} ShimSite;

// Must be inlined into the shim so that __builtin_frame_address(0) is the shim's own frame.
#define SHIM_SITE(site)                                                                  \
  ShimSite site;                                                                         \
  do {                                                                                   \
    uintptr_t *fp_ = (uintptr_t *)__builtin_frame_address(0);                            \
    (site).bridge = fp_[0];                                                              \
    (site).bridge_lr = ((uintptr_t *)(site).bridge)[1];                                  \
    (site).guest_fpcr = ((uint64_t *)(site).bridge)[4];                                  \
    (site).first = (site).bridge + 48;                                                   \
  } while (0)

static int eh_trace(void) {
  static int v = -1;
  if (v < 0) v = getenv("NPS5_EH_TRACE") != NULL;
  return v;
}

#define MAX_WALK (1u << 22)

// Walks the guest frames above `first`. Calls pred(frame_sp, lr, entry_rsp, arg) for each valid
// frame, stops when it returns nonzero (returned: that frame's sp). Returns 0 when the chain ends
// (host boundary or corrupt frame) first; *why then says which.
typedef int (*FramePred)(uintptr_t sp, uintptr_t lr, uint64_t entry_rsp, void *arg);
static uintptr_t walk_frames(uintptr_t first, FramePred pred, void *arg, const char **why) {
  uint64_t prev_e = 0;
  uintptr_t p = first;
  for (unsigned n = 0; n < MAX_WALK; n++, p += 16) {
    uintptr_t lr = ((uintptr_t *)p)[0];
    uint64_t e = ((uint64_t *)p)[1];
    if (!recomp_pc_in_guest_code(lr) || e <= prev_e) {
      if (why) *why = n == 0 ? "the calling frame is not a recompiled guest function" :
                               "the host stack above this point is not a chain of guest frames (an HLE callback or thread entry boundary)";
      return 0;
    }
    if (pred(p, lr, e, arg)) return p;
    prev_e = e;
  }
  if (why) *why = "frame chain too long";
  return 0;
}

// ---------------------------------------------------------------- diagnostics
// Names the guest functions on the stack, innermost first: the function that called the shim
// (bridge_lr, the host return address saved in the bridge frame just below `first`), then the
// function that called it (the lr of each recorded frame), and so on. `thrower` receives the
// function that called __cxa_throw/__cxa_rethrow, when the chain contains one.
static _Thread_local GuestContext *t_chain_ctx; // the context whose shim is reporting (flat model)

static void guest_chain_flat(GuestContext *ctx, char *out, size_t cap, char *thrower, size_t tcap) {
  uint64_t c[64];
  size_t cn = recomp_shadow_chain(ctx->shadow_sp, ctx->rsp, c, 64), n = 0;
  int found_throw = 0, depth = 0;
  for (size_t i = 0; i < cn; i++) {
    char tmp[128];
    uint64_t fn = 0;
    const char *nm = NULL;
    if (c[i] == RECOMP_CHAIN_BOUNDARY) {
      snprintf(tmp, sizeof tmp, "<host boundary>");
    } else if (!nps5_diag_host_pc_to_guest(c[i] - 4, NULL, &fn, &nm)) {
      continue;
    } else if (nm) {
      snprintf(tmp, sizeof tmp, "%s", nm);
    } else {
      snprintf(tmp, sizeof tmp, "fn_0x%llx", (unsigned long long)fn);
    }
    if (found_throw == 1 && thrower) { snprintf(thrower, tcap, "%s", tmp); found_throw = 2; }
    if (nm && (!strcmp(nm, "__cxa_throw") || !strcmp(nm, "__cxa_rethrow")) && !found_throw) found_throw = 1;
    if (depth < 24 && n + strlen(tmp) + 6 < cap) {
      n += (size_t)snprintf(out + n, cap - n, "%s%s", depth ? " <- " : "", tmp);
      depth++;
    }
  }
}

static void guest_chain(uintptr_t first, char *out, size_t cap, char *thrower, size_t tcap) {
  size_t n = 0;
  int found_throw = 0, depth = 0;
  out[0] = 0;
  if (thrower) thrower[0] = 0;
  if (recomp_call_model_flat()) {
    if (t_chain_ctx) guest_chain_flat(t_chain_ctx, out, cap, thrower, tcap);
    return;
  }
  uint64_t prev_e = 0;
  uintptr_t p = first, lr = ((uintptr_t *)(first - 48))[1];
  for (unsigned i = 0; i < 4096; i++, p += 16) {
    if (!recomp_pc_in_guest_code(lr)) break;
    uint64_t gpc = 0, fn = 0;
    const char *nm = NULL;
    // lr is the instruction after the bl: look the call site up.
    if (nps5_diag_host_pc_to_guest(lr - 4, &gpc, &fn, &nm)) {
      char tmp[128];
      if (nm) snprintf(tmp, sizeof tmp, "%s", nm); else snprintf(tmp, sizeof tmp, "fn_0x%llx", (unsigned long long)fn);
      if (found_throw == 1 && thrower) { snprintf(thrower, tcap, "%s", tmp); found_throw = 2; }
      if (nm && (!strcmp(nm, "__cxa_throw") || !strcmp(nm, "__cxa_rethrow")) && !found_throw) found_throw = 1;
      if (depth < 24 && n + strlen(tmp) + 6 < cap) {
        n += (size_t)snprintf(out + n, cap - n, "%s%s", depth ? " <- " : "", tmp);
        depth++;
      }
    }
    lr = ((uintptr_t *)p)[0];
    uint64_t e = ((uint64_t *)p)[1];
    if (e <= prev_e) break;
    prev_e = e;
  }
}

__attribute__((noreturn, format(printf, 3, 4))) static void eh_fatal(GuestContext *ctx, uintptr_t first, const char *fmt, ...) {
  char msg[300], chain[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  t_chain_ctx = ctx;
  guest_chain(first, chain, sizeof chain, NULL, 0);
  nps5_log(NPS5_S_RUNTIME, NPS5_L_ERROR, "FATAL non-local jump: %s; guest call chain: %s", msg, chain[0] ? chain : "(unavailable)");
  fprintf(stderr, "\nFATAL: %s\n", msg);
  nps5_diag_set_note("%.250s | guest chain: %.200s", msg, chain);
  (void)ctx;
  fflush(stderr);
  abort();
}

// abort(): name the guest function that threw an exception nobody caught. std::terminate ->
// abort leaves the thrower's frames intact (phase 1 found no handler, nothing was unwound),
// so the guest frame chain above this shim contains __cxa_throw and, right above it, the thrower.
void shim_guest_abort(GuestContext *ctx) {
  SHIM_SITE(site);
  char chain[400], thrower[160];
  t_chain_ctx = ctx;
  guest_chain(site.first, chain, sizeof chain, thrower, sizeof thrower);
  if (thrower[0]) {
    nps5_log(NPS5_S_RUNTIME, NPS5_L_ERROR, "abort() after an uncaught C++ exception thrown by guest function %s; guest call chain: %s", thrower, chain);
    nps5_diag_set_note("uncaught C++ exception thrown from guest function %.120s; guest call chain: %.250s", thrower, chain);
  } else {
    nps5_log(NPS5_S_RUNTIME, NPS5_L_ERROR, "guest abort(); guest call chain: %s", chain[0] ? chain : "(unavailable)");
    nps5_diag_set_note("guest abort() from %.120s; guest call chain: %.250s", chain[0] ? chain : "(no pc map)", chain);
  }
  (void)ctx;
  fflush(stderr);
  abort();
}

// ---------------------------------------------------------------- setjmp / longjmp
// Guest jmp_buf (FreeBSD x86-64 jmp_buf is 96 bytes; sigjmp_buf is larger). Layout we write:
//   +0 rip  +8 rbx  +16 rsp  +24 rbp  +32 r12  +40 r13  +48 r14  +56 r15
//   +64 x87 control word (u16)  +68 mxcsr (u32)  +72 generation (u64)  +80 magic (u64)
#define JB_GEN 72
#define JB_MAGIC 80
#define JB_MAGIC_VAL 0x4e5053354a4d5031ull /* "NPS5JMP1" */

typedef struct {
  uint64_t gen;        // 0 = empty
  uint64_t env;        // diagnostics only (a jmp_buf may legally be copied; the generation is the key)
  uintptr_t host_sp;   // host sp of the guest function that called setjmp (its [x30, rsp] slot)
  uintptr_t frame_lr, frame_e; // that slot's contents at setjmp time, to detect a reused slot
  uintptr_t host_lr;   // host address after the setjmp call in that function
  uint64_t fpcr;       // guest FPCR at the call
  uint64_t rip, rbx, rsp, rbp, r12, r13, r14, r15;
  uint32_t mxcsr;
  uint16_t fpu_cw;
  // Flat model: the execution context (thread or fiber) that called setjmp, its host boundary,
  // and the innermost shadow entry that was live when setjmp returned (address, contents and the
  // shadow stack's epoch: the address only means something while the epoch is unchanged).
  GuestContext *ctx;
  uint64_t boundary;
  uint64_t anchor, anchor_ra, anchor_rsp, epoch;
} JmpRec;

// Per-thread table, direct-mapped by generation. A generation is never reused (process-wide
// counter), so a longjmp through a stale or foreign buffer cannot select a wrong record.
#define JMP_SLOTS 1024
static _Thread_local JmpRec t_jmp[JMP_SLOTS];
static atomic_uint_fast64_t g_jmp_gen = 1;

void shim_guest_setjmp(GuestContext *ctx) {
  SHIM_SITE(site);
  const int flat = recomp_call_model_flat();
  if (!flat && !recomp_pc_in_guest_code(site.bridge_lr)) eh_fatal(ctx, site.first, "setjmp called from outside recompiled guest code");
  uint64_t env = ctx->rdi;
  if (env + 96 > ctx->mem_size) eh_fatal(ctx, site.first, "setjmp(env=0x%llx): jmp_buf outside guest memory", (unsigned long long)env);
  uint64_t ret_rip = *(uint64_t *)(ctx->mem_base + ctx->rsp);
  uint64_t gen = atomic_fetch_add(&g_jmp_gen, 1);
  JmpRec *r = &t_jmp[gen % JMP_SLOTS];
  r->gen = gen;
  r->env = env;
  if (flat) {
    // Entries left behind by activations the guest already unwound by itself would be popped
    // by the next mismatching RET; drop them now so that the anchor below names a live frame.
    recomp_shadow_trim(ctx, ctx->rsp);
    // The top shadow entry is the one of this very call when the guest called setjmp (through
    // a PLT stub or directly); the entry above it is then the innermost one that stays live.
    RecompShadowEntry *e = (RecompShadowEntry *)(uintptr_t)ctx->shadow_sp;
    if (ctx->shadow && e < ctx->shadow->bottom && e->guest_ra == ret_rip && e->guest_rsp == ctx->rsp) e++;
    r->ctx = ctx;
    r->boundary = ctx->host_boundary;
    r->anchor = (uint64_t)(uintptr_t)e;
    r->anchor_ra = e->guest_ra;
    r->anchor_rsp = e->guest_rsp;
    r->epoch = ctx->shadow ? atomic_load(&ctx->shadow->epoch) : 0;
    r->host_sp = 0;
  } else {
    r->host_sp = site.first;
    r->frame_lr = ((uintptr_t *)site.first)[0];
    r->frame_e = ((uintptr_t *)site.first)[1];
    r->host_lr = site.bridge_lr;
  }
  r->fpcr = site.guest_fpcr;
  r->rip = ret_rip;
  r->rsp = ctx->rsp + 8; // after the return address is popped (SHIM_RETURN)
  r->rbx = ctx->rbx; r->rbp = ctx->rbp;
  r->r12 = ctx->r12; r->r13 = ctx->r13; r->r14 = ctx->r14; r->r15 = ctx->r15;
  r->mxcsr = ctx->mxcsr;
  r->fpu_cw = ctx->fpu_cw;
  uint8_t *jb = ctx->mem_base + env;
  memcpy(jb + 0, &r->rip, 8); memcpy(jb + 8, &r->rbx, 8); memcpy(jb + 16, &r->rsp, 8); memcpy(jb + 24, &r->rbp, 8);
  memcpy(jb + 32, &r->r12, 8); memcpy(jb + 40, &r->r13, 8); memcpy(jb + 48, &r->r14, 8); memcpy(jb + 56, &r->r15, 8);
  memcpy(jb + 64, &r->fpu_cw, 2); memcpy(jb + 68, &r->mxcsr, 4);
  memcpy(jb + JB_GEN, &gen, 8);
  uint64_t magic = JB_MAGIC_VAL;
  memcpy(jb + JB_MAGIC, &magic, 8);
  ctx->rax = 0;
  SHIM_RETURN();
}

typedef struct { uintptr_t target; } FindSp;
static int pred_sp_eq(uintptr_t sp, uintptr_t lr, uint64_t e, void *arg) {
  (void)lr; (void)e;
  return sp == ((FindSp *)arg)->target;
}

// Flat model. The setjmp frame is live when (1) the same execution context is running inside
// the same host -> guest entry, (2) the guest stack has not been popped past the setjmp's stack
// pointer, and (3) the shadow entry that was the innermost live one at setjmp time is still
// there (checked only while the shadow stack has not been compacted or invalidated since: the
// cache may be discarded at any time, and then (1) and (2) are all that can be known - which is
// what the hardware would check, i.e. nothing more).
__attribute__((noreturn)) static void longjmp_flat(GuestContext *ctx, JmpRec *r, uint64_t env, uint32_t val) {
  if (r->ctx != ctx)
    eh_fatal(ctx, 0, "longjmp(env=0x%llx, val=%u): the setjmp (guest rip 0x%llx) ran on another thread or fiber",
             (unsigned long long)env, val, (unsigned long long)r->rip);
  if (r->boundary != ctx->host_boundary)
    eh_fatal(ctx, 0,
             "longjmp(env=0x%llx, val=%u) cannot reach its setjmp frame (guest rip 0x%llx): an HLE callback or thread entry boundary lies between them, or the setjmp's host entry already returned (SPEC 3.7)",
             (unsigned long long)env, val, (unsigned long long)r->rip);
  if (ctx->rsp > r->rsp)
    eh_fatal(ctx, 0, "longjmp(env=0x%llx, val=%u): target frame (setjmp at guest rip 0x%llx) already returned: the guest stack pointer of the setjmp (0x%llx) is below the current one (0x%llx)",
             (unsigned long long)env, val, (unsigned long long)r->rip, (unsigned long long)r->rsp, (unsigned long long)ctx->rsp);
  RecompShadow *s = ctx->shadow;
  if (s && atomic_load(&s->epoch) == r->epoch) {
    RecompShadowEntry *a = (RecompShadowEntry *)(uintptr_t)r->anchor;
    if (r->anchor < ctx->shadow_sp || a->guest_ra != r->anchor_ra || a->guest_rsp != r->anchor_rsp)
      eh_fatal(ctx, 0, "longjmp(env=0x%llx, val=%u): target frame (setjmp at guest rip 0x%llx) already returned: its shadow stack entry was popped%s",
               (unsigned long long)env, val, (unsigned long long)r->rip, r->anchor < ctx->shadow_sp ? "" : " and reused");
  }
  uintptr_t host = recomp_cont_resolve(ctx, r->rip, RECOMP_MISS_RESUME);
  if (!host)
    eh_fatal(ctx, 0, "longjmp(env=0x%llx, val=%u): the setjmp return address 0x%llx has no host continuation",
             (unsigned long long)env, val, (unsigned long long)r->rip);
  ctx->rax = val ? val : 1;
  ctx->rbx = r->rbx; ctx->rbp = r->rbp; ctx->rsp = r->rsp;
  ctx->r12 = r->r12; ctx->r13 = r->r13; ctx->r14 = r->r14; ctx->r15 = r->r15;
  ctx->mxcsr = r->mxcsr;
  ctx->fpu_cw = r->fpu_cw;
  recomp_shadow_trim(ctx, r->rsp); // the activations below the setjmp frame are gone
  recomp_resume_guest(ctx, 0, host, r->fpcr);
}

void shim_guest_longjmp(GuestContext *ctx) {
  SHIM_SITE(site);
  uint64_t env = ctx->rdi;
  uint32_t val = (uint32_t)ctx->rsi;
  if (env + 96 > ctx->mem_size) eh_fatal(ctx, site.first, "longjmp(env=0x%llx): jmp_buf outside guest memory", (unsigned long long)env);
  const uint8_t *jb = ctx->mem_base + env;
  uint64_t gen, magic;
  memcpy(&gen, jb + JB_GEN, 8);
  memcpy(&magic, jb + JB_MAGIC, 8);
  if (magic != JB_MAGIC_VAL)
    eh_fatal(ctx, site.first, "longjmp(env=0x%llx, val=%u): the jmp_buf was never initialised by setjmp", (unsigned long long)env, val);
  JmpRec *r = &t_jmp[gen % JMP_SLOTS];
  if (r->gen != gen)
    eh_fatal(ctx, site.first,
             "longjmp(env=0x%llx, val=%u): no live setjmp record for generation %llu on this thread (setjmp ran on another thread, or %u newer setjmps evicted it)",
             (unsigned long long)env, val, (unsigned long long)gen, JMP_SLOTS);
  if (recomp_call_model_flat()) longjmp_flat(ctx, r, env, val);
  if (r->host_sp < site.first)
    eh_fatal(ctx, site.first, "longjmp(env=0x%llx, val=%u): target frame (setjmp at guest rip 0x%llx) already returned: its host frame is below the caller's",
             (unsigned long long)env, val, (unsigned long long)r->rip);
  // The recorded slot must still hold the same activation, and a chain of guest frames must
  // lead to it (no HLE callback boundary in between).
  if (((uintptr_t *)r->host_sp)[0] != r->frame_lr || ((uintptr_t *)r->host_sp)[1] != r->frame_e)
    eh_fatal(ctx, site.first, "longjmp(env=0x%llx, val=%u): target frame (setjmp at guest rip 0x%llx) already returned: its host stack slot was reused",
             (unsigned long long)env, val, (unsigned long long)r->rip);
  FindSp f = {r->host_sp};
  const char *why = NULL;
  if (!walk_frames(site.first, pred_sp_eq, &f, &why))
    eh_fatal(ctx, site.first, "longjmp(env=0x%llx, val=%u) cannot reach its setjmp frame: %s", (unsigned long long)env, val, why ? why : "frame not found");
  if (ctx->rsp > r->rsp)
    eh_fatal(ctx, site.first, "longjmp(env=0x%llx, val=%u): the guest stack pointer of the setjmp (0x%llx) is below the current one (0x%llx)",
             (unsigned long long)env, val, (unsigned long long)r->rsp, (unsigned long long)ctx->rsp);

  ctx->rax = val ? val : 1;
  ctx->rbx = r->rbx; ctx->rbp = r->rbp; ctx->rsp = r->rsp;
  ctx->r12 = r->r12; ctx->r13 = r->r13; ctx->r14 = r->r14; ctx->r15 = r->r15;
  ctx->mxcsr = r->mxcsr;
  ctx->fpu_cw = r->fpu_cw;
  recomp_resume_guest(ctx, r->host_sp, r->host_lr, r->fpcr);
}

// ---------------------------------------------------------------- C++ landing pads
// LLVM libunwind Registers_x86_64 (the _registers array): rax rbx rcx rdx rdi rsi rbp rsp r8..r15 rip.
typedef struct { uint64_t rax, rbx, rcx, rdx, rdi, rsi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rip; } UnwRegs;

typedef struct { uint64_t rsp; } FindE;
// The target frame (the function containing the landing pad) is the first frame, walking up from
// the unwinder, whose entry rsp is above the rsp the unwinder installs: its callee's entry rsp was
// rsp-8 (the pushed return address) and every frame above has an even higher entry rsp.
static int pred_e_above(uintptr_t sp, uintptr_t lr, uint64_t e, void *arg) {
  (void)sp; (void)lr;
  return e > ((FindE *)arg)->rsp;
}

void shim_guest_unwind_jumpto(GuestContext *ctx) {
  SHIM_SITE(site);
  if (ctx->rdi + sizeof(UnwRegs) > ctx->mem_size) eh_fatal(ctx, site.first, "unwinder jumpto: register set outside guest memory");
  UnwRegs rg;
  memcpy(&rg, ctx->mem_base + ctx->rdi, sizeof rg);
  uintptr_t pad = recomp_call_model_flat() ? recomp_cont_resolve(ctx, rg.rip, RECOMP_MISS_RESUME) : pad_lookup(rg.rip);
  if (!pad)
    eh_fatal(ctx, site.first,
             "C++ unwinder resumes at guest pc 0x%llx which has no host label (not a block of a function with an LSDA, or translated without landing-pad tables)",
             (unsigned long long)rg.rip);
  if (recomp_call_model_flat()) {
    // No host frame to find: the pad's continuation depends on its address alone. What must
    // hold is the boundary rule - the frame the unwinder installs may not lie above the stack
    // of the innermost host -> guest entry.
    RecompBoundary *b = (RecompBoundary *)(uintptr_t)ctx->host_boundary;
    if (b && rg.rsp > b->guest_rsp)
      eh_fatal(ctx, 0, "C++ exception cannot reach the frame of landing pad 0x%llx (guest rsp 0x%llx): it lies above an HLE callback or thread entry boundary (entered with guest rsp 0x%llx) (exceptions are barred at HLE callbacks; SPEC 3.7)",
               (unsigned long long)rg.rip, (unsigned long long)rg.rsp, (unsigned long long)b->guest_rsp);
    if (eh_trace()) {
      char chain[400];
      t_chain_ctx = ctx;
      guest_chain(0, chain, sizeof chain, NULL, 0);
      fprintf(stderr, "[eh] jumpto: pad guest 0x%llx host %p rsp 0x%llx (flat); guest chain: %s\n",
              (unsigned long long)rg.rip, (void *)pad, (unsigned long long)rg.rsp, chain);
    }
    ctx->rax = rg.rax; ctx->rbx = rg.rbx; ctx->rdx = rg.rdx; ctx->rdi = rg.rdi; ctx->rsi = rg.rsi;
    ctx->rbp = rg.rbp; ctx->rsp = rg.rsp;
    ctx->r8 = rg.r8; ctx->r9 = rg.r9; ctx->r10 = rg.r10; ctx->r11 = rg.r11;
    ctx->r12 = rg.r12; ctx->r13 = rg.r13; ctx->r14 = rg.r14; ctx->r15 = rg.r15;
    ctx->rcx = rg.rip;
    recomp_shadow_trim(ctx, rg.rsp); // the unwound activations
    recomp_resume_guest(ctx, 0, pad, site.guest_fpcr);
  }
  FindE f = {rg.rsp};
  const char *why = NULL;
  uintptr_t target = walk_frames(site.first, pred_e_above, &f, &why);
  if (!target)
    eh_fatal(ctx, site.first, "C++ exception cannot reach the frame of landing pad 0x%llx (guest rsp 0x%llx): %s (exceptions are barred at HLE callbacks; SPEC 3.7)",
             (unsigned long long)rg.rip, (unsigned long long)rg.rsp, why ? why : "frame not found");
  if (eh_trace())
    fprintf(stderr, "[eh] jumpto: pad guest 0x%llx host %p rsp 0x%llx -> host frame %p (first guest frame %p, %ld frames up) lr=%p E=0x%llx\n",
            (unsigned long long)rg.rip, (void *)pad, (unsigned long long)rg.rsp, (void *)target, (void *)site.first,
            (long)((target - site.first) / 16), (void *)((uintptr_t *)target)[0], (unsigned long long)((uint64_t *)target)[1]);
  if (eh_trace()) {
    char chain[400];
    guest_chain(site.first, chain, sizeof chain, NULL, 0);
    fprintf(stderr, "[eh] guest chain from jumpto: %s\n", chain);
  }
  // The registers jumpto would load, then "popq %rdi; popq %rcx; jmp *%rcx" with rsp = rg.rsp.
  ctx->rax = rg.rax; ctx->rbx = rg.rbx; ctx->rdx = rg.rdx; ctx->rdi = rg.rdi; ctx->rsi = rg.rsi;
  ctx->rbp = rg.rbp; ctx->rsp = rg.rsp;
  ctx->r8 = rg.r8; ctx->r9 = rg.r9; ctx->r10 = rg.r10; ctx->r11 = rg.r11;
  ctx->r12 = rg.r12; ctx->r13 = rg.r13; ctx->r14 = rg.r14; ctx->r15 = rg.r15;
  ctx->rcx = rg.rip;
  recomp_resume_guest(ctx, target, pad, site.guest_fpcr);
}
