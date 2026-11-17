# M1 Rosetta oracle, ELF level (SPEC §8, §8.1)

`tools/ref_rosetta.sh --elf` runs the **PS5 payload ELFs themselves** (`tests/ps5/build/<test>.{plt,noplt}.elf`) on x86-64 under Rosetta, via AnyPS5's relinker, and stores the stdout as the reference `tests/ps5/ref/<test>.<variant>.stdout`. The lifted arm64 output must equal it (`tools/ref_compare.sh`). This complements the source-level oracle (same C sources built for x86-64 macOS) described in `docs/m1/tests.md`.

## Why the ELF-level path failed, and what the relinker needs

Three independent problems, found by reading `work/s3/rosetta/AnyPS5-x86/core/relinker` and the libs it targets:

1. **Import names.** On macOS the relinker keeps import names as they appear in `.dynsym` (`BindNamedImportsByNid` is skipped when `macos`), so a payload ELF yields Mach-O binds to `_sceKernelWrite`. AnyPS5's built libs (`libkernel.prx`, `libc.prx`) export **NID** names in their Mach-O export trie (`_4wSze92BhLI`; `nm` still shows `_x_nid_postfix` from the unpatched symtab, `dyld_info -exports` shows the truth). Hence dyld's `symbol not found in flat namespace '_sceKernelWrite'`. Fix: `.dynsym` names -> NID. NID = base64url-ish of the byte-reversed first 8 bytes of SHA-1(name + 16-byte suffix), alphabet `A-Za-z0-9+-`, 11 chars (same as `core/relinker/relinker/tests/macos/nid.py`; verified `sceKernelWrite` -> `4wSze92BhLI`, `strlen` -> `j4ViWNHEgww`). No SCE dynamic tags are needed on macOS: DT_NEEDED already carries `libkernel_web.sprx` etc. (the script symlinks the `.sprx` names to the `.prx` files, the S3b workaround), and a bare NID without `#lib#mod` is resolved flat across all loaded libs.
2. **Entry point.** Payload ELFs have `e_entry == 0` (CRT `_start` sits at vaddr 0) and the relinker aborts with `Code analysis: no code entry points`. Worse, the SDK CRT (`__crt_start`) expects the elfldr's `payload_args` (`rdi->dlsym` function pointer, rwpair, ...) and then resolves libkernel with raw FreeBSD `syscall`s through its own rtld (`__rtld_payload_new`, `kernel_dynlib_dlsym`). AnyPS5 provides none of that (its entry convention is `rdi = &argc`; `syscall` instructions are rejected by the relinker: `Forbidden syscall instruction`). Running the CRT was tried: it jumps to address 1 (`args->dlsym` read from `argc`). Fix: bypass the CRT with a generated entry stub `main(argc, argv, envp)`; it returns, and AnyPS5 then aborts with `guest entry returned` (rc 134), which the script treats as normal completion. (A raw `exit` syscall in the stub is refused by the scanner.)
3. **Process parameters.** AnyPS5's libc heap init needs a `PT_SCE_PROCPARAM` (`size>=0x40`, magic `ORBI` at +8, `+0x38 -> libc params {size>=0x38, +0x30 -> replacement table {0x78, 2, hooks=0}}`); without it any test that touches the heap (`t_tls`, emulated TLS) dies with `application heap: invalid or missing process parameters`. The chain contains two absolute pointers, so `.rela.dyn` is copied and extended with two `R_X86_64_RELATIVE` entries.

## Choice: (a) ELF post-processor, `cmd/ps5nidify`

Option (b), a shim dylib for the ~50 plain-name imports, would still need the entry and procparam fixes (those are independent of imports), would have to be injected with `DYLD_INSERT_LIBRARIES` into a relinked binary, and would hard-code a name list that changes per test. (a) is generic (computes NIDs from whatever the ELF imports), has no runtime moving parts, and the NID function is 15 lines. Cost: one Go file, `cmd/ps5nidify/main.go`, byte-level so it works on the SDK's slightly malformed ELFs (PT_DYNAMIC size covers more than `.dynamic`).

What it does: (1) rewrites every undefined `.dynsym` name (without `#`) to its NID, with new strings in a new `.dynstr` (DT_STRTAB/DT_STRSZ and the section header updated); (2) if `e_entry == 0` appends a stub segment (`push rbp; mov rbp,rsp; mov rax,rdi; mov edi,[rax]; lea rsi,[rax+8]; lea rdx,[rsi+rdi*8+8]; call main; pop rbp; ret`) and points `e_entry` at it (`-entry sym` uses a symbol instead); (3) adds the procparam chain and relocations. Everything goes in two new PT_LOADs past the end of image and file; two or three program headers are added into the zero gap after the phdr table. `ps5nidify -list x.elf` prints name/NID pairs. Run: `go build -mod=vendor ./cmd/ps5nidify` (the script builds it to `tests/ps5/build/ps5nidify`).

