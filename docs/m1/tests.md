# M1 test corpus and reference pipeline

Branch `m1-tests`. Everything lives in `tests/ps5/` and `tools/`; no lifter/loader/emitter/runtime code is touched.

## Layout

| Path | Purpose |
|---|---|
| `tests/ps5/src/*.c *.cpp`, `ps5test.h` | the test programs (PS5 payload SDK, C11/C++17) |
| `tests/ps5/tests.list` | test name, source, extra PS5 build flags (SIMD tests add `-mavx2 -mfma ...`) |
| `tests/ps5/expected/<test>.txt` | expected stdout (the broken set: expected stdout prefix + `<test>.report` tokens) |
| `tests/ps5/rosetta_known.txt` | known Rosetta deviations filtered by `ref_rosetta.sh` |
| `tools/m1_build_tests.sh` | builds each test twice: `<n>.plt.elf` (`-fplt`) and `<n>.noplt.elf` (SDK default `-fno-plt`) into `tests/ps5/build/` |
| `tools/m1_run.sh` | the harness: translate, compile/link, run sandboxed, compare, PASS/FAIL table |
| `tools/m1_native_build.sh` | S2's `build_manual.sh` adapted (flags read from `build.ninja`, cached runtime objects, parallel) |
| `tools/m1_gen_expected.sh` | regenerates expected files on the host from the portable models |
| `tools/ref_rosetta.sh` | Rosetta (x86-64) reference runner |
| `tools/m1_stubs/` | S2's stub headers (`minimp3`, `atrac9`) so Ajm can be attempted; GPU `.mm` are skipped |

Run: `tools/m1_run.sh [--variant plt|noplt|both] [--call-model nested|flat] [--timeout N] [--rebuild] [--no-elf-build] [test ...]`
(`--call-model` keeps the outputs apart in `build/work-<model>/` and `build/m1_results-<model>.txt`).
Results go to stdout and `tests/ps5/build/m1_results.txt`; per-test artefacts (translate log, emitted dir, `native`, raw and filtered stdout, stderr) are in `tests/ps5/build/work/<test>.<variant>/`.
The harness builds `./cmd/ps4-recomp` with `GOFLAGS=-mod=vendor` from the checkout it lives in (so after a merge it exercises the merged translator), or uses `PS4RECOMP_BIN`. It never runs `scripts/build.sh`, never runs x86 code. The translated arm64 program runs under `env -i`, HOME/TMPDIR confined to `tests/ps5/build/home`, `gtimeout` (default 30 s), stdin `/dev/null`, cwd = the emitted directory (the current runtime loads `guest_image.bin` from its cwd). Runtime chatter tagged `[ps4-...]`/`[nps5-...]` on stdout is dropped before comparing (`M1_NOISE_RE`).

## The tests

All print through `sceKernelWrite` (libkernel import) using a tiny formatter in `ps5test.h` (no printf, so a diff points at the lifter, not stdio), except `t_varargs`/`t_abi`, which also exercise libc formatting/math deliberately. Output is hex FNV checksums plus a few readable values.

