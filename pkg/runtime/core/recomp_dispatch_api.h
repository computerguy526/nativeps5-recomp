// SPDX-License-Identifier: GPL-2.0-or-later
//
// Boundary between the call/return model (flat host stack + shadow return stack, SPEC 3.3,
// docs/m1/flatstack.md) and the dispatch-miss slow path / dynamic code publishing (tier 1,
// SPEC 3.4 and 3.10).
//
// Ownership:
//   call/return model : CALL/RET/indirect-call lowering, the shadow stack, recomp_enter_guest
//                       boundaries, setjmp/longjmp/EH, fibers. Calls recomp_dispatch_miss.
//   tier 1            : recomp_dispatch_miss (translate on miss) and publishing of new code
//                       (recomp_register_fn / recomp_cont_register).
//
// The one thing to know about the flat model: the host stack carries no guest activation, so
// the host continuation of a guest address is a pure function of (guest RIP, code generation).
// The shadow stack is only a cache of that function for return addresses. Whenever the cache
// misses, the runtime needs "guest RIP in, continue there", which is this API.
#ifndef RECOMP_DISPATCH_API_H
#define RECOMP_DISPATCH_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GuestContext;

// Why the fast path could not continue by itself.
typedef enum RecompMissReason {
  RECOMP_MISS_RET = 1,      // RET: the popped guest return address has no host continuation in the
                            // shadow stack (mismatch, discarded cache) nor in the static tables
  RECOMP_MISS_CALL = 2,     // CALL to a guest address without a translation. The guest return
                            // address is already on the guest stack and its shadow entry is pushed
  RECOMP_MISS_JUMP = 3,     // indirect JMP (tail call, jump table, PLT) to such an address
  RECOMP_MISS_RESUME = 4,   // non-local resume: longjmp target, unwinder context install
                            // (landing pad), fiber entry
} RecompMissReason;

// ---- implemented by tier 1 (a weak default that returns 0 lives in nps5_shadow.c) -------------
//
// The slow-path entry. `guest_rip` has no host continuation in any table the call/return model
// knows (dispatch table of function entries, continuation table of return sites and landing
// pads). Return the host address of code that continues the guest at guest_rip, or 0 when it
// cannot be produced; the caller then stops with a diagnostic (crash.txt names guest_rip, the
// reason and the guest call chain).
//
// Contract of the returned address ("block entry state"; it is what the lifter emits at a
// basic-block label, i.e. the *jump entry* of a function and not its call entry, see below):
//   - entered with `br`, in the guest register state: the 16 GPRs pinned (rax x19, rbx x20,
//     rcx x21, rdx x22, rsi x23, rdi x24, rbp x25, rsp x26, r8-r15 x8-x15), FS base x27, memory
//     base x28, xmm/ymm in v0-v31, FPCR as the guest set it;
//   - x29 = shadow stack pointer of the running fiber/thread (flat model). The target pushes
//     nothing on entry: for RECOMP_MISS_CALL the caller has pushed the shadow entry already;
//   - host sp = the flat host stack pointer of the innermost recomp_enter_guest boundary;
//     x0-x7, x16, x17, x30 and NZCV are dead (a guest block starts with the carry flag in the
//     block-boundary sense, C = !CF, and flags are dead at every target that reaches here);
//   - the code must be in a region recomp_pc_in_guest_code() reports as guest code, and must
//     stay valid until recomp_shadow_invalidate() has been called for it.
//
// Calling context: the guest thread itself, in ordinary (non-signal) context, on its host
// stack, with no runtime lock held. *ctx holds the 16 GPRs, ctx->rip == guest_rip; the vector
// registers and flags are saved by the caller around the call, so plain C is fine. It may block
// (compile, wait for another thread compiling the same address) and may be entered by several
// threads at once, also for the same guest_rip. It must not unwind or longjmp out.
//
// Whether the result is also published (so the next miss is served from the tables) is the
// implementation's choice: recomp_register_fn for function entries, recomp_cont_register for
// any other address.
uintptr_t recomp_dispatch_miss(struct GuestContext *ctx, uint64_t guest_rip, uint32_t reason);

