# Vector registers: lane liveness for SSE/AVX state (m3-simdregs)

Branch `m3-simdregs`. Code: `pkg/disasm/vec_liveness.go`, `pkg/lifter/arm64_lifter_veclive.go`, the
`fixUpperHalves*` functions in `pkg/lifter/arm64_lifter.go`, flag `-simd-liveness abi|strict|off`.
Tools: `cmd/simdstat`, `tools/simdbench.sh`, `tools/gen_veclive_cases.py`.

## 1. What was there (as built on main, 521c553)

The brief for this work assumed guest XMM/YMM registers live in a memory context. They do not, and have
not since PS4Recomp:

| Guest | Host | Notes |
|---|---|---|
| xmm0-15, bits 0..127 | v0-v15 | pinned for as long as guest code runs |
| ymm0-15, bits 128..255 | v16-v31 | pinned; the same registers are the lowerings' vector scratch |
| x87 stack, MMX, MXCSR | `GuestContext` memory (through the TCB slot) | MMX aliases `fpu80[n].mant`; FPCR holds RC/FZ |

`GuestContext.xmm[]` is only a transfer area at guest/host transitions (xmm0-7), and `ymmh[]` is never
written. So there is nothing to "move into registers". The cost that `work/ue-arm-study/REPORT.md`
measured (math kernels at about 5 host instructions per x86 instruction) came from three things the
lowerings did unconditionally:

1. **Upper-half zeroing.** Every VEX-encoded write to an XMM register zeroes bits 128..255, so every
   such lowering ended in `movi v<16+n>.16b, #0`.
2. **Scalar lane merge.** `vmulsd xmm0, xmm1, xmm2` keeps bits 64..127 of xmm1 in xmm0. The lowering
   computed into a scratch register, copied xmm1 to xmm0 and inserted the scalar: 3 instructions for
   one `fmul`, and a longer dependency chain.
3. **Scratch spills.** All 32 NEON registers hold guest state, so scratch is borrowed from v16-v31.
   In a function that names a YMM register anywhere (`Function.UsesYmm`), every borrowed register was
   saved to and restored from the host stack around the instruction (`fixUpperHalves`). In a function
   that names none, nothing was saved, on the argument "no upper half can be live here". That
   argument is not sound: such a function can carry a caller's `__m256` argument to a tail call, or
   be a split-off part of a YMM function.

## 2. Design

No register is moved. The mapping above stays, so nothing changes for the runtime, the bridges, tier 1
or code translated with the feature off; code built with different settings can call each other.

**Lane liveness (disasm).** A backward dataflow pass over each function records, per instruction,
which lanes of the 16 guest registers cannot be read afterwards before being rewritten
(`Instruction.VecDead`). Four lanes per register: L0 = bits 0..31, L1 = 32..63, H = 64..127,
U = 128..255. The transfer function is exact for the instructions that matter (VEX and legacy scalar
arithmetic, conversions, scalar and full moves, bitwise ops, broadcasts, low unpacks, scalar FMA,
VZEROUPPER/VZEROALL) and conservative for everything else: an instruction without a rule reads every
lane of every vector register it names and kills nothing; a VEX instruction with a vector destination
that is not also a source kills the destination and reads its sources in full.

**Zero lanes (lifter).** Forward through a block, the lifter tracks which host registers v0-v15 are
known to hold zero in bits 32..63 / 64..127, from the text it emitted (`vecZero`). A scalar load
zeroes them; an arm64 scalar instruction zeroes them. This is what keeps scalar code short when the
result is live in full (see "return" below).

**What the lifter drops** (`vecLivePeephole`, applied to the lowering of one instruction):

- `movi vU<n>.16b, #0` when U of that register is dead after the instruction and nothing later in
  the lowering reads it. In a legacy SSE instruction that shares the code of its VEX form the line
  is always dropped (the instruction keeps the upper half; before, it was zeroed and then restored
  from a spill).