| Test | Covers |
|---|---|
| `m1_main` | **M1 exit test**: two pthreads + mutex, C++ throw/catch across three frames with RAII destructors, `std::exception`, rethrow, `setjmp`/`longjmp` through recursion, checksums |
| `t_eh` | non-local flow (docs/m1/setjmp-eh.md): 6-frame unwind with destructors, `catch(...)`+rethrow, exception on a second thread, nested `jmp_buf`s, `longjmp(..,0)` -> 1, repeated `setjmp` on one buffer, `sigsetjmp`/`siglongjmp`, longjmp out of a catch handler. Expected output derived by hand from the language semantics (not from a Rosetta run) |
| `broken_longjmp` | extra broken program: `longjmp` to a `setjmp` whose function returned; report must say "already returned" |
| `t_arith` | add/sub/adc/sbb/and/or/xor/neg/inc/dec at 8/32/64 bit over 18 edge values x carry-in, all of CF PF AF ZF SF OF read via `pushfq`; `lahf`; `jp`/`jnp` consumers; DF via `std`/`cld`+`pushfq`; cmov/setcc; shifts/rotates/bswap/clz/ctz; 128-bit mul, div |
| `t_simd` | 71 cases: the Syobon census forms (`VPBROADCAST{B,W,D,Q}` all operand classes, `VBROADCASTI128`, `VPERMQ/PD/D/PS`, `VPERM2I128`, `VPMULHUW`, `VPS{LL,RL,RA}V{D,Q}`, `VPMASKMOVD` load+store, `PSHUFW`, `VF[N]M{ADD,SUB}{SS,SD,PS,PD}` in 132/213/231 shapes, reg and mem operands). Exact encodings that clang canonicalises away (VPERMQ, VPERM2I128, VBROADCASTI128, VPBROADCASTQ, 231 FMAs) are forced with inline asm; verified in `llvm-objdump` |
| `t_x87` | `fcomip` flags incl. unordered (PF) with `jp`, `fistp` under all four rounding modes, `fscale`, `fldpi`, `fsqrt`, `fxch`, extended-precision `(1e16+1)-1e16`, 64-bit mantissa of 1/3 |
| `t_string` | `rep movsb/movsq/stosb/stosq/cmpsb/scasb/lodsb` with DF=0 and DF=1; libc `strlen/strchr/strrchr/memchr/strcmp/memcmp/memset/memcpy/memmove` over lengths and alignments |
| `t_switch` | 48-way dense switch (jump table), signed/offset/u16/char-range switches, computed-goto bytecode VM |
| `t_vdispatch` | virtual calls, multiple and virtual inheritance, `dynamic_cast`/RTTI, function-pointer tables, pointer-to-member-functions, callbacks, `qsort` comparator |
| `t_tls` | `__thread` initialised/zero/array/pointer TLS in main and two pthreads, address-taken TLS (SDK uses emulated TLS) |
| `t_callmodel` | call/return model (docs/m1/flatstack.md): `ret imm16`, `push addr; ret`, a return address replaced by the callee, `call next; pop`, 100000-deep recursion (past the shadow stack), `longjmp` out of 88573 frames and 2000 short ones, nested `qsort` comparators (HLE -> guest -> HLE -> guest), tail calls through pointers, four threads. Passes with `-call-model flat` only: the nested model ignores guest return addresses |
| `t_recurse` | 20000-deep recursion, fib(27), Ackermann, mutual recursion, recursion through function pointers, VLA frames |
| `t_varargs` | int/double/mixed/struct-by-value `va_arg`, `va_copy`, register-save and overflow areas, `vsnprintf`/`snprintf`/`sprintf` with many formats incl. `%f %e %g` |
| `t_abi` | struct-by-value in all SysV classes (SSE/INT/mixed/MEMORY/`float[3]`), `long double` (x87 return), `__int128`, 19-argument mixed call, calls through volatile pointers, libc `div/ldiv/lldiv` struct returns, `qsort`/`bsearch`, `strtod`, `frexp`/`modf` out-pointers, math |
| `broken_import` | §8.2 #1: call to an undefined import (`sceNps5NoSuchFunction`, linked with `--unresolved-symbols=ignore-all`) from `bad_import_caller` |
| `broken_unsupported` | §8.2 #2: `PDEP` emitted via `.byte` in `rare_insn_site` (swap bytes if the lifter ever supports it) |
| `broken_null` | §8.2 #3: null store in `null_deref_site` |
| `broken_exception` | §8.2 #4: uncaught `std::runtime_error` thrown from `uncaught_thrower` |

For the broken set the harness requires: nonzero exit, stdout equal to the expected `... before` line, and every token in `expected/<n>.report` (function names, import name) present in stderr or in any file under a `runs/` bundle the run produced. `PASS` = all three, `PARTIAL` = crashed as intended but the report does not name the function. This is the §8.2 acceptance check; it will start passing once the diagnostics branch writes `runs/<ts>-<title>-<phase>/crash.txt`.

