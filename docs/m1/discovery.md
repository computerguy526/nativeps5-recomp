# M1 code discovery

Branch `m1-discovery`. Scope: `pkg/disasm` (seeding, CFG, jump tables, LSDA), `pkg/analyzer` (metrics). Measure with
`DISCO_ELFS=a.elf:b.elf [DISCO_OPTS=data,lp,rs] [DISCO_ORACLE=1] [DISCO_KINDS=1] [DISCO_DUMP=<name>] go test ./pkg/analyzer -run TestDiscoveryStats -v`.

## 1. Crash diagnosis: none of the three is a discovery problem

All three "unresolved indirect jump/call" targets are **data addresses in `.bss`**, and each is the value of a GOT slot after
`ResolveModuleRelocations` (pkg/elfloader/modules.go, step 1 export map):

| test | target | what lives there | call site |
|---|---|---|---|
| t_vdispatch | 0x58920 | `.bss` `malloc` (STT_OBJECT, local) | `call [rip+GOT]` for `malloc` (return 0x32b9e) |
| t_tls | 0x14960 (both variants) | `.bss` `malloc` (local variable) | `call [rip+GOT]` for `malloc` in `__emutls_get_address` (return 0x9797; the `from RIP` slot differs by variant) |
| t_varargs | 0x14138 (plt), 0x148d8 (noplt) | `.bss` `vsnprintf` function-pointer variable | `call [rip+GOT]` for `vsnprintf` |

The Payload SDK links static `.symtab` entries named `malloc`, `vsnprintf`, `snprintf`, `strlen`, ... that are **local `.bss`
variables** (kernel-payload function-pointer slots). The `main.Symbols` loop in `ResolveModuleRelocations` adds every named
symbol with a non-zero address to the export map regardless of type, so the import `malloc` resolves to the address of that
variable, the slot is overwritten with a `.bss` address instead of its own stub address, and the call dispatches to data.
Not a missing function, not PLT/TLS init, not a vtable-only function, not a landing pad.

**Fix (not applied: it is in `pkg/elfloader`, which this task excludes):** in `ResolveModuleRelocations`, the loop over
`main.Symbols` must only admit `sym.Type == elf.STT_FUNC` (a data object cannot satisfy a function-import slot). Patch:

```go
for _, sym := range main.Symbols {
    if sym.Name != "" && sym.Address != 0 && sym.Type == elf.STT_FUNC {
```

I could not validate the end-to-end effect (the edit was refused by the permission layer), so "past the crash" is unverified.
Each of the three tests also contains other SDK `.bss` function-pointer variables, so expect the next failure to be one of the
same imports (`strlen`, `memcpy`, ...) unless the filter is applied everywhere the export map is built (DynSymbols too, if a
data symbol shares a name).

## 2. Seeding / discovery changes and measurements

Baseline = `bc94b50`. "funcs" = functions the CFG walk decodes (seeds plus call/tail-call/LEA discovered).

### (a) Initialized writable data scan (`SeedOptions.WritableData`, on)
PS5 SDK images put `.data.rel.ro`, `.got`, `.rodata` in one RW segment, so the read-only scan skipped every vtable. The new scan
reads file-backed words of non-exec writable segments. In FDE-covered code only an FDE start is accepted; elsewhere the word
must be 16-aligned and start a decodable, non-privileged straight-line run.
Result: **0 new seeds on every corpus binary** (also on stripped variants): every code pointer in data is already a
relocation target, an FDE start or a symbol. Rejected-candidate count: 0. Kept as a guard for images without FDEs/relocs
(unit test: mid-instruction word rejected, aligned prologue accepted).

### (b) Jump tables
Existing recovery already covers rel32/rel8/rel16/abs64, `lea`/`add`, `cmp/ja`, BEXTR/BZHI/`and` bounds, byte-indexed
dispatch (see jumptable.go). Added: cross-block recovery (`crossBlockTable`): when the `jmp reg` window has no load/add, use the
nearest predecessor block that writes the register (<=4 blocks back, all defining paths must agree). Handles Duff-style loop
entries.

The raw "recovered / indirect jmp" rate the S2 report used (29-42%) is misleading: most unresolved `jmp reg` are not switches.
`MeasureDiscovery` classifies them:

