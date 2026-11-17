# Three small lifter speedups (m3-liftmisc)

Branch `m3-liftmisc`, on the integration branch with m3-callic. Three independent changes, each behind its own
translate-time flag, each default on, each byte-identical to the old lowering when off.

| item | flag | what |
|---|---|---|
| 1. FP compare mapping | `-fp-cmp-fuse` | `UCOMIS/COMIS` followed by `J/SET/CMOV{A,AE,B,BE,P,NP}` (and `JE/JNE/SETE/SETNE`) read the ARM `FCMP` flags directly; no x86 flag conversion |
| 2. Push/pop fusion | `-pushpop-fuse` | runs of `PUSH r64` / `POP r64` become one rsp update and `STP`/`LDP` pairs |
| 3. Indirect jmp cache | `-jmp-ic` (needs `-call-ic`, flat model) | the call-site inline cache of docs/m3/callic.md for `jmp reg` / `jmp [mem]` |

Code: `pkg/disasm/fpfuse.go` (analysis), `pkg/lifter/arm64_lifter_fpfuse.go`, `arm64_lifter_pushpop.go`,
`arm64_lifter_callic.go` (`indirectJump*`), `pkg/emitter/asm_trampoline_flat.go` (`_recomp_indirect_jump_ic`),
`cmd/liftmiscstat` (static counts), `tools/liftbench.sh` + `tests/ps5/bench/liftbench.c` (micro-benchmark),
`tests/ps5/src/t_liftmisc.c` (end to end). The flags are in the translation stamp, the lifter `ConfigKey` (chunk
cache) and the tier-1 manifest (`fp_cmp_fuse`, `pushpop_fuse`, `jmp_ic`; fragments are lifted as the title was).

## Sites on the UE title (static, `cmd/liftmiscstat`, read-only eboot, nothing run)

258,974 reachable functions, 3,342,498 blocks, 20,084,962 x86 instructions.

| | count | host instructions (blocks that hold a site, peephole and TSO on) |
|---|---|---|
| FP compares (`UCOMIS/COMIS`, SSE and VEX) | 43,147 | |
| ... fused (all readers local, flags dead after) | 41,751 (96.8 %); not fused: 398 no mappable consumer, 998 flags live out of the block | 1,747,532 -> 1,629,658 in those blocks: **-117,874** (2.8 per compare) |
| `PUSH r64` / `POP r64` | 963,094 / 874,711 | |
| ... in runs of 2 or more | 694,283 / 786,916 (189,090 / 188,948 runs; lengths 2:110k 3:55k 4:56k 5:73k 6:85k) | 15.74M -> 14.34M in the 357,700 blocks that hold a run: **-1.40M** (-8.9 % there, about 1.7 % of the title) |
| `jmp reg` / `jmp [mem]` (cached) | 298 / 2,133 | |
| `jmp` with a recovered jump table (not cached) | 3,074 | |
| for comparison: `call reg` / `call [mem]` | 4,827 / 247,078 | |

Consumers rewritten: JA 7,564, JAE 8,531, JB 6,128, JBE 9,874, JE 3,079, JNE 4,073, JP/JNP 154, CMOVcc 828, SETcc 2,268. Whole title: 20.1M x86
instructions, so the three together remove about 1.5M host instructions (about 1.9 % of the translated code).

## 1. FP compare mapping

### Before

`UCOMISS/UCOMISD/COMISS/COMISD` (and the VEX forms) lowered to `fcmp` plus `cset w2, vs; axflag; rmif x2, #0, #1`
(three instructions that turn ARM's FCMP result into the lifter's x86 model: Z = ZF, C = !CF, V = PF, N = 0) and, when PF or AF are live
(docs: `arm64_lifter_parity.go`), `cset w16, vc; ctx load; strb pf; strb af`. The flags are lazy only in PF/AF; ZF, CF
live in NZCV with the carry sense tracked statically (`carryPol`).

### FCMP flags and the x86 result

| outcome | ARM NZCV after `fcmp a, b` | x86 ZF PF CF |
|---|---|---|
| a < b | 1000 | 0 0 1 |
| a = b | 0110 | 1 0 0 |
| a > b | 0010 | 0 0 0 |
| unordered | 0011 | 1 1 1 |

V is the unordered flag in both models, so PF readers (`vs`/`vc`) and the PF spill machinery are unchanged. Conditions with
the same truth table over the four outcomes:

| x86 | ARM | |
|---|---|---|
| JA (CF=0, ZF=0) | `gt` | unordered gives N != V, false |
| JAE (CF=0) | `ge` | |
| JB (CF=1) | `lt` | unordered true |
| JBE (CF=1 or ZF=1) | `le` | unordered true |
| JP / JNP | `vs` / `vc` | |
| JE (ZF=1) | `eq` or `vs` | two branches |
| JNE (ZF=0) | not `vs` and `ne` | `b.vs skip; b.ne L` |
| SETE / SETNE | `cset vs; csinc ..., ne` / `cset vc; csel ..., ne` | 2 instructions |

`CMOVE`/`CMOVNE` are not fused: the raw form is two dependent CSELs on the destination, and a `cmovne; cmovp` select loop was
30 % slower than with the converted flags (0.75 -> 0.99 ns per element in `fp_ne`). `S`, `O`, `L`, `G`... (never produced
after an FP compare by a compiler) are not fused either.

### When it applies (`pkg/disasm/fpfuse.go`)

Per compare: scan forward through instructions that cannot disturb the raw flags, collect the consumers, and require that the
flags are dead afterwards. The scan accepts:

* consumers `J/SET/CMOV{A,AE,B,BE,P,NP}`, `JE/JNE/SETE/SETNE` (CMOV only with a 32/64-bit register destination);
* instructions that read and write no flag (`flagNeutral`: moves, SSE/AVX/x87, lea ...); the lifter checks the lowered text and, if it
  writes NZCV without saving it (`cmp`, `fcmp`, `adds`, `bl` ...), puts the three-instruction conversion in front of
  it and the later consumers use the converted flags (`fpRawAfter`);
* an instruction that defines every flag (CMP, ADD, TEST, UCOMIS ..., CALL, a shift by a non-zero immediate, INC/DEC with CF dead after)
  ends the window;
