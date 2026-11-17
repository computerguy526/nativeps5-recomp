# FEX-Emu ASM oracle (instruction-level differential suite)

SPEC 8.1 asks for instruction-form, width, addressing-mode and edge-case coverage. Without any x86 hardware
or Rosetta in the loop, this suite uses the 2,291 unit tests in FEX-Emu's `unittests/ASM` (MIT; clone at
`repos/FEX`, commit ea43314f; credited in `NOTICE`, not vendored) as the oracle: each test carries
the architecturally expected register values after it runs. **No x86 code is ever executed.** The tests are
only assembled and lifted.

## Pipeline (`cmd/fexasm`, `tools/fex_oracle.sh`)

1. `nasm` (FEX's own recipe: prepend `BITS 64`, append `ret`, include dirs `Includes/` and the test's
   directory) to a flat binary.
2. The blob is wrapped in a synthesised ET_EXEC ELF and loaded with `pkg/elfloader`; `pkg/disasm`
   discovers the reachable functions and blocks; `pkg/lifter` (Arm64Lifter, base address mode, TSO on, as
   `ps4-recomp` does) lowers them, with the same per-block loop as the emitter.
3. The ARM64 text is assembled and linked with `clang` against a small host harness
   (`cmd/fexasm/harness/fex_main.c`): a 28 GiB `PROT_NONE` arena (guest address A is at `arena+A`, x28 = arena),
   the declared `MemoryRegions`/`MemoryData`, FEX's default 128 MiB data/stack area at `0xe0000000`
   with RSP = `0xe8000000`, all other GPRs/vectors zero, MXCSR 0x1f80, x87 empty with CW 0x37f.
   The guest state is pinned in host registers exactly as the lifter expects; MMX/x87 live in a fake
   `GuestContext` reached through the TCB slot at `[FS+0x70]`.
4. The binary runs under `tools/run_sandboxed.sh` (scrubbed env, timeout, one subprocess per test, so a crash
   or hang only fails that test). The end marker is `hlt` (the disassembler treats HLT as a privileged
   opcode, i.e. data, so the harness patches each reachable HLT to INT3 and lifts it as a jump to the
   exit stub) or the appended `ret` of the entry function. The harness dumps GPRs, XMM, YMM upper halves, MM0-7 and memory-region
   hashes, and `fexasm` compares exactly the registers listed in the test's `RegData` (XMM entries with four
   elements also compare the YMM upper half; `Ignore` is honoured).
5. Indirect calls/jumps use a generated dispatcher over the discovered functions/blocks. Targets that only
   appear as `lea reg,[rip+x]` / `mov reg,imm` in a test that branches indirectly are seeded as functions
   (a real ELF gets them from symbols and unwind data); holes inside seeded entries do not count as unsupported
   because the guess may be data.

Run: `tools/fex_oracle.sh [filter-regexp]` (env `FEX_DIR`, `FEX_J`, `FEX_TIMEOUT`, `FEX_BASELINE`). It builds
`tests/fex/fexasm`, runs the (filtered) corpus in parallel, rewrites `tests/fex/results.json` (a filtered run
updates only its own entries) and prints the table plus a diff against the previous results (regressions / newly
passing). Per-test files are kept with `-keep` under `tests/fex/work/` (gitignored). `results.json` has no timestamps
and is deterministic for a given lifter.

## Classification

* **PASS**: every `RegData` register matched.
* **FAIL**: `mismatch` (register differs), `crash:SIGBUS guest` (faulting guest access), `crash:brk 0x0`
  (lifter trap), `crash:brk 0x778/0x779` (indirect call/jump to an address the harness could not resolve),
  `assemble-error` (lifter emitted bad ARM64), `translate-error` (no decodable entry).
* **SKIP** with a reason: `unsupported-instruction` (the lifter emitted a `// UNSUPPORTED:` stub; the forms are
  recorded in `results.json` and ranked below), `runtime-helper:<sym>` (lifted code calls a runtime
  helper the harness does not provide), `host-feature:<f>` (AVX-512/AVX-VNNI/AVX-NE-CONVERT/AES256/SHA512/3DNow/EMMI/... a PS5 Zen 2
  CPU lacks), `syscall/host-specific` (syscall, cpuid, rdtsc, xgetbv, int, rdrand), `32bit`, `self-modifying-code`,
  `precision-x87-80bit` (an `X87/` test that fails and uses `tword`/`fbstp`/`fbld`: the lifter models x87 as
  double, so exact 80-bit results cannot match; the `X87_F64/` tests are the meaningful x87 set),
  `nasm-error`, `disabled-upstream`.

