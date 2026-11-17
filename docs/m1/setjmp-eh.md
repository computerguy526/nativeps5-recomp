# setjmp/longjmp and C++ exceptions in the nested host-stack model (M1)

Scope: SPEC §3.3 (call/return), §3.7 (exceptions, `longjmp`), §3.10 (diagnostics). Branch `m1-setjmp`.
Code: `pkg/runtime/core/nps5_eh.c`, `pkg/emitter/asm_emitter.go` (host frame, landing-pad table),
`pkg/emitter/asm_trampoline.go` (`_recomp_resume_guest`), crash walker in `pkg/runtime/core/nps5_diag.c`.

## 1. What was actually wrong

* `m1_main` did not die in setjmp. The `[recomp-jmp] Hooked unresolved setjmp stub` line is a startup
  message from a hook that registers an asm routine in the dispatch table (direct `bl` calls to the PLT
  stub never consult that table). The first real failure was `aligned_alloc` unresolved, called from the
  guest's own `__cxa_allocate_exception`.
* C++ EH works the way SPEC §3.7 predicts: **libc++abi and libunwind are guest code** in the test ELFs
  (statically linked, LLVM libunwind, `__cxa_throw`, `_Unwind_RaiseException`, `unwind_phase2`, ...). Phase 1
  (search) and phase 2 (cleanup) run recompiled and read the guest `.eh_frame` / `.gcc_except_table`
  through guest memory; they need nothing from the host except `malloc`/`pthread_getspecific`/`getenv`.
  They only fail at the last step: `__libunwind_Registers_x86_64_jumpto` loads the target frame's registers
  and `ret`s to the landing pad, i.e. to the middle of a function whose host activation is several host
  frames up the host stack.
* PS4Recomp's emitter bound **every symbol of the image by name** to a host shim, so the guest's own
  `__cxa_throw` was replaced by `shim_guest_cxa_throw` ("FATAL: exception unwinding is not supported").
  The `.symtab` also contains local `.bss` objects named like functions (`calloc`, `aligned_alloc`);
  both the emitter and `ResolveModuleRelocations` bound/resolved imports to those data addresses.
  Both now skip `STT_OBJECT`/`STT_TLS` symbols.

## 2. Host frame layout (the one lifter-side change)

Every recompiled function now starts `stp x30, x26, [sp, #-16]!` instead of `str x30, [sp, #-16]!`
(same size, same cost, epilogue unchanged): the host frame is `[host return address, entry guest RSP]`
where entry RSP is the guest address holding that activation's return address. The shim bridge
(`_recomp_call_shim`) adds a 48-byte frame `[x29,x30][shim,ctx][guest FPCR]` below the calling guest
function. So, seen from a shim, the host stack is

```
bridge frame (48) | G1 [lr,E1] | G2 [lr,E2] | ... | Gn | host boundary (enter_guest frame, or thread entry)
```

with E1 < E2 < ... strictly increasing and every lr inside `__TEXT,__recomp_guest`
(`recomp_pc_in_guest_code`). `walk_frames` (nps5_eh.c) validates exactly that, so a chain that runs into an
HLE-callback boundary is detected and reported instead of mis-parsed. Tail calls (`ldr x30,[sp],#16; b`)
pop their frame, so the 1:1 correspondence guest activation <-> host slot holds.

`recomp_resume_guest(ctx, host_sp, host_pc, fpcr)` (asm) reloads the 16 guest GPRs from `ctx`, sets the
guest FPCR, moves `sp` to a frame slot and `br`s into the target function body. The abandoned host frames
(callee guest frames, bridge, shim C frame) are dropped; nothing in them owns resources (HLE functions
are leaf-like by the §3.7 callback-barrier rule).

## 3. setjmp / longjmp

Names bound (`shims.go`): `setjmp _setjmp sigsetjmp` and the PS4 NID `gNQ1V2vfXDE` -> `shim_guest_setjmp`;
`longjmp _longjmp siglongjmp` -> `shim_guest_longjmp`. They are ordinary shims, so direct `bl` to a PLT stub,
GOT calls and guest-defined libc symbols are all covered. The old asm routine, `GuestJmpRecord` and the
startup dispatch hook are deleted.

