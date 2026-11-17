# M1 libc HLE

Branch `m1-libc`. Code: `pkg/runtime/modules/libc/` (runtime), `pkg/emitter/libc_shims.go` (binding), `tests/libc/libc_unit.c` + `tools/libc_unit.sh` (host unit test), `pkg/emitter/libc_shims_test.go` (binding test).

## Why
PS5 payload-SDK ELFs import libc by plain name (library "plaintext": `strstr`, `div`, `qsort`, ...). Retail PS5 titles import it by NID (`viiwFMaNamA#H#I` = `strstr`); their bundled `libc.prx` is hollow, so the C library has to come from the HLE. Before this branch every one of these hit `shim_unresolved_stub` (`[hle] unresolved import called: strstr`).

## Inventory (imports of the M1 tests, `hello_cxx`, and the Syobon eboot)
Sources: `hle_report.txt` of each translation (`tests/ps5/build/work/*/out/`, Syobon with `-allow-unsupported`), `nm -u` for tests that fail to translate. 149 distinct import names; **138 are now bound by plain name and by NID** (before: 0 of the libc ones). Syobon alone imports 110 names that no shim served (92 by NID through `libSceImeDialog`/`libSceLibcInternal` library ids, 13 `libSceLibcInternal`, 5 other).

Not libc, untouched: `pthread_*`, `sceKernelWrite`, `sceUserService*`, `sceAudioOut` NIDs, `sceNps5NoSuchFunction` (intentionally unresolved, the `broken_import` test), `setjmp`/`longjmp` (m1-setjmp).
Data imports `__stdinp __stdoutp __stderrp __isthreaded __mb_cur_max` are bound by a different mechanism, see "Data imports".

Used by M1 tests (all now bound): `strstr` (t_simd); `div ldiv lldiv qsort bsearch strtod frexp modf ldexp atan2 lround` (t_abi); `sprintf vsnprintf snprintf` (t_varargs, m1_main); `malloc calloc realloc free aligned_alloc abort getenv isxdigit memchr memcmp strchr strrchr fprintf fputc fputs fwrite fflush vfprintf` (m1_main, t_tls, t_vdispatch, t_string, broken_exception).

## Coverage (312 shims)
| Area | Functions |
|---|---|
| string/memory | `memchr memrchr memcmp bcmp bcopy bzero memccpy mempcpy strchr strrchr strchrnul strstr strcasestr strncmp strcasecmp strncasecmp strcoll strxfrm strcat strncat strlcpy strlcat stpcpy stpncpy strnlen strspn strcspn strpbrk strdup strndup strtok strtok_r strsep strerror strerror_r` (memcpy/memmove/memset/strlen/strcpy/strncpy/strcmp already existed) |
| ctype | `isalnum isalpha isascii isblank iscntrl isdigit isgraph islower isprint ispunct isspace isupper isxdigit toupper tolower toascii` (C locale) |
| wchar | `wcslen wcscmp wcsncmp wcscoll wcsxfrm wcsstr wmemcmp wmemchr mbrtowc mbtowc mbrlen wcrtomb mbsrtowcs wcstombs wcstod wcstof wcstold wcstol wcstoll wcstoul wcstoull` (byte semantics: the C locale) |
| stdlib | `div ldiv lldiv imaxdiv abs labs llabs imaxabs qsort bsearch atoi atol atoll atof strtol strtoll strtoul strtoull strtoimax strtoumax strtod strtof strtold rand srand rand_r getenv setenv unsetenv atexit __cxa_atexit __cxa_finalize abort _Exit setlocale getcwd` |
| heap | `malloc calloc realloc reallocf free aligned_alloc memalign posix_memalign malloc_usable_size` |
| stdio | `fopen fdopen fclose fflush fread fwrite fputs fputc putc puts putchar fgetc getc getchar ungetc fgets fseek fseeko ftell ftello rewind ferror feof clearerr fileno setvbuf setbuf remove perror printf vprintf fprintf vfprintf dprintf sprintf snprintf vsprintf vsnprintf asprintf vasprintf sscanf vsscanf fputwc fgetwc ungetwc` |
| math | `sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 log10 log1p sqrt cbrt pow hypot fmod fmin fmax fdim copysign remainder nextafter floor ceil trunc round rint nearbyint fabs erf erfc tgamma lgamma logb fma ldexp scalbn frexp modf remquo sincos ilogb lround llround lrint llrint` plus every `...f` variant, `isnan isinf finite __isnanf __isinff __fpclassify{d,f}` |
| time | `time clock difftime gmtime gmtime_r localtime localtime_r mktime timegm strftime sleep` (timezone is UTC) |
| signal sets | `sigemptyset sigaddset sigdelset sigismember` |
| logged stubs | `dlopen dlsym dlclose dlerror fork execvp execve waitpid kill sysctl sysctlbyname pthread_set_name_np _init_env swprintf vswprintf fscanf scanf system popen qsort_r` |

