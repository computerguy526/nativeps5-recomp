# m3-lift2: read-only loads, leaf red zone, BMI and narrow-flag peepholes

Branch `m3-lift2`. Items 2, 3 and 5 of `work/ue-arm-study/REPORT-v2.md` section 4. Each is a lifter option, on by default,
recorded in `ConfigKey`, the translation stamp, `lifter_manifest.json` and the tier-1 manifest. With an option off the
output is byte-identical to a translator without it (checked on the micro-benchmark: every `code_*.s` equal to the
`4e90ec1` build with all three off). Item 4 (`[reg]` / `[reg+index]` alignment test) is not done: `m3-coldpath` already
outlines the crossing path of exactly that shape (`orderedAccess`), so a second change would collide with it.

| flag | default | what | code |
|---|---|---|---|
| `-ro-loads` | on | loads of read-only image memory at a constant address are plain | `pkg/disasm/roload.go`, `elfloader` (`ROAddrRanges`) |
| `-redzone-leaf` | on | with `-tso-elide stack`: the red zone of a call-entered function is private from its first instruction | `pkg/disasm/jumpin.go`, `stackpriv.go` |
| `-bmi-peep` | on | BLSI/BLSR/BLSMSK, LZCNT/TZCNT, BEXTR, `test r8,r8`, `xor r,r`, `xor r8,imm; movzx`, `cmp [m],0; je` | `pkg/lifter/arm64_lifter_bmipeep.go`, ZF/SF liveness in `carry_liveness.go` |

Tools: `cmd/lift2stat` (static counts, `-lift` adds host instruction counts), `tools/lift2bench.sh` (uebench A/B).

## 1. `-ro-loads`: plain loads from read-only image memory

### What

The disassembler marks an instruction `ROLoad` when it only reads its memory operand (`mov r, [m]`, `movzx`, `cmp`,
`test`, ALU with a memory source; not `lea`, stores, locked or exchange instructions, vector or string instructions, no
segment override) and the operand is

* RIP-relative with the address in `LoadedELF.ROAddrRanges`, or
* the stack guard value: `[r]` (no index, `disp` inside the 8 bytes) where `r` holds the address of the guard.

`ROAddrRanges` = `PT_GNU_RELRO` and `PT_SCE_RELRO` (end rounded down to 16 KiB, the page ld.so protects) plus every
`PT_LOAD` that is `PF_R` only (rodata). Executable and no-permission segments are left out. Ranges are shifted with the
module bias and merged with the companions' (`ApplyBias`, `Merge`). The lifter does not order such an instruction
(`applyTSO` sees `roLoad`), so it is a plain `ldr`.

The guard pointer: `mov r64, [rip+slot]` where `slot` is read-only and the image holds the canary address in it
(`applyStackCanary` writes it for every `__stack_chk_guard` relocation, all modules share one canary page). The
register stays a guard pointer along a forward dataflow over the blocks of the function (meet: intersection; a call keeps
the callee-saved registers, any other write clears the register; the entry block and blocks nobody reaches start with
none; a function with an unresolved indirect jump gets no cross-block tracking).

### Why it is sound

An acquire load matters only if another thread can write the location while this thread runs; it keeps the load ordered
against later accesses of this thread so that data published behind a flag is seen. For a location nobody writes
during the program, ordering against it is vacuous.