## Results (lifter at the m1-fexharness base commit bc94b50)

| Directory | Total | PASS | FAIL | SKIP |
|---|---:|---:|---:|---:|
| (top) | 22 | 15 | 6 | 1 |
| 3DNow | 27 | 0 | 0 | 27 |
| Atomics | 12 | 0 | 12 | 0 |
| ConstProp | 1 | 1 | 0 | 0 |
| FEX_bugs | 128 | 58 | 26 | 44 |
| Flags | 1 | 0 | 1 | 0 |
| GameTests | 1 | 1 | 0 | 0 |
| H0F38 | 86 | 56 | 3 | 27 |
| H0F3A | 45 | 18 | 5 | 22 |
| MMX | 1 | 0 | 0 | 1 |
| Multiblock | 2 | 2 | 0 | 0 |
| OpSize | 110 | 92 | 7 | 11 |
| Primary | 201 | 92 | 64 | 45 |
| PrimaryGroup | 156 | 137 | 14 | 5 |
| REP | 40 | 31 | 3 | 6 |
| REPNE | 27 | 18 | 1 | 8 |
| SSE4a | 4 | 0 | 4 | 0 |
| SSELanePreservation | 231 | 184 | 8 | 39 |
| Secondary | 83 | 52 | 10 | 21 |
| SecondaryModRM | 4 | 0 | 2 | 2 |
| SelfModifyingCode | 3 | 0 | 0 | 3 |
| TwoByte | 221 | 164 | 29 | 28 |
| VEX | 503 | 307 | 30 | 166 |
| X87 | 217 | 19 | 61 | 137 |
| X87_F64 | 150 | 101 | 2 | 47 |
| modrm_oob | 15 | 3 | 2 | 10 |
| **All** | **2291** | **1351** | **290** | **650** |

## Skip reasons

| Reason | Tests |
|---|---:|
| unsupported-instruction | 520 |
| precision-x87-80bit | 38 |
| host-feature (3DNOW) | 34 |
| runtime-helper:_recomp_vpcmpe_stri | 9 |
| runtime-helper:_recomp_vpcmpistri | 8 |
| host-feature (AVX_NE_CONVERT) | 7 |
| host-feature (AES256) | 4 |
| host-feature (AVX_VNNI) | 4 |
| runtime-helper:_recomp_vaeskeygenassist | 3 |
| self-modifying-code | 3 |
| syscall/host-specific (cpuid) | 3 |
| host-feature (AVX_IFMA) | 2 |
| host-feature (Linux) | 2 |
| host-feature (RAND) | 2 |
| syscall/host-specific (int) | 2 |
| host-feature (AFP) | 1 |
| host-feature (AVX_VNNI_INT16) | 1 |
| host-feature (AVX_VNNI_INT8) | 1 |
| host-feature (FSGSBASE) | 1 |
| host-feature (SERIALIZE) | 1 |
| host-feature (SHA512) | 1 |
| syscall/host-specific (rdtsc) | 1 |
| syscall/host-specific (syscall) | 1 |
| syscall/host-specific (xgetbv) | 1 |

## FAIL kinds

| Kind | Tests |
|---|---:|
| mismatch | 206 |
| crash:SIGBUS guest | 53 |
| crash:brk 0x779 | 23 |
| crash:brk 0x0 | 5 |
| assemble-error | 1 |
| crash:brk 0x778 | 1 |
| translate-error | 1 |

## Top failing mnemonics (FAIL tests containing the mnemonic; setup MOV/LEA/CALL/JMP/PUSH/POP excluded)