A stub logs once per name (`libc stub NAME called (not implemented): returning 0x.. rip=..`), sets errno (`ENOSYS`/`ENOENT`/`ECHILD`) where the real call would fail, and returns -1 or NULL; never a silent 0.

## Binding
`pkg/emitter/libc_shims.go` lists the names; `init()` adds each to `CanonicalShims` as `shim_<name>`, and the existing `initCanonicalNIDMap` derives the NID of every name (and of `_name`), so one entry binds both forms. `libc_shims_test.go` checks every name by plain name and by NID, plus the 99 NIDs the Syobon eboot actually imports (taken from its import table), so the NID scheme is verified against real data.

**Fallback semantics.** These shims are bound only to an import that nothing else serves (the slot still points at itself/its PLT stub after `ResolveModuleRelocations`), and never to a symbol the guest image defines itself (`bindAddr` skips them). Existing shims (memcpy, ...) keep their unconditional binding. So a PS4 title that bundles a real `libc.prx`, or a guest with its own `strstr`, keeps running its own code; the HLE heap never mixes with a guest `malloc`.

## Data imports
`__stdoutp` and friends are GLOB_DAT imports of *variables*; a function stub is wrong for them (the guest dereferences the slot). `recomp_init_runtime_file` (recomp_runtime.c, after the image is loaded) walks the registered unresolved slots and calls `nps5_libc_bind_data_import`, which allocates an 8-byte guest cell holding the guest `FILE *` (or the value, for `__isthreaded`=1, `__mb_cur_max`=1) and writes the cell's address into the GOT slot. This is the only change outside the module besides `shim_exit` (runs `atexit` handlers through a weak hook).

