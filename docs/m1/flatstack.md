# The flat call model (M1)

Scope: SPEC §3.3 (calls, returns, host stack), §3.7 (longjmp, C++ unwinding, fibers), §3.10
(diagnostics). Branch `m1-flatstack`. Translate-time flag `-call-model flat|nested`, default `flat`;
`lifter_manifest.json` records it and the runtime reads `g_recomp_call_model` (defined by the flat
trampoline), so one runtime source tree serves both.

Code: `pkg/lifter/arm64_lifter_call.go` (lowering), `pkg/emitter/asm_trampoline_flat.go`
(`recomp_enter_guest`, shim bridge, indirect entries, slow-path stubs), `pkg/emitter/asm_emitter.go`
(call entries, continuation table), `pkg/runtime/core/nps5_shadow.[ch]` (shadow stacks, continuation
table, slow paths), `nps5_eh.c` (`*_flat`), `nps5_diag.c` (crash chain),
`pkg/runtime/core/recomp_dispatch_api.h` (contract with tier 1).

## 1. The idea

The guest stack is the only call stack. A guest CALL pushes the real return address on the guest stack; a
guest RET goes where the guest stack says. No recompiled function has a host frame, so the host stack
pointer does not move while guest code runs.

The consequence everything else rests on: **the host continuation of a guest address is a pure function of
the address** (and the code generation). It needs no activation, so it can be looked up - dispatch table
for function entries, continuation table for return sites and other block starts, tier 1 for the rest. The
shadow return stack only caches that function for the return addresses of live calls. A wrong or empty
cache costs time, never correctness: a RET that misses is continued by address.

What the nested model got wrong and this one gets right (all in `tests/ps5/src/t_callmodel.c`): `push addr;
ret`, a return address replaced by the callee, `call next; pop`, `ret imm16` (was not even popped), a jump
into a block of another function, recursion bounded by the guest stack instead of the host stack.

## 2. Code shape

Registers: x29 = shadow stack pointer while the host pc is in `__recomp_guest`; x30 = scratch; x7 = guest
return address at a call entry and at `_recomp_ret_slow`. Everything else as before.

```
CALL f        sub x26, x26, #8 ; movz x7, #ra ; str x7, [x28, x26] ; bl _fn_f          (as nested, x7 for x16)
_fn_f:        stp x7, x26, [x29, #-32]!      // call entry: shadow entry {ra, rsp, cont}
              str x30, [x29, #16]
_fn_f+8:      ...                            // jump entry = body (tail calls, dispatcher, slow paths)
RET [imm]     ldr x7, [x28, x26]             // guest return address
              ldp x17, x0, [x29], #32        // shadow top, popped
              cmp x7, x17 ; ccmp x26, x0, #0, eq
              add x26, x26, #8+imm
              b.ne L_fn_f_retslow            // -> b _recomp_ret_slow
              ldur x30, [x29, #-16] ; ret
JMP f         b _fn_f+8                      // no epilogue
```

* **`bl` / `ret` (S2 variant B3).** The call still uses `bl` and the return `ret x30`, so the host
  return-address predictor stays paired; the shadow entry only verifies. The push is on the callee side
  (one copy per function instead of one per call site; the `bl` has already produced the continuation in
  x30).
* **Entry** `{guest_ra, guest_rsp, host_cont, aux}`, 32 bytes, stack grows down. `guest_rsp` is the address
  of the return-address slot. RET compares both `{ra, rsp}`: the address decides where to go, the stack
  pointer keeps the cache aligned with the activation (recursion reuses addresses).
* **Fiber identity and code generation** are not compared on the fast path. Identity is structural: every
  GuestContext (thread or fiber) owns its shadow stack and x29 follows the context. A code generation
  change poisons the affected entries instead (`recomp_shadow_invalidate`, §6), which costs the fast path
  nothing.
* **Shim stubs** have the same two entries. The bridge (`_recomp_call_shim`) runs the C shim on host
  frames below the flat stack pointer and then does what a guest RET does (the shim popped the return
  address with `SHIM_RETURN`). A PLT stub's `jmp [got]` reaches the stub's jump entry; the entry of the
  original call is then on top and matches.