- The lane merge `mov vD.16b, vS.16b; mov vD.s[0], vT.s[0]` (or `.d[0]`), when each lane above the
  scalar is either dead in D or known zero in S: the scalar is produced directly in D by the arm64
  scalar instruction, which zeroes the rest. If the lanes are dead, the zeros are unobservable; if
  they are known zero, the zeros are the architectural value.
- VZEROUPPER/VZEROALL zeroing of dead registers.
- Independent of liveness: `movi vD, #0` right before a scalar `ldr` of the same register; a
  register copied onto itself.

**Scratch** (`fixUpperHalvesLive`). A borrowed v16-v31 register whose guest upper half is live moves
to one whose upper half is dead. Only when all candidates are live is it saved and restored, as
before. This applies to every function: the `UsesYmm` shortcut is not used in this mode.

**Mapping and the ABI.** AAPCS64 makes v8-v15 callee-saved in their low 64 bits and v0-v7, v16-v31
caller-saved. The existing contract already handles this and is unchanged: `recomp_enter_guest` saves
the host's d8-d15; `_recomp_call_shim` passes xmm0-7 through `ctx` and lets the host clobber the rest;
instruction helpers and the miss path save all 32. Section 4 lists what each path clobbers.

**Flag.** `-simd-liveness abi` (default), `strict`, `off`. Recorded in the tier-1 manifest
(`simd_liveness`) so fragments are lifted in the same mode; modes can be mixed freely, each function
is sound on its own. `off` skips the pass: the emitted assembly is byte-identical to main (checked on
`t_simd`, `t_simdlive`, `m1_main`). A function that was not analysed (`Function.VecLive` unset, e.g.
hand-built in tests) is lifted as before, whatever the flag.

## 3. Flush points

"Flush" here means: the point is treated as reading (keeping live) a set of lanes, and the forward
zero-lane knowledge is discarded. For each point, why that set is enough.

| Point | abi mode | strict mode | Zero-lane facts |
|---|---|---|---|
| Call (direct, indirect, to a shim/HLE thunk) | before: xmm0-7 all four lanes live, xmm8-15 dead. After: only what later code reads | everything live | cleared |
| Return | ymm0 (all lanes) and xmm1 (bits 0..127) live | everything live | n/a |
| Direct jump out of the function, indirect jump without a recovered table, fall-through into an address that is not a block | everything live | everything live | n/a |
| Call into the function itself (`call next; pop`, local subroutine) | everything live | everything live | cleared |
| HLT, UD2, INT n (except INT 0x41, a no-op), IRET, SYSRET | everything live | same | cleared (block ends) |
| FXSAVE/FXRSTOR, XSAVE*/XRSTOR* | everything live, nothing killed | same | cleared |
| Block boundary | join of successors | same | cleared at every block leader |
| Lowering with internal control flow, helper call (`bl`), register list | by the transfer function | same | cleared for what it names (all, on `bl`) |
| x87/MMX instruction | transparent (no XMM operand) | same | bridge call clears all |
| Gather (`vpgatherqd xmm, [base+ymm*s], xmm` ...) | the VSIB index register is read (bits 0..127 for an XMM index, all lanes for a YMM one), with destination and mask; it is not an operand of its own, `vecVsibReads` adds it | same | by the transfer function |
| SYSCALL | transparent | same | cleared (`bl`) |

**Calls.** System V x86-64 has no callee-saved vector register, passes vector arguments in
xmm0-7 (ymm0-7 for `__m256`), and no callee can read the others. So before a call the lanes of
xmm0-7 are live and those of xmm8-15 are dead; after it every lane is defined by the callee or
undefined. The bridge agrees: `_recomp_call_shim` stores and reloads only xmm0-7 and documents that
xmm8-15 and the upper halves do not survive. xmm0-7 are kept live in full, U included, because the
callee's signature is unknown. Not assumed: anything about which arguments a particular callee takes.