## ABI notes
* Shim contract: args in `rdi rsi rdx rcx r8 r9`; return in `rax` (+`rdx`), `xmm0` low lane for float/double, FPU stack for `long double` (`strtold`, `wcstold` push onto `fpu_stack`); every path ends in `SHIM_RETURN()` (pops the return address).
* **div family.** `div_t {int quot; int rem}` is one INTEGER eightbyte: `rax = (uint32)quot | rem << 32`. `ldiv_t`/`lldiv_t`/`imaxdiv_t` are two INTEGER eightbytes: `rax = quot`, `rdx = rem`. Division by zero and `INT_MIN / -1` raise SIGFPE as x86 `idiv` would (logged first).
* **Varargs.** A variadic shim (`snprintf`, `sscanf`, `fprintf`, ...) reads integer arguments from the argument registers after the fixed ones, then from the overflow area at `rsp+8` (rsp points at the return address at shim entry); doubles from `xmm0..7` (`al` is not needed because the xmm are always captured), then the overflow area. `long double` is read from the overflow area, 16-byte aligned, as an x87 80-bit value converted to host `long double` (= double on arm64, so `%Lf` loses precision beyond double).
* **Guest `va_list`** (`vsnprintf vsprintf vfprintf vprintf vasprintf vsscanf`): the argument is the guest address of `{u32 gp_offset, u32 fp_offset, u64 overflow_arg_area, u64 reg_save_area}`; the engine reads and updates it in place (gp: `gp_offset < 48` then `reg_save_area + gp_offset`, else overflow; fp: `fp_offset < 176` step 16, else overflow step 8). Unit-tested with a crafted reg-save area plus overflow area.
* **printf engine.** Parses flags/width/precision (`*` supported)/length (`hh h l ll j z t L q`)/conversion, converts the argument, then formats one conversion at a time with host `snprintf`. Positional `%n$` is rejected with an error log. `%n` writes through the guest pointer. `%s` with a NULL pointer prints `(null)`.
* **Callbacks into guest code** (`qsort`, `bsearch`, `__cxa_finalize`, `atexit` handlers): `nps5_libc_call_guest` (core.c) builds the call frame at `rsp-128`, 16-byte aligned with a null return address so the callee sees `rsp % 16 == 8`, dispatches through `recomp_dispatch` and restores the caller-saved registers. (`recomp_call_guest` pushes 8 bytes without re-aligning, which misaligns SysV callees; the module avoids it.) `qsort` is FreeBSD's Bentley-McIlroy, so equal-key ordering matches the console.
* **errno** is the guest-visible per-thread slot `fs_base + 0x100` returned by `__error()` (`set_guest_errno`). `strto*` set `ERANGE`; FreeBSD and macOS errno numbers agree for the values used.
* **rand** is FreeBSD's Park-Miller (`srand(1); rand()` = 16807) so seeded sequences match the console.
* **Heap** lives in guest memory: power-of-two classes 32 B..64 KiB carved from 1 MiB chunks (`recomp_vm_alloc_named_aligned`), larger blocks are individual VM allocations released with `recomp_vm_free`; 16-byte header, 16-byte alignment, `memalign` via over-allocation. Invalid/double free is logged, not fatal.
* **Pointer translation** only through `NPS5_G2H(ctx, guest)` / `NPS5_H2G(ctx, host)` (inline functions in `nps5_libc.h`, 0 maps to NULL, arguments evaluated once). When the memory model changes, change those two functions; no shim indexes `mem_base` directly. (An early macro form evaluated its argument twice and broke `sscanf`; the unit test caught it.)
* FP: libm results come from the macOS libm (within 1 ulp of x86 libm for the transcendental set; `sqrt floor ceil fmod ldexp ...` are exact). t_abi prints transcendentals rounded to 1e-6, so it passes.

## Results
`tools/m1_run.sh` before/after on this branch (see commit message of the docs commit for the table):

| Test | before | after |
|---|---|---|
| t_simd plt | CRASH `unresolved import strstr` | **PASS** |
| t_abi plt / noplt | CRASH `unresolved import div` | **PASS / PASS** |
| t_varargs plt | CRASH (jump to 0x14138) | CRASH, same non-libc fault (see below); its libc imports are bound |
| total | 8 PASS of 30 | 11 PASS of 30 |

Remaining failures are not libc: `t_simd.noplt` / `t_varargs.noplt` / `t_tls` (GOT slot overwritten or shadowed, below), `m1_main` (`setjmp`, m1-setjmp), `t_vdispatch` (indirect call to 0x58920), `t_arith/t_x87/t_string` (translate: unsupported instructions, m1-fex), `broken_null/exception` (memory / diagnostics).

## Findings for other agents
* **Loader (`ResolveModuleRelocations`)**: it builds the import-resolution map from `main.Symbols`, which includes STB_LOCAL symbols. `t_simd.noplt.elf` has a local bss object named `strlen` at 0x188d8; the GOT slot of the `strlen`/`strstr` import was resolved to it, so the call jumps into .bss (`Unresolved indirect jump/call to 0x188d8 from RIP=0x123c8`). Verified by merging m1-loader into this branch: the crash persists. Only global, defined, non-local symbols may satisfy an import. The same fault family likely explains `t_tls` (`0x14960`, GLOB_DAT `pthread_getspecific`) and `t_varargs` (`0x14138`).
* **Memory agent**: heap chunks come from `recomp_vm_alloc_named_aligned(ctx, 1 MiB, 4096, RW, 0, "libc.heap")`, large blocks from the same call with `recomp_vm_free`; callers use `ctx->mem_base`/`mem_size` through `nps5_g2h`. A SysV→AAPCS thunk would let the numeric shims (`div`, math) be plain C functions; they are written against `GuestContext` today.
* **setjmp agent**: `setjmp`/`longjmp` are deliberately not in this module.

