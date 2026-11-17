# Stack privacy: which stack accesses may skip x86 ordering (`-tso-elide`)

Branch `m3-tsoelide`. Code: `pkg/disasm/stackpriv.go` (the analysis), `pkg/lifter/arm64_lifter_tso.go`
(`TSOElideMode`, `tsoAppliesMode`, `applyTSO`), `cmd/tsostat` (static counts), `tools/tsoelide_test.sh` (litmus),
`tools/tsobench.sh` (microbenchmark), `tests/ps5/src/t_tsoelide.c`, `tests/ps5/tsoelide/t_tsolitmus.c`.

## 0. What this work found

The task was to emit plain loads and stores for stack accesses that provably need no x86 ordering, and to keep
acquire/release everywhere else. The starting assumption, that stack accesses are ordered today, is wrong:

* The lifter has always emitted **every** RSP- or RBP-based scalar access plain, by a rule on the base register alone
  (`tsoApplies`, inherited from the C backend's `MEM_LD*`/`MEM_ST*`). On PPSA30552 that is 29.3 % of the accesses the
  TSO rewrite could order. There is no ordered stack access left to elide.
* That rule is not sound, and the violation is observable, not theoretical. A stack object whose address reaches
  another thread is shared memory; the owner still reads and writes it RSP/RBP-relative, hence unordered. The litmus
  tests of §7 show x86-forbidden outcomes on an M4 in every run of the default build: 2.4k-16k (owner reads), 1.3M-4.1M (owner writes) and 630-1,640 wrong responses of 1.5M (request/response) per run; 13.6 M forbidden outcomes in 5 runs. A request/response
  exchange that is correct on x86 returns wrong answers.
* An RBP-based access is also plain when RBP is a general register: a load through a heap pointer that happens to
  live in RBP is unordered. The title is built with frame pointers, so it has few of these (2,656, for example a byte
  copy loop through `[rbp]` at 0x5245880); in Greenhollow, built without, they are 4,278 of the 11,703 "stack"
  accesses (36.6 %).

So the deliverable changed direction: a pass that proves which stack accesses are thread-private, and two modes in
which only those are plain and every other access is ordered, stack-relative ones included:

| `-tso-elide` | plain accesses | stands on |
|---|---|---|
| `off` (escape hatch; was the default before §13) | every RSP/RBP-based access; the lowering as it was, byte for byte | nothing: unsound |
| `stack` | accesses the pass proves private; in a function that lets a frame address escape, only pushes, register save slots and the return address | A1-A5 (§2) |
| `stack-region` | `stack`, plus, in such a function, slots at a known offset entirely below the lowest escaping address | A1-A6 |

Both modes remove the forbidden outcomes (0 in every run). They make more accesses ordered than today, not fewer,
and cost CPU time (§8). §9 says what the choice is.

## 1. What x86 guarantees and what the translation must keep

x86-TSO: each thread's stores enter a FIFO store buffer and reach memory in program order; a load sees the thread's own
buffered stores, else memory. Between two accesses of one thread, another thread can observe every order except
store→load (a store may still be buffered when a later load of another location reads memory). Load→load, load→store
and store→store are kept.

ARM keeps none of them for plain accesses. The lifter restores the three with acquire loads (LDAPR/LDAPUR) and release
stores (STLR/STLUR) (`arm64_lifter_tso.go`, head comment): an acquire load is ordered before every later access of the
thread, a release store after every earlier one. A plain access has neither property.

An access may be plain when no execution can tell: when the location it addresses is, at that moment, accessed by
this thread only. Call such a location **private**. The claim proved in §4:

> If every plain access of every thread addresses a location that is private at that point of the execution, each
> ARM execution of the translated program has the outcome of some x86-TSO execution of the guest program.

What must hold even for private data:

* *Same-thread semantics.* A load of a private location returns the thread's latest store to it. Every architecture
  guarantees this for a single thread (store-to-load forwarding is not observable as a reordering), so plain accesses
  keep it.
* *Precise asynchronous entry.* A guest exception handler (SIGUSR1, `sceKernelRaiseException`) runs on the interrupted
  thread. The host delivers the signal at an instruction boundary with all earlier stores of the thread visible to
  the thread itself, so the handler reads the frame it interrupted as x86 would show it (it is the same thread). The
  red zone below RSP is skipped by the runtime as before. `t_tsoelide` covers a handler that reads the interrupted frame.
* *Publication.* A private location can become shared later (its address is stored where another thread finds it).
  The plain accesses made before that must be ordered before everything the other thread does with the location. §4
  shows that the store that publishes the address provides this, because it is itself a release store.

Out of scope, unchanged in both modes: vector, MMX and x87 accesses and string instructions are never ordered (the
existing policy, `tsoApplies`); locked instructions and XCHG are atomics. RIP-relative and absolute addresses are
shared memory and stay ordered: no global is elided.

## 2. Assumptions

Nothing about a stack can be proved for arbitrary machine code: any code that knows RSP can compute any stack address.
The proof is relative to five assumptions about the guest (six for `stack-region`). Each is something the compilers that produce PS5 code
guarantee; each names what breaks it.

* **A1, provenance and lifetime.** A location in the frame of activation F is accessed by code outside F's translated
  function only through a value derived from an address that F itself put into a register (`lea`, `mov r, rsp`,
  arithmetic on those) and that then left F's registers; and only while F is live. *Not covered:* code that reconstructs
  a caller's frame address from its own stack pointer (LLVM's x86 machine outliner refuses RSP-relative instructions;
  hand-written assembly could do it), a stack scanner that walks another thread's stack through a saved context
  (conservative collectors stop the world through the kernel first), and a dangling pointer to a returned frame used
  by another thread while the stack is reused.