**Return.** `__m256` is returned in ymm0, `__m128`/double/float in xmm0, `{double,double}` and
`_Complex double` in xmm0:xmm1. Without the return type, all of ymm0 and bits 0..127 of xmm1 are
live. A function that returns a `double` therefore still has H(xmm0) live; the zero-lane tracking is
what removes the merge there when the chain started from a scalar load in the same block, and it is
exact because the zeros are the architectural value.

**Other exits.** A direct jump to another function's entry is usually a tail call, but
`Disassembler.isTailCallTarget` accepts any address in code, and compilers split functions
(hot/cold): the target may be a continuation of the same source function, with any register live.
The same holds for an indirect jump whose table was not recovered: it may be a switch whose blocks
were not discovered, which tier 1 then lifts as a fragment entered with the full register state.
Keeping everything live covers both, and covers the tier-1 miss path: a fragment is lifted by the same
pass, its own exits keep everything live, and the miss path preserves all 32 host registers
(`_recomp_dispatch_continue`, SAVE_Q0_31).

**HLE thunks, callbacks.** A thunk is entered by a call, so the call rule applies. Host-to-guest
callbacks (`recomp_enter_guest`, qsort comparators, thread entries) load xmm0-7 from `ctx` and enter a
function entry, where nothing about the registers is assumed (liveness is backward; zero facts start
empty).

**setjmp/longjmp, exceptions.** `setjmp` is a call; no vector register is live across it in ABI
code, and the as-built `longjmp` does not restore XMM state anyway (`docs/m1/setjmp-eh.md` §8). A
landing pad is reached from a call site: registers there are undefined by the ABI, and the pass does
not need to know the pad's predecessors because it is backward (the call kills, whatever follows).
The zero facts need predecessors, which is why they are block-local: a landing pad, a return site
and a jump-table target are all block leaders.

**Signals.** A host signal frame saves and restores all V registers, so interrupted guest code
resumes with what it had. Stale values in dead lanes are visible only to something that inspects the
interrupted context: the crash report (`crash.txt` dumps xmm0-15), or a guest handler reading
`ucontext` FP state (not modelled today). This is the one observation the design does not serve:
dead lanes in a crash dump can differ from x86.

**x87/MMX.** Neither lives in a V register. The x87 bridge saves v0-v7 and its C code uses nothing
else (`TestX87RuntimeVectorRegs`, `TestX87BridgeKeepsVectors`); MMX lowerings borrow v16-v31 through
the same scratch pass. FXSAVE reads everything.

**MXCSR.** Exception flags are not derived from FPSR (STMXCSR stores the context value), so an
arithmetic instruction that runs on a stale dead lane cannot surface a flag.

## 4. What clobbers V registers

| Path | Saves | Clobbers | Consequence |
|---|---|---|---|
| `_recomp_call_shim` (HLE, imports, SYSCALL) | xmm0-7 via ctx | v8-v15 bits 64..127, v16-v31 | matches the call rule; SYSCALL already lost xmm8-15 high halves before this work |
| `recomp_enter_guest` | host d8-d15 | loads v0-v7 | function entry |
| `_recomp_x87`, `_recomp_x87_a`, `_recomp_x87_run` | v0-v7 | none (tested) | transparent |
| Instruction helpers (CPUID, PCMPxSTRx, AESKEYGENASSIST, ...) | all 32 | none | transparent |
| `_recomp_dispatch_continue` (ret mismatch, table miss, tier 1) | all 32 | none | transparent |
| Lowering scratch | per instruction | v16-v31 it names | renamed to a dead upper half or spilled |

## 5. Soundness limits (abi mode)

- A callee that does not follow System V: `preserve_all`/`regcall`-style conventions, TLS descriptor
  calls, hand-written routines that keep values in xmm8-15 across a call or read them at entry.
  Clang for PS5 uses none of these by default. `strict` assumes no convention at all.
- Reading the vector state of interrupted code (signals, crash dumps): see above.
- The zero-lane tracker reads emitted text. It is conservative by construction (anything it does not
  recognise clears what it names; helper calls, branches to global symbols, uses of x30 and register
  lists clear everything; facts end at every block leader and address gap), and
  `TestVecLiveDifferential` exercises it, but a lowering that writes a v0-v15 register without naming
  it would break it. None does.