## Gaps
* `fscanf`, `scanf`, `swprintf`, wide printf, `%n$` positional printf, `qsort_r`, `strptime`, locale beyond "C", `localeconv`, `long double` math (`sinl`, ...) are not implemented (stubs where imported by the corpus).
* `FILE` is an opaque guest block {magic, index}; FreeBSD macros that read the struct directly (`__sgetc`, `feof`/`fileno` as macros) will not work; the corpus only imports the functions.
* `atexit`/`__cxa_atexit` handlers run from `shim_exit` only; returning from `main` into a guest `exit` path that does not call `shim_exit` skips them.
* `getenv` sees an empty environment (guest-owned), `getcwd` returns `/app0`, timezone is UTC.
* `%Lf`/`%Le` precision is limited to double.
* Not run: Syobon end to end (blocked on the lifter, 163 unsupported opcodes, and the memory model).

## File family error convention (m3-bootfix)
* libc/POSIX names (`mkdir`, `unlink`, `rename`, `rmdir`, `truncate`, `chmod`, `open`, `stat`, ...) return -1 and set the guest errno.
* `sceKernel*` file calls (`sceKernelOpen/Close/Read/Lseek/Stat/Fstat/Mkdir/Rmdir/Unlink/Rename/Truncate/Chmod/Utimes/Getdents/Getdirentries/Fsync/Ftruncate/Pwrite`, `sceKernelCheckReachability`) return `0x80020000 | errno` (AnyPS5 `KernelErrors.hpp` `SceKernelError`); the guest errno is still set. `ps4_syscalls.c` maps the few host (macOS) errnos that differ from FreeBSD (`ENOATTR`) before OR-ing. `sceKernelMkdir/Unlink/Rename/Truncate/Chmod/Utimes/Getdents` are thin wrappers (`sce_wrap`) over the POSIX shim that convert -1/errno.
* The NIDs of `sceKernelMkdir/Rename/Unlink/Truncate` (`1-LFLmRFxxM`, `52NcYU9+lEo`, `AUXVxWeJU-A`, `WlyEA-sLDf0`) bind the `sceKernel*` shims; the libc `rmdir` NID (`c7ZnT7V1B98`) stays POSIX.
* Test: `tools/kernelfs_unit.sh` (`tests/kernelfs/kernelfs_unit.c`).

## Pak locale fallback
`ps4_pak_lookup` no longer serves `en.res` for a missing `en_US.res` (ICU walks the parent chain itself and handles ENOENT, as on the console). `NPS5_PAK_LOCALE_FALLBACK=1` restores the old behaviour for a title that turns out to depend on it. No title of the current corpus (Syobon, Greenhollow, Calculator3D, the UE title PPSA30552) is known to need it: the UE log shows ICU loading its bundles from the pak by exact name.

## Title identity (PS5)
`ps4_sfo_find_and_load` returns the PS4 `param.sfo` when there is one, else converts `sce_sys/param.json` (PS5) to an in-memory SFO: `titleId` -> `TITLE_ID`, `contentId` -> `CONTENT_ID`, `contentVersion` -> `APP_VER`, `masterVersion` -> `VERSION`, `applicationCategoryType` -> `APPLICATION_CATEGORY_TYPE` (+ `CATEGORY=gd`), `userDefinedParam1..4` -> `USER_DEFINED_PARAM_n`, `localizedParameters[defaultLanguage].titleName` -> `TITLE`. sceAppContent (`AppParamGetString/Int`), SaveData (sfo title id), the game key and the GPU-cache directory therefore see the PS5 title id instead of `CUSA00000`. libSceSystemService has no title-id surface in this tree.
