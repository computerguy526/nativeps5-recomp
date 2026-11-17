# Lean HLE calls (branch m3-leanshim)

Every guest -> host shim call used to go through `_recomp_call_shim`: store the 16 guest GPRs and xmm0-7 to the
context, build a frame, run the entry hook (the default HLE call counter), set FPCR, call the shim (which reads its
arguments from the context and writes RAX back into it), run the exit hook, reload GPRs and xmm0-7, and then do what a
guest RET does. About 100 instructions, 12-14 ns on an M4 Pro. A hot libc call (`memcpy` of 64 bytes, `malloc`,
`free`, `strlen`) costs less than that itself, and the native UE allocator replacement (branch m3-mallochle) pays the
bridge on both halves of every `Malloc`/`Free` pair.

The lean path serves leaf host functions that are marked safe. It maps the System V argument registers straight to
AAPCS64 argument registers, stores only what a signal handler or a crash report needs, and brings the result back in
RAX. Everything else keeps the full bridge.

## Numbers

Machine: M4 Pro, shared (load 4-8 during the runs), best of 3 / median of 3 runs, flat call model, the same translated
binary run with `NPS5_LEAN=0` (full bridge, as before this change) and by default (lean). `tools/shimbench.sh`;
every call is a guest -> host shim call through the libc import; `_direct` rows are the same work done by guest code
(a one-class free list, an unrolled copy), i.e. the floor a guest-side replacement could reach.

| case (ns per call) | full bridge | lean bridge | direct (guest code) |
|---|---|---|---|
| malloc + free (per call) | 13.4 | 6.8 | 1.6 |
| memcpy 64 B | 12.7 | 5.6 | 1.7 |
| memset 64 B | 14.0 | 5.7 | |
| strlen | 13.2 | 7.9 | |
| abs (not lean; control) | 11.3 | 11.0 | |

The UE `FMallocBinned3` replacement (`tools/uemalloc_test.sh`, then `tools/uembench.sh`): one engine `Malloc` +
`Free` pair through the vtable, 2 M iterations, median of 5: **31.7 ns with the full bridge, 13.0 ns lean** (2.4x).
The allocator itself (`uem_alloc` with a thread-cache hit, `uem_free`) is about 3 ns of that; the rest is two bridges
and two virtual calls.

The non-lean shims pay three extra instructions (the mode check in the stub) and nothing else.

## Design

```
guest code                    lean stub (guest section)                 _recomp_call_lean (host section)
bl _fn_0x...  ------------->  call entry (push shadow entry)
                              w7 = g_nps5_lean_mode; 0 -> full bridge
                              x16 = ctx (TCB slot)
                              x0..x(n-1) = rdi, rsi, rdx, rcx, r8, r9
                              x6 = ctx, x17 = lean function
                              ctx->rip = guest address of the stub
                              16 GPRs -> ctx, x29 -> ctx->shadow_sp
                              b _recomp_call_lean  ------------------>   x30 = shadow top's host continuation
                                                                         x29 = host_boundary record (frame link)
                                                                         blr lean_<name>(a0..a5, ctx)
                                                                         x19 (rax) = x0
                                                                         x8-x15 <- ctx
                                                                         x29 <- ctx->shadow_sp ; x26 += 8
                              Lshim_return (shared with the full bridge): the usual RET check <------- b
```

* The stub is in `__recomp_guest` up to and including the branch to `_recomp_call_lean`; everything after the GPR store
  is host code, so a signal that arrives from then on finds the guest registers in the context (the same rule the full
  bridge follows, `recomp_pc_in_guest_code`).
* A lean function is `uint64_t lean_<shim>(uint64_t a0, ..., uint64_t a5, GuestContext *ctx)` (`NPS5_LEAN_FN`,
  `core/nps5_lean.h`). Arguments the function does not use hold garbage. It returns the new RAX. A `void` function
  returns `ctx->rax` (which is the RAX the guest called with), exactly what `SHIM_RETURN` leaves.
* The stub moves only as many argument registers as the function reads (`LeanShim.Args`).
* The full shim (`void shim_<name>(GuestContext *)`) stays: it is the fallback `NPS5_LEAN=0` selects, the reference of
  the differential tests, and it is what the generated `guest_modules.c` falls back to (a weak `lean_<name>` calls
  `shim_<name>(ctx)` and returns `ctx->rax`) in builds that do not define the lean function.
* The nested call model has no lean bridge: its stubs are the plain ones.

### Marking a function lean