| Mnemonic | Failing tests | Tests containing it |
|---|---:|---:|
| ADD | 40 | 504 |
| FLD | 37 | 262 |
| LAHF | 36 | 112 |
| AND | 28 | 248 |
| CMP | 25 | 175 |
| FLD1 | 25 | 86 |
| FLDZ | 25 | 70 |
| CLD | 23 | 95 |
| XOR | 20 | 170 |
| VMOVAPS | 19 | 359 |
| FWAIT | 15 | 105 |
| MOVSB | 14 | 231 |
| FNSTSW | 13 | 72 |
| FDIVP | 12 | 17 |
| SAR | 11 | 55 |
| SETE | 11 | 37 |
| INC | 9 | 27 |
| JG | 9 | 48 |
| LDMXCSR | 9 | 11 |
| MOVQ | 9 | 123 |
| MOVSD | 9 | 221 |
| CMPXCHG | 8 | 14 |
| CMPXCHG8B | 8 | 16 |
| MOVAPD | 8 | 194 |
| VMOVAPD | 8 | 417 |
| CLC | 7 | 106 |
| JNE | 7 | 34 |
| MOVAPS | 7 | 117 |
| TEST | 7 | 18 |
| UD2 | 7 | 54 |
| ADC | 6 | 50 |
| FISTP | 6 | 21 |
| JE | 6 | 51 |
| PUSHFQ | 6 | 14 |
| IMUL | 5 | 25 |
| LOOPNE | 5 | 49 |
| MOVZX | 5 | 86 |
| OR | 5 | 137 |
| SBB | 5 | 111 |
| SHL | 5 | 128 |

## Mnemonics with the highest failure rate (at least 3 failing tests)

A test names many mnemonics, so the first table is dominated by setup instructions (ADD, CMP, ...). The rate separates the instruction under test from the setup.

| Mnemonic | Failing | Containing | Rate |
|---|---:|---:|---:|
| EXTRQ | 4 | 4 | 100% |
| INSERTQ | 4 | 4 | 100% |
| LDMXCSR | 9 | 11 | 81% |
| FCHS | 3 | 4 | 75% |
| VPERMILPD | 3 | 4 | 75% |
| FDIVP | 12 | 17 | 70% |
| VPTEST | 4 | 6 | 66% |
| CVTPS2DQ | 3 | 5 | 60% |
| DIV | 3 | 5 | 60% |
| JNP | 3 | 5 | 60% |
| CMPXCHG | 8 | 14 | 57% |
| CMPXCHG8B | 8 | 16 | 50% |
| IDIV | 3 | 6 | 50% |
| XADD | 3 | 6 | 50% |
| NEG | 4 | 9 | 44% |
| NOT | 4 | 9 | 44% |
| PUSHFQ | 6 | 14 | 42% |
| JP | 3 | 7 | 42% |
| TEST | 7 | 18 | 38% |
| POPFQ | 3 | 8 | 37% |
| CMPSB | 4 | 11 | 36% |
| FLDZ | 25 | 70 | 35% |
| INC | 9 | 27 | 33% |
| VMOVDQA | 4 | 12 | 33% |
| FADD | 3 | 9 | 33% |
| LAHF | 36 | 112 | 32% |
| DEC | 4 | 13 | 30% |
| FIST | 3 | 10 | 30% |
| STOSQ | 3 | 10 | 30% |
| SETE | 11 | 37 | 29% |
| FLD1 | 25 | 86 | 29% |
| FISTP | 6 | 21 | 28% |
| JAE | 3 | 12 | 25% |
| CLD | 23 | 95 | 24% |
| JNE | 7 | 34 | 20% |
| SAR | 11 | 55 | 20% |
| IMUL | 5 | 25 | 20% |
| FISTTP | 3 | 15 | 20% |
| JG | 9 | 48 | 18% |
| FNSTSW | 13 | 72 | 18% |

## Top unsupported forms at translate time (tests, skipped as unsupported-instruction)

| Form | Tests |
|---|---:|
| FNINIT | 89 |
| STD | 43 |
| MOV m,r8 | 35 |
| MOV r64,m | 35 |
| VGATHERQPS xmm,m,xmm | 16 |
| VPGATHERQD xmm,m,xmm | 16 |
| MOV r8,m | 15 |
| FXAM | 9 |
| MOV m,r32 | 9 |
| FNSAVE m | 8 |
| FSIN | 8 |
| VGATHERQPD xmm,m,xmm | 8 |
| VGATHERQPD ymm,m,ymm | 8 |
| VPGATHERQQ xmm,m,xmm | 8 |
| VPGATHERQQ ymm,m,ymm | 8 |
| FBSTP m | 7 |
| FLDPI | 7 |
| FPTAN | 7 |
| FRSTOR m | 7 |
| FBLD m | 6 |
| FIDIVR m | 6 |
| FINCSTP | 6 |
| FISUBR m | 6 |
| FYL2X | 6 |
| PCMPESTRM xmm,xmm,imm | 6 |
| PCMPISTRM xmm,xmm,imm | 6 |
| WRGSBASE r64 | 6 |
| FCOS | 5 |
| FYL2XP1 | 5 |
| LCALL m | 5 |
| RDFSBASE r64 | 5 |
| VGATHERDPS xmm,m,xmm | 5 |
| VPCMPESTRM xmm,xmm,imm | 5 |
| VPCMPISTRM xmm,xmm,imm | 5 |
| WRFSBASE r64 | 5 |
| XRSTOR m | 5 |
| XSAVE m | 5 |
| FDECSTP | 4 |
| FPATAN | 4 |
| FSINCOS | 4 |