* **A2, objects.** An address derived from a frame address by adding an index or offset stays inside the source-level
  object it was derived from, and no source-level object overlaps a register save slot (a slot written by PUSH of a
  callee-saved register that still holds its entry value) or the return address. *Not covered:* out-of-bounds stack
  indexing, which is undefined behaviour in the source.
* **A3, calls.** A callee returns with RSP and the callee-saved registers (RBX, RBP, R12-R15) as they were, and treats
  the caller's RBP as an opaque value to save and restore: it does not dereference or publish it. *Not covered:*
  private calling conventions that pass a frame address in RBP. (A frame address in any *other* register at a call,
  callee-saved or not, is counted as passed to the callee. Exempting RBX and R12-R15 as well would change 30 of the
  title's 258,974 functions.)
* **A4, control flow.** A block of a translated function is entered at run time only along the edges the
  disassembler recovered, or by a transfer that arrives with the state those edges establish: a return to a return
  site (also from `longjmp`, which restores RSP and the callee-saved registers of the `setjmp`), or an indirect jump
  taken with the frame released. *Detected rather than assumed:* an indirect jump with a live frame and no recovered
  table makes the function unanalysable (nothing private), and so does a block no recovered path reaches.

* **A5, red zone.** Code lowers RSP only when nothing below it is live: a function that keeps data in the red zone
  does not push, call or allocate while that data is in use. (This is how the red zone is defined: anything below
  RSP is lost at the next push.) *Not covered:* hand-written code that addresses memory below RSP as a fixed scratch
  area across pushes.

  *Extension (`-redzone-leaf`, docs/m3/lift2.md section 2):* in a function entered by a call, the red zone below the entry
  stack pointer counts as allocated from the first instruction, since the call is what ended the caller's red zone; a
  function that a direct branch from code with an unlowered stack pointer can enter does not get it.

* **A6, direction (mode `stack-region` only).** A frame address that leaves the registers is used by whoever receives
  it with non-negative offsets only: it is the address of the lowest byte they access. Then memory below the lowest
  escaping address of a function is unreachable for others, and slots there stay private although the function is
  escaped. This is weaker than A2: A2 lets a callee move inside the object in both directions, and C code does that
  (`container_of` on a member address; a wait node embedded in a stack object and linked into a list). *Not covered:*
  exactly that, an escaped interior pointer whose receiver steps back to the enclosing object, when the enclosing
  object is not also escaped by its own address. The compiler puts spills and scalar locals below the address-taken
  objects when stack protectors are on (LLVM's layout for `-fstack-protector-strong`, which the title uses), which is
  why the rule has an effect at all; nothing guarantees that layout.

## 3. The analysis

`disasm.AnalyzeStackPrivacy(fn)` runs on every decoded function (next to the flag liveness passes) and sets
`Instruction.StackPriv`. The zero value means "not analysed, nothing private", so code that builds instructions by
hand gets the conservative lowering.

**Unit and entry state.** The unit is the translated function. That may be a fragment that is entered by a jump with a
live frame: a cold part, a tail shared by several functions (`splitSharedTargets` makes those entries), a landing pad
(seeded as an entry), a tier-1 fragment. So nothing is assumed about memory at the entry. With R the stack pointer
at the entry of the analysed code, only memory in **[RSP, R)** can be private: memory this activation allocated itself
by lowering RSP below R. Never private: the return address and incoming stack arguments (at or above R; for a
fragment that is the enclosing function's frame), and memory below the current RSP of code that has not lowered RSP
yet (a fragment of a leaf function shares that red zone with the code that jumped to it). Once the analysed code has
lowered RSP below R on every path (`push rbp` is enough), the 128 bytes below the current RSP are its own by A5 and
are treated like allocated memory: this is the frame of a leaf function in a frame-pointer build.

**State** per program point, a forward dataflow over the recovered graph:

* per GPR one of `clean` (not derived from a frame address of this activation), `exact(c)` (= R + c), `own` (points
  into an object in [RSP, R), offset unknown) or `mixed` (may be derived from a frame address, nothing else known);
* `low`: [R+low, R) is known to be allocated (with an exact RSP it is the RSP offset; `and rsp, -N` and
  `sub rsp, imm` keep or extend it when RSP is no longer exact; an unknown write to RSP loses it), and whether RSP
  has been below R on every path (`lowered`);
* the set of callee-saved registers that still hold their entry value, and the set of register save slots (a slot
  written by `push` of such a register), both intersected at joins.

`lea`, `mov`, `add`/`sub` with an immediate keep exact offsets; adding a `clean` register or scaling an index turns an
exact in-frame address into `own` (A2); a load, a call result and a caller-saved register after a call are `clean`;
anything else that reads a tainted register gives `mixed`. Implicit register writes are tabled (MUL/DIV, CPUID,
CMPXCHG, string instructions, PCMPxSTRI ...); an integer instruction the table does not know clobbers RAX, RCX and RDX
as well as its operands.

**Escape.** A function is *escaped* when a tainted value (`exact` below R, `own` or `mixed`) may leave the registers
anywhere in it:

1. stored: `mov [m], r`, `push r` (`push rsp` always), `xchg`, `cmpxchg`, any instruction with a memory or vector destination that reads
   the register, `stos` with a tainted RAX;
2. passed: live in any register except RSP and RBP at a `call` or `syscall`;
3. read by an instruction the pass does not model.

Comparing a frame address or using it as an address component is not an escape. A value loaded from memory is `clean`:
a frame address can only be in memory after rule 1, and then the function is escaped anyway.

The flag is per function, not per program point. "Before the first escape" would be sound by the publication argument
of §4 only if the code before the escape could not run again after it, and the graph cannot show that: the
continuation table resumes a function at any return site, which is what `longjmp` uses. `t_tsoelide` has the case (a
loop body re-entered through `setjmp` after its frame escaped).

**Result.** An explicit memory operand is private (`StackPrivMem`) when the function is neither escaped nor
unanalysable, there is no segment override, and the address is `own`, or `exact(c)` with [c, c+size) inside [R+low, R) or, when `lowered` and RSP is exact,
inside the red zone [RSP-128, RSP).
The implicit load of `pop` and `leave` is private (`StackPrivPop`) when its slot is a register save slot, or under the
same conditions as an explicit access. Everything else is not private.

**Region rule (`stack-region`).** The pass also records the lowest offset among the escaping addresses of a function
(`StackPrivInfo.EscapeLow`; minus infinity when a value of unknown offset escapes: an `own` or `mixed` value, anything
an unmodelled instruction reads). In an escaped function an access is region-private (`StackPrivMemRegion`,
`StackPrivPopRegion`) when it would be private without the escape, its address is an exact offset with no index, and
it ends at or below `EscapeLow`. The bound is over the whole function, so it does not depend on the order of
execution.

`push`, `call`, `ret` and `pushf`/`popf` keep their plain stack access in both modes: a pushed slot is new memory
below RSP whose address cannot exist yet (A1), and the return address is a save slot (A2).

## 4. Soundness

Fix an ARM execution of the translated program. Call an access *ordered* if it was emitted as acquire/release or as an
atomic, *plain* otherwise.

**Lemma 1 (plain accesses address private locations).** Let a plain access of thread T address location ℓ in mode
`stack`. The pass marked it private, so in T's current activation F: ℓ lies in [RSP, R) (by the `low` invariant, which
holds on every recovered path, and by A4 on every executed path), or in the red zone below RSP after F lowered RSP,
or is reached through an `own` pointer, which by A2 addresses an object in [RSP, R); and F is not escaped. ℓ was dead
stack when F first lowered RSP: it is below R, and the code that ran before F's entry has nothing live below its RSP
except a red zone, which by A5 is dead once F pushes. Before F lowers RSP nothing below R is treated as private. By A1 another
thread can only access ℓ through an address F materialised and let out of its registers, and F never does (not
escaped; values in callee-saved registers and RBP come back untouched by A3). A register save slot is private by A2
and A1 directly: no object overlaps it, so no in-bounds pointer reaches it. Hence no other thread accesses ℓ during F.
The same holds for the slots written by `push` and `call`.

**Lemma 2 (publication).** A location ℓ that was accessed plain in activation F may be accessed by another thread
only after F has ended (A1), that is, never, or as a different object of a later activation, which the lemma then
covers anew. Within one function, mode `stack` has no "private, later shared" location: the escape flag is per
function. Across functions the pattern exists: the caller writes outgoing stack arguments with plain stores, and the
callee may take the address of such an argument and publish it. The callee's publishing store is a release store (its
target is not private in the callee: a store to a private slot does not publish), or a locked instruction, or the
address travels through an HLE call, which synchronises through the host kernel. All of these order every earlier
access of the thread, plain ones included, before the point where the other thread can obtain the address with an
acquire load. So the other thread sees the caller's plain stores as x86 would (they precede the publishing store in
the thread's store buffer). After the callee returns the argument object is dead (A1).

**Theorem.** Erase the plain accesses from the execution. What remains are ordered accesses and atomics, for which the
existing argument holds unchanged (acquire and release constrain the ordered accesses among themselves whatever plain
accesses sit between them): there is an x86-TSO execution E with the same values for them. By Lemma 1 a plain load
reads a location only its own thread writes, so on ARM it returns the thread's latest earlier store to it, exactly what
store forwarding returns on x86. Insert the plain accesses into E at their program-order positions; no other thread's
access conflicts with them (Lemma 1), and where a location is handed over, the hand-over is ordered (Lemma 2). The
result is an x86-TSO execution of the guest with the outcome of the ARM execution.

Reordering a private access against shared ones is unobservable for the same reason on both sides: the only thread
that could observe the order is the one that executes it, and for it ARM's single-thread semantics is sequential.

For `stack-region`, Lemma 1 gets one more case: ℓ at a known offset below every escaping address of F. By A6 the
receivers of F's addresses reach only memory at or above the address they got, so not ℓ; the rest is unchanged.

Why `off` is not covered by this: its plain set is "base register is RSP or RBP", and Lemma 1 fails for an escaped
object, for a stack argument whose address the callee published, and for an RBP that is not a frame pointer.

## 5. Fragments, tier 1, hot/cold splits

The entry-state rule of §3 is what makes the analysis independent of how a translated function is reached. A cold
part or shared tail accesses the enclosing function's frame at offsets at or above its own entry RSP: ordered. What it
allocates itself is analysed like any frame. Escapes that happened in the enclosing function before the jump concern
only memory above R. The converse case, a fragment that leaks an address of the enclosing frame and returns control to
the enclosing function's code, cannot occur in this translator for a jump: a direct branch out of a function goes to
another function's entry, and the blocks after it belong to that translation, which treats the frame as foreign. It can
occur for a call (an outlined function), which is A1.

Tier-1 fragments are decoded by the same disassembler, so they are analysed the same way. The mode is recorded in the
tier-1 manifest (`tso_elide` in `tier1.json`) and `fragment-serve` lifts in that mode; the fragment cache version hash
includes it. `lifter_manifest.json` has `tso_elide`; the chunk cache key (`ConfigKey`) gains ` elide=stack` in that mode
and is unchanged for `off`.

## 6. Signals, setjmp/longjmp, exceptions

* **Guest exception handlers** run on the interrupted thread on its own stack below the interrupted RSP. They are
  function entries: their frames are analysed like any other. Reading the interrupted frame from the handler is a
  same-thread access (§1). A handler that hands the interrupted frame to another thread is outside A1 (the address
  comes from a context, not from the frame's function).
* **setjmp/longjmp.** `longjmp` restores RSP and the callee-saved registers saved by `setjmp` and continues at the
  `setjmp` return site, with the frame contents as they are now. The register facts at a return site (A3) are those of
  the `setjmp` call, which is what `longjmp` restores. The escape flag being per function covers code that runs again
  after an escape. Volatile locals modified between the two keep their values in memory in both modes.
* **C++ exceptions.** Landing pads are function entries of their own, so a pad starts with no knowledge: accesses to
  the frame it cleans up are at or above its entry RSP and ordered. Callee-saved registers restored by the unwinder
  are `clean` there, which is the conservative reading.

## 7. Tests

| test | what | result |
|---|---|---|
| `pkg/disasm/stackpriv_test.go` (22 cases) | leaf frame, derived pointer, stack argument, red zone, return address, index; escape by call, by store, through a vector register; frame pointer and `leave`; RBP as a general pointer; pop of an escaped slot; callee-saved register across a call; realigned frame; joins; depth mismatch; unresolved indirect jump; indirect tail call; implicit register writes | pass |
| `pkg/lifter/arm64_tsoelide_test.go` `TestTSOElideStackShapes` | per instruction and address mode (base, identity): the privacy bits decide; push stays plain, pop/leave ordered unless private, vector accesses untouched | pass |
| `TestTSOElideOffIgnoresBits` | `off`: the text does not depend on the bits; chunk cache key unchanged for `off`, different for `stack`; `Clone` keeps the mode | pass |
| `TestTSOElideStackExec` | about 130 stack sequences (every size, push/pop, `leave`, `pop [rsp+8]`, `push [rsp]`, RMW, indexed, RSP at 8 alignments inside a 16-byte block) plus the memory corpus, executed on the host: `off` vs `stack` with nothing private vs `stack` with everything private give identical registers, flags and memory | pass |
| `tests/ps5/src/t_tsoelide.c` (in `tests.list`, both import variants) | stack buffer handed to a thread and back, `longjmp` into a function whose frame escaped after the `setjmp` (global and stack `jmp_buf`), guest exception handler reading the interrupted frame (200 raises), pop of a slot a callee wrote, red-zone leaf, RBP as a pointer, stack arguments whose address the callee takes, private frame, realigned frame, VLA | pass in `off` and `stack` |
| `tools/tsoelide_test.sh` (`t_tsolitmus.c`) | message passing through an escaped stack object with the owner as reader and as writer, and a request/response exchange; counts x86-forbidden outcomes | `stack`: 0 in 5 runs (also `stack-region`: 0 in 5 runs); `off`: 2.4k-16k (owner reads), 1.3M-4.1M (owner writes) and 630-1,640 wrong responses of 1.5M (request/response) per run; 13.6 M forbidden outcomes in 5 runs |
| `tools/m1_run.sh` | whole guest corpus | `off`: 42/42; `stack` (`M1_XLATE_FLAGS="-tso-elide stack" M1_WORK_TAG=elide`): 42/42 (run before the red-zone and region rules were added: **rerun**; `stack-region` **not run**) |
| FEX oracle (`tools/fex_oracle.sh`, 2,291 tests) | `off` against the committed baseline; `stack` (`-tso-elide stack`) against the `off` run | **not finished** (stopped: the runs were stalled by the title gate; commands in §11) |
| `tools/regress.sh --full` | gate, default mode | **not run** (§11) |
| byte identity | Syobon translated by the base commit and by this branch with `-tso-elide off` | all 53 `code_*.s` identical (same SHA-1 over their concatenation); `lifter_manifest.json` gains `"tso_elide": "off"`; lowering id and tier-1 version differ as for any lifter source change (checked at 590532b, before the later analysis-only commits) |

The litmus tests are not in `tests.list`: they are racy by design and `off` fails them.

## 8. Numbers

### Static, PPSA30552 eboot (`cmd/tsostat`; counts only, 258,974 functions reachable from the static seeds, 21.2 M instructions)

```
functions 258974, instructions 21188123
  escaped 76513 (29.5%), not analysable 3 (0.0%), blocks without a path from the entry 2
  with rsp/rbp-relative scalar accesses 93986, of those escaped 76397 (81.3%)
  first escape call             65439
  first escape pushed           6522
  first escape stored           4534
  first escape unmodelled use   18
  not analysable: enter                    1
  not analysable: unresolved indirect jump 2
orderable scalar accesses 5925675
  today (-tso-elide off): plain (rsp/rbp base) 1735548 (29.3%), ordered 4190127 (70.7%)
  -tso-elide stack:       plain 147806 (2.5%), ordered 5777869 (97.5%)
  -tso-elide stack-region: plain 361123 (6.1%), ordered 5564552 (stack plus slots below every escaping frame address)
    rsp/rbp base:   stays plain 147754 (8.5%), becomes ordered 1587794 (91.5%)
      not-stack        2656 (0.2%)
      escaped          1355419 (78.1%)
      poisoned         1714 (0.1%)
      above-entry-rsp  4408 (0.3%)
      red-zone         29 (0.0%)
      inexact          10262 (0.6%)
      below-escapes    213306 (12.3%)
    other base:     becomes plain 52 (0.0%), stays ordered 4190075
      not-stack        4141674 (98.8%)
      escaped          43136 (1.0%)
      poisoned         1522 (0.0%)
      above-entry-rsp  1299 (0.0%)
      inexact          88 (0.0%)
      segment          2345 (0.1%)
      below-escapes    11 (0.0%)
lowering (host instructions; acquire loads; release stores; fenced split accesses are in both paths)
  off           insts 79448028 (+0.00%)  ldapr/ldapur 3778438 (+0.0%)  stlr/stlur 1633373 (+0.0%)
  stack         insts 93995713 (+18.31%)  ldapr/ldapur 5089104 (+34.7%)  stlr/stlur 2899079 (+77.5%)
  stack-region  insts 91936088 (+15.72%)  ldapr/ldapur 4872020 (+28.9%)  stlr/stlur 2751305 (+68.4%)
pop/leave loads 874714: private or save slot 869274 (99.4%), ordered under stack 5440
```

(`below-escapes` = private only under the region rule. Lowering counts come from `tsostat -lift`: every function lowered once per mode, nothing written.)

Reading: the title is built with frame pointers and stack protectors; four of five functions that touch their frame
pass a frame address to a callee (`lea rdi, [rbp-0x78]; call`), which is how C++ passes `this` of a local. Those
functions hold 83.5 % of today's plain stack accesses. The accesses the pass proves private that today's rule orders
(a derived pointer in a function with no escape) are 52: there is nothing to gain on that side.

Syobon (6,931 functions): 

```
functions 6931, instructions 657714
  escaped 1028 (14.8%), not analysable 11 (0.2%), blocks without a path from the entry 0
  with rsp/rbp-relative scalar accesses 1645, of those escaped 912 (55.4%)
  first escape call             931
  first escape pushed           16
  first escape stored           81
  not analysable: unresolved indirect jump 11
orderable scalar accesses 149815
  today (-tso-elide off): plain (rsp/rbp base) 55550 (37.1%), ordered 94265 (62.9%)
  -tso-elide stack:       plain 24676 (16.5%), ordered 125139 (83.5%)
  -tso-elide stack-region: plain 27041 (18.0%), ordered 122774 (stack plus slots below every escaping frame address)
    rsp/rbp base:   stays plain 24676 (44.4%), becomes ordered 30874 (55.6%)
      not-stack        279 (0.5%)
      escaped          25201 (45.4%)
      poisoned         322 (0.6%)
      above-entry-rsp  860 (1.5%)
      red-zone         60 (0.1%)
      inexact          1787 (3.2%)
      below-escapes    2365 (4.3%)
    other base:     becomes plain 0 (0.0%), stays ordered 94265
      not-stack        92132 (97.7%)
      escaped          873 (0.9%)
      poisoned         1248 (1.3%)
      inexact          12 (0.0%)
pop/leave loads 19238: private or save slot 19010 (98.8%), ordered under stack 228
```

### Run time

Microbenchmark, `tools/tsobench.sh 3` (min ns per iteration, M4, machine loaded; checksums equal in all modes):

| case | off | stack | stack-region | why |
|---|---|---|---|---|
| spill_private | 1.88 | 2.67 (+42 %) | 2.66 | a frameless leaf that keeps its locals in the red zone without ever lowering RSP: not provable for a fragment (§3); needs proof that the entry is a real function entry |
| spill_escaped | 1.87 | 2.71 (+45 %) | 1.94 (+4 %) | 4 loads + 3 stores per iteration; the escaped slot is above the two others |
| array_private | 309 | 441 (+43 %) | 447 | `[rsp+8*r8-0x80]`: the compiler folded a bias into the displacement, base+disp is outside the frame |
| array_escaped | 312 | 408 (+31 %) | 427 | |
| object_escaped | 8.29 | 8.18 | 8.10 | two calls per iteration dominate |
| fib36 | 1.32 | 1.33 | 1.29 | pushes, save-slot pops and returns stay plain |
| stack_args | 1.56 | 1.60 (+3 %) | 1.56 | |

Syobon, title screen, 40 s, `NPS5_PERF=1`, one run per mode (load average 9-13, the retail title running at the same
time; **repeat on a quiet machine**): flip-thread CPU per frame `off` 28.12 ms, `stack` 29.62 ms (+5.3 %),
`stack-region` 29.47 ms (+4.8 %); 30.2 fps at p50 in all three (the title's own cap). The frame budget is 33.3 ms, so
`stack` leaves about 3.7 ms. Syobon static: host instructions +14.0 % (`stack`), +12.8 % (`stack-region`).

Greenhollow: **not measured** (§11). PPSA30552: not run (rule); static only, +18.3 % host instructions with `stack`,
+15.7 % with `stack-region`, acquire loads +34.7 % / +28.9 %, release stores +77.5 % / +68.4 %. Most of the instruction
growth is the inline slow path of the alignment check (about 10 instructions per ordered 2/4/8-byte access, not
executed for aligned addresses; perf.md lists moving it out of line as open).

## 9. What to do with it

`-tso-elide stack` is the sound mode; `off` is faster and wrong for a class of correct x86 programs. The cost of
`stack` is ordered loads of frame slots in functions that pass a frame address anywhere, which is most non-leaf C++.
`stack` is the default (branch `m3-tsostack`, §13); `off` stays as the escape hatch and is byte-identical to the
lowering before this work. §10 is the plan for recovering the cost, §13 the validation and measurements.

## 10. Recovering the cost: what was tried, what is left (handoff)

Where the cost is: on the title 78-90 % of today's plain stack accesses are in functions that pass a frame address
to a callee. Precision inside non-escaped functions is exhausted (above-entry, red zone and inexact together are
0.9 %).

Done in this branch:

1. **Red zone of a function with a frame of its own** (A5). Syobon: 27 % → 44 % of stack accesses stay plain.
2. **Region rule** (`stack-region`, A6): slots below the lowest escaping address. Title: 8.5 % → 20.8 % stay plain.
3. **Callee-saved spills and returns never escape**: pushes, save-slot pops (99.4 % of the title's pops are plain) and
   the return address were plain from the start.

Measured and found small (`tsostat -ipa N`, `StackPrivOpts`; counts only, not wired into the translator):

4. **Escape through visible calls.** A callee that neither keeps nor forwards a pointer argument (the pass run with
   the argument registers tainted, a tainted return value or tail call counting as an escape, iterated from "no
   callee is clean") does not make the caller's address escape. Title: 22,899 of 259,123 functions qualify after 5
   rounds, and the share of stack accesses that stay plain moves from 8.5 % to 9.2 % (`stack`) and from 20.8 % to
   25.7 % (`stack-region`). Reasons: indirect and virtual calls, PLT/import calls, and callees that forward `this`.
   Exempting RBX/R12-R15 at calls: 30 functions.

Not done, in order of expected value:

5. **Per-slot escape in both directions.** The region rule keeps only what is *below* every escaping address. To keep
   slots *above* an escaped object the pass needs the object's upper end. Candidates for a provable bound: (a) the
   next register save slot or the canary slot (`mov rax, fs:[0x28]`-style load stored to a fixed slot and compared
   before return); (b) the next materialised address above it, under "distinct `lea` offsets that both escape are
   distinct objects or the same object" (the lower one's object ends at or before the upper one only if they are
   distinct: not provable, an assumption like A6); (c) a size the code itself reveals (a constant length passed with
   the address, `rep stos` over the object). None of these is in the binary as a fact; each is another assumption to
   state. Start from `EscapeLow` in `stackpriv.go`: collect all escaping exact offsets instead of the minimum.
6. **Imports that do not keep pointers.** A table by import name (memcpy, memset, memcmp, strlen, the lean HLE shims)
   used as `CleanCallee` for calls through PLT stubs and the GOT. Cheap; gain unknown (`tsostat` cannot see it yet
   because a PLT stub ends in an unresolved indirect jump).
7. **Wiring the summaries of item 4** into the translator, if 5 and 6 make them worthwhile: the set has to be
   computed over all functions before emission (the discovery walk), handed to the emit clones, and hashed into the
   chunk cache key and the tier-1 version; fragments must assume no callee is clean.
8. **Stores only.** `stlr` costs almost nothing in time (perf.md); the time cost is the acquire loads. A mode that
   keeps release stores for escaped frames and proves "no other thread writes this slot" for the loads would be
   cheaper, but that proof is the same object-extent question as item 5.
9. **Frameless red-zone leaves** (`spill_private` above): private once the entry is known to be a real function
   entry. Evidence that would do: the FDE's initial CFA rule (a function entry has CFA = RSP+8; a split cold part
   does not), or a set of direct call targets over the whole image.
10. **Folded bias** (`array_private`): `[rsp+idx*8-0x80]` with base+disp outside the frame. Needs a range for the
    index.
11. **Code size**: the alignment test and its inline slow path in front of each ordered access. For RSP/RBP-relative
    accesses with a constant offset the test could be one `tbnz` on the frame register, or dropped where the pass
    knows RSP modulo 16 (it is 16-aligned at a call by the ABI: an assumption).
12. Realigned frames (`and rsp, -32`): a second symbolic base. 0.6 % of the title's accesses.

## 11. Measurements still missing, with commands

All from `wt/tsoelide`; `export NPS5_JOBS=3 M1_JOBS=3 GOMAXPROCS=3`. W = `work/tsoelide` (local, not in git).

* Go tests: `LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test -p 3 ./pkg/disasm ./pkg/lifter ./pkg/emitter ./pkg/cli`
  (green at f30f938; rerun at the branch head).
* Guest corpus per mode: `tools/m1_run.sh`; `M1_XLATE_FLAGS="-tso-elide stack" M1_WORK_TAG=elide tools/m1_run.sh --no-elf-build`;
  `M1_XLATE_FLAGS="-tso-elide stack-region" M1_WORK_TAG=region tools/m1_run.sh --no-elf-build`.
* Litmus: `tools/tsoelide_test.sh --runs 5` (done at f30f938: `off` 13.6 M forbidden outcomes, `stack` 0, `stack-region` 0).
* FEX oracle, 0 regressions wanted in each step:
  `FEX_J=3 tools/fex_oracle.sh > $W/fex_off.log; cp tests/fex/results.json $W/fex_off.json; git checkout tests/fex/results.json`
  then `FEX_J=3 FEX_BASELINE=$W/fex_off.json tools/fex_oracle.sh "" -tso-elide stack > $W/fex_stack.log; git checkout tests/fex/results.json`
  and the same with `-tso-elide stack-region`. (`run_sandboxed.sh` now skips the title gate for `/tests/fex/` paths.)
* Gate: `tools/regress.sh --full -j 3` (default mode; about 33 min; its title runs wait for the gate).
* Microbenchmark: `tools/tsobench.sh 5`.
* Syobon and Greenhollow CPU per frame: `$W/perf.sh <syobon|greenhollow> <off|stack|stack-region> 40 <tag>` (translates
  with `-gpu anyps5 -tso-elide <mode>`, links, runs 40 s with `NPS5_PERF=1`, prints `perf.txt`; the builds are in
  `$W/perf/<title>-<mode>` and are stale after any change to the pass: delete them first). Interleave the modes,
  three runs each, quiet machine. Greenhollow uses `work/regress/refs/inputs/greenhollow.pad`.
* Static counts: `go build -o /tmp/tsostat ./cmd/tsostat`, then `tsostat -lift <eboot>` and `tsostat -ipa 5 <eboot>`
  (title: about 7 min and 3 min, one core, no files written).
* Byte identity of `off` at the branch head: translate Syobon with the base commit's translator and this one
  (`-j 3 -compile=false -no-cache -o <dir>/out`), `cat <dir>/out/code_*.s | shasum` must agree.

## 12. Risks

* The proof stands on A1-A4. A title with hand-written assembly that addresses a caller's frame, or with a custom
  `setjmp` that is reached by a jump rather than a call, is outside it. `off` has the same exposure and more.
* Vector, x87 and string accesses are unordered in both modes, on the stack and off it. A lock-free protocol that
  publishes with `movaps` is not protected. This predates the branch and is not changed by it.
* `popf` and `ret` loads are always plain (A2); a program that takes the address of a `pushf` slot or of its return
  address and has another thread write it is not handled.
* The implicit-write table is hand-made. A missing entry would leave a register marked as a frame pointer after an
  instruction overwrote it, and an access through it plain. Unknown integer instructions clobber RAX/RCX/RDX and their
  operands; vector instructions are assumed to write only their explicit GPR operands (PCMPxSTRI is tabled).
* `stack` raises code size and CPU time (§8).

## 13. `stack` is the default (branch `m3-tsostack`, on `m3-tsoelide` edbe103)

The user approved the switch. `pkg/cli` (`Config.TSOElide` default `"stack"`) and `cmd/fexasm` default to `stack`;
`-tso-elide off` is the escape hatch. The library default of `NewArm64Lifter` is still `off`, so lifter unit tests and
other library users are unchanged. Recorded in `lifter_manifest.json` (`tso_elide`), in the chunk cache key
(`Arm64Lifter.ConfigKey`: `elide=<mode> priv=<rules id>`, absent for `off`), in the tier-1 version hash (a non-`off`
mode) and in the fragment info; an old translation with no mode field is read as `off`, which is what it is.

**Byte identity of `off`.** Syobon translated (`-j 3 -compile=false -no-cache`) by the base commit 784f742 and by this
head with `-tso-elide off`: SHA-1 of the concatenated `code_*.s` equal (`f4a34417...`); the default (`stack`) differs
and its manifest says `"tso_elide": "stack"`.

**Chunk cache.** The privacy bits and the import table were not part of the cache key. Now the key carries
`priv=<id>`, the id being a hash of the pass version (`stackPrivRulesVersion`, bump it when `stackpriv.go` changes what
it proves) and the sorted clean-import table, and the chunk digest (`chunkDigests`) hashes `Instruction.StackPriv` in the
stack modes (not in `off`, whose digests are unchanged). Tests: `TestStackPrivRulesID` (a table or version change changes
the id), `TestTSOElideConfigKeyHasRules`, `TestChunkDigestCoversStackPriv`.

### Validation at head (all `-j 3`, `stack` unless noted)

| check | result |
|---|---|
| go test (`pkg/disasm lifter emitter cli cmd/...`) | green |
| FEX oracle, 2,291 tests (`FEX_J=3`) | `off` vs committed baseline: 1,860 pass, 0 regressions; default (`stack`) vs the `off` run: 1,860 pass, 0 regressions, 0 other changes |
| `tools/m1_run.sh --call-model flat` | 42/42 under `stack` (incl. `t_tsoelide`, both import variants) |
| litmus `tools/tsoelide_test.sh --runs 5` | `stack` 0 forbidden outcomes, `stack-region` 0; `off` 10,826,233 (reported, not a verdict) |
| `tools/regress.sh --full -j 3` (private `REGRESS_DIR`) | gotest, m1_flat 42/42, tier1, calc3d (3480 frames), Syobon (1776 frames), Greenhollow (9432 frames) PASS. **memtests (164/214), mem_prove (0/5) and diag (0/6) FAIL**: every program built through `tools/build_wt.sh` exits at once with code -1 and no guest HLE call, with the same result when the translator is the base commit's 784f742 (and with `-tso-elide off`), so it is this worktree's link step, not the lowering: `build_wt.sh` stubs 17 symbols (`recomp_resolve_symbol`, `recomp_module_register`, ... because the runtime compile stopped at the first Gnm error) where the `wt/regress` binaries stub 10 and run. Not resolved; rerun those three checks in a tree whose runtime builds. |

`run_sandboxed.sh` in this branch got the CPU-only exemption from the title gate (it had only been in `wt/tsoelide`); a
stray `pgrep -f` match on another agent's shell command line had made every FEX test wait.

### Run time, `stack` vs `off` (load average 3-17 from other agents: not a calm machine)

Syobon, title screen, 40 s, `NPS5_PERF=1`, three interleaved runs per mode, median (runs):

| | off | stack | |
|---|---|---|---|
| flip-thread CPU per frame | 27.31 ms (26.85, 27.31, 28.40) | 29.97 ms (29.19, 29.97, 30.48) | +9.7 % |
| process CPU per frame | 30.15 ms | 32.12 ms | +6.5 % |
| frame time p50 | 33.08 ms | 33.07 ms | the title's own 30 fps cap; 3.4 ms of the 33.3 ms budget left |

Greenhollow (60 s of scripted pad input, 120 Hz vsync-capped, 4.4-4.7 k frames): guest-thread CPU per frame 0.341 ms
(off) vs 0.332 ms (stack), process CPU 3.19 vs 2.83 ms, p50 8.33 ms both: no measurable difference, the title is far
from CPU bound.

`tools/tsobench.sh 3` (min ns per iteration): spill_private 1.85 / 2.62 (+41.6 %); spill_escaped 1.78 / 2.60 (+46.1 %,
`stack-region` +5.6 %); array_private 304 / 416 (+36.7 %); array_escaped 303 / 409 (+35.2 %); object_escaped 8.01 /
8.11 (+1.2 %); fib36 1.26 / 1.30 (+3.2 %); stack_args 1.49 / 1.53 (+2.7 %). Checksums equal. Same as §8.

### Cost recovery

* **Item 6 (imports that keep no pointer) is done** (`pkg/disasm/cleanimports.go`, `StackPrivOpts.ImportClean`). A
  direct call through a PLT stub (`jmp [rip+disp]` over a clean GOT slot, with or without `endbr64`/`bnd`) or through the
  GOT slot (`call [rip+disp]`) to an import in the table does not make argument-register frame addresses escape. Table
  (by NID, so no name database is needed): memcpy, memmove, memset, memcmp, memchr, bcopy/bzero/bcmp, the str*
  compare/length/copy/search functions, the printf family and puts/fputs/fputc/fwrite/fread, libm value functions
  (sin ... pow, sqrt, floor, ..., f-suffixed forms, sincos, modf, frexp, ldexp), abs. Excluded on purpose: anything that
  writes a pointer through a pointer (strtol/strtod end pointer, strtok, strsep, getline: the pass tracks registers,
  not memory), anything with callbacks (qsort, bsearch, atexit) or that retains (setvbuf, pthread_*).
  Soundness: the callee reads or writes through the address during the call and keeps no copy, so no other thread can
  come to hold it; the result of memcpy/strchr points into an argument, so RAX is tainted after the call when any
  argument register is, and storing or passing it escapes as before. Everything else a call does is unchanged. Unit test
  `TestStackPrivCleanImport` (clean via stub, via GOT, other callee still escapes, tainted RAX stored escapes, untainted
  does not). Gain: **small**. Syobon static (`tsostat`, 25 clean GOT slots + stubs): plain stack accesses 24,676 ->
  24,887 (+0.4 % of the title's rsp/rbp accesses; 7 functions stop being escaped); expected from §10 item 4. The title's
  frame addresses mostly go to its own functions and to C++ methods. PPSA30552 not measured (rule: static only, and no
  re-run was needed to see the gain would be of the same order); I did not re-measure run time for this item since 0.4 %
  of accesses is below the noise.
* **Item 5 (per-slot escape) not done**: every candidate bound for the object extent is a new assumption (§10), and
  without evidence of a gain larger than item 6's, adding one to a mode whose point is soundness was not justified.

### Risks added by this step

* The default is now slower: about +10 % flip-thread CPU on Syobon, +14 % host instructions statically, up to +40 %
  on ordered-access-bound loops (tsobench). Titles that are CPU bound at their frame cap may need `-tso-elide off`
  (unsound for escaped frames, §12), or the per-function opt-out `NPS5_TSO_RELAX_FUNCS`.
* The three `build_wt.sh` based gate checks above are red in this worktree for a reason that predates this branch.