**setjmp** writes the guest `jmp_buf` (rip, rbx, rsp-after-return, rbp, r12-r15, x87 CW, MXCSR, a 64-bit
*generation*, a magic) and a per-thread record in a 1024-slot direct-mapped table indexed by the
generation: host sp of the calling guest function (`G1`), that slot's `[lr, E]`, the host address after the
call, the guest FPCR, the saved registers. Generations come from a process-wide counter and are never
reused, so a copied or re-used `jmp_buf` always selects the right record (or none).

**longjmp** reads generation+magic from the guest buffer and fails loudly (`eh_fatal` -> `nps5_log` ERROR,
`nps5_diag_set_note`, `abort()` -> `crash.txt` with the note and a guest call chain) when: the buffer has no
magic; no live record for that generation on *this thread* (per-thread table: cross-thread longjmp is
undefined anyway); the recorded host frame is below the caller's (the setjmp function returned); the slot
no longer holds the recorded `[lr, E]` (reused by another activation); the frame chain from the caller does
not reach the target (HLE callback boundary or corruption); or the recorded guest RSP is below the current
one. Otherwise it restores rbx/rbp/rsp/r12-r15, MXCSR, x87 CW, sets `rax = val ? val : 1` (32-bit), and
resumes after the setjmp call at its host return address with the recorded host sp and FPCR. Caller-saved
registers are left as they are (undefined by the standard). Signal masks are not saved
(`sigsetjmp(…, savemask)` ignores it); xmm/x87 stack contents are not restored.

Tests: `m1_main` (setjmp through recursion), `t_eh` (nested buffers, `longjmp(…,0)`, 5 generations on one
buffer, `sigsetjmp`, longjmp out of a catch handler), `broken_longjmp` (stale buffer -> "already returned").

## 4. C++ exceptions

`__libunwind_Registers_x86_64_jumpto` (a local symbol in the guest image) is bound to
`shim_guest_unwind_jumpto`. It receives the guest `Registers_x86_64` (rax..r15, rip at +0x80) and:

1. Looks the landing-pad guest pc up in the **pad table**. The emitter writes `{guest pc, host label}` pairs
   to `__DATA,__nps5_pads` for every non-entry block of every function with an LSDA (all functions when the
   image carries no LSDA info). Landing pads are ordinary lifted blocks (reached by no branch, so they have
   labels but not dispatch entries); the labels are the whole mechanism.
2. Walks the guest frames upward and picks the first whose entry RSP is **above the RSP the unwinder
   installs**. The callee's entry RSP is `rsp-8`, the target's is higher, so this is exactly the frame that
   contains the pad (verified in `[eh]` traces: it is the function that called `__cxa_throw` for the first
   cleanup, and so on up for each `_Unwind_Resume`).
3. Loads the registers as `jumpto` would (including its `popq %rdi; popq %rcx; jmp *%rcx` quirk, rcx=rip) and
   `recomp_resume_guest`s to the pad with `sp` = that frame's slot.

Phase 1 never touches the stack, so for an uncaught exception `std::terminate` -> `abort` runs on top of the
intact thrower frames (§5). Exceptions crossing an HLE callback (qsort comparator, thread entry) hit the
frame-chain validation and terminate with a diagnostic, as SPEC §3.7 demands.

`m1_main` and `t_eh` (deep unwind with destructors, catch(...)+rethrow, exception on a second thread) pass.

## 5. Diagnostics for uncaught exceptions

`abort` is bound to `shim_guest_abort` (`shims_eh.go`, deliberately overriding any other binding): it logs
the chain and sets the crash note. Independently, the **crash handler** now writes `guest_call_chain:` in
`crash.txt` for any crash with a shim bridge on the stack (so also the unresolved-`fwrite` abort that
`abort_message` hits today): innermost-first call sites by function name from the pc map, the entry marked
`<- thrower of the uncaught exception` right after `__cxa_throw`/`__cxa_rethrow`, and an `uncaught_exception:
yes` line. Example (`broken_exception`):

```
  #3 0x9216 in …failed_throw…   #4 0x91f2 in __cxa_throw   #5 0x8f0f in _ZL16uncaught_throweri   <- thrower…
```

