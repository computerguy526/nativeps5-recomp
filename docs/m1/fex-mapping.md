# FEX-Emu mapping for lifter gaps (M1 step 1)

Scope: x86 forms that `pkg/lifter` does not lower, or lowers with a known wrong result, measured on the inputs we have. Each row gives the x86 form, where the lifter stands today, how often the form occurs, the FEX-Emu dispatcher function that implements it, a one-line ARM64 lowering idea, and the FEX unit test(s) that cover it.

Priority 1 rows block the M1 test suite (`tests/ps5/build/work/*/out/unsupported.txt`) and come first. Priority 2 rows are gaps or wrong results in the Syobon / PS5 SDK binaries.

Reference: FEX-Emu (MIT License, Copyright (c) 2019 Ryan Houdek). Paths below are relative to the FEX sparse clone, `FEXCore/Source/Interface/Core/`. The clone is read only; nothing was built or run.

## Method

- Translate: `GOFLAGS=-mod=vendor go build ./cmd/ps4-recomp`, then `ps4-recomp -c=false -allow-unsupported -o <dir> <elf>`.
  - Syobon eboot (`work/s2/out/syobon_eboot.elf`): 2 stubs, both PSHUFW. Matches the census in `docs/m1/lifter.md`.
  - PS5 SDK `hello_cxx.elf`: 0 stubs. PS5 `hello_world.elf`: 0 stubs.
- Occurrence counts are distinct guest addresses in the `// 0xADDR: MNEMONIC operands` comments of the emitted `code_*.s`. Each lowered instruction appears once. Counts are per mnemonic (first token).
- The `tests/ps5` test ELFs and PS4 `hello_world` were not re-translated here. Their stub sites come from the `unsupported.txt` files the coordinator named, or from the coordinator's description.
- Flags are listed as written by x86: `Z`, `S`, `C`, `O`, `P` (PF), `A` (AF), `D` (DF).

## Priority 1: blocks the M1 test suite

