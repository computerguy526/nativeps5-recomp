# M1 loader: PS5 binaries

Branch `m1-loader`. Scope: `pkg/elfloader`, seeding in `pkg/disasm/seed.go`, dispatch registration for GOT imports in `pkg/emitter/asm_emitter.go`, `cmd/loaderinfo`, `tools/`.

## Changes
1. **SELF**: `IsSELF` accepts `54 14 F5 EE` (PS5, fake-signed plaintext) as well as `4F 15 3D 1D`. The extractor was already layout-agnostic. `libc.prx` (PS5 magic) now loads: 2,669 dynsyms, 1,896 relocations.
2. **Dynamic section** (`parseSCEDynamic`): PS4 binaries keep tables in the PT_SCE_DYNLIBDATA blob, named by offset (0x6100003x tags). PS5 eboots and PRXs name tables by guest VA with standard tags (DT_SYMTAB 6, DT_STRTAB 5, DT_STRSZ, DT_RELA 7/8, DT_JMPREL 0x17/DT_PLTRELSZ 2, DT_SYMENT, DT_NEEDED 1) inside an ordinary mapped segment (a flags=0 PT_LOAD). Symbol table size comes from tag 0x6100003f, else STRTAB-SYMTAB. SCE library tags are shifted +0x30 on PS5 (EXPORT_LIB 0x43, IMPORT_LIB 0x45). DT_INIT is added to the init list only for binaries without section headers (previous behaviour).
3. **GOT-style imports (`-fno-plt`)**: `call [rip+GOT]` reads a slot that holds its own address (set by `ResolveModuleRelocations`). That address is never a CFG target, so no dispatch entry existed. `EmitGuestModules` now synthesizes a stub and `recomp_register_fn` for every shim-bound PLT/GOT slot that is not a discovered function, as it already did for unresolved slots. No lifter change.
4. **Seeding**: every .eh_frame FDE start is now a seed (previously only with `-a`), plus DT_FINI and DT_FINI_ARRAY. Relocation-target seeding (RELATIVE and R_X86_64_64 slots, RO-segment scan) was already there and now sees the real relocations. Init array was already seeded.
5. **Bounded parsing**: PT_LOAD memsz/vaddr overflow and a 4 GiB image cap, filesz <= memsz, file range inside the file, PT_DYNAMIC range, table VAs inside the image, SYMENT >= 24, symbol index within the table, relocation table size cap. Unsupported relocation types return `*UnsupportedRelocError` ("unsupported relocation type N at offset 0xX"). Supported: NONE, 64, PC32, GLOB_DAT, JUMP_SLOT, RELATIVE, DTPMOD64, DTPOFF64, TPOFF64.

## Numbers (Syobon eboot, `out/loaderinfo`)
| | before | after |
|---|---|---|
| relocations | 0 | 3,679 (3,478 RELATIVE, 201 GLOB_DAT) |
| imports (distinct) | 0 | 201 (91 host shims, 86 linked PRX, 24 unresolved; 21 of those named by the NID DB) |
| dynsyms | 0 | 202 |
| needed libs / import libs | 0 / 0 | 10 / 11 |
| code seeds | 2,066 | 6,925 |
| reachable functions | 5,246 | 6,931 (FDEs: 6,925) |

PS4 `hello_world.elf`: reachable 4,968 -> 4,988 (FDE seeding).

## Acceptance
- (a) `ps5_hello_k` built with the default `-fno-plt` prints both lines (`fib=463`) and exits cleanly; `-fplt` still works. Run via `tools/full.sh` + `tools/run_sandboxed.sh`.
- (b), (c) above. (d) `pkg/elfloader/malformed_test.go` (truncated header, bad phdr offset, file range, overflow, huge memsz, wrong machine, unknown reloc, bad symbol index, table outside image, truncated/oversized SELF). `go test ./pkg/...` passes.

## Gaps
- Unresolved data-class GOT imports (GLOB_DAT to data symbols) remain routed to `shim_unresolved_stub`; only function imports run.
- `libc.prx` text is hollow in this dump (0 FDEs); it parses but yields almost no code.
- No IRELATIVE/COPY/DTPMOD-style reloc handling beyond the listed types; they are rejected, not emulated.
- SELF extractor only handles plaintext, uncompressed segments (as before).
- Image is still a dense slice from 0 (L2 owns the relocation of the image base).