Needs the pc map (default; `NPS5_NO_PCMAP=1` leaves function names out). `NPS5_EH_TRACE=1` prints every
landing-pad transfer and its guest chain.

## 6. Pitfalls found

* `m1_native_build.sh` fills unresolved link symbols with zero-returning stubs. A C declaration
  `_recomp_resume_guest` is the symbol `__recomp_resume_guest`: the call silently returned. Declare asm
  entries without the leading underscore.
* Guest-section code placed after the pcmap's function entries is attributed to the previous function
  (shim stubs show up as `_ZnamSt11align_val_t` etc.).
* The temporary `shim_tmp_*` allocator (`nps5_tmp_alloc.c`) has been removed: the HLE libc module provides
  malloc/calloc/realloc/free/aligned_alloc/getenv.

## 7. Migration to the flat-stack / shadow-stack model (§3.3)

**Done on `m1-flatstack`: see docs/m1/flatstack.md §5.** `-call-model flat` is the default; sections 2-6
above describe `-call-model nested`, which is kept for A/B. The table below was the plan; the implemented
mapping differs in one respect: no host continuation is recorded anywhere (it is a pure function of the
guest rip in the flat model), so the setjmp record and the EH path keep guest state only.

What stays: the guest-visible state (generation-tagged `jmp_buf` + per-thread record table, register
sets, FPCR/MXCSR/CW capture), the pad table, `jumpto` interception, the validation rules, and the
diagnostics. What changes is only "where is the target's continuation":

| nested model (now) | flat model |
|---|---|
| host frame `[lr, entry RSP]`, found by walking the host stack | shadow-stack entry `(guest ret addr, guest RSP, fiber id, code gen, host continuation)` |
| liveness: frame chain from caller reaches recorded host sp | liveness: a shadow entry with the recorded guest RSP/identity/generation exists; longjmp pops the shadow stack to it (and clears the cache if it is gone: slow path) |
| EH target = first frame with `E > installed RSP` | EH target = first shadow entry with `guest RSP > installed RSP` (same predicate); pop to it, then `br` the pad |
| `recomp_resume_guest` moves `sp` | no host `sp` change: host `sp` is flat; set the shadow stack pointer instead and `br` host continuation / pad |
| setjmp record holds host sp + host lr | setjmp record holds a shadow-stack depth/entry id + the host continuation address (as a shadow entry it is just the call's continuation) |
| HLE-callback boundary = frame chain ends | boundary = a marker shadow entry pushed by `recomp_enter_guest`; walking past it is the same fatal diagnostic |

Requirements for the flat-stack work: (1) the shadow entry must carry the entry guest RSP (it does in §3.3)
and the pad/continuation lookup must stay deterministic: keep the `__nps5_pads` table and make pad pcs
valid dispatch targets for the slow path, so that a discarded shadow stack still resumes correctly via the
dispatch table; (2) "never match by return PC alone" - the generation counter in the jmp_buf plays that
role for setjmp and the RSP predicate for EH; (3) a longjmp/EH to a frame whose shadow entry was
discarded (cache overflow) must fall back to dispatch by guest pc + guest RSP, so the pad table and
setjmp's return address (already in the jmp_buf) are the sources of truth, not the host continuation;
(4) fibers: shadow stack per fiber, `jmp_buf` records tagged with fiber identity; (5) the crash walker
(`guest_call_chain`) should read the shadow stack instead of the host frame slots; (6) the host `bl`
frame is gone, so `stp x30, x26` disappears from the prologue (free for S2 benchmarks either way).

## 8. Known gaps

* Signal mask save/restore, `sigaltstack`, and x87/xmm state across longjmp are not modelled.
* Exceptions and longjmp across HLE callbacks (qsort/bsearch comparators, thread start) are fatal by design.
* `_Unwind_ForcedUnwind`/pthread_cancel unwinding and `__cxa_call_unexpected` are untested.
* Libc++abi variants that implement the context install differently (libgcc `uw_install_context`,
  libunwind built with a different register file) need their own `jumpto` binding; the target-frame logic
  is shared.
* Pad table covers LSDA functions only; a pad in a function with no LSDA record fails with a clear message.