| # | x86 form | Lifter today | Occurrences (Syobon / cxx / hello_world / test ELFs) | FEX dispatcher (file:line) | ARM64 lowering idea | Flags | FEX ASM test(s) and RegData location |
|---|---|---|---|---|---|---|---|
| P1-1 | `STD` (FD) | Stub: `arm64_lifter.go:1059` ("std: backward string operations not lowered"). `CLD` is a nop at `arm64_lifter.go:1053`. The string ops in `arm64_lifter_string.go:32` assume DF=0. | 0 / 0 / 0. Test ELFs: `t_arith` 1 (0xb1f4 noplt, 0xb214 plt), `t_string` 1 (0x9d37 noplt, 0x9d27 plt). | Table entry `BaseTables.h:86` (0xFC group, covers FC/FD) → `FLAGControlOp` `OpcodeDispatcher.cpp:1208`. CLD at `:1222` stores DF=+1, STD at `:1226` stores DF=-1. String ops read DF at `OpcodeDispatcher.h:296`. | Keep DF in a ctx slot as a stride (+1/-1). STD/CLD write it. Each string op advances by `width * stride` (`cneg`/`csneg` on the stored sign). REP loops use the same stride. | D (written by STD/CLD); read by string ops | `unittests/ASM/Primary/Primary_A6_REP_down.asm`: RegData lines 3-8 (RAX 0x8300, RCX 0x9, RDX 0, RDI 0xE000000C, RSI 0xE000001C), `std` at line 36. `unittests/ASM/Primary/Primary_AD_REPNE_qword_down.asm`: RegData lines 3-6 (RAX 0x5152535455565758, RSI 0xE0000000), `std` at line 33. Also `Primary/Primary_A4_REP_Down.asm`, `Primary/Primary_A4_REP_Down_Overlapping.asm`, `FEX_bugs/repeat_stringops_crash.asm`. |
| P1-2 | `FCOMIP st, st(i)` (DF F0+i) | Missing. `arm64_lifter_fpu.go:403` handles FUCOMI, FUCOMIP and FCOMI but not FCOMIP. `arm64_lifter.go:1893` dispatch list also lacks it, so it falls to the stub path. | 0 / 0 / 0 (Syobon, cxx not affected). Test ELFs: `t_x87` noplt 2 (0xa0f4, 0xa134), plt 2 (0xa144, 0xa184). PS4 `hello_world`: coordinator reports it, not re-counted here. | Table `X87Tables.cpp:802` (DF F0+i, FLAGS_POP). Handler `FCOMI` `X87.cpp:659` (PopTwice). EFLAGS write is the non-`FLAGS_X87` branch at `X87.cpp:690`. | Copy the FUCOMIP path (`fpu.go:403-420`) and add the pop. Use `fcmpe` for FCOMI/FCOMIP (x86 signals invalid on any NaN) and `fcmp` for FUCOMI/FUCOMIP (quiet NaN does not signal). Only the sticky IE bit differs, and it is masked by default. | Z, P (V), C written; O, S cleared | `unittests/ASM/X87/DF_F0.asm`: RegData lines 3-7 (RBX 0, RCX 1, RDI 0x40). `unittests/ASM/X87/DF_E8.asm` (FUCOMIP) and `X87/DB_E8.asm` (FUCOMI) share the same expected registers. `X87/DB_F0.asm` (FCOMI). |
| P1-3 | `FLDPI` (D9 EB) | Missing: no case in `arm64_lifter_fpu.go`. Stub. | 0 / 0 / 0. Test ELFs: `t_x87` noplt 1 (0xa1e0), plt 1 (0xa230). | Table `X87Tables.cpp:356` → `FLD_Const(NAMED_VECTOR_X87_PI)`, implementation `X87.cpp:94-99`. | Same shape as FLDL2T (`arm64_lifter_fpu.go:579`): `movz`/`movk` the double pi bits `0x400921FB54442D18` into x16, `fmov d28, x16`, then `emitFpuPushD0`. Our x87 is double precision, so the 80-bit value is not reproduced exactly (FEX has the same reduced-precision mode, see test). | none | `unittests/ASM/X87_F64/D9_EB_F64.asm`: RegData lines 3-5 (RAX 0x40490fdb, the float32 rounding of pi), env `FEX_X87REDUCEDPRECISION=1`. `unittests/ASM/X87/D9_EB.asm`: RegData MM7 = [0xC90FDAA22168C235, 0x4000] (80-bit pi), which our double model cannot match exactly. |
| P1-4 | `FLDL2E` (D9 EA) | Missing (stub). | 0 / 0 / 0 (not in inputs). | Table `X87Tables.cpp:355` → `FLD_Const(NAMED_VECTOR_X87_LOG2_E)`, `X87.cpp:94-99`. | `movz`/`movk` double log2(e) = `0x3FF71547652B82FE` into x16, `fmov d28, x16`, push. | none | `unittests/ASM/X87_F64/D9_EA_F64.asm`: RegData lines 3-5 (RAX 0x3fb8aa3b, float32). `unittests/ASM/X87/D9_EA.asm`. |
| P1-5 | `FLDLG2` (D9 EC) | Missing (stub). | 0 / 0 / 0. | Table `X87Tables.cpp:357` → `FLD_Const(NAMED_VECTOR_X87_LOG10_2)`. | double log10(2) = `0x3FD34413509F79FF`, same shape as P1-4. | none | `unittests/ASM/X87_F64/D9_EC_F64.asm`: RAX 0x3e9a209b (float32). `unittests/ASM/X87/D9_EC.asm`. |
| P1-6 | `FLDLN2` (D9 ED) | Missing (stub). | 0 / 0 / 0. | Table `X87Tables.cpp:358` → `FLD_Const(NAMED_VECTOR_X87_LOG_2)`. | double ln(2) = `0x3FE62E42FEFA39EF`, same shape as P1-4. | none | `unittests/ASM/X87_F64/D9_ED_F64.asm`: RAX 0x3f317218 (float32). `unittests/ASM/X87/D9_ED.asm`. |
| P1-7 | `FCOMI st, st(i)` (DB F0+i), `FUCOMI st, st(i)` (DB E8+i), `FUCOMIP st, st(i)` (DF E8+i) | Lowered: `arm64_lifter_fpu.go:403-420`, dispatch `arm64_lifter.go:1893`. Only FCOMI needs a change: it uses `fcmp`, and it should use `fcmpe` (see P1-2). | FUCOMI 0 / 94 / 0; FUCOMIP 0 / 9 / 0; FCOMI 0 / 0 / 0. | Table `X87Tables.cpp:689` (DB E8, FUCOMI), `:691` (DB F0, FCOMI), `:800` (DF E8, FUCOMIP). Handler `X87.cpp:659`. | Already lowered. Change only the FCOMI variant to `fcmpe`. | Z, P (V), C | `unittests/ASM/X87/DB_E8.asm`, `X87/DF_E8.asm` (RegData: RBX 0, RCX 1, RDI 0x40, RSI 0x45, RBP 0x45), `X87/DB_F0.asm` (RBX 0, RCX 1, RDI 0x40). |
| P1-8 | `FLD1` (D9 E8), `FLDZ` (D9 EE), `FLDL2T` (D9 E9) | Lowered: `arm64_lifter_fpu.go:123` (FLD1), `:118` (FLDZ), `:579` (FLDL2T). Recorded here as the pattern for P1-3..6. | FLDZ 1 / 109 / 0; FLD1 0; FLDL2T 0. | `X87Tables.cpp:353` (FLD1), `:354` (FLDL2T), `:359` (FLDZ), all `FLD_Const`. | Already lowered. | none | `unittests/ASM/X87/D9_E8.asm` (MM7 = [0x8000000000000000, 0x3FFF] = 1.0), `X87/D9_E9.asm` (MM7 = [0xD49A784BCD1B8AFE, 0x4000] = log2 10), `X87/D9_EE.asm` (zero). |

## Priority 2: wrong results or unlowered forms in the measured binaries

