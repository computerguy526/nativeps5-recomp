# M1 lifter pre-L2 structural steps

Branch m1-lifter (pkg/lifter, pkg/emitter; small touches in pkg/cli and pkg/analyzer for flags and the analyze count).

## 1. Memory operand abstraction
All guest accesses go through `pkg/lifter/arm64_lifter_mem.go`: `gm(addrReg)` (operand), `gAdd(dst, src)` (host pointer for ldxr/ld1-style users), `memOperand(...)` (operand plus setup lines), `RewriteGuestMem(text)` (hand-written trampolines, Bloodborne patch). The ~290 literal `[x28, xN]` sites were converted mechanically.

Modes, selected at translate time (`-addr-mode base|identity`, `lifter.SetAddrMode`, process-wide, set before lifting):
- `base` (default, first-class: Calculator3D needs 0x2_0000_0000+, which collides with the dyld shared cache): `[x28, xN]`. Output is byte-identical to before.
- `identity` (guest VA == host VA): `[xN, #0]`; no lowering reads x28; plain MOV load/store uses the guest register in place; stack accesses fold displacement (`[x26, #8]`). `[xN, #0]` (not `[xN]`) is deliberate: it is how the TSO rewriter recognises a guest access, distinct from host-pointer accesses. Non-stack accesses never fold a displacement for the same reason.
- TSO rewriter works on both forms. readMemTo/writeMemFrom keep their implicit "address left in x16/x17" contract; only the `*Direct` variants (plain MOV) skip it.
- The emitter writes `lifter_manifest.json` (addr_mode, tso, allow_unsupported, unsupported_sites) next to the code; the runtime/loader must agree.

Tests: no x28 read in identity lowerings (TSO on/off); base and identity lowerings run natively on the host (arm64 only) leave identical registers and memory over a 30-case memory corpus.

### What L2 must change for identity mode to run
Loader/runtime place guest memory at the linked/rebased VA (>= 0x70_0000_0000); `MemoryImage` stops being a dense slice from 0; every emitted absolute constant (return addresses, `lea rip`, dispatch keys, relocation addends, guest_image) carries the bias. x28 may stay 0 (trampolines are rewritten). Base mode needs none of that and works at any arena.

## 2. Unsupported policy
Every `// UNSUPPORTED:` stub is recorded (address, function, mnemonic, operand form). The emitter fails the translate with a per-form summary plus the first 40 sites and writes `unsupported.txt` (full list) unless `-allow-unsupported`. `ps4-recomp analyze` now derives its count from the lowering (`Arm64Lifter.Probe`); `IsOpcodeSupported` is deprecated.

## 3. Vector scratch
`fixUpperHalves` is still the regex post-pass (explicit scratch allocation NOT done). The control-leaves hole is fixed: the save is always emitted; the restore goes before every exit (ret, br, branch to L_fn_*, tail call `b _sym`); conditional exits are inverted around the restore; an epilogue `ldr x30, [sp], #16` before an exit gets the restore ahead of it. Regression test: arm64_upper_test.go. Constraint learned: the pass renames scratch registers one by one, so no lowering may depend on consecutive scratch registers (a TBL register pair broke assembly; VPERMD uses two single-register lookups).

## 4. AVX2/FMA
VPBROADCAST B/W/D/Q, VPERMQ/VPERMPD, VPERMD/VPERMPS, VPERM2I128, VBROADCASTI128, VPMASKMOVD/Q, VPSLLV/VPSRLV D/Q, VPSRAVD, VPMULHUW, all VF[N]M{ADD,SUB}{132,213,231}{SS,SD,PS,PD}. Semantic tests (register and memory forms, dst-aliasing) run the lifted side natively against Go references; no x86 code is run. FMSUB/FNMSUB negate C before the fused op so exact cancellation gives +0.

## Census (before -> after; stubs = `// UNSUPPORTED:` markers)
| Binary | before | after |
|---|---|---|
| Syobon eboot | 5,265 | 2 (PSHUFW mm,mm,imm, MMX) |
| PS5 hello_cxx | 101 | 0 |

analyze "unsupported": Syobon 28,534 -> 2; hello_cxx 1,144 -> 0.

Emitted asm (code_*.s), base vs identity:
| Binary | base | identity |
|---|---|---|
| Syobon | 70.4 MB, 2.88M lines | 68.4 MB, 2.81M lines (-2.8%) |
| hello_cxx | 20.3 MB, 901,878 lines | 19.7 MB, 881,353 lines (-2.9%) |
(Baseline before this work: Syobon 71.7 MB; base mode output unchanged apart from the new lowerings.) Both modes assemble cleanly with clang for all Syobon chunks.