* Generated thunks: set `Lean: true` in the thunkgen descriptor (`pkg/thunkgen/libkernel.go`, e.g. `leanMutexOp`).
  `thunkgen` rejects, at generation time, a lean descriptor with a float, struct-array or pointer-slot argument,
  more than six guest arguments, `Pre`/`Post` statements, `Indirect`, or `RetNoReturn`. It generates
  `lean_<shim>` next to the thunk, with the same marshalling (`NPS5_G2H`, sign extension of `RetInt`, `RetPtr`
  translation) reading from the parameters instead of the context.
* Hand-written shims: add the shim to `leanHandwritten` in `pkg/emitter/lean.go` (name, argument count) and define
  `NPS5_LEAN_FN(<name>)` in the runtime. The link fails without the definition (or the weak fallback above is used).
* Eligible: never enters guest code (no callback, no `recomp_call_*`), never switches the execution context (fibers,
  `longjmp`), integer/pointer arguments only (at most six), result in RAX only, no dependence on the guest FPCR
  (RMode/FZ stay as the guest set them), an ordinary AAPCS64 function. It may block (a mutex wait): the guest
  registers are already in the context, which is all the full bridge offers a blocked thread as well.
* Marked now: `memcpy memmove memset strlen strcpy strncpy strcmp` (kernel/syscalls, lean entries in
  `core/nps5_lean.c`), `malloc calloc realloc free aligned_alloc memalign malloc_usable_size`, `memchr memcmp bcmp bzero
  strchr strrchr strncmp strcasecmp strncasecmp strcat strncat strnlen toupper tolower` (libc module),
  `pthread_mutex_lock/trylock/unlock` and `scePthreadMutexLock/Trylock/Unlock` (generated), and the UE allocator shims
  `ue5_malloc_{malloc,realloc,free,quantize,getsize,nop}` (the constructor stays full: it runs once).

### Switches

| | |
|---|---|
| `NPS5_LEAN=0` (run time) | every lean stub takes the full bridge: the escape hatch. One byte, `g_nps5_lean_mode` |
| `NPS5_LEAN=0` (translate time) | no lean stubs are emitted at all |
| `NPS5_LEAN_HOOKS=1`, `NPS5_PERF=1`, `NPS5_TRACE_HLE=...` | mode 2: lean calls run the entry and exit hooks like full ones (perf attribution per shim, HLE trace). They appear as `lean_<name>` |
| `NPS5_LEAN_CHECK=1` | mode 3: hooks plus the register-preservation checker below |