## Reading the results

* The suite finds real lowering gaps. Highest-value clusters at this commit: AF/PF are not modelled (`LAHF`,
  `PUSHFQ`, `JP/JNP`, and the many `ADD/CMP`-then-`LAHF` tests, e.g. `Primary_39/3B/3D/84/85/A7/A8/A9`);
  `REPNE` string ops; `LDMXCSR` (9 of 11); `CMPXCHG/CMPXCHG8B/XADD/NEG/NOT` on LOCK-prefixed misaligned
  operands (SIGBUS: ARM atomics fault on unaligned/split access, x86 LOCK does not); x87 control and rounding
  (`FDIVP`, `FNSTSW`, `FISTP`, `FLD1/FLDZ` mismatches); 16-bit relative jumps (`66 E9`, decoded as rel32); `FNINIT`
  (89 tests skipped on it alone) and `STD`.
* Setup instructions (`ADD`, `CMP`, `AND`, `FLD`, ...) top the plain count table because a test names many
  mnemonics; use the failure-rate table to find the instruction under test.
* A test that passes is evidence for the exercised forms only. Many VEX tests check just a few GPR sums.
* Not covered: flags are never compared directly (only through `LAHF`/`SETcc`/`PUSHF` in the test), memory is
  hashed by the harness but not compared (FEX tests assert registers only), 32-bit mode tests are not in this
  clone's corpus (none had `Mode`/`bits 32`), the harness reads MXCSR/FPCR defaults only.

## Known harness limitations

* The disassembler stops at privileged opcodes, so HLT handling relies on a byte patch (reachable HLTs only).
* Indirect-branch targets are guessed (see 5 above); a data blob falsely seeded can be lifted but is never entered.
* Targets of an indirect `jmp` into the middle of an instruction stream that no discovered block starts at
  fail with `crash:brk 0x779`; that is a CFG limit, and the same limit applies to the real pipeline without
  symbols.
* No `-addr-mode identity` run yet (needs the L2 loader work); lift with x86 ordering off via `-tso=false`.
* Runtime helpers (`_recomp_vpcmpistri`, `_recomp_vpcmpe_stri`, `_recomp_vaeskeygenassist`) are not linked into
  the harness, so those 20 tests are skipped.


## Rerun on main 2104899

Full corpus rerun on `main` at 2104899 (the merge of m1-clitests, after the FEX-guided lowerings and the MULX fix),
run in the detached worktree `wt/fexrun` with `GOFLAGS=-mod=vendor go build ./...` and `tools/fex_oracle.sh`
(5m47s, exit 0). `tests/fex/results.json` was rewritten from this run. Still 2291 tests; no tests added or removed.

### Totals

| Run | Total | PASS | FAIL | SKIP |
|---|---:|---:|---:|---:|
| Before (m1-fexharness base bc94b50) | 2291 | 1351 | 290 | 650 |
| After (main 2104899) | 2291 | 1465 | 230 | 596 |
| Delta | 0 | +114 | -60 | -54 |

Transitions: FAIL to PASS 64, SKIP to PASS 50, SKIP to FAIL 4 (all `X87/D9_EA..ED.asm`, which were
`unsupported-instruction` (FLDL2E, FLDPI, FLDLG2, FLDLN2) and now lift and run, failing as `mismatch`), PASS to FAIL **0**.

### Per-directory (PASS / FAIL / SKIP; before to after)

