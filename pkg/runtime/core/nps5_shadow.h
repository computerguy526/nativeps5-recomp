// SPDX-License-Identifier: GPL-2.0-or-later
//
// Flat call model runtime: the per-context shadow return stack, the continuation table and the
// slow paths behind _recomp_ret_slow / _recomp_dispatch_continue. Design: docs/m1/flatstack.md.
// The contract with tier 1 is recomp_dispatch_api.h.
#ifndef NPS5_SHADOW_H
#define NPS5_SHADOW_H

#ifndef __cplusplus
#include <stdatomic.h> // <atomic> and <stdatomic.h> do not mix before C++23: C++ sees RecompShadow opaque
#endif
#include <stddef.h>
#include <stdint.h>

#include "recomp_dispatch_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GuestContext;

// 0 = nested, 1 = flat. Defined by the flat trampoline (trampoline_arm64.s); the weak default
// in nps5_shadow.c serves nested builds and host tests that link no trampoline.
extern uint64_t g_recomp_call_model;
// Call-site inline caches (docs/m3/callic.md): drop every cached {target, entry} (a dispatch
// table entry was replaced) or those whose host entry is in [lo, hi) (code unloaded).
extern uint64_t g_recomp_ic[3]; /* epoch, enabled, address of _recomp_indirect_call (an empty slot's entry) */
void recomp_ic_flush_all(void);
void recomp_ic_flush_range(uintptr_t lo, uintptr_t hi);
static inline int recomp_call_model_flat(void) { return g_recomp_call_model != 0; }

// The frame recomp_enter_guest builds in the flat model (asm_trampoline_flat.go). While guest
// code of a context runs, the host sp is the address of its innermost RecompBoundary
// (GuestContext.host_boundary). It is the authoritative record of a host -> guest entry: the
// shadow stack may be discarded at any time, the boundary cannot.
typedef struct RecompBoundary {
  uint64_t host_fp, host_lr;
  uint64_t x19_x28[10];
  uint64_t d8_d15[8];
  struct GuestContext *ctx;
  uint64_t host_fpcr;
  struct RecompBoundary *prev; // ctx->host_boundary before this entry
  uint64_t guest_rsp;          // guest rsp at entry: the slot of the guest return address
  uint64_t guest_ra;           // the guest return address the entered function returns to
  uint64_t shadow_sp;          // ctx->shadow_sp at entry (diagnostics)
} RecompBoundary;
_Static_assert(sizeof(RecompBoundary) == 208, "RecompBoundary is the recomp_enter_guest frame of the flat trampoline");
_Static_assert(offsetof(RecompBoundary, prev) == 176 && offsetof(RecompBoundary, guest_ra) == 192, "RecompBoundary layout");

// One shadow stack: [guard page][entries, growing down][guard page]. `bottom` is the sentinel
// entry at the high end; the stack is empty when the shadow sp equals it.
#ifdef __cplusplus
typedef struct RecompShadow RecompShadow;
#else
typedef struct RecompShadow {
  uint8_t *map;
  size_t map_len;
  RecompShadowEntry *lo;     // lowest usable entry address (first byte above the low guard)
  RecompShadowEntry *bottom; // sentinel {RECOMP_SHADOW_RA_BOTTOM, ~0, 0, 0}
  struct GuestContext *ctx;
  // Bumped whenever entries are moved or dropped behind the guest's back (overflow compaction,
  // invalidation): pointers into the stack taken under an older epoch are meaningless.
  _Atomic uint64_t epoch;
  uint64_t code_gen;         // recomp_code_generation() when the stack was created/last flushed
  _Atomic uint64_t overflows, slow_rets, slow_hits, slow_boundary;
} RecompShadow;
#endif

// A slot below the shadow stack pointer that guest code may still read: a RET pops the entry
// and loads its continuation afterwards. Code that enters the guest asynchronously on top of
// interrupted guest code (a signal handler) must leave this many entries untouched.
#define RECOMP_SHADOW_REDZONE 2

// Called by recomp_enter_guest on the first entry of a context: maps its shadow stack
// (NPS5_SHADOW_ENTRIES entries, default 32768, rounded up to whole pages) and sets ctx->shadow_sp.
// Aborts when it cannot.
void recomp_shadow_attach(struct GuestContext *ctx);
// Unmaps the shadow stack of a context that is being freed. No-op without one.
void recomp_shadow_release(struct GuestContext *ctx);

// SIGSEGV/SIGBUS first chance: a shadow push that hit the low guard page. Keeps the newest half
// of the entries, rewrites the faulting instruction's base register in the signal context and
// returns 1 (the instruction is retried). 0 = not ours. Async-signal-safe.
int recomp_shadow_handle_fault(void *ucontext, uint64_t fault_addr);

// A signal handler is about to enter guest code on ctx (recomp_dispatch): when the signal
// interrupted that context's guest code, its live shadow stack pointer is in the interrupted
// x29, not in ctx. Sets ctx->shadow_sp from it, below the red zone. Async-signal-safe.
void recomp_shadow_sync_from_signal(struct GuestContext *ctx, void *ucontext);

// The C side of _recomp_dispatch_continue (reason: RecompMissReason). Never returns 0.
uintptr_t recomp_flat_continue(struct GuestContext *ctx, uint64_t guest_rip, uint32_t reason);

// Pops every entry of ctx's shadow stack whose return-address slot is below guest rsp `rsp`
// (activations the guest stack no longer contains). Returns the last entry popped (the oldest
// dead one; still readable) or NULL.
RecompShadowEntry *recomp_shadow_trim(struct GuestContext *ctx, uint64_t rsp);

// Host address of Lenter_guest_ret: the continuation of every host -> guest entry.
uintptr_t recomp_shadow_boundary_cont(void);

// Lookup in the continuation table alone (static return sites / landing pads + registered
// continuations), without the dispatch table. Works in both call models (the nested model
// lists landing pads only).
uintptr_t recomp_cont_table_lookup(uint64_t guest_rip);

// Guest call chain for diagnostics, innermost first: fills conts[] with the host continuation
// of each live shadow entry starting at `top` (entries whose slot is below `rsp` are skipped;
// a host boundary is reported as RECOMP_CHAIN_BOUNDARY). `top` may come from a signal context:
// it is validated against the registered stacks. Async-signal-safe. Returns the count.
#define RECOMP_CHAIN_BOUNDARY 1ull
size_t recomp_shadow_chain(uint64_t top, uint64_t rsp, uint64_t *conts, size_t max);

// Process totals since start (summary.txt; NPS5_SHADOW_STATS=1 prints them to stderr at exit).
typedef struct RecompShadowStats {
  uint64_t stacks;          // shadow stacks created
  uint64_t slow_rets;       // RETs that left the fast path, of which:
  uint64_t slow_hits;       //   continued from the shadow stack after resynchronising it
  uint64_t slow_boundary;   //   returned to a host boundary
  uint64_t slow_by_address; //   continued by guest address (continuation/dispatch table, tier 1)
  uint64_t overflows;       // overflow compactions
  uint64_t dispatch_slow;   // indirect calls/jumps and resumes that needed the continuation table or tier 1
} RecompShadowStats;
void recomp_shadow_stats(RecompShadowStats *out);

#ifdef __cplusplus
}
#endif

#endif // NPS5_SHADOW_H