- In the flat call model every block is a possible continuation of the slow paths
  (`asm_chunk.go`, landing-pad table). That is why the zero facts are block-local and never cross a
  block boundary, and why the backward pass needs no predecessor information.

## 6. Tests

- `pkg/disasm/vec_liveness_test.go`: the pass at each flush point (call abi/strict, return, jump out,
  indirect jump, trap, get-PC call, FXSAVE, SYSCALL), upper halves under VEX/legacy/VZEROUPPER,
  scalar pass-through chains, branches and loops, conservative default and implicit XMM0, gather
  VSIB index (`TestVecLiveGatherIndex`), masked load/store masks, `/is4` blend selectors, MASKMOVDQU
  (`TestVecLiveMaskOperands`).
- `pkg/lifter/arm64_sha_gather_test.go` `TestVecLiveGatherIndexNotScratch`: the title bug (PPSA30552
  intro crash, fn_0x331c760): a `vpermd` before a gather whose YMM index is not read again must not
  borrow the index's upper half as scratch; lowering text and host execution in abi and strict mode.
- `pkg/lifter/arm64_veclive_test.go`: the lowering at the same points (fold, return keeps lanes, zero
  lanes and their flush at calls and block boundaries, argument registers before a call, scratch
  renaming versus spilling, legacy SSE keeps U, VZEROUPPER, off is unchanged) and the tracker itself
  (`TestVecZeroTracker`).
- `TestVecLiveDifferential`: 600 generated sequences (31k instructions; scalar and packed, VEX and
  legacy, YMM, loads/stores, forward branches; half end by storing all of ymm0-15, half by storing a
  few lanes and VZEROALL) run on the host three ways: liveness off in a function that keeps every
  upper half, strict, abi. General registers and all stored vector state must be identical. No x86
  code runs. It found two bugs during development: VZEROUPPER's registers treated as scratch, and
  a zero-lane fact about the destination used after the lowering itself had overwritten the
  destination (VRCPSS; `TestVecLiveZeroFactStaleInsideLowering`). The second one needed a
  1500-sequence corpus to show up; 7,500 more sequences (seeds 4242, 1, 77, 31337) pass. Five
  hand-made mutations of the pass (pass-through lanes dropped, legacy move kills U, tracker never
  forgets, branch join replaced by assignment, second source of a bitwise op not read) all fail it.