**Hooks.** The entry hook (`g_nps5_hle_hook`) is installed by default; it counts every HLE call in a hash table with
atomics (`summary.txt`'s "top HLE calls", `g_hle_last` for the hang report). Running it would cost as much as the
bridge, so the default lean path skips it: lean shims are not counted and are not the "last HLE call" of a thread in a
hang report. Everything that asks for the hooks (the perf trace sets `g_nps5_hle_exit_hook`, the HLE trace) calls
`nps5_lean_require_hooks()` and switches the process to mode 2, so the perf exit hook added by m3-syoperf keeps working
for lean calls. Crash reports are unaffected: `ctx->rip` is stored by the lean stub like the full one.

## The register-preservation argument

Guest registers are pinned in host registers while guest code runs (`asm_trampoline.go`):

| guest | host | AAPCS64 class | what the lean call does |
|---|---|---|---|
| rax | x19 | callee-saved | the callee keeps it; the bridge then overwrites it with the result (`mov x19, x0`) |
| rbx, rcx, rdx, rsi, rdi, rbp | x20, x21, x22, x23, x24, x25 | callee-saved | preserved by the callee. rcx, rdx, rsi, rdi are copied to x3, x2, x1, x0 as arguments first, and are dead after a System V call anyway |
| rsp | x26 | callee-saved | preserved; the bridge adds 8 (the return address pop) |
| FS base | x27 | callee-saved | preserved |
| memory base | x28 | callee-saved | preserved (see "x28" below) |
| r8-r11 | x8-x11 | caller-saved | clobbered by the callee. r8, r9 are copied to x4, x5. They are dead after a System V call, but the bridge reloads them from the context anyway, so lean leaves exactly the register state the full bridge leaves |
| r12-r15 | x12-x15 | caller-saved | **not covered by the AAPCS callee-saved set, and callee-saved in System V.** The stub stores them to the context, the bridge reloads them. This is the one place where the lean path has to do work for correctness |
| xmm0-15 (low halves) | v0-v15 | v0-v7 caller-saved; low 64 bits of v8-v15 callee-saved | System V: every xmm register is caller-saved, so the guest does not expect them to survive. A lean function takes and returns no vector, so xmm0-7 are neither stored nor reloaded. (`d8-d15` preserved by the AAPCS callee is more than needed.) |
| ymm upper halves | v16-v31 | caller-saved | dead after a call (SysV; vzeroupper rules), not preserved |
| flags | NZCV (+ lazy PF/AF/DF bytes in the context) | clobbered | undefined after a System V call; nothing is preserved. DF is never touched by the bridge; lean functions run with DF as the guest has it, which the ABI requires to be clear at a call |
| MXCSR RMode/FZ | FPCR | | not touched: the callee runs with the guest's rounding mode. Lean functions are integer-only; a function that does floating-point math must not be lean (it could see a rounding mode the full bridge hides by resetting FPCR) |
| x30 | | | scratch in guest code (the call entry saved it on the shadow stack) |
| x29 | | callee-saved | the shadow stack pointer in guest code; see below |
| x0-x7, x16, x17, x18 | | | scratch in guest code (x18 never touched) |

So the AAPCS64 callee-saved set (x19-x28, x29, d8-d15) covers rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, FS base, the
memory base and the shadow stack pointer, and a System V caller does not rely on more. It does **not** cover r12-r15
(host x12-x15 are caller-saved in AAPCS64 but callee-saved in System V), which are saved in the context by the stub
(that store is also what makes the register state visible to a signal handler) and reloaded by the bridge. r8-r11 are
reloaded too, to keep the full bridge's observable behaviour: `tests` compare every GPR.

A function that is not an ordinary AAPCS64 function (inline assembly that writes x19-x28, a context switch, a
`longjmp`) breaks this. That is what the checker is for.

### x28 (memory base, the bias)

x28 is `ctx->mem_base`, the host address of guest address 0 (`host = guest + bias` in base mode; identity mode
leaves it unused). It is callee-saved, so lean functions cannot change it, and a lean function reaches guest memory
the way every shim does, through `NPS5_G2H(ctx, p)` / `ctx->mem_base`, with the context passed as the last argument
(`x6`). The checker compares x28 before and after the call. Nothing in the bridge reloads it from the context
(the full bridge does, because it cannot assume the callee preserved it, and a context switch may change it: a lean
function must not switch contexts).

### x29 (the shadow return stack)

In guest code x29 is the shadow return stack pointer (flat call model, `docs/m1/flatstack.md`). The lean stub
stores it to `ctx->shadow_sp` (a signal handler's guest code pushes below it, `recomp_shadow_sync_from_signal`) and
the bridge replaces it by the frame record of the innermost `recomp_enter_guest` frame (`ctx->host_boundary`) with the
guest call's host continuation as the return address, so a frame-pointer walk from the lean function (the crash
reporter, the perf sampler) finds the host frames and not the shadow stack. After the call the bridge reloads x29 from
`ctx->shadow_sp`; a lean function must not move it. The return goes through `Lshim_return`, the full bridge's own
epilogue: pop the guest return address, compare it with the shadow top, take `_recomp_ret_slow` on a mismatch. A tail
jump to a shim stub (a PLT stub's `jmp`) works the same way as with the full bridge because the continuation comes
from the shadow top, not from x30.

### Flags

x86 flags are dead after a System V call, so the bridge saves and restores none: NZCV is whatever the callee left.
The lifter keeps PF/AF/DF as lazy bytes in the context (`recomp_context.h`); the bridge does not touch them. A lean
function must not read them (none does).

### Signals arriving inside a lean call

The stub stores the 16 GPRs, `ctx->rip` and `ctx->shadow_sp` before the pc leaves `__recomp_guest`, so from the first
host instruction on, the context holds the guest state the signal handler needs: `host_sigusr1_handler` builds the
Orbis ucontext from `ctx` (rax .. r15, rsp, rip), runs the guest handler on `ctx`, and restores the saved CPU state.
The registers that are live in x19-x28 survive the handler because every host frame between the handler and
`recomp_enter_guest` is AAPCS64; r12-r15 are reloaded from the context after the call, which the handler restored.
What the handler sees differs from a full-bridge call only in xmm0-7 (stale, i.e. the values at the call; dead by the
ABI) and in `ctx->rax` being the RAX at the call (as in the full bridge, where the shim has not written it yet).
SIGSEGV/SIGBUS from a lean function (a bad guest pointer in `memcpy`) are reported by the crash handler from the
context like a fault in a full shim. In checked mode `ctx->lean_active` is cleared around a guest signal handler:
that handler enters the guest legitimately, not the lean function.

A signal in the stub itself is the same case as for the full bridge: before the branch the pc is in guest code and the
live registers are the guest registers (x0-x7/x16/x17 are scratch there); after it the context is complete.

### errno and TLS

errno is the guest TLS slot `fs_base + 0x100` (`set_guest_errno`), written through `ctx->mem_base + ctx->fs_base`.
`ctx->fs_base` is the thread's FS base and does not change while the thread runs guest code or a lean function; `x27`
(preserved) holds the same value. The context passed as `x6` is the one in the TCB slot `[x28 + x27 + 0x70]`, i.e. the
running execution context of the calling thread (a fiber's own, while a fiber runs), so `set_guest_errno` and the heap
(`malloc` failing with ENOMEM) write the calling thread's slot. The lean path takes no lock for this and has no
thread-local state of its own: two threads in lean calls share nothing but what the function itself shares (the libc
heap lock, the UE allocator's atomics). The boundary test runs three guest threads calling a lean shim 40000 times each
and checks each thread's own errno, FS base and registers.

## The checker (`NPS5_LEAN_CHECK=1`, mode 3)

A function wrongly marked lean can break the guest silently (a clobbered rbx shows up three frames later). Mode 3 runs
every lean call through the slow path of `_recomp_call_lean`, which

* snapshots x19-x28 and d8-d15 before and after the call and compares them (`nps5_lean_check_regs`),
* checks that x29 is still this frame's record and `ctx->shadow_sp`, `ctx->host_boundary` are unchanged,
* sets `ctx->lean_active` for the duration of the call; `recomp_call_entry` (the one C entry into guest code, used by
  callbacks, `recomp_call_guest`, `nps5_libc_call_guest`) aborts with "lean HLE function ... entered guest code" while
  it is set,
* runs the entry and exit hooks like mode 2.

A violation prints the function, the register and the two values, then aborts (`nps5_lean_violation`). The
tests below include a shim that clobbers x19, x28, x29 and d9 and one that calls back into the guest.
Run a title with `NPS5_LEAN_CHECK=1` after marking a new function.

## Tests

| | |
|---|---|
| `go test ./pkg/emitter` (`TestLean*`, the boundary harness `testdata/boundary`) | stub shape; flat-only; and, in the real bridge: **lean vs full** random calls in modes 0-3 (6 argument registers, result, every GPR including r8-r11, rsp, FS/memory base, errno), **a SIGUSR1 handler in the middle of a lean call** (modes 1-3), **three guest threads** x 40000 calls, **a fault inside a lean function** (the crash report shows the guest rbx/rbp from the context), **the checker catches** a lean function that clobbers x19, x28, x29, d9, and one that enters the guest (modes: 3). Mutation check: dropping the r12-r15 reload fails the diff and signal cases |
| `tools/lean_unit.sh` | host differential test of **every lean-marked shim** (libc, memcpy family, mutex thunks) against its full shim with random arguments: same RAX, same guest memory, lean entry does not touch the context, the full shim changes only RAX and rsp; plus a heap test mixing full and lean `malloc/free/realloc/calloc/aligned_alloc/memalign` |
| `go test ./pkg/thunkgen` | generation, including the rejection rules |
| `tools/uemalloc_test.sh`, `tools/uembench.sh` | end to end with the replacement allocator, lean by default; Malloc+Free pair cost |
| `tools/shimbench.sh` | the numbers above |
| `tools/m1_run.sh` with `M1_EXTRA_ENV="NPS5_LEAN_CHECK=1"` / `"NPS5_LEAN=0"` | the whole corpus in the checked and in the full mode |

## Adding the malloc replacement (m3-mallochle)

Merged into this branch (clean merge). `shim_ue5_malloc_*` are lean (`Malloc`: this, Count, Align = a0, a1, a2;
`Realloc`: a1-a3; `Free`: a1; `QuantizeSize`: a1, a2; `GetAllocationSize`: a1, a2). They take no lock on the fast path
(thread cache), abort on out-of-memory, never enter the guest. Any other function replacement (`-hle-replace`) can be
lean the same way: it is a normal shim stub, so listing the shim in `leanHandwritten` and defining `lean_<name>`
is all it takes.

## Risks and limits

* Lean calls are invisible to the default HLE call counter and to the hang report's "last HLE call" (see Hooks).
  `NPS5_LEAN_HOOKS=1` or `NPS5_LEAN=0` brings them back.
* The guest FPCR is not reset around a lean call: no lean function may use floating point.
* xmm0-7 are not preserved across a lean call. The ABI allows it; hand-written guest assembly that keeps a value in
  xmm0-7 across a call to `memcpy` or `malloc` would break (it would break on the console's libc as well).
* A mutex lock that blocks is a lean call that blocks: it appears in the stall profiler as host code in
  `lean_pthread_mutex_lock`, not as `shim_pthread_mutex_lock`.
* The checker proves AAPCS conformance and absence of guest re-entry on the paths a run exercises; it cannot see a
  function that is lean-unsafe only on a rare path.