* **Flags** are dead across CALL/RET in this lifter already (carry-liveness pass), so `cmp`/`ccmp` are free.

### Why x29

No callee-saved register is free: x19-x26 are guest GPRs, x27 the FS base, x28 the memory base (base
addressing is the default and first-class), x18 is reserved, x30 is written by `bl`. The scratch registers
x0-x7/x16/x17 are used by every lowering and die in the C helpers that lowerings call mid-instruction; S2
showed that keeping the pointer in memory costs +140-260 %. x29 is callee-saved under AAPCS64 (helper and
libm calls keep it), and no lowering used it: guest code builds no frame records, and the nested model left
x29 pointing at the `recomp_enter_guest` frame.

The price: x29 is not a frame pointer while guest code runs. The bridge puts a valid record chain back
before it calls C (`[ctx->host_boundary, continuation of the innermost guest call]`), the crash walker starts
from `ctx->host_boundary` when the pc is in guest code, and the three helper lowerings store their frame
record without linking it. The invariant "pc in `__recomp_guest` => x29 is the shadow pointer" is what a
signal handler relies on (`recomp_shadow_sync_from_signal`); `TestFlatModelLeavesX29Alone` guards it.

### Why x7 and not x16

x16/x17 are the linker's veneer registers (AAPCS64 IP0/IP1): a branch island between a far `bl` and its
target may clobber them, and every far call would then miss the shadow check. x7 is plain scratch.

## 3. Host boundaries

`recomp_enter_guest` builds a 208-byte frame (`RecompBoundary`, nps5_shadow.h): the host callee-saved
registers plus `{previous ctx->host_boundary, guest rsp at entry, guest return address at entry}`. It sets
`ctx->host_boundary` to that frame (= the flat host sp of the guest code it starts), pushes a shadow entry
whose continuation is `Lenter_guest_ret`, and enters the function's jump entry. The entered function's RET
matches that entry like any other.

The boundary record, not the shadow entry, is authoritative: the shadow stack may be discarded, the record
cannot. It gives (a) the host sp to return to for every non-local resume, (b) the rule "a guest stack
pointer above the boundary's entry rsp is only legal as the return of the entered function", which is how
a longjmp, an exception or a stray RET across an HLE callback or thread entry is detected (fatal with a
diagnostic, SPEC §3.7), (c) the chain of nested callbacks.

## 4. Slow paths

`_recomp_ret_slow` (mismatch) and the indirect call/jump entries (dispatch table miss) end in
`_recomp_dispatch_continue`: store the guest registers, call `recomp_flat_continue(ctx, rip, reason)`, reload,
`br`. All 32 vector registers survive; NZCV does not (dead at every way in).

`recomp_flat_continue` for a RET to `rip` with stack pointer `rsp` after the pop:

1. Pop every shadow entry whose slot is below `rsp` (`recomp_shadow_trim`): those activations are gone,
   whoever unwound them. The entry of this very return, if cached, is the oldest of them.
2. `rsp` above the innermost boundary's entry rsp: return to the host if `rip` is the boundary's return
   address, else fatal.
3. The oldest popped entry names `rip`: continue at its continuation (the cache was merely misaligned).
4. Otherwise `recomp_cont_resolve`: continuation table (`__DATA,__nps5_pads`: every non-entry block and
   every return site, plus `recomp_cont_register`), dispatch table (function entry + 8), then
   `recomp_dispatch_miss` (tier 1; the weak default returns 0), else fatal with crash.txt.

**Overflow.** Each stack is `[guard][32768 entries][guard]` (`NPS5_SHADOW_ENTRIES`, rounded up to whole
16 KiB pages, so 511 entries at least; 1 MiB of address space per context, touched lazily). A push into the low
guard faults; the SIGSEGV/SIGBUS first-chance hook recognises the `stp Xt, Xt2, [Xn, #-32]!`, keeps the
newest half of the entries at the top of the mapping, rewrites `Xn` in the signal context and retries. The
dropped activations return through step 4. No check on the fast path. **Underflow** reads the bottom
sentinel (an address no guest RET can produce) and takes the slow path.