| Directory | Total | Before P/F/S | After P/F/S | PASS delta |
|---|---:|---|---|---:|
| (top) | 22 | 15 / 6 / 1 | 18 / 3 / 1 | +3 |
| 3DNow | 27 | 0 / 0 / 27 | 0 / 0 / 27 | +0 |
| Atomics | 12 | 0 / 12 / 0 | 0 / 12 / 0 | +0 |
| ConstProp | 1 | 1 / 0 / 0 | 1 / 0 / 0 | +0 |
| FEX_bugs | 128 | 58 / 26 / 44 | 68 / 17 / 43 | +10 |
| Flags | 1 | 0 / 1 / 0 | 0 / 1 / 0 | +0 |
| GameTests | 1 | 1 / 0 / 0 | 1 / 0 / 0 | +0 |
| H0F38 | 86 | 56 / 3 / 27 | 56 / 3 / 27 | +0 |
| H0F3A | 45 | 18 / 5 / 22 | 18 / 5 / 22 | +0 |
| MMX | 1 | 0 / 0 / 1 | 1 / 0 / 0 | +1 |
| Multiblock | 2 | 2 / 0 / 0 | 2 / 0 / 0 | +0 |
| OpSize | 110 | 92 / 7 / 11 | 94 / 5 / 11 | +2 |
| Primary | 201 | 92 / 64 / 45 | 155 / 35 / 11 | +63 |
| PrimaryGroup | 156 | 137 / 14 / 5 | 144 / 7 / 5 | +7 |
| REP | 40 | 31 / 3 / 6 | 31 / 3 / 6 | +0 |
| REPNE | 27 | 18 / 1 / 8 | 18 / 1 / 8 | +0 |
| SSE4a | 4 | 0 / 4 / 0 | 0 / 4 / 0 | +0 |
| SSELanePreservation | 231 | 184 / 8 / 39 | 190 / 8 / 33 | +6 |
| Secondary | 83 | 52 / 10 / 21 | 52 / 10 / 21 | +0 |
| SecondaryModRM | 4 | 0 / 2 / 2 | 0 / 2 / 2 | +0 |
| SelfModifyingCode | 3 | 0 / 0 / 3 | 0 / 0 / 3 | +0 |
| TwoByte | 221 | 164 / 29 / 28 | 173 / 21 / 27 | +9 |
| VEX | 503 | 307 / 30 / 166 | 311 / 26 / 166 | +4 |
| X87 | 217 | 19 / 61 / 137 | 22 / 65 / 130 | +3 |
| X87_F64 | 150 | 101 / 2 / 47 | 107 / 0 / 43 | +6 |
| modrm_oob | 15 | 3 / 2 / 10 | 3 / 2 / 10 | +0 |

### Newly passing, by directory

Total 114 tests moved to PASS: from FAIL 64, from SKIP 50. By directory: Primary 63, FEX_bugs 10, TwoByte 9, PrimaryGroup 7, SSELanePreservation 6, X87_F64 6, VEX 4, (top) 3, X87 3, OpSize 2, MMX 1.

* FAIL to PASS by directory: Primary 29, FEX_bugs 9, TwoByte 8, PrimaryGroup 7, VEX 4, (top) 3, OpSize 2, X87_F64 2
* SKIP to PASS by directory: Primary 34, SSELanePreservation 6, X87_F64 4, X87 3, FEX_bugs 1, MMX 1, TwoByte 1

Largest gains are in `Primary` (+63, most of them FAIL to PASS), `FEX_bugs` (+10), `TwoByte` (+9) and
`PrimaryGroup` (+7). `MMX` moved from 1 SKIP to 1 PASS.

### Regressions (PASS to FAIL/SKIP)

None. No test that passed before fails or skips now.

### Failure and skip reasons after the rerun

| FAIL kind | Before | After |
|---|---:|---:|
| mismatch | 206 | 146 |
| crash:SIGBUS guest | 53 | 53 |
| crash:brk 0x779 | 23 | 23 |
| crash:brk 0x0 | 5 | 5 |
| crash:brk 0x778 | 1 | 1 |
| assemble-error | 1 | 1 |
| translate-error | 1 | 1 |

The `mismatch` count drops by 60; the crash kinds are unchanged, so the remaining FAIL work is the SIGBUS
misaligned LOCK forms, the indirect-branch CFG limit (`brk 0x779`), and the `brk 0x0` lifter traps.

Largest skip buckets after: unsupported-instruction 468 (was 520),
precision-x87-80bit 36 (was 38), host-feature:3DNOW 34 (was 34).