* at the end of a block every exit must be flag-dead (a cross-block backward "any flag live" analysis, RET and tail calls are dead), except
  the fall-through of a conditional branch on the flags into a block with no other predecessor, where the scan continues
  (`jne L; jp L`, clang's `x == y`; clamp chains). The first instruction of such a block is `FpRawIn`.

Anything else (an `adc`, a `lahf`, `pushfq`, a successor that reads CF) leaves the compare on the old lowering, which
is still there and still exact. A fused compare no longer stores PF/AF into the context bytes: their only readers are in the window and
read host V.

### Result

Statically: UE 96.8 % of the compares, Syobon 501 of 526 (95 %). The micro-benchmark (`tests/ps5/bench/liftbench.c`, `fp_*`) shows the
saving where the compare+branch is the loop: `fp_branch` (two predictable compare+branch pairs per element, no select) **0.67 -> 0.55 ns/element
(-18 %)**. On loops bound by a loop-carried dependency or a mispredicted branch (`fp_clamp`, `fp_sel`, `fp_ne`, `fp_count`: 0.74-1.5 ns) the
three removed instructions are off the critical path and the change is inside the noise (+-3 %). The first version of the benchmark also found
the `cmovne; cmovp` slowdown above and a real bug (below, Tests).

## 2. Push/pop fusion

A run of consecutive `PUSH r64` (or `POP r64`) of one basic block (instruction order unchanged, no `rsp`, no 16-bit, memory or immediate forms,
at most 16, and for pops no destination twice: `ldp` with the same destination twice is unpredictable)
lowers to

```
push rbp; push r15; push r14; push rbx        sub x26, x26, #32
                                              add x16, x28, x26        (base mode; identity mode uses x26)
                                              stp r15, rbp, [x16, #16]
                                              stp rbx, r14, [x16]
pop  ...                                      add x16, x28, x26 ; ldp a, b, [x16] ; ... ; add x26, x26, #8n
```

2 instructions per register become 1 (0.5 for pairs, plus 2 for the run). The memory image is that of the sequence: the
first-pushed register at the highest address, the stores issued highest first, the final `rsp` the same. A skipped
instruction emits nothing (a comment with `-asm-comments`), the whole run is lowered at its first instruction. The flags
are untouched (no NZCV writes).

### Faulting addresses and the guard page

Measured on the host with a guard page (`TestPushPopFuseFaults`, SIGSEGV with the signal context; fused / unfused):

| | unfused | fused |
|---|---|---|
| push run, last slot in the guard page: fault address | the guard page (first slot that touches it) | the same address |
| rsp at the fault | already lowered by the pushes issued so far (`sub` before each `str`: the faulting push has lowered it too) | lowered by the whole run |
| stores before the faulting pair | all done | all done |
| the valid half of a straddling pair | stored (it is a separate `str`) | **not stored** (a faulting `stp` is all-or-nothing on this hardware) |
| pop run reading into a guard page: fault address | the first load that touches it | same page; a straddling `ldp` may have loaded its valid register |
| rsp at the fault | raised by the loads that completed | **unchanged** (raised once after the last load) |

So the access that faults is the same, in the same order. What differs is guest-visible state at the fault: `rsp` (push: the whole run;
pop: none of it) and the memory/registers of the faulting pair. That matters only to a guest that resumes after a stack fault inside a run;
the guard page policy (a stack fault is fatal and reported at the guest pc of the run's first instruction, because the run is one host code
sequence mapped to its first pc) has no such path. Signals delivered between the pushes of a run cannot be observed
(the run is one host sequence; asynchronous signals are delivered at host instruction boundaries, and the guest-visible stack below `rsp`
of a signal frame is not read by the guest). Another thread cannot see the stack of this one.

## 3. Indirect `jmp` inline cache

`jmp reg` / `jmp [mem]` (vtable tail calls, `return o->vt->f(o)` at -O2) now get a per-site record of the same layout as the
call sites (48 bytes in `__DATA,__nps5_ic`, two slots and a miss counter, `{guest target, host entry}` per slot,
empty = `{~0, _recomp_indirect_call}`). The only difference is the entry a slot holds: the **jump entry** (call entry + 8, the dispatch
table's entry past the shadow push that `_recomp_indirect_jump` uses), so a hit is

```
adrp/add x1, site ; ldp x2, x3, [x1] ; cmp x2, x0 ; b.ne 7f ; br x3
7: ldp x2, x3, [x1, #16] ; cmp x2, x0 ; b.ne 8f ; br x3
8: (local jump table, as before) ; b _recomp_indirect_jump_ic
```

* The miss path `_recomp_indirect_jump_ic` is `_recomp_indirect_call_ic` minus the return-address registers: same megamorphic bypass
  (64 misses), same epoch protocol (epoch read before the lookup, one 16-byte `stp`, `dmb ish`, epoch re-read, un-publish if it changed),
  same 1-in-16 replacement of slot 1, a target that is not in the dispatch table is never cached and goes to the
  slow path with `RECOMP_MISS_JUMP`.
* Invalidation is the call IC's: `recomp_register_fn` replacement flushes everything, `recomp_shadow_invalidate(lo, hi)` flushes by host
  range. A jump slot's host entry (call entry + 8) lies in the same function, so the range test treats it like a call slot. Nothing
  in the runtime changed.
* The cache check comes before the local jump table (`emitLocalJumpTable`: a `cbz` per block of the function for a `jmp` the disassembler
  could not resolve). That is safe: a slot is only ever filled by the miss path, which is reached after the local table failed, so a
  cached target is never a block of this function. A jump with a **recovered jump table** (a switch) gets no cache (its targets are blocks of
  the function; it could only miss).
* A jump to `~0` hits an empty slot and enters `_recomp_indirect_call` with garbage return registers instead of the structured
  `RECOMP_MISS_JUMP` report; it is fatal either way.
* Needs `-call-ic` (the record section, the flush walker and the runtime enable bit) and the flat model.

## Micro-benchmark (`tools/liftbench.sh`, M4 Pro, load 5-15, min of 5, ns per unit)

| case | off | fp | pp | jmp | all |
|---|---:|---:|---:|---:|---:|
| fp_branch | 0.670 | **0.550** | 0.670 | 0.680 | **0.530** |
| fp_count / fp_clamp / fp_eq / fp_ne / fp_sel | 1.54 / 1.28 / 0.55 / 0.74 / 0.74 | same within 3 % | | | same |
| pp6 (6 push + 6 pop per call) | 2.79 | 2.71 | **2.30** | 2.73 | **2.37** |
| pp4 | 2.68 | 2.73 | **2.19** | 2.59 | **2.28** |
| tail_ptr_mono | 1.25 | 1.34 | 1.32 | **1.05** | 1.30 |
| tail_ptr_poly2 | 1.64 | 1.69 | 1.53 | 1.52 | 1.43 |
| tail_ptr_rr4 | 1.63 | 1.60 | 1.72 | 1.87 | 1.84 |
| tail_virt_mono | 2.04 | 1.94 | 1.86 | **1.75** | 1.80 |
| tail_virt_poly2 | 2.02 | 2.20 | 2.13 | 2.06 | 1.92 |

The machine was shared (load 5-15), so single cells move by +-8 % between runs (an earlier run with load 5 had tail_ptr_mono 1.23 -> 1.01, pp6
2.54 -> 2.06, tail_virt_mono 1.98 -> 1.73, and no change in the fp rows other than fp_branch). Read the pattern, not the cell: push/pop runs -18 %, the jump cache
-15 % when monomorphic, a few % worse when 2-4 targets alternate. All checksums are equal in every configuration.

Cases: `fp_*` compare loops (the compiler is told not to vectorise), `pp6`/`pp4` functions with 6 / 4 live callee-saved values across a
call (push and pop runs), `tail_ptr_*` (`return f(x)` through a pointer, one / two / four targets), `tail_virt_*` (`return o->vt->f(o, x)`, one object type / two).

## Tests

* `TestFpCmpFuseExec`: SET/CMOV/J x 8 conditions x 9 outcomes (less, equal, greater, NaN in x, in y, both, +0 vs -0, inf, -inf vs inf) x reg/mem
  source x float/double x fused/not, expected from the x86 flag definition. `TestFpCmpFuseWindows` (`jcc; jcc` chains across blocks,
  every pair; instructions between compare and consumer), `TestFpCmpFuseClamp` (clang's clamp), `TestFpCmpFuseInc`, `TestFpCmpFuseShape`,
  `TestFpRawAfter`. The clamp test is the one that failed on the bug the micro-benchmark found (a fused compare starting a window
  inside the previous window's block was converted away by the kill rule: NaN took the wrong path).
* `TestPushPopFuseExec` (runs of 1..12, memory image and rsp against the model, split runs, duplicate destinations, `push rsp`/`pop rsp`),
  `TestPushPopFuseShape`, `TestPushPopFuseFaults` (the table above, with real faults).
* `TestJmpICLowering` (exact lowering, off, needs call IC, nested ignores it), `tests/ps5/src/t_liftmisc.c` (end to end: all three
  with NaN/inf/zero, deep recursion through push/pop, tail calls through changing pointers and vtables; expected output from the
  host build).
* FEX oracle (`tools/fex_oracle.sh`, whole corpus, `FEX_TIMEOUT=90`): **regressions 0, newly passing 0, other changes 0** against the committed baseline. The
  corpus exercises the fused paths (`TwoByte/0F_2E`, `0F_2F`, `VEX/vucomis*`, `vcomis*`, `OpSize/66_2E`, `FEX_bugs/Test_CmpSelect_Merge_Float*`, every PUSH/POP and `jmp r/m` test) because the oracle lifts with the
  defaults. (A first run with the default 20 s timeout under load showed 53 "regressions", all `exit--1` kills; re-running the same tests alone passed and a full re-run with 90 s gave 0.)
* `tools/regress.sh --full -j 3` from this tree (rev 045e1e1): gotest PASS (15 packages), memtests PASS 240/240, m1_flat PASS 42/42 (including `t_liftmisc`, plt and noplt), tier1 PASS,
  mem_prove 5/5, diag 6/6, Calculator3D PASS (118.8 fps, frame 0 identical), Syobon PASS (29.7 fps, audio 65.7 %), Greenhollow: the first run's evaluator crashed on the zero-byte last frame that the
  80 s SIGTERM left (`regress_report.py` `read_bmp`; the title itself ran 786 frames); `--only greenhollow` rerun PASS (119 fps, audio 100 %, frames_ref 98.7 %).

## Risks

* The FP window analysis trusts that a lowering of a flag-neutral x86 instruction leaves NZCV alone or saves and restores it
  (the existing rule for every instruction between a compare and its branch), and checks the lowered text for writers on top of that.
  A lowering that restores only part of NZCV would be a bug already.
* Fused compares no longer store PF/AF; any new reader of those bytes between a compare and a kill must be added to `fpClassify` as a read.
* Fault-state differences of push/pop runs (above).
* The jump cache is slower than the plain dispatch for sites with more than two live targets (+9 % two targets alternating,
  +17 % four targets round robin in `tail_ptr_*`, after the megamorphic bypass), faster for one target (-18 %) and vtable tail calls
  (-13 %). Switch tables are excluded.
* Merge note: my first merge of origin/m3-callic into the integration base auto-merged into a broken trampoline (`Lshim_return` undefined: leanshim and callic both touch
  `asm_trampoline_flat.go`); merging the local `integrate` branch, which carries the real resolution, fixed it. Rebase onto main should take `integrate`'s side there.
* `regress_report.py` should skip a zero-byte final frame (a harness flake, not part of this change).