**Asynchronous entry.** A RET pops before it loads the continuation, so one slot below x29 is a red zone.
The SIGUSR1 guest-exception handler, the only code that enters the guest on top of interrupted guest code,
takes the live pointer from the interrupted x29 and pushes below `RECOMP_SHADOW_REDZONE`.

## 5. setjmp/longjmp, C++ EH, fibers (migration of docs/m1/setjmp-eh.md §7)

| nested | flat (implemented) |
|---|---|
| target = a host frame found by walking `[lr, entry rsp]` slots | no host frame: continuation = `recomp_cont_resolve(guest rip)`; frames dropped = `recomp_shadow_trim(guest rsp)` |
| `recomp_resume_guest` moves `sp` to the frame | sets `sp = ctx->host_boundary`, `x29 = ctx->shadow_sp`, `br` |
| setjmp record: host sp, slot contents, host lr | owner context, `ctx->host_boundary`, the innermost live shadow entry (address, contents, stack epoch) |
| longjmp liveness: frame chain reaches the recorded slot | same context; same boundary; guest rsp not above the setjmp's; the anchor entry still in place (only while the stack's epoch is unchanged - after a compaction the cache proves nothing and the first three are all that can be known) |
| EH target frame: first frame with entry rsp above the installed rsp | not needed; rule (b) of §3 against the installed rsp |
| HLE boundary = the frame chain ends | `RecompBoundary` |
| chain for diagnostics from host slots | from the shadow stack |

Unchanged: the generation-tagged `jmp_buf`, the per-thread record table, the `jumpto` interception, the
pad lookup (now the continuation table), the messages. `broken_longjmp` still reports "already returned".
The interception of `jumpto` is no longer structurally required - an unintercepted `jmp *%rcx` to a landing
pad resolves through the continuation table and the next RET resynchronises the shadow stack - but it
keeps the diagnostics and trims eagerly.

**Fibers.** The shadow stack belongs to the GuestContext, and a fiber is a GuestContext: it gets its stack
on its first `recomp_enter_guest` and loses it in `fiber_free_host`. A switch happens inside a shim; the
bridge reloads x29 from the context that made the call, so both fiber modes (coroutines and host threads)
work without code in ps4_fiber.c. jmp_buf records carry the owning context.

## 6. Dispatch API (tier 1)

`recomp_dispatch_api.h`. Tier 1 implements `uintptr_t recomp_dispatch_miss(ctx, guest_rip, reason)` (reasons
RET, CALL, JUMP, RESUME): called on the guest thread, in normal context, with no lock held, GPRs in `*ctx`,
vectors saved by the caller; returns host code in *block entry state* (what a jump entry is) or 0. It
publishes with `recomp_register_fn` (function entries) or `recomp_cont_register` (anything else) and must
call `recomp_shadow_invalidate(lo, hi)` before unloading code. Emitted tier-1 code follows the same entry
ABI because it comes from the same lifter.

## 7. Diagnostics

crash.txt `guest_call_chain:` is the shadow stack, innermost first, one call site per live entry, with
`<host boundary: HLE callback or thread entry>` lines; entries the guest stack has popped are skipped. It is
now printed for faults in guest code too (the nested chain needed a shim bridge on the stack). A
`shadow_stack:` line gives depth, slow returns and overflows; summary.txt has the process totals;
`NPS5_SHADOW_STATS=1` prints them at exit. Tail-called functions do not appear (as before).

## 8. Results

* Go: `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test ./pkg/...` green. New: `TestFlatCallRetLowering`,
  `TestNestedCallRetLowering`, `TestFlatModelLeavesX29Alone`; `TestGuestStateBoundary` builds and runs the
  whole boundary harness once per model (22 cases each: fibers in both modes, guest exception handlers in
  shims/helpers/fibers, thread start, crash and longjmp reports).