// ---- implemented by the call/return model (nps5_shadow.c) -------------------------------------

// Publishes a host continuation (block entry state, see above) for a guest address that is not
// a function entry: a return site, a landing pad, the entry of a tier-1 fragment. Thread-safe;
// readers are lock-free. Registering an address again replaces the continuation.
void recomp_cont_register(uint64_t guest_rip, uintptr_t host_addr);

// Looks guest_rip up in everything already published: the continuation table, then the dispatch
// table of function entries (returned as the function's jump entry). 0 when unknown. Lock-free,
// async-signal-safe.
uintptr_t recomp_cont_lookup(uint64_t guest_rip);

// Full resolution as the slow paths do it: recomp_cont_lookup, then recomp_dispatch_miss.
uintptr_t recomp_cont_resolve(struct GuestContext *ctx, uint64_t guest_rip, uint32_t reason);

// Must be called before host code in [host_lo, host_hi) is unloaded or overwritten: every
// shadow entry of every thread and fiber whose continuation lies in the range is poisoned, so
// the next RET through it takes the slow path (SPEC 3.10: "do not unload code while any shadow
// continuation references it"). Removes continuation-table entries pointing into the range as
// well. The caller is responsible for no thread executing inside the range.
void recomp_shadow_invalidate(uintptr_t host_lo, uintptr_t host_hi);

// Process-wide code generation: bumped by recomp_shadow_invalidate, recorded in each thread's
// shadow stack header (diagnostics; the RET fast path does not load it).
uint64_t recomp_code_generation(void);

// ---- function entry ABI of translated code ----------------------------------------------------
//
// Nested model (-call-model nested): one entry, called with `bl`, returns with `ret`.
//
// Flat model (-call-model flat): every function symbol (and every shim stub) has two entries.
//   call entry  = the symbol / the address given to recomp_register_fn:
//       in:  x30 = host continuation of the call (the caller used `bl`, which keeps the return
//            address predictor paired with the final `ret`), x7 = guest return address, which
//            the caller has also pushed on the guest stack ([rsp] == x7). Not x16/x17: a linker
//            veneer between a far branch and its target may clobber those.
//       It pushes the shadow entry {x7, rsp, x30} and falls into the jump entry.
//   jump entry  = call entry + RECOMP_FLAT_JUMP_ENTRY_OFFSET: tail calls, indirect jumps,
//       recomp_dispatch_miss results. Pushes nothing.
// A guest RET pops the guest return address and compares {address, rsp} with the shadow top:
// equal, it pops the entry and `ret`s to its continuation; otherwise it goes to
// _recomp_ret_slow (guest return address in x7), which resynchronises the shadow stack and
// continues at recomp_cont_resolve(ctx, address, RECOMP_MISS_RET).
#define RECOMP_FLAT_JUMP_ENTRY_OFFSET 8

// Shadow stack entry (32 bytes, the stack grows down, x29 points at the newest entry).
typedef struct RecompShadowEntry {
  uint64_t guest_ra;  // guest return address; RECOMP_SHADOW_RA_* for the special entries
  uint64_t guest_rsp; // guest rsp at function entry = address of the return address slot
  uint64_t host_cont; // host continuation (block entry state)
  uint64_t aux;       // normal entries: not written. Boundary entries: host sp of the boundary
} RecompShadowEntry;

// guest_ra values that no guest address can equal (non-canonical).
#define RECOMP_SHADOW_RA_BOTTOM 0xfffffffffffffff1ull   // bottom sentinel of a shadow stack
#define RECOMP_SHADOW_RA_POISON 0xfffffffffffffff2ull   // invalidated entry

#ifdef __cplusplus
}
#endif

#endif // RECOMP_DISPATCH_API_H