## Usage

```
tools/m1_build_tests.sh                      # ELFs, if not built
tools/ref_rosetta.sh --elf [--variant plt|noplt|both] [test ...]
tools/ref_compare.sh [--variant v] [--results-dir tests/ps5/build/work] [test ...]
```
`ref_rosetta.sh --elf` sandbox: `env -i PATH=/usr/bin:/bin HOME=TMPDIR=tests/ps5/build/rosetta_home`, `gtimeout 30`, stdin `/dev/null`; both the relinker and the relinked binary run via `arch -x86_64`; only the self-built test ELFs are ever run. Per-test artefacts in `tests/ps5/build/rosetta/<test>.<variant>/` (nidify.log, relink.log, input.elf, eboot). AnyPS5 runtime chatter on stdout (`[src/Thread.cpp:947 RunThread] ...`) is dropped from the stored reference. `ref_compare.sh` compares `<dir>/<test>.<variant>/stdout.txt` (the m1_run.sh layout) with `tests/ps5/ref/`, honouring `tests/ps5/rosetta_known.txt` and `tests/ps5/ref/anyps5_known.txt`. A full `--elf` run takes about 3 min (relinking dominates).

## Results (2026-11-14, AnyPS5 ea0c471)

Reference vs `tests/ps5/expected/` (source-level oracle): 22 of 22 cells (11 tests x 2 variants) match, 18 exactly and 4 modulo a documented known deviation.

| Test | plt | noplt | Note |
|---|---|---|---|
| m1_main | match | match | threads, C++ EH, setjmp/longjmp run under AnyPS5 |
| t_arith, t_x87, t_string, t_switch, t_vdispatch, t_tls, t_recurse, t_abi | match | match | |
| t_simd | match* | match* | * modulo the Rosetta `VPSLLVQ`/`VPSRLVQ` deviation already in `rosetta_known.txt` (raw ref has the `MISMATCH sllvq_y ...` lines) |
| t_varargs | match** | match** | ** modulo AnyPS5 printf tie rounding (below); raw ref differs on 2 lines |

`broken_*` tests are not run at ELF level (no stdout contract; they test translator failure reports).

### Mismatch explanations

* **t_varargs, 2 lines** (`%.0f` of 0.5 prints `1` instead of `0`; `%.1f` of 8.25 prints `8.3` instead of `8.2`): `sprintf`/`vsnprintf` are imports here, resolved to AnyPS5's `libc.prx`, whose float formatting rounds decimal ties half-up. IEEE-exact libc (FreeBSD, which the PS5 libc derives from, and macOS) rounds half-even. Classification: AnyPS5 HLE difference. `expected/t_varargs.txt` is right; the reference is wrong on those lines. Filtered by `tests/ps5/ref/anyps5_known.txt`. Consequence for our runtime: its `sprintf` should follow the half-even behaviour; do not "fix" the expected file.
* **t_simd, sllvq/srlvq**: Rosetta ignores count bits 32..63 (unchanged from `docs/m1/tests.md`); real semantic difference in Rosetta, model follows the SDM.
* No oracle bug found in the expected files; the ELF-level run agrees with them on every other line, so the lifter's expected outputs now rest on three paths (arm64 models, x86 source under Rosetta, x86 ELF through AnyPS5).

## Limits and gaps

* The reference executes the ELF's own code but **not the SDK CRT**; libc/libkernel functions that the ELF imports run AnyPS5's implementations (HLE), not the PS5's. The oracle therefore validates instruction semantics and program logic, not SDK startup or PS5 libc behaviour. Anything printing libc-dependent values (printf rounding, `strtod`, math) can differ for HLE reasons; use `anyps5_known.txt`.
* Stub returns instead of exiting: rc is always 134 + `guest entry returned`; exit codes are not reproduced. A test whose `main` calls `exit()` itself would go through AnyPS5's libc `exit`, which is fine.
* An import the AnyPS5 libs do not export (e.g. `broken_import`) fails in dyld at start, which is also a usable oracle for that case.
* TLS, `fs`/`gs` accesses and threads work (t_tls, m1_main match), but the SDK-specific emulated-TLS runtime is only exercised through the main-stub path.
* Depends on `work/s3/rosetta/AnyPS5-x86/build` (override with `ANYPS5_BUILD`) and Rosetta.