* *RELRO and rodata.* The loader (here `elfloader` at translation time, the runtime's linker at start) writes them before
  the first guest thread runs; the program never does: on the console they are mapped read-only after relocation, and a
  store would fault. A module the program loads later (`sceKernelLoadStartModule`) is relocated before any code of it
  runs, and the HLE call that hands out the module handle synchronises through the kernel, so no thread sees the module's
  RELRO half written. A slot the runtime patches after start (lazy import binding) holds a pointer to immutable code or
  data; a reader needs only the atomicity of the aligned 8-byte load, which a plain load has.
* *Guard value.* The canary is a constant of the image, written by the loader (`canaryValue` in
  `elfloader/loader.go`), before any thread starts; no guest code stores to it (the only way to its address is the
  RELRO slot, whose users only load). Prologue and epilogue both read it; there is nothing to order against.
* *Dataflow.* A register is a guard pointer only when a read-only slot holding the canary address was loaded into it and
  nothing wrote it since on every path: the load of `[r+disp]` therefore reads the canary page. Callee-saved registers
  survive calls by the ABI the stack pass already assumes (A3); `longjmp` restores them to the `setjmp` values, which
  are on a path that had the pointer too (the setjmp return site is a call continuation, entered with the callee-saved
  registers of the call).

### Numbers (PPSA30552 eboot, `cmd/lift2stat`, read-only dump, never run)

| | sites |
|---|---|
| RIP-relative loads from RELRO / rodata | 135,474 |
| of which the guard pointer load | about 121,000 |
| guard value loads (`mov r,[r]` through the guard pointer) | 122,124 |
| **plain loads in total** | **257,598** |

RO ranges of the eboot: `[0x55d0000,0x5dd47f0)` (rodata), `[0x5dd8000,0x6ad0000)` (RELRO).

Host instructions (`lift2stat -lift`, blocks that hold a site, option off / on): 11,000,043 / 10,009,701, **-990,342**;
acquire loads 1,150,850 / 893,252 (-257,598, one per site). An ordered access carries the address add and the 16-byte
crossing test; a plain one is a single `ldr`.

## 2. `-redzone-leaf`: the red zone of a call-entered function

### What

`-tso-elide stack` (docs/m3/tsoelide.md) makes an access plain only when the stack privacy pass proves the slot private.
Below RSP, a slot is private only once the analysed code has lowered RSP (`lowered`: assumption A5 says that from then
on nothing the code before left below RSP is live). A frameless leaf function, which keeps its locals in the 128 bytes
below RSP and never lowers RSP, therefore had every local ordered ("red-zone" in `tsostat`): `stack_locals` in
`uebench` went from 1.45x to 3.50x native.

With `-redzone-leaf` a function that is entered by a call starts with `lowered = true`: **the 128 bytes below the entry
stack pointer R are its own from the first instruction**. A function that is entered by a jump keeps the old rule.

### Which functions

`call` pushes the return address, so by the red zone rule the caller's data below its own RSP is dead across the call:
A5 with the call as the event that kills the caller's red zone. A *jump* into a function does not push anything. The
jumper may be code that never lowered RSP and keeps live data (and published addresses) in its red zone; the target,
a fragment of the same frame (cold split, shared tail), must not take that red zone for its own.

Discovery (`WalkReachable`, which every translation runs first) records, for every direct `jmp`/`jcc` that leaves its
function, whether RSP may still be unlowered at that point (`noteJumpOuts`: a forward dataflow over the function's blocks
with `PUSH`, `PUSHF`, `CALL`, `ENTER`, `SUB RSP` as the instructions that certainly lower it). A target of such a branch is
*jump entered*. Branches from code that has lowered RSP (an epilogue's tail call, a cold jump out of a function with a
frame) are not recorded: below their RSP nothing is live by A5, so the target may own it. Indirect jumps are tail calls
with the frame released (A4). A function is `CallEntered` when the walk completed and nothing recorded it. A lone
fragment build (`fragment-serve`) has no walk and treats every function as jump entered.

### Proof

Lemma 1 of tsoelide.md needs one more case: ℓ is in the red zone [R-128, R) of a call-entered function F that has not
lowered RSP yet. ℓ was dead when F was entered: the call pushed the return address at R, so every byte below the
caller's RSP is dead by A5 (the caller's frame data lie above its RSP, its red zone is dead across a call, and nothing
below R is live to a third party: the thread's stack below RSP is owned by the thread only). Another thread can reach ℓ
only through an address F let out of its registers (A1), and F is not escaped (an escaped function has nothing
private but save slots). The same argument as for the red zone after `push rbp`; the new fact is only that the call plays
the role of the first RSP lowering. Lemma 2 (publication) is unchanged: a red zone slot is private memory, nothing
is handed over through it without an escape.

What is excluded: a function reached by a direct branch from code that has not lowered RSP (it may continue a frame
whose red zone is live), indirect `jmp` into the middle of a function (not an entry), and the unwinder entering a
landing pad (the pad runs in the frame of a function that has lowered RSP).

### Numbers

On PPSA30552 the effect is almost nothing: **705** of 259,105 functions are jump entered, 258,338 are call entered, and
their private orderable accesses go from 148,433 to 148,447 (+14 of 5,921,818). The title is built with frame
pointers: a function that keeps locals below RSP pushes `rbp` first, which already counted as lowering. The study's
"5-10% regression" under `-tso-elide stack` is therefore not red zone locals. `uebench stack_locals` is the case the
rule is for (a frameless leaf with a `volatile` array): section 5.

## 3. `-bmi-peep`: BMI and narrow-flag peepholes

### Lowerings

`liftArithFast`, `liftBmiFast`, `liftBitScanFast`, `liftBextrFast` (`arm64_lifter_bmipeep.go`) run before the general
lowering and return "not applicable" for anything they do not cover; the general lowering is untouched and is the
reference of the differential test. Host instructions, flags dead / flags live (CF dead, ZF and SF live) / CF live:

| x86 | general | now |
|---|---|---|
| `blsi dst, src` (32/64) | 4 / 8 / 8 | `neg t,src; and dst,src,t` 2 / `neg; ands` 2 / `neg; ands; cset ne; rmif` 4 |
| `blsr` | 4 / 8 / 8 | `sub t,src,#1; and` 2 / `sub; ands` 2 / `subs; cset lo; ands; rmif` 4 |
| `blsmsk` | 4 / 8 / 8 | `sub; eor` 2 / `sub; eor; tst` 3 / `subs; cset lo; eor; tst; rmif` 5 |
| `lzcnt dst, src` | 3 / 5 | `clz dst,src` 1 / `cmp src,#0; clz; ccmp dst,#0,#0,ne` 3 |
| `tzcnt` | 4 / 5 | `rbit t,src; clz dst,t` 2 / `cmp; rbit; clz; ccmp` 4 |
| `bextr`, control set by `mov r,imm` earlier in the block | 14 / 15 | `ubfx dst,src,#s,#l` 1 / `ubfx; tst` 2 |
| `test r8,r8` / `test r16,r16` | 4 | `tst w,#0xff` 1 (SF dead) / `lsl w17,w,#24; tst w17,w17` 2 |
| `xor r,r` (32/64) | 1 / 2 | `mov w,wzr` 1 / `ands w,wzr,wzr` 1 |
| `xor/or/and r8,imm`, flags dead | 4 | `eor w16,w,#imm; bfi x,x16,#0,#8` 2 |
| ... followed by `movzx r32/r64, r8` (same register) | 5 | `eor w16,w,#imm; uxtb w,w16` 2, the `movzx` lowers to nothing |
| `cmp [m],0; je/jne` (size 1/2/4/8, all flags dead after the branch, local target) | load + 3 + `b.eq` | load; `cbz/cbnz w17,L` 2 |

`andn` was already one instruction (`bic`/`bics`). The 8-bit forms exclude AH/BH/CH/DH. `bextr` with a control that is
not a constant of the block keeps the general lowering; `bzhi`, 16-bit forms and memory-destination compares likewise.

### Why they are equal

* *BLSI/BLSR/BLSMSK.* `ands dst, src, t` computes the result and SF/ZF from it with CF = OF = 0 on the host (V = 0 is OF);
  CF is `src != 0` (BLSI: `ne` after the `ands`, since the result is zero exactly for a zero source) or `src == 0`
  (BLSR, BLSMSK: the borrow of `subs src,#1`, `lo`), inserted into C with `rmif` in the same sense as before (`carryAdd`).
  For BLSMSK, ZF = 0 and SF come from `tst dst,dst` (the result is never zero). The destination may be the source:
  every instruction reads the source before the destination is written.
* *LZCNT/TZCNT.* The `cmp src,#0` precedes the write of `dst`, so it reads the source even when `dst == src`; `ccmp dst,#0,#0,ne`
  is the general lowering's flag sequence (Z = result == 0 and C = !CF for a nonzero source, NZCV = 0 for zero).
* *BEXTR.* The control is a constant `v`: start = `v[7:0]`, len = `v[15:8]`; the field is cut at the operand size, a start
  at or past it or a zero length gives 0, exactly the general lowering's `csel`/`csinv` clamps. Found by looking back for
  the nearest earlier instruction of the block that writes the register (`disasm.RegWrites`); it must be `mov r, imm`.
* *`test r8,r8`.* ZF depends on the 8 bits; `tst w,#0xff` gives it, with C = V = 0 as TEST defines CF = OF = 0. N would
  be 0 where SF is bit 7, so this form needs SF dead; otherwise `lsl w17,w,#24` puts bit 7 in N.
* *`xor r,r`.* The result is 0, ZF = 1, SF = CF = OF = 0: `ands w, wzr, wzr`.
* *`xor r8,imm; movzx`.* `movzx` writes the whole register from the low byte: `(al ^ imm) & 0xff` in one `uxtb`.
* *`cmp [m],0; je`.* The compare defines ZF = (value == 0); when every flag is dead after the branch (below) the flag
  computation is unobservable and the branch tests the loaded value directly. The load keeps its acquire ordering (it is
  the same `ldr` the compare used; the TSO rewrite orders it as before).

### ZF/SF liveness

`pkg/disasm/carry_liveness.go` computes, besides CF/OF/PF/AF, `Instruction.ZeroDead` and `SignDead` over the same blocks
and the same conservative rules (unknown instructions read both, unknown successors keep both live; `zeroSignEffect`
lists the readers: `je/jne/jbe/ja` read ZF, `js/jns` SF, `jl/jge` SF, `jle/jg` both, `lahf`, `pushf` both; and the
writers it kills only when they define the flag: arithmetic, logic, shifts by a non-zero immediate, compares, `bsf/bsr/tzcnt/lzcnt/popcnt`
for ZF; `bt*`, `mul`, `div` and rotates define neither). Both bits are part of the chunk digest when `-bmi-peep` is on.

## 4. Verification

* `TestBmiPeepMatchesGeneralLowering` (`arm64_bmipeep_test.go`): **13,634** cases, each lifted with `-bmi-peep` off (the
  general lowering) and on, run on the host from identical registers and memory (19 input values including 0, 1, the sign
  bits, all-ones, 7 reader sets that make exactly the declared flags live: CF, ZF, SF, OF and combinations; the
  `SETcc` readers copy the flags into registers). Registers and the memory window must be identical. A mutation of the
  `cset` condition or of `cbz`/`cbnz` is caught (228 and 20 cases differ). The instruction under test has its liveness
  bits overridden to the declared set, so the dead-flag lowerings run (the harness ends sequences with every flag live).
* `TestBmiPeepShapes`: the instruction counts of the table above. `TestAnalyzeROLoads*`, `TestLeafRedZone`,
  `TestNoteJumpOuts`: the disassembler passes. `TestROLoadPlain`: lowering of a flagged instruction, off.
* Off byte-identical: `uebench.elf` translated with `-ro-loads=false -redzone-leaf=false -bmi-peep=false` equals the `4e90ec1`
  translation file by file (`code_*.s`).

## 5. Measurements

Static, PPSA30552 eboot (`lift2stat`, read-only dump, nothing run; the other modules are not in the tool):

| item | sites | host instructions of the blocks that hold them (off -> on) |
|---|---|---|
| `-ro-loads` | 257,598 plain loads (135,474 RIP-relative, 122,124 guard value) | 11,000,043 -> 10,009,701 (-990,342), acquire loads -257,598 |
| `-redzone-leaf` | +14 private accesses (148,433 -> 148,447 of 5,921,818); 705 jump-entered functions | negligible |
| `-bmi-peep` | `test r8,r8` 84,363; `cmp byte [m],0` followed by `je/jne` 52,046; `andn` 14,047; `lzcnt` 10,230; `blsi` 9,730; `xor r,r; test` 9,689; `bextr` 3,505; `bzhi` 2,254; `xor r8,1; movzx` 1,349; `tzcnt` 643 | 6,216,410 -> 5,692,424 (-523,986) |

The whole eboot lowers to about 80 M host instructions, so the options remove about 1.2 % and 0.65 %.

`tools/lift2bench.sh 5` (`uebench`, five interleaved runs, minimum, ns per unit; the machine was shared, differences under
about 5 % are noise; `-tso-elide stack`, checksums equal in every variant):

| case | all off | `-ro-loads` | `-redzone-leaf` | `-bmi-peep` | all on |
|---|---|---|---|---|---|
| stack_locals | 6.55 | 6.54 | **2.75 (-58 %)** | 6.64 | **2.72 (-58.5 %)** |

native on this machine: 1.91, so stack_locals goes from **3.43x to 1.42x native**. The other fifteen cases are within noise
(uebench has no BMI instructions, and its guard loads are outside its hot loops).