* `tools/m1_run.sh --call-model flat`: 34/36 (only t_x87). `--call-model nested`: 32/36 (t_x87 and
  t_callmodel, which needs guest-authoritative returns; the 34 older rows are 32/34 in both models). Also
  34/36 with a one-page shadow stack (`NPS5_SHADOW_ENTRIES=16`): t_callmodel then runs through 1227
  compactions and 185,558 slow returns, t_recurse through 133 and 34,048. With the default size only
  t_callmodel leaves the fast path (10 compactions, 101,842 slow returns, 1000 indirect-jump lookups in the
  continuation table); every other test has zero slow returns.
* `pkg/runtime` `TestShadowStack` (tests/shadow_test): mapping, overflow through the real guard-page fault,
  trim, the RET slow path cases, continuation table, invalidation, release.
* `tools/memtests.sh --strict`: 240 pass, 0 fail (guest programs translated flat).
* `tools/mem_prove.sh` 5/5 (flat is the default; includes identity addressing).
* FEX oracle on the call/ret/stack set (492 tests: CALL, JMP, jump, Primary, PrimaryGroup, Multiblock,
  OpSize and every test named after call/ret/push/pop/enter/leave/jmp/loop): 403 PASS / 41 SKIP / 48 FAIL in
  both models, identical per test (`fexasm -call-model`). Against the committed baseline the nested run
  gains Primary_C2 (RET imm16).
* `tools/callbench.sh 9` (call-saturated; M4 Pro under load, runs interleaved, min / median ns per call):

  | case | nested | flat | delta |
  |---|---|---|---|
  | fib(38) | 1.38 / 1.43 | 1.40 / 1.46 | +1.4 % / +2.1 % |
  | leaf | 1.10 / 1.13 | 1.08 / 1.19 | -1.8 % / +5.3 % |
  | mix (4 sites, 2 deep) | 1.35 / 1.45 | 1.37 / 1.48 | +1.5 % / +2.1 % |
  | indirect | 1.39 / 1.52 | 1.51 / 1.59 | +8.6 % / +4.6 % |
  | recursion 20000 deep | 6.88 / 7.07 | 7.11 / 7.58 | +3.3 % / +7.2 % |
  | recursion 100000 deep (past the shadow stack) | 6.62 / 7.24 | 17.5 / 18.7 | +164 % |
  | shim call (`abs`) | 7.48 / 8.12 | 9.10 / 9.63 | +22 % / +19 % |
  | qsort comparator callbacks | 30.3 / 31.9 | 32.3 / 32.9 | +6.8 % / +3.3 % |

  `t_recurse` wall time (9 runs, min / median): nested 131 / 144 ms, flat 135 / 146 ms (startup dominates).
* Syobon eboot translate: asm text 119.7 -> 121.9 MB (+1.8 %), guest code 9,169,036 -> 9,275,256 bytes
  (+1.2 %), continuation table 1.58 -> 1.86 MB, objects 33.35 -> 33.95 MB.

## 9. Known gaps and risks

* A return past a compacted shadow stack costs a full slow path (~25 ns); nothing refills the cache.
  Recursion deeper than 32768 is the only workload that sees it.
* The shim call costs about 1.5 ns more than nested (shadow pointer save/reload, the bridge's RET check).
* `GuestContext sub = *ctx; recomp_call_guest(&sub, ...)` (ps4_font.c) runs guest code on a copy that shares
  the shadow stack; the original's `shadow_sp` is stale afterwards and the shim's return resynchronises it
  through one slow RET. The copy's boundary is not the one the TCB context names, so a longjmp or exception
  inside such a callback is not checked against the right boundary.
* Code that the static translation never discovered (the target of `push addr; ret` with no other
  reference) ends in "no host continuation" until tier 1 serves `recomp_dispatch_miss`.
* x29 is not a frame pointer in guest code: external samplers (Instruments, `sample`) see a broken chain
  there. In-process walkers are handled.
* The red zone covers the one asynchronous entry that exists (SIGUSR1). Any new code that enters the guest
  from a signal handler must call `recomp_shadow_sync_from_signal`.
* Signal masks, x87/xmm state across longjmp, exceptions across HLE callbacks: unchanged from
  setjmp-eh.md §8.