| # | x86 form | Lifter today | Occurrences (Syobon / cxx / hello_world) | FEX dispatcher (file:line) | ARM64 lowering idea | Flags | FEX ASM test(s) and RegData location |
|---|---|---|---|---|---|---|---|
| P2-1 | `PSHUFW mm, mm/m64, imm8` (NP 0F 70) | Stub. No case in the MMX switch `arm64_lifter_mmx.go:203`. | 2 / 0 / 0 (sites 0x22f494 `PSHUFW M0,M0,0x44` and 0x22f567 `PSHUFW M2,M2,0x44`, both in fn 0x22f430). | Table `SecondaryTables.h:90` → `PSHUFW8ByteOp` `Vector.cpp:968` (64-bit MMX path). Do not confuse with `PSHUFWOp` `Vector.cpp:1488`, which is the 66/F2 XMM form (`SecondaryTables.h:172`, `:199`). | Load mm into v16.8b. Build the byte index vector at translate time from imm8: lane i takes bytes `2*sel_i` and `2*sel_i+1`. `ldr d17` from a literal, then `tbl v16.8b, {v16.16b}, v17.8b`. Identity (0xE4) is a copy. Single-register table, so it respects the scratch-rename constraint in `docs/m1/lifter.md` section 3. | none | `unittests/ASM/MMX/pshufw.asm`: RegData lines 3-5, RAX 0xd0f9338e (CRC over all 256 immediates, register and memory forms). `unittests/ASM/TwoByte/0F_70.asm`: RegData line 3, cases at lines 26-27. |
| P2-2 | PF after an integer ALU op (ADD/ADC/SUB/SBB/AND/OR/XOR/TEST/CMP/INC/DEC/NEG/SHIFTS) | Not computed. V holds OF after integer ops. PF is read from V only after FP compares: JP/JNP/SETP/SETNP/CMOVP/CMOVNP use `vs`/`vc` via `condFor`, `arm64_lifter.go:331-352`. A JP after an integer op therefore reads OF. | Consumers: JP 60, JNP 50, SETP 6, SETNP 2 (118 total). 112 of them follow an FP compare (adjacent by address). 6 JP sites have no flag writer before them (listed in the appendix). 0 in hello_cxx. CMOVP/CMOVNP 0. | `CalculatePF` `Flags.cpp:222-225` (stores the raw 8-bit result; parity is lazy). `LoadPFRaw` `Flags.cpp:174-180` (computes parity at read). Integer writers: `ADCOp` `OpcodeDispatcher.cpp:259`, `TESTOp` `:1004`/`:1008`, `INCOp` `:3240`. Logical flags `Flags.cpp:430`. | Keep the lazy scheme FEX uses: each integer flag writer stores the low result byte to a ctx slot only where a PF reader is reachable (flags liveness). A PF reader computes parity from that byte: `eor w1,w0,w0,lsr #4; eor w1,w1,w1,lsr #2; eor w1,w1,w1,lsr #1`, PF = `~w1 & 1`. Keep V = OF. Do not change the FP-compare PF in V. | P (read), and written by integer ops | `unittests/ASM/FEX_bugs/xor_flags.asm`: RegData line 3, RAX 0x4600. After XOR, AH=0x46, so PF (bit 2) = 1 and ZF (bit 6) = 1, AF (bit 4) = 0. `unittests/ASM/FEX_bugs/Test_JP.asm`: RegData line 4, RAX 0x0000000cafecafe. `unittests/ASM/FEX_bugs/ShiftPF.asm`: RegData lines 4-5 (RBX 0x6, RAX 1), `setp al` at line 23. |
| P2-3 | `PUSHFQ` (9C), `POPFQ` (9D) | PUSHFQ: `liftPushfq` `arm64_lifter_ext.go:614`, PF and AF stored as 0. POPFQ: dispatch `arm64_lifter.go:1999`, `liftPopfq`. | PUSHFQ 4, POPFQ 2 (Syobon only). Sites: PUSHFQ 0x1c21a0, 0x1c21ac, 0x1c23b8, 0x1c23c4; POPFQ 0x1c21ab, 0x1c23c3. | `PUSHFOp` `OpcodeDispatcher.cpp:3761` (`GetPackedRFLAG`, so PF and AF come from the lazy state). `POPFOp` `:3767`. | PUSHFQ builds the PF bit from the lazy parity (P2-2) and the AF bit from the lazy AF (P2-5). POPFQ stores bit 2 into an explicit PF override slot, not into the parity source. | all six read/written | `unittests/ASM/FEX_bugs/InitialPFFlag.asm`: RegData line 4, RAX 0x202 (pushfq of initial flags, PF = 0). `unittests/ASM/FEX_bugs/xor_flags.asm` (sahf/lahf round trip, RAX 0x4600). |
| P2-4 | `SAHF` (9E) | Lowered wrongly: `liftFlagsOp` (`arm64_lifter_alu.go:2021`, SAHF case) restores only SF, ZF and CF. AH bit 2 (PF) and bit 4 (AF) are ignored. | 0 / 0 / 0 in inputs. | `SAHFOp` `OpcodeDispatcher.cpp:1187` (packs AH into RFLAGS). | Store AH bit 2 as an explicit PF override (see P2-3). Ignore AF until P2-5 is done. | S, Z, C, P, A | `unittests/ASM/FEX_bugs/xor_flags.asm`: `sahf` at line 14, RegData line 3. |
| P2-5 | AF (adjust flag) | Never modelled (`arm64_lifter_alu.go:2035`, `arm64_lifter_ext.go:613`). Written as 0 by LAHF/PUSHFQ. | AF readers in inputs: PUSHFQ/POPFQ sites from P2-3 (6). BCD readers: 0. | `CalculateAF` `Flags.cpp:227`. `ZeroPF_AF` `Flags.cpp:29` (used by the AVX integer ops, e.g. `AVX_128.cpp:1804`). | AF = bit 4 of `a ^ b ^ res`. Keep operands or the XOR lazily, only where a PUSHFQ/LAHF reader is reachable. | A (read by PUSHFQ/LAHF only) | `unittests/ASM/FEX_bugs/xor_flags.asm` covers AF=0 after a logical op (AH=0x46 has bit 4 clear). No test reads AF after an arithmetic op in the files I opened. |
| P2-6 | BCD: `DAA` (27), `DAS` (2F), `AAA` (37), `AAS` (3F), `AAM` (D4), `AAD` (D5) | Stub: no case; falls to the default branch in `liftInstructionInternal` (`arm64_lifter.go:1024`). | 0 / 0 / 0. | `DAAOp` `OpcodeDispatcher.cpp:2911`, `DASOp` `:2939`, `AAMOp` `:2992`, `AADOp` `:3007`. | None needed. All six are invalid in 64-bit mode (#UD), so a trap is correct. Keep the stub and do not lower them. | A, C, S, Z, P | Not read in this pass. |

## Notes

- The P1 forms can all be lowered with the same pattern as FLDL2T and FCOMIP-minus-pop. The only fix to existing code is the FCOMI `fcmpe` change in P1-7.
- Several PF readers are on the FP path already handled: 112 of 124 PF consumers in Syobon follow an FP compare. The 6 JP sites without a flag writer nearby are a flow question, and the appendix lists them.
- x87 is double precision in the lifter. The F64 FEX tests check float32 rounding (`FEX_X87REDUCEDPRECISION=1`), so they are the comparable ones for our model. The non-F64 `X87/*.asm` tests check 80-bit values, which our model cannot match bit for bit.
- Not counted here: PS4 `hello_world` FCOMIP (reported by the coordinator, not re-translated), and the `tests/ps5` binaries (coordinator's `unsupported.txt`).

## Appendix A: PF and AF consumer sites in Syobon

Each row is a guest address, the instruction, the nearest preceding flag writer skipping Jcc/SETcc/CMOVcc (by address order, which approximates the flow predecessor), and a class:

- `fp_compare`: that writer is a (V)(U)COMIS, FCOMI/FUCOMI(P), FCOM(P) or FTST/FXAM.
- `other`: that writer is not an FP compare; for the 6 JP sites it needs flow analysis.

PUSHFQ and POPFQ are listed under the same classes. They are AF and PF consumers, so the class is relevant to them too.

| Address | Instruction | Preceding flag writer | Class |
|---|---|---|---|
| `0x3398d` | `JP .+45` | `0x33987 VUCOMISS X2, X3` | fp_compare |
| `0x339ce` | `JP .+50` | `0x339c8 VUCOMISS X0, X3` | fp_compare |
| `0x33a12` | `JP .+10` | `0x33a0c VUCOMISS X4, X1` | fp_compare |
| `0x33a2b` | `JP .+11` | `0x33a25 VUCOMISS X3, X1` | fp_compare |
| `0x34a2a` | `JNP .-66` | `0x34a24 VUCOMISS X0, X1` | fp_compare |
| `0x37579` | `JNP .+3703` | `0x37573 VUCOMISS X5, X4` | fp_compare |
| `0x37589` | `JNP .+3687` | `0x37583 VUCOMISS X3, X2` | fp_compare |
| `0x3f2cb` | `JNP .+489` | `0x3f2c5 VUCOMISS X3, X4` | fp_compare |
| `0x3f365` | `JNP .+156` | `0x3f35f VUCOMISS X2, X3` | fp_compare |
| `0x3f5cd` | `JNP .+1435` | `0x3f5c7 VUCOMISS X0, X2` | fp_compare |
| `0x3f806` | `JNP .+236` | `0x3f7ff VUCOMISS X5, [RBP-0x50]` | fp_compare |
| `0x3f906` | `JNP .-412` | `0x3f900 VUCOMISS X1, X4` | fp_compare |
| `0x3fa00` | `JNP .+1828` | `0x3f9fa VUCOMISS X6, X2` | fp_compare |
| `0x3fa87` | `JNP .+641` | `0x3fa81 VUCOMISS X12, X2` | fp_compare |
| `0x3fb89` | `JNP .-1571` | `0x3fb83 VUCOMISS X0, X4` | fp_compare |
| `0x3fd19` | `JNP .+248` | `0x3fd12 VUCOMISS X6, X12` | fp_compare |
| `0x3fe47` | `JNP .+215` | `0x3fe40 VUCOMISS X2, X8` | fp_compare |
| `0x3ff2f` | `JNP .+182` | `0x3ff29 VUCOMISS X8, X4` | fp_compare |
| `0x4002e` | `JNP .+535` | `0x40028 VUCOMISS X0, X2` | fp_compare |
| `0x4014c` | `JNP .+519` | `0x40146 VUCOMISS X0, X6` | fp_compare |
| `0x4028c` | `JNP .+443` | `0x40285 VUCOMISS X2, X8` | fp_compare |
| `0x40381` | `JNP .+572` | `0x4037b VUCOMISS X0, X4` | fp_compare |
| `0x4045d` | `JNP .+352` | `0x40457 VUCOMISS X8, X4` | fp_compare |
| `0x40613` | `JNP .+403` | `0x4060d VUCOMISS X0, X3` | fp_compare |
| `0x406f1` | `JNP .+1130` | `0x406eb VUCOMISS X2, X3` | fp_compare |
| `0x407c2` | `JNP .+1330` | `0x407b8 VUCOMISS X0, [RBP-0x170]` | fp_compare |
| `0x408d0` | `JNP .+150` | `0x408ca VUCOMISS X2, X4` | fp_compare |
| `0x4097e` | `JNP .+179` | `0x40978 VUCOMISS X0, X2` | fp_compare |
| `0x40a45` | `JNP .+687` | `0x40a3f VUCOMISS X4, X2` | fp_compare |
| `0x40b73` | `JNP .+179` | `0x40b6d VUCOMISS X0, X2` | fp_compare |
| `0x40c3a` | `JNP .+186` | `0x40c34 VUCOMISS X3, X2` | fp_compare |
| `0x40f41` | `JNP .+176` | `0x40f3b VUCOMISS X1, X3` | fp_compare |
| `0x41057` | `JNP .+210` | `0x41051 VUCOMISS X0, X3` | fp_compare |
| `0x4113a` | `JNP .+219` | `0x41134 VUCOMISS X5, X2` | fp_compare |
| `0x52fb6` | `SETNP AL` | `0x52fb0 VUCOMISD X0, [RBP+8*RCX-0x40]` | fp_compare |
| `0x53478` | `SETP AL` | `0x53470 VUCOMISD X7, [+0x2a8ca8]` | fp_compare |
| `0x5362b` | `SETP AL` | `0x53627 VUCOMISD X7, X0` | fp_compare |
| `0xc758c` | `JP .+34` | `0xc7586 VUCOMISS X0, X1` | fp_compare |
| `0xc759b` | `JP .+19` | `0xc7595 VUCOMISS X0, X1` | fp_compare |
| `0xcffd9` | `JP .+86` | `0xcffd3 VUCOMISS X0, X1` | fp_compare |
| `0x17fada` | `JP .+20` | `0x17fad0 VUCOMISD X1, [+0x17c648]` | fp_compare |
| `0x1c21a0` | `PUSHFQ` | `0x1c2199 MOV [RBP-0x4], 0x0` | other |
| `0x1c21ab` | `POPFQ` | `0x1c21aa PUSH RAX` | other |
| `0x1c21ac` | `PUSHFQ` | `0x1c21ab POPFQ` | other |
| `0x1c23b8` | `PUSHFQ` | `0x1c23b0 MOV [RSP+Reg(0)-0x8], 0x0` | other |
| `0x1c23c3` | `POPFQ` | `0x1c23c2 PUSH RAX` | other |
| `0x1c23c4` | `PUSHFQ` | `0x1c23c3 POPFQ` | other |
| `0x1cc258` | `JP .+4` | `0x1cc252 VCVTSD2SS X0, X0, X0` | other |
| `0x1cc269` | `JP .+4` | `0x1cc263 VSUBSS X1, X2, X1` | other |
| `0x1cecf8` | `JNP .+360` | `0x1cecee VUCOMISS X0, [+0x13f062]` | fp_compare |
| `0x1ced9b` | `JNP .+197` | `0x1ced91 VUCOMISS X0, [+0x13efbf]` | fp_compare |
| `0x1cee79` | `JNP .+631` | `0x1cee6f VUCOMISS X0, [+0x13eee1]` | fp_compare |
| `0x1ceffb` | `JNP .+123` | `0x1ceff1 VUCOMISS X3, [+0x13ed5f]` | fp_compare |
| `0x1cf087` | `JNP .+109` | `0x1cf07d VUCOMISS X3, [+0x13ecd3]` | fp_compare |
| `0x1cfb92` | `JP .+12` | `0x1cfb8c VUCOMISS X0, X2` | fp_compare |
| `0x1cfb9a` | `JNP .+426` | `0x1cfb94 VUCOMISS X1, X2` | fp_compare |
| `0x1d1add` | `JP .+33` | `0x1d1ad7 VUCOMISS X4, X0` | fp_compare |
| `0x1d1ae9` | `JP .+21` | `0x1d1ae3 VUCOMISS X3, X0` | fp_compare |
| `0x1d1afa` | `JNP .-139` | `0x1d1af4 VUCOMISS X0, X3` | fp_compare |
| `0x1de941` | `JP .+26` | `0x1de93b VUCOMISS X0, X2` | fp_compare |
| `0x1de94d` | `JP .+14` | `0x1de947 VUCOMISS X1, X3` | fp_compare |
| `0x1de9dd` | `JP .+42` | `0x1de9d6 MOV EBX, 0x657` | other |
| `0x1de9e8` | `JP .+31` | `0x1de9df VUCOMISS X3, [R14+R15+0x8]` | fp_compare |
| `0x1de9f3` | `JNP .+240` | `0x1de9ea VUCOMISS X0, [R14+R15+0xc]` | fp_compare |
| `0x1e2edb` | `JP .+699` | `0x1e2ed1 VUCOMISS X1, X2` | fp_compare |
| `0x1e2ef7` | `JP .+876` | `0x1e2eed VUCOMISS X0, X1` | fp_compare |
| `0x1e3196` | `JNP .-699` | `0x1e3190 VUCOMISS X1, X2` | fp_compare |
| `0x1e31ec` | `JNP .-757` | `0x1e31e6 VUCOMISS X0, X1` | fp_compare |
| `0x1e3263` | `JNP .-876` | `0x1e325d VUCOMISS X0, X1` | fp_compare |
| `0x1e32c5` | `JNP .-974` | `0x1e32bf VUCOMISS X0, X1` | fp_compare |
| `0x1e6d14` | `JP .+24` | `0x1e6d0a VUCOMISS X0, [+0x127046]` | fp_compare |
| `0x1e6d28` | `JNP .+196` | `0x1e6d1e VUCOMISS X0, [+0x127032]` | fp_compare |
| `0x1e6eae` | `JP .+24` | `0x1e6ea4 VUCOMISS X0, [+0x126eac]` | fp_compare |
| `0x1e6ec2` | `JNP .+198` | `0x1e6eb8 VUCOMISS X0, [+0x126e98]` | fp_compare |
| `0x1e76b5` | `JP .+24` | `0x1e76ab VUCOMISS X0, [+0x1266a5]` | fp_compare |
| `0x1e76c9` | `JNP .+1249` | `0x1e76bf VUCOMISS X0, [+0x126691]` | fp_compare |
| `0x1e7863` | `JP .+21` | `0x1e785c VUCOMISS X0, [RAX+8*RCX]` | fp_compare |
| `0x1e799b` | `JP .+37` | `0x1e7995 MOV [RSI+0x14], R11L` | other |
| `0x1e79c8` | `JP .+39` | `0x1e79c2 VUCOMISS X4, X0` | fp_compare |
| `0x1e7f79` | `JP .+21` | `0x1e7f71 VUCOMISS X1, [R13]` | fp_compare |
| `0x1e7f83` | `JNP .+43` | `0x1e7f7b VUCOMISS X3, [R13+0x4]` | fp_compare |
| `0x1e7fa6` | `JP .+8` | `0x1e7fa0 VUCOMISS X0, X1` | fp_compare |
| `0x1e7fae` | `JNP .-112` | `0x1e7fa8 VUCOMISS X2, X3` | fp_compare |
| `0x1e7fba` | `JP .+128` | `0x1e7fb0 VUCOMISS X0, X1` | fp_compare |
| `0x1e804a` | `JP .-397` | `0x1e8040 VUCOMISS X2, X3` | fp_compare |
| `0x1e997d` | `JP .-86` | `0x1e9977 VUCOMISD X2, X1` | fp_compare |
| `0x1ea60d` | `JP .+33` | `0x1ea607 VUCOMISS X0, X4` | fp_compare |
| `0x1ea615` | `JP .+25` | `0x1ea60f VUCOMISS X3, X5` | fp_compare |
| `0x1ea63a` | `JP .+912` | `0x1ea630 VUCOMISS X2, X5` | fp_compare |
| `0x1ea64a` | `JP .+896` | `0x1ea640 VUCOMISS X1, X4` | fp_compare |
| `0x1ea78a` | `JNP .-1265` | `0x1ea780 VUCOMISS X1, [+0x1235d8]` | fp_compare |
| `0x1ea79e` | `JNP .-1285` | `0x1ea794 VUCOMISS X0, [+0x1235c4]` | fp_compare |
| `0x1ebcbe` | `JP .+20` | `0x1ebcb4 VUCOMISS X0, [+0x12209c]` | fp_compare |
| `0x1ebcd2` | `JNP .+64` | `0x1ebcc8 VUCOMISS X0, [+0x122088]` | fp_compare |
| `0x1ebdc0` | `JP .+90` | `0x1ebdbb MOVSXD R14, R14L` | other |
| `0x1ebdd1` | `JP .+73` | `0x1ebdc9 VUCOMISS X1, [RCX+RBX+0x4]` | fp_compare |
| `0x1ebdeb` | `JP .+47` | `0x1ebde3 VUCOMISS X1, [RDX+R14]` | fp_compare |
| `0x1ebdfd` | `JP .+29` | `0x1ebdf4 VUCOMISS X1, [RDX+R14+0x4]` | fp_compare |
| `0x1ebe35` | `JP .+90` | `0x1ebe2f VUCOMISS X1, X0` | fp_compare |
| `0x1ebe46` | `JP .+73` | `0x1ebe3e VUCOMISS X1, [RCX+RBX+0x4]` | fp_compare |
| `0x1ebe60` | `JP .+47` | `0x1ebe58 VUCOMISS X1, [RDX+R14]` | fp_compare |
| `0x1ebe72` | `JP .+29` | `0x1ebe69 VUCOMISS X1, [RDX+R14+0x4]` | fp_compare |
| `0x1ebeaa` | `JP .+253` | `0x1ebea0 VUCOMISS X1, X0` | fp_compare |
| `0x1ebec2` | `JP .+229` | `0x1ebeb6 VUCOMISS X0, [RCX+RBX+0x4]` | fp_compare |
| `0x1ebedf` | `JP .+200` | `0x1ebed3 VUCOMISS X0, [RDX+R14]` | fp_compare |
| `0x1ebf00` | `JP .+45` | `0x1ebefa VUCOMISS X1, X0` | fp_compare |
| `0x1ebf11` | `JP .+28` | `0x1ebf09 VUCOMISS X1, [RCX+RBX+0x4]` | fp_compare |
| `0x1ebf46` | `JP .+40` | `0x1ebf40 VUCOMISS X1, X0` | fp_compare |
| `0x1ebf56` | `JP .+24` | `0x1ebf4e VUCOMISS X1, [RCX+RBX+0x4]` | fp_compare |
| `0x1ebf85` | `JP .+38` | `0x1ebf7f VUCOMISS X1, X0` | fp_compare |
| `0x1ebf95` | `JP .+22` | `0x1ebf8d VUCOMISS X0, [RCX+RBX+0x4]` | fp_compare |
| `0x1ef443` | `JP .+20` | `0x1ef43e MOV RDX, RSI` | other |
| `0x1ef44f` | `JP .+8` | `0x1ef445 VUCOMISS X1, [+0x11e90b]` | fp_compare |
| `0x2069e3` | `JP .+94` | `0x2069dd VUCOMISD X0, X1` | fp_compare |
| `0x206cba` | `JP .+141` | `0x206cb0 VUCOMISD X0, X1` | fp_compare |
| `0x20e534` | `SETNP AL` | `0x20e52c VUCOMISD X0, [+0xedb94]` | fp_compare |
| `0x20e9ad` | `SETP AL` | `0x20e9a9 VUCOMISD X0, X1` | fp_compare |
| `0x20ebf0` | `SETP CL` | `0x20ebec VUCOMISS X0, X1` | fp_compare |
| `0x20f767` | `SETP AL` | `0x20f75f VUCOMISD X2, [+0xeca51]` | fp_compare |
| `0x20fa0b` | `SETP AL` | `0x20fa03 VUCOMISD X2, [+0xec7ad]` | fp_compare |
| `0x216697` | `JP .+62` | `0x216691 VUCOMISS X0, X1` | fp_compare |
| `0x2166e5` | `JP .+245` | `0x2166d7 VUCOMISS X0, [+0xf7679]` | fp_compare |
| `0x24d86e` | `JP .+40` | `0x24d868 VUCOMISS X9, X7` | fp_compare |
| `0x24d89e` | `JP .+53` | `0x24d898 VUCOMISS X8, X6` | fp_compare |

Counts: 124 sites, of which fp_compare 112 (JP 54, JNP 50, SETP 6, SETNP 2) and other 12 (JP 6, PUSHFQ 4, POPFQ 2).

## Implementation status (branch m1-fex)

Everything in the Priority 1 and Priority 2 tables is lowered, except P2-6 (BCD, invalid in 64-bit mode, stays a trap). Code: `pkg/lifter/arm64_lifter_parity.go` (PF/AF/DF), `arm64_lifter_string.go` (direction), `arm64_lifter_fpu.go` (FCOMIP, constants), `arm64_lifter_mmx.go` (PSHUFW), `pkg/disasm/parity.go` (liveness and PF location). Attribution: `NOTICE`.

### Model

| State | Where | Encoding |
|---|---|---|
| DF | `GuestContext.df` (0x9e) | 0 or 1. STD/CLD/POPFQ write it. Every string instruction reads it and steps RSI/RDI by `width - 2*width*df`. PUSHFQ reports it in bit 10. It is never assumed 0. |
| PF (integer) | `GuestContext.pf` (0x9c) | Lazy, as in FEX: a byte whose parity is the flag (PF = 1 iff even). Writers store the low byte of the result. |
| AF | `GuestContext.af` (0x9d) | Lazy: AF = bit 4 of `af ^ pf`. Writers store the XOR of their operands. |
| PF (FP compare) | host V | Unchanged from before. V stays OF after integer operations. |

Two passes in `pkg/disasm` keep this free where nobody reads the flags:

- PF/AF liveness (two more bits in the CF/OF pass). A writer stores the bytes only when a reader (JP/JNP, SETP/SETNP, CMOVP/CMOVNP, FCMOVU/FCMOVNU, LAHF, PUSHFQ) can follow before the next writer. An indirect jump without a recovered table counts as reaching every block of the function or a function entry (the dispatcher enters nothing else), so PF/AF are live across it only when some block reads them.
- PF location (forward). Each instruction is told whether PF is in host V (every path comes from an FP compare through lowerings that keep NZCV) or in the context byte. A V-held PF is stored only where a successor expects the byte, or before a lowering that keeps PF but rewrites V (rotates, variable shifts, REP SCAS/CMPS).

Integer writers record PF/AF from their operands before the lowering runs (the low byte of the result depends only on the low bytes of the operands and the carry in), so no ALU lowering changed. Readers with PF in the byte do not touch NZCV: JP/JNP test the parity bit with TBZ/TBNZ; SETP/CMOVP/FCMOVU save NZCV, put PF in V for the existing lowering and restore it.

### x87 precision

The x87 stack holds doubles. FLDPI, FLDL2E, FLDLG2, FLDLN2 (and FLDL2T, FLD1, FLDZ) load the double nearest to the exact constant, which is what FSTP m64 of the hardware's 64-bit-mantissa constant stores under round to nearest. This is FEX's reduced-precision mode, and the `X87_F64` FEX tests are the reference. Not modelled: the RC-dependent rounding of the internal constant, and anything that needs the 64-bit mantissa (`t_x87`'s `ext_add_sub` and `ext_mant_third` still fail for that reason). FCOMI/FCOMIP use `fcmpe`, FUCOMI/FUCOMIP `fcmp`; the difference is only FPSR.IOC, which nothing reads (the x87 status word's IE bit is not modelled).

### Corrections to the tables above

- P1-2 and P1-7, "Flags: Z, P (V), C written; O, S cleared": S is cleared, but V holds PF after an FP compare, so OF is not 0 in NZCV. PUSHFQ now reports OF = 0 there (it knows from the location pass). JO/JL-family conditions directly after an FP compare still read V = PF (no site in the inputs; compilers use the unsigned conditions).
- P1-1: an F2 (REPNE) prefix on MOVS/STOS/LODS means REP. The old lowering only looked for F3, so `repne lodsq` (FEX `Primary_AD_REPNE_qword_down.asm`) ran once. Fixed with the rewrite.
- P1-1, `Primary_A6_REP_down.asm`: the strings are in NASM double quotes, where `\0` is two characters. The compared bytes are 'e' and 't', which is why AH is 0x83 (AF = 0); with real NULs it would be 0x93.
- `Primary_A7_word_down.asm` (useful for AF = 1): its comment block describes a different result (0x87) from its RegData (0x1200). The RegData is right.
- P2-2, occurrence note ("112 of them follow an FP compare, adjacent by address") and Appendix A: almost none of the JP/JNP sites is in the block of its compare. The usual shape is `ucomiss; jne L; jp L`, so the JP starts a new block. The appendix's "preceding flag writer" column skips the Jcc, and for the 6 `other` JP rows it then lists a MOV/MOVSXD/VCVTSD2SS, which writes no flags. All 6 are the same shape with one or more flag-neutral instructions between the compare and the JNE (for example `0x1cc246 VUCOMISS; VMOVSD; VCVTSD2SS; JNE; 0x1cc258 JP`). The location pass handles all 118 JP/JNP/SETP/SETNP sites with PF in V: no site in Syobon reads PF from the context byte.
- P2-3: no separate "PF override slot" is needed for POPFQ/SAHF. A byte of the right parity (0 or 1) goes in the same lazy byte, and AF goes in as `flags & 0x10`, as FEX's `SetPackedRFLAG` does.
- P2-5, "no test reads AF after an arithmetic op": `Primary_A7_word_down.asm` and `Primary_9E.asm` do (through CMPSW and SAHF/LAHF).
- Line references `DAAOp :2911` and `INCOp :3240` point inside the functions (they start at 2892 and 3220). `BaseTables.h` is under `OpcodeDispatcher/`.

### Tests

- `pkg/lifter/arm64_fex_cases_test.go` (generated by `tools/gen_fex_cases.py`): 29 FEX unit tests restated for the native exec harness, expected registers from their RegData.
- `pkg/lifter/arm64_fex_test.go`: sweeps against Go models of the SDM: every string instruction x width x DF x prefix x count (560 cases), PF/AF of ADD/OR/ADC/SBB/AND/SUB/XOR/CMP/TEST/NEG/INC/DEC over widths and operand forms read through PUSHFQ, LAHF, SETP/SETNP and CMOVP/CMOVNP (about 2,500 cases), the other writers (XADD, CMPXCHG, POPCNT, PTEST, shifts, SAHF, POPFQ), PF across blocks, PSHUFW over all 256 immediates. Every case runs twice: with no liveness information, and through the emitter's block and flag passes.
- `pkg/disasm/parity_test.go`: liveness and location facts on synthetic functions.

Only the lifted arm64 code runs. No x86 code is executed.

### Measurements

| | before | after |
|---|---|---|
| Syobon lifter stubs | 2 (PSHUFW) | 0 |
| Syobon emitted asm (`code_*.s`) | 4,648,130 lines, 113,997,020 bytes | 4,648,372 lines, 114,007,189 bytes (+242 lines, +0.005%; +0.009% bytes) |
| PS5 hello_cxx emitted asm | 1,223,018 lines | 1,223,018 lines (byte identical) |
| PS4 hello_world stubs | 1 (FCOMIP) | 0 |

In Syobon the growth is the 4 PUSHFQ and 2 POPFQ sites, the 2 PSHUFW sites and 35 stores of a V-held PF at exits to blocks that can reach an unknown continuation. All 51 chunks assemble with clang.

### Gaps

- x87 is still double precision (see above).
- OF-reading conditions (JO/JNO/JL/JGE/JLE/JG, SETcc/CMOVcc of those) directly after an FP compare read V = PF.
- The 16-bit PUSHF/POPF (66 9C / 66 9D) have no lowering and fail the translation.
- A LOCKed instruction whose PF/AF are read records them from a plain read of the operand before the atomic operation.
- A shift of RCX by CL whose PF is read falls back to a translation failure if the shift lowering ever uses w6 (it does not today).
- `GuestContext.pf` starts as 0, which reads as PF = 1; x86 starts with PF = 0 (FEX `InitialPFFlag.asm`).
- Fault restart of a partially executed REP instruction (SPEC 3.10) is unchanged: not modelled.