## How expected outputs were produced

The tests are deterministic. Each source compiles both for the PS5 and, with `-DNPS5_HOST`, for any host (`sceKernelWrite` -> `write(2)`). Hardware-dependent tests (`t_arith`, `t_x87`, `t_string`, `t_simd`) contain **two implementations**: x86-64 inline asm/intrinsics and a portable C model; the x86 build prints the hardware result and also compares it to the model (`model_mismatches=0` is part of the expected output). On arm64 the model is printed, so one expected file serves all.

1. `tools/m1_gen_expected.sh` builds the sources natively for arm64 macOS (clang, `-DNPS5_HOST`) and records stdout as `expected/`.
2. `tools/ref_rosetta.sh` builds the **same sources for x86_64-apple-macos** and runs them under Rosetta. Result: **11/11 expected files match** (m1_main, t_arith, t_simd, t_x87, t_string, t_switch, t_vdispatch, t_tls, t_recurse, t_varargs, t_abi). So the expected outputs are confirmed by two independent execution paths: arm64 natively (models) and x86-64 under Rosetta (hardware semantics), and the models for flags, x87 and string ops agree with Rosetta's x86.

One known deviation, recorded in `tests/ps5/rosetta_known.txt`: Rosetta's `VPSLLVQ`/`VPSRLVQ` ignore bits 32..63 of each 64-bit count (counts >= 64 must give 0 per the SDM). The model follows the SDM; `ref_rosetta.sh` filters those two cases. This reaffirms the spec: Rosetta never arbitrates alone.

## Rosetta oracle: status