- `tests/ps5/src/t_simdlive.c`, in `tests.list` twice: `t_simdlive` (AVX2+FMA, the SDK's default CPU)
  and `t_simdlive_sse` (`-mno-avx`: legacy SSE2 encodings). Calls in the middle of scalar and packed
  sequences, eight `__m128d` arguments built by scalar intrinsics, `__m256` through a tail call and
  a call, a `__m256` argument zero-extended by a VEX.128 write right before the call, scalar
  intrinsics returned as `__m128`/`__m128d`, varargs, qsort callback, indirect calls,
  setjmp/longjmp. Three mutations of the ABI rules (upper halves of xmm0-7 dead at a call; xmm4-7
  dead at a call; a return reads only the scalar) each make it fail.
- FEX oracle with `-simd-liveness off` and `strict` (the harness default is `strict`: FEX tests follow
  no calling convention and compare every register after the final `ret`). 2,291 tests:
  off 1823 PASS / 153 FAIL / 315 SKIP, strict 1825 PASS / 151 FAIL / 315 SKIP. No test gets worse; the
  two that change, `VEX/vpmaddubsw` and `VEX/vpsadbw`, go from FAIL to PASS: their lowerings borrow
  scratch registers in a function without YMM instructions, where the old rule let them overwrite
  upper halves that the test then checks. (The committed `tests/fex/results.json` predates several
  lifter fixes on main and was left as it is; "off" above is main's behaviour today.)

## 7. Measurements

Method of `work/ue-arm-study/REPORT.md`: static host instructions per guest instruction, on the UE 5.6
title's `eboot.bin` (read-only input; only counts are kept). `cmd/simdstat` lifts each function with
the pass off and in both modes and counts the emitted instructions; nothing is assembled or run.
Addresses are those of the report minus the 0x400000 load bias.

Two math kernels of the report:

| Kernel | x86 insts | off (= main) | strict | abi | native ARM (report) |
|---|---|---|---|---|---|
| 4x4 double inverse, scalar LWC (report 0x5355d0) | 270 | 1363 (5.05/x86) | 550 (2.04) | **498 (1.84)** | about 271 (1.0) |
| YMM double/float transform (report 0x2c846d0) | 284 | 1380 (4.86) | 733 (2.58) | **646 (2.27)** | about 1 per op |

For the inverse: upper-half zeroings 204 to 1, spill instructions 318 to 0, lane inserts 158 to 2.

Whole title (250,747 reachable functions; "vector-heavy" = at least 35 % SSE/AVX instructions and at
least 8 of them, the report's class):

| Class | functions | x86 insts | off | strict | abi |
|---|---|---|---|---|---|
| vector-heavy | 8,651 | 1.69 M | 7.47 M (4.43/x86) | 6.57 M (3.89) | 6.40 M (3.79), -14.4 % |
| any vector instruction | 82,779 | 13.47 M | 55.43 M (4.12) | 54.09 M (4.02) | 53.78 M (3.99), -3.0 % |

| Removed in abi mode (any-vector class) | off | abi |
|---|---|---|
| upper-half zeroings | 939,952 | 369,121 |
| spill instructions (save + restore) | 632,624 | 26,758 |
| lane inserts | 278,626 | 123,945 |

What is left in vector-heavy code is mostly not vector state: address computation for memory
operands, constants built in general registers, shuffles and blends lowered lane by lane. The
remaining zeroings are almost all for xmm0-7 before a call or a return (one `movi`, which the core
eliminates at rename).

Timing figures (the same two kernels run in a harness, and `tools/simdbench.sh`) are in section 8.

## 8. Timing

Apple M4, machine shared with other agents (load average 15-19 during these runs; a requested quiet
window was only partly effective), minimum of 4-7 interleaved runs. Ratios are stable across runs
with different load; absolute times are not.

**The two UE kernels**, lifted from the title and run in `tools/simdkern` (same inputs for every
mode, outputs compared and equal; 5.2 ns of each figure is the harness loop):

| Kernel | off (= main) | strict | abi | speed-up |
|---|---|---|---|---|
| 4x4 double inverse (report 0x5355d0) | 251.4 ns | 20.0 ns | 19.4 ns | 13x (17x net of the harness) |
| YMM transform (report 0x2c846d0) | 64.1 ns | 15.6 ns | 14.1 ns | 4.6x (6.6x net) |

The off figure is far worse than the instruction count suggests because every scalar operation saved
and restored its scratch register on the stack: the restore puts a store-to-load forward on the
dependency chain of the next operation.

**`tools/simdbench.sh`** (`tests/ps5/bench/simdbench.c`: the same C built with the PS5 SDK at -O2 with
AVX2+FMA and translated, and built natively for arm64 at -O2; checksums equal in all four builds):

| Kernel (ns per call or element) | native | off | strict | abi | off / native | abi / native |
|---|---|---|---|---|---|---|
| inverse4d (scalar LWC) | 9.80 | 135.47 | 14.44 | 14.07 | 13.8x | **1.44x** |
| mul4d (packed double, YMM) | 8.98 | 12.77 | 8.57 | 8.55 | 1.42x | **0.95x** |
| transform_pos (double to float) | 0.81 | 2.67 | 1.84 | 1.86 | 3.30x | 2.29x |
| quat_mul_norm (float, shuffles, sqrt) | 18.62 | 25.45 | 23.00 | 24.04 | 1.37x | 1.29x |
| blend (float loop, 8 lanes) | 0.05 | 0.50 | 0.08 | 0.08 | 10.4x | **1.60x** |
| len_sum (scalar reduction, returned) | 0.46 | 1.28 | 1.26 | 1.29 | 2.78x | 2.80x |
| lerp_chain (call per element) | 2.45 | 5.13 | 5.17 | 5.13 | 2.10x | 2.10x |

Against the goal of about 2x native: the scalar LWC and packed kernels are at 1.0-1.6x. Three are
not moved by this work: `len_sum` (open item 1: the accumulator's lane insert stays on the loop
chain), `lerp_chain` (call and return cost, owned by the call work) and `transform_pos`
(address computation and lane-by-lane shuffles).

Gates (`tools/regress.sh --full` from `m3-regress`, run against commit e671d4f in a private
`REGRESS_DIR`): go test 13 packages ok, memtests 240/240, `m1_run` flat 42/42 (the 38 of main plus
the two builds of `t_simdlive` in both import variants), tier1 PASS, mem_prove 5/5, diag 6/6,
Calculator3D PASS with frame 0 identical to the x86 reference (max channel difference 0), Syobon
PASS. Greenhollow renders (reference frames 98.7-99.0 % within tolerance, audio 100 %) but missed
the gate's fps and flip-gap thresholds under load; main 521c553, run right after it under the same
load, missed them by the same margin (23.6 vs 23.4 fps, need 30), so this is not a difference of the
branch. The commits after e671d4f change tests, the trap list and tooling; `go test ./pkg/...` and
the `t_simdlive` runs were repeated on the head.

## 9. Merge notes

- `m3-boot`/`m3-iterspeed` add `Arm64Lifter.Clone`, `ResetChunk` and `ConfigKey` (chunk cache key).
  `Clone` copies the struct, so `noVecLive` and the zero-lane tracker come along (the tracker resets
  itself at a new function). **`ConfigKey` must include the liveness mode** (`Disassembler.VecLive`
  and `Arm64Lifter.VecLiveness()`), otherwise cached chunk objects of another mode are reused.
- `tests/ps5/tests.list` gains two entries: `tools/m1_run.sh` reports 42 runs instead of 38.
- `go test ./...` also walks `tests/ps5/build/**/out/runtime` when a harness left output there (true
  before this branch for `m1_run`/`callbench` outputs as well); remove the build directory or test
  `./pkg/... ./cmd/...`.
- The peephole agent's copy propagation and this pass both rewrite the text of one instruction's
  lowering. This pass runs first inside `LiftInstructionToBuf` (before the TSO rewrite); the tracker
  only needs every write to v0-v15 to name the register.

## 10. Open items

- **Scalar accumulators live at a return or a call across a loop.** `sum += f(x)` in xmm0, returned:
  bits 64..127 of xmm0 are live at the `ret`, the loop body is its own block, so the zero-lane facts
  from the preheader (`vxorps xmm0, xmm0, xmm0`) are gone and the body keeps its lane insert on the
  loop-carried chain (`len_sum` in simdbench stays near 3x native for this reason). Carrying the
  facts across blocks needs every way into a block to be known, and in the flat model any block can
  be continued from the slow paths; an opt-in mode that assumes entries only along CFG edges and
  after calls, or that a `ret` reads only what the function wrote in full, would close it.
- Interprocedural argument liveness (what a direct callee reads at entry) would remove most of the
  remaining upper-half zeroings before calls; they are zero idioms, so the gain is code size.
- Shuffles and blends are lowered lane by lane and with masks built in general registers
  (`vshufps`, `vshufpd`, `vblendps`): visible in `quat_mul_norm` and `transform_pos`. Not vector
  state; left for the peephole work.
- `SYSCALL` goes through `_recomp_call_shim`, which does not keep xmm8-15 high halves or the upper
  halves although the kernel would. Unchanged by this work.
- The crash report prints xmm0-15 as they are in the host registers; dead lanes can now hold stale
  values (section 3, signals).