## Gaps
- Explicit vector scratch allocation not done; PF/AF/DF untouched; PSHUFW (MMX) missing.
- identity mode is untested at runtime (needs L2); only differential host runs of lowerings.
- `LIFTER_NO_X86=1` skips the Rosetta reference tests; the pre-existing exec suite runs x86 under Rosetta by default (it ran once, before the env var existed, as part of the baseline `go test`).
- (fixed in m1-lifterfu) AddrMode is a field of the lifter: `NewArm64LifterMode`, `l.SetAddrMode/AddrMode`; no package global.

## m1-lifterfu
- SYSCALL is lowered everywhere to `_recomp_call_shim(shim_raw_syscall)` (rax = number; rdi, rsi, rdx, r10, r8, r9; result in rax; CF + errno in rax on error, loaded from r11 bit 0 into the C flag; rcx/r11 clobbered). `shim_raw_syscall` remaps onto `shim_syscall` with a fake call frame below the red zone (6th argument at [rsp+8]); errno comes from the guest TCB errno slot. Test `t_syscall` (raw mmap/munmap/write via inline SYSCALL) and `arm64_syscall_test.go`.
- Identity-mode "x28 in ~114 lines of code_000.s": all false positives of a substring grep (`#0x289`, `0x28c` immediates, `0x28` in comments). With a token match `x28` there are 0 uses in lifted code in identity mode; the only x28 left in the output is the hand-written trampoline (save/restore of the memory base, ctx load) which is legitimate. Lint: `identityViolations` (arm64_mem_test.go) runs on every exec case when `LIFTER_EXEC_ADDR_MODE=identity` (the whole exec/FEX suite then also runs in identity mode) and on the memory corpus.

## m2-gamelift (instruction coverage for the UE title)
New files: `arm64_lifter_sha.go`, `arm64_lifter_gather.go`; tests `arm64_sha_gather_test.go`. Dispatch in `arm64_lifter.go`.
- **SHA-NI**: SHA1RNDS4/NEXTE/MSG1/MSG2 and SHA256RNDS2/MSG1/MSG2 map onto arm64 FEAT_SHA1/FEAT_SHA256 (`sysctl hw.optional.arm.FEAT_SHA1/FEAT_SHA256` = 1 on Apple silicon), no runtime helper. Sequences follow FEX `OpcodeDispatcher/Crypto.cpp`; the header of `arm64_lifter_sha.go` documents the layout differences (SHA1 lane reversal = rev64+ext, E folded into W for RNDS4 so arm64 takes E=0, RNDS2 does 4 arm rounds but only the first 2 matter, MSG2 via SHA256SU1 with zero accumulator). Fixed a FEX operand-order detail by oracle: SHA1MSG1 is `ext src, dst, #8`. Cost: 4-12 instructions per op.
- **PADDUSW, PSUBUSW, PSUBSW, PMULHUW** SSE and MMX forms (VEX forms already existed). Also fixed an existing bug: MMX PMULHW/PMULHUW used `uzp2 .4h` and lost the upper two lanes (now `shrn #16`); caught by FEX TwoByte/0F_E4, 0F_E5.
- **AVX2 gathers** (VGATHERDPS/DPD/QPS/QPD, VPGATHERDD/DQ/QD/QQ, xmm/ymm): element-wise through `gm()` (base-mode bias, identity mode and TSO rewrite apply). Base+disp once into x6, per element `tbz` on the mask MSB, `add x16, x6, sext(idx), lsl scale` (64-bit wrap), load, insert. Mask register (and xmm-form upper halves, dst lanes beyond the gathered count) cleared at the end, not per element, so a faulting element leaves earlier mask bits set; masked-off lanes never touch memory. Cost: ~6 instructions plus a branch per element (up to 8). `fixUpperHalves` treats an xmm mask as a legit upper-half write.
- FEX oracle (VEX, H0F38, H0F3A, TwoByte, OpSize, SSELanePreservation, FEX_bugs, MMX): see `tests/fex/results.json`. Remaining gather skips are the `_overflow` tests and FEX_bugs/vgather_xmm4, which the harness marks unsupported because data after `hlt` is decoded as code (not a lifter gap; covered by `TestAVX2GatherIndexOverflow`).