| binary | indirect jmp | recovered before -> after | unresolved: GOT tail | vtable/callback tail | other |
|---|---|---|---|---|---|
| t_vdispatch | 117 | 40 -> 40 | 2 | 74 | 1 |
| t_tls | 14 | 4 -> 4 | 2 | 8 | 0 |
| t_varargs | 15 | 5 -> 5 | 2 | 8 | 0 |
| hello_cxx | 211 | 84 -> 84 | 2 | 119 | 6 |
| Syobon eboot | 1638 | 562 (34.3%) -> **584 (35.7%)**, +140 targets | 752 | 94 | 208 |
| hello_world (PS4) | 79 | 75 (94.9%) | 0 | 2 | 2 |

Excluding tail jumps through a loaded pointer (`mov rax,[rip+GOT]; jmp rax`, `jmp [rax+off]`), Syobon recovery is
584/792 = **73.7%** (was 562/792 = 71.0%); the remaining 208 are `jmp reg` whose register is a parameter, a call result or an
object-array callback load (`mov rax,[rdi+8*rax+0x34]`), i.e. dynamic dispatch. The matcher finds **0** unresolved sites
whose register is built by `add` + indexed load without a plain memory load (`tableShaped=0`), so no further static switch
tables are visible. 752 GOT tails are import wrappers: resolved by the import mechanism at run time, not tables.

**Ground truth:** SDK-built images carry `.llvm_jump_table_sizes` (address + length of every jump table). On the test ELFs and
hello_cxx every table is recovered and every oracle entry is in the target set:

| binary | oracle tables | found | exact count | entries | missed |
|---|---|---|---|---|---|
| t_vdispatch | 38 | 38 | 34 | 965 | 0 |
| t_tls | 4 | 4 | 4 | 69 | 0 |
| t_varargs | 5 | 5 | 5 | 105 | 0 |
| hello_cxx | 83 | 83 | 64 | 1658 | 0 |

The inexact ones have an unproven bound (scan to first non-target, byte-dispatch cap): they read correctly except one table
that over-reads 4 entries into the following table (spurious targets, bounded by the function extent).

### (c) LSDA landing pads and return sites (`SeedOptions.LandingPads` on, `ReturnSites` off)
`ParseLSDA` walks `.eh_frame`, CIE augmentation `zPLR`, each FDE's LSDA and its call-site table.
Landing pads sit in the middle of functions and are only reached by the unwinder, so the CFG never decoded them:

| binary | landing pads | funcs before -> after | insts before -> after | decode errors | mid-instruction entries |
|---|---|---|---|---|---|
| t_vdispatch | 85 | 835 -> 920 | 53,246 -> 53,787 | 0 | 0 |
| hello_cxx | 1,332 | 2,409 -> 3,857 | 158,342 -> 176,635 (+11.5%) | 0 | 0 |
| Syobon, t_tls, t_varargs | 0 (no LSDA) | unchanged | unchanged | | |

Landing-pad code that was previously **not lifted at all** is now lifted and has a dispatch entry (SPEC 3.2). Return sites
(end of every throwing call-site region, 95 / 1,540) are implemented but **off by default**: they double hello_cxx's decoded
code (176k -> 322k insts) because each becomes a duplicate function tail. They are only needed if something returns to a guest
address through dispatch; revisit once the runtime miss log shows such misses.

### (d) Tail-call targets
Already handled: a direct `jmp`/`jcc` leaving the function queues its target. Result: functions beyond FDE starts are 6
(Syobon), 3-4 (tests); no mid-instruction entries anywhere. Nothing to add.

## 3. Totals (default options, after)

| binary | FDEs | seeds | funcs | FDEs reached | seed decode errors | mid-inst entries |
|---|---|---|---|---|---|---|
| t_vdispatch | 676 | 917 | 920 | 676 | 0 | 0 |
| t_tls | 7 | 161 | 165 | 7 | 0 | 0 |
| t_varargs | 5 | 159 | 163 | 5 | 0 | 0 |
| hello_cxx | 2,253 | 3,741 | 3,857 | 2,253 | 0 | 0 |
| Syobon eboot | 6,925 | 6,925 | 6,931 | 6,925 | 0 | 0 |

Stripped test ELFs (`llvm-strip --strip-all`): seeds = FDEs + relocation/dynsym seeds (t_vdispatch 676 FDEs, 681 functions);
non-FDE asm routines are only found from `.symtab` or calls.

## 4. Still needs the runtime miss fallback
- GOT/PLT tail calls and indirect calls through loaded pointers (752 + 94 on Syobon, ~120 in hello_cxx): virtual/callback
  dispatch has no static target set.
- `jmp reg` with a parameter/call-result register (208 on Syobon).
- Functions with no FDE, symbol, relocation or direct call (asm stubs in a stripped image).
- Return sites (flag above), `setjmp`/`longjmp` resume addresses.
- Tables with unproven bounds can still miss a case if the scan stops early.