Source-level Rosetta runs (`tools/ref_rosetta.sh`) work and confirm the expected files (above). The ELF-level
oracle (running the PS5 payload ELFs under Rosetta via AnyPS5's relinker) is documented, with the reasons it
failed and what the relinker needs, in `docs/m1/oracle.md`.

## Baseline

Baseline at repo commit `22ebefd` (fork + spec, no M1 work merged). Produced by `tools/m1_run.sh --no-elf-build` (ELFs built beforehand), 2026-11-14, M4 Pro. Full run ~1.5 min with warm runtime-object cache (~2 min cold).

```
M1 test run  translator=$NPS5_ROOT/wt/tests/tests/ps5/build/ps4-recomp  repo=22ebefd  2026-11-14T12:19Z
TEST                VARIANT VERDICT         XLATE   BUILD   RUN    NOTE
m1_main             plt    CRASH           1s      3s      3s     rc=134 [recomp-jmp] Hooked unresolved setjmp stub at 0x33250 (setjmp)
m1_main             noplt  CRASH           0s      3s      0s     rc=134 [recomp-jmp] Hooked unresolved setjmp stub at 0x40518 (setjmp)
t_arith             plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_arith             noplt  CRASH           1s      2s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xca48 (return addr=0x9629)
t_simd              plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_simd              noplt  CRASH           1s      1s      1s     rc=134 FATAL: Unsupported guest instruction executed!
t_x87               plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_x87               noplt  CRASH           1s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_string            plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_string            noplt  CRASH           1s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_switch            plt    PASS            1s      2s      0s     
t_switch            noplt  CRASH           0s      2s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xc9f0 (return addr=0x8e6a)
t_vdispatch         plt    CRASH           0s      4s      0s     rc=134 FATAL: Unresolved indirect jump/call to 0x58920 (return addr=0x32b9e)
t_vdispatch         noplt  CRASH           1s      2s      0s     rc=134 FATAL: Unresolved indirect jump/call to 0x58920 (return addr=0x32b6e)
t_tls               plt    CRASH           0s      2s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xca40 (from RIP=0x94d0, return addr=0x9
t_tls               noplt  CRASH           0s      1s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xca40 (return addr=0x8e10)
t_recurse           plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_recurse           noplt  CRASH           0s      2s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xca48 (return addr=0x8e92)
t_varargs           plt    CRASH           0s      2s      2s     rc=134 FATAL: Unresolved indirect jump/call to 0x14138 (from RIP=0xa250, return addr=0x
t_varargs           noplt  CRASH           0s      2s      1s     rc=134 FATAL: Unresolved indirect jump/call to 0xc9b8 (return addr=0x8e29)
t_abi               plt    CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
t_abi               noplt  CRASH           0s      2s      0s     rc=134 FATAL: Unsupported guest instruction executed!
broken_import       plt    PARTIAL         1s      1s      1s     crash rc=134; report lacks: bad_import_caller
broken_import       noplt  FAIL            0s      1s      1s      stdout-differs (rc=134)
broken_unsupported  plt    PARTIAL         0s      2s      0s     crash rc=1; report lacks: rare_insn_site
broken_unsupported  noplt  FAIL            0s      2s      1s      stdout-differs (rc=134)
broken_null         plt    FAIL            0s      2s      1s     exit 0 stdout-differs (rc=0)
broken_null         noplt  FAIL            0s      2s      0s      stdout-differs (rc=134)
broken_exception    plt    PARTIAL         1s      2s      0s     crash rc=134; report lacks: uncaught_thrower
broken_exception    noplt  FAIL            0s      2s      0s      stdout-differs (rc=134)
-- 1 PASS, 29 not passing, 30 total
```

Reading the baseline:

* `t_switch.plt` already passes: jump tables, 48-way switch, VM dispatch lift correctly.
* `-fno-plt` variants fail at the first import (`Unresolved indirect jump/call to 0x...`, S2 finding #3): GOT-style imports are not resolved by the loader.
* `-fplt` failures are mostly `Unsupported guest instruction executed!` (`SYSCALL`-shaped libc stubs, and in t_arith/t_simd/t_x87/t_string the AVX2/x87/flag forms), and unresolved `libSceLibcInternal` imports (`setjmp`, `qsort`, `div`, ... not HLE'd).
* `m1_main.plt` reaches `setjmp` and dies there: `Hooked unresolved setjmp stub`.
* `broken_null.plt` exits 0: the baseline guest address space maps address 0, so the null store succeeds silently (matches S2 finding #4; the identity-arena work should make it fault).
* No diagnostics bundles exist yet, so the broken set is `PARTIAL`/`FAIL`.

## Current status

Re-run at repo commit `4c9d416` (loader, lifter, diagnostics, discovery, oracle, libc, setjmp/EH and FEX-harness merged, plus this fixup branch), 2026-11-14. 24 PASS of 34 (up from 1 of 30 at the baseline).

```
M1 test run  translator=$NPS5_ROOT/wt/fixup/tests/ps5/build/ps4-recomp  repo=4c9d416  2026-11-14T13:12Z
TEST                VARIANT VERDICT         XLATE   BUILD   RUN    NOTE
m1_main             plt    PASS            2s      7s      1s     
m1_main             noplt  PASS            1s      8s      0s     
t_arith             plt    TRANSLATE-FAIL  1s      -       -      Error: failed to emit ARM64 assembly: 1 unsupported instruction(s), 1 distinct form(s):
t_arith             noplt  TRANSLATE-FAIL  0s      -       -      Error: failed to emit ARM64 assembly: 1 unsupported instruction(s), 1 distinct form(s):
t_simd              plt    PASS            1s      3s      1s     
t_simd              noplt  PASS            0s      4s      0s     
t_x87               plt    TRANSLATE-FAIL  0s      -       -      Error: failed to emit ARM64 assembly: 3 unsupported instruction(s), 2 distinct form(s):
t_x87               noplt  TRANSLATE-FAIL  0s      -       -      Error: failed to emit ARM64 assembly: 3 unsupported instruction(s), 2 distinct form(s):
t_string            plt    TRANSLATE-FAIL  0s      -       -      Error: failed to emit ARM64 assembly: 1 unsupported instruction(s), 1 distinct form(s):
t_string            noplt  TRANSLATE-FAIL  1s      -       -      Error: failed to emit ARM64 assembly: 1 unsupported instruction(s), 1 distinct form(s):
t_switch            plt    PASS            0s      4s      1s     
t_switch            noplt  PASS            1s      4s      1s     
t_vdispatch         plt    FAIL-OUTPUT     1s      6s      0s     rc=0 < qsort_fnv=0xc46217049ebf7f88
t_vdispatch         noplt  FAIL-OUTPUT     2s      7s      1s     rc=0 < qsort_fnv=0xc46217049ebf7f88
t_tls               plt    PASS            0s      4s      0s     
t_tls               noplt  PASS            1s      4s      0s     
t_recurse           plt    PASS            1s      4s      1s     
t_recurse           noplt  PASS            0s      4s      0s     
t_varargs           plt    PASS            1s      3s      1s     
t_varargs           noplt  PASS            0s      4s      1s     
t_abi               plt    PASS            0s      4s      0s     
t_abi               noplt  PASS            1s      3s      0s     
broken_import       plt    PASS            1s      3s      0s     rc=134, report ok
broken_import       noplt  PASS            0s      3s      0s     rc=134, report ok
broken_unsupported  plt    PASS            0s      5s      0s     rc=1, report ok
broken_unsupported  noplt  PASS            0s      3s      0s     rc=1, report ok
broken_null         plt    FAIL            0s      5s      0s     exit 0 stdout-differs (rc=0)
broken_null         noplt  FAIL            1s      4s      0s     exit 0 stdout-differs (rc=0)
broken_exception    plt    PASS            1s      6s      0s     rc=134, report ok
broken_exception    noplt  PASS            1s      4s      1s     rc=134, report ok
t_eh                plt    PASS            1s      5s      0s     
t_eh                noplt  PASS            0s      6s      1s     
broken_longjmp      plt    PASS            0s      4s      0s     rc=134, report ok
broken_longjmp      noplt  PASS            1s      3s      0s     rc=134, report ok
-- 24 PASS, 10 not passing, 34 total
```

Reading it:

* `t_arith`, `t_string` (`STD`, backward string ops) and `t_x87` (`FCOMIP`, `FLDPI`) stop at translation on unsupported forms; lifter work.
* `t_vdispatch` fails only the `qsort_fnv` line. Cause: the test's LCG reduces `% 1000` with `mulx rdx, rdx, rdi` (hi and lo destination the same register); the lifter's MULX lowering writes lo before computing hi from RDX, so the array is filled with garbage before qsort runs. The HLE qsort and its guest comparator calls are correct (verified: sorts the garbage input correctly). Fix is in `pkg/lifter/arm64_lifter_bmi.go` (compute `umulh` into a scratch first, write hi last).
* `broken_null` exits 0 until the memory-image work makes address 0 fault.
* `tools/m1_native_build.sh` now prints every stubbed unresolved symbol and fails if any is `recomp_*`/`nps5_*`/`shim_*`. Remaining stubs are GPU/audio/VFS (`ps4_gnm_*`, `ps4_vfs_*`, `ps4_ajm_*`).

## Gaps and notes

* No arm64 execution of the *instruction-level differential* suite (spec §8 item 1) here; the tests are program-level.
* `t_simd` expects the lifter to support every listed form; today most are unsupported (this is the point: it is the census made executable).
* x87 test cases that depend on the 64-bit mantissa use hardcoded model constants (documented in source); they are confirmed only by Rosetta, which implements x87 extended precision.
* `t_abi` transcendental results are rounded to 1e-6 before printing, so a 1-ulp libm difference between the PS5 libc and macOS cannot cause a false failure; other math functions printed as exact hex are exact operations only.
* Thread scheduling is not pinned: `m1_main`/`t_tls` print only schedule-independent values after `pthread_join`.
* The harness compiles the emitted C/asm with the flags in `build.ninja`; GPU `.mm` and Ajm are skipped and unresolved symbols stubbed (bring-up concession from S2, fine for console-only M1).
* The baseline uses `hello_world`-style SDK flags `-O2`; try `OPT=-O0 tools/m1_build_tests.sh` for unoptimised variants.
