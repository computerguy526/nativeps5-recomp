# M1 diagnostics (SPEC 8.2)

Every translate and every run leaves a bundle under `runs/` (`$NPS5_RUNS_DIR`, default `./runs`):
`runs/<yyyymmddThhmmss.mmm>-<title>-<phase>/`, `phase` = `translate` or `run`. Symlinks `runs/latest`,
`runs/latest-translate`, `runs/latest-run` point at the newest.

## Translate bundle (Go, `ps4-recomp` CLI)
| file | content |
|---|---|
| `manifest.json` | tool git commit (+dirty), input path/sha256/size, settings, `NPS5_*`/`PS4_*` env subset, host `hw.model`, OS build, result/error, wall time |
| `log.jsonl` | JSONL records `{t, level, subsys, msg, ...}` |
| `census.json` | unsupported stubs counted from emitted `// UNSUPPORTED:` markers (guest addr, mnemonic, operand form, function, kind), counts by form; functions emitted vs found vs FDEs (`fde_count` = `loaded.FuncBounds`); indirect-jmp sites with recovered/unrecovered jump-table addresses; indirect calls; GOT tail jumps; UD2 stubs |
| `imports.json` | per import: library, NID, resolved name, `bound` (host shim / guest export / defined) or `missing`, relocs |
| `addrmap.txt` | text addrmap: `F` function (guest entry, host symbol, name, guest insts, chunk), `L` basic-block labels (guest PC to host label), `U` unsupported stubs |
| `hle_report.txt` | copy of the existing HLE report |

Translation also (a) instruments each emitted chunk `.s` with local labels and a
`__DATA,__nps5_pcmap` table, one `{host pc, guest pc|flags<<56, name}` record per guest instruction
(flags: 1 unsupported stub, 2 unsupported indirect jmp/call, 4 function entry), and (b) writes
`nps5_buildinfo.c` into the output dir (git commit, input sha256, addrmap path, translate bundle
path, guest image size), compiled and linked with the runtime. `NPS5_NO_PCMAP=1` skips the pcmap,
`NPS5_NO_BUNDLE=1` disables the bundle. The instrumentation adds labels and data only, no instructions.

## Run bundle (C runtime, `pkg/runtime/core/nps5_diag.[ch]`)
Created by a constructor before `main`. Files: `manifest.json` (written at start, rewritten at exit
with result `ok`/`error`/`crash`), `log.jsonl`, `summary.txt`, and on a crash `crash.txt` + `pcmap.bin`.

**Structured log.** `NPS5_LOG=hle=debug,loader=trace` (a bare level or `*=level` sets all; default `info`).
Subsystems: loader lifter dispatch tier1 hle kernel mem gpu audio pad sys vm runtime. API:
`NPS5_LOG(NPS5_S_HLE, NPS5_L_INFO, "fmt", ...)`. Repeats from one call site are rate-limited
(5 per second, then a coalesced record with `"repeat": N`). Warn and above echo to stderr as
`[subsys] msg`. The old `[ps4-sys]`/`[ps4-vm]`/`[ps4-mem]`/`[ps4-kernel]`/`[ps4-vfs]`/`[ps4-gnm]`
printf lines were converted to `PS4_LOGF(subsys, level, "ps4-xx", fmt, ...)`: same stdout/stderr
text as before, plus a log.jsonl record when the level passes (the converted chatty ones are
`debug`, so `NPS5_LOG=sys=debug,vm=debug` records them).

**HLE counters / trace.** `_recomp_call_shim` (asm_trampoline.go) calls `g_nps5_hle_hook(ctx, shim_fn)` when
non-null (a load and branch otherwise); the hook counts per shim. `NPS5_NO_HLE_COUNT=1` leaves it null.
`NPS5_TRACE_HLE=<glob>` logs matching calls with rdi..r9 (raw hex, not typed yet).

**Crash handler.** SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGABRT, installed at startup and again from
`recomp_install_crash_handler` (chains to the previous handler, so the old stderr report still prints).
`crash.txt` is written through a pre-opened fd with a signal-safe formatter (no stdio, no malloc, no locks)
and contains: signal, reason (memory fault in guest code / UNSUPPORTED stub / guest trap / abort with note),
host PC and image offset, guest RIP + function + offset (host PC looked up in the sorted pcmap; for faults
inside HLE code the innermost guest frame on the frame-pointer chain), fault address classified
(guest arena with guest address, host null page, GPU carveout, host stack, above/below arena), live guest
registers read from the pinned host registers (RAX x19, RCX x21, RDX x22, RBX x20, RSP x26, RBP x25, RSI x23,
RDI x24, R8-R15 x8-x15, FS x27, base x28), NZCV, xmm0-15, fpcr/fpsr, a frame-pointer host backtrace mapped to
guest frames, the HLE call in progress/last. After the core report, `dladdr` symbolises the host PC (this
second pass is not signal-safe; the core report is already on disk) and `pcmap.bin` is dumped so
`nps5 addr2guest` works offline. A second crash during handling exits immediately. SIGABRT and SIGTERM/SIGINT
finalise `summary.txt`/`manifest.json` best effort (2 s alarm bound). `NPS5_NO_TERM_HANDLER=1` disables the
SIGTERM/SIGINT handler.

Call model note (docs/m1/flatstack.md): with `-call-model flat` (default) `guest_call_chain:` is read from the
shadow return stack (live pointer in x29 when the pc is in guest code, `ctx->shadow_sp` otherwise), is also
printed for faults inside guest code, shows `<host boundary>` lines for HLE callbacks, and is followed by a
`shadow_stack: depth= slow_rets= overflows=` line; the frame-pointer backtrace starts at the context's
`recomp_enter_guest` frame because x29 is not a frame pointer in guest code. `summary.txt` gains
`call_model`, `shadow_stacks`, `shadow_slow_rets`, `shadow_overflows`, `dispatch_slow`. `NPS5_SHADOW_STATS=1`
prints the totals to stderr at exit; `NPS5_SHADOW_ENTRIES=<n>` sizes the stacks (default 32768).

Register note: the RAX..RDI mapping is the lifter's: RAX x19, RBX x20, RCX x21, RDX x22, RSI x23, RDI x24, RBP x25,
RSP x26 (matches `recomp_runtime.c`'s legacy dump).

**Unsupported / unresolved hits.** `shim_unresolved_stub` and `recomp_unsupported_instruction` call
`nps5_diag_unresolved_hit` / `nps5_diag_unsupported_hit`: logged once at error level, counted, listed in
`summary.txt`, and the crash report carries a `note:`.

**Exit summary** (`summary.txt`, stderr line only if something went wrong): wall time, faults, crashed flag, HLE
calls total and top 20 by name (shim function names via `dladdr`; the link must keep local symbols, i.e. no
`-Wl,-x`), unresolved import table, unsupported hit.

Env: `NPS5_RUNS_DIR`, `NPS5_TITLE` (default: input file name, else executable), `NPS5_NO_BUNDLE`,
`NPS5_LOG`, `NPS5_TRACE_HLE`, `NPS5_NO_HLE_COUNT`, `NPS5_DUMP_PCMAP=1` (write pcmap.bin at exit),
`NPS5_NO_TERM_HANDLER`, `NPS5_NO_PCMAP` (translate).

## CLI helpers (`ps4-recomp <cmd>`)
- `doctor`: clang, go/ninja/git versions, host model/OS build, arena probe (compiles and runs a tiny arm64
  program that `mach_vm_allocate`s 0x70_0000_0000 + 0x8C_0000_0000 FIXED), Rosetta (`arch -x86_64 /usr/bin/true`),
  KosmicKrisp ICD under `work/s3/install` (and whether `VK_ICD_FILENAMES` selects it). Exit status 1 only
  when a required check fails.
- `last [bundle]`: manifest summary, log level counts, census/imports one-liners, `summary.txt`, `crash.txt`.
- `addr2guest [-bundle dir] [-addrmap file] <host_pc | +image_offset>`: uses the bundle's `pcmap.bin`.

## Building and running (dev loop)
```
GOFLAGS=-mod=vendor go build -o out/ps4-recomp ./cmd/ps4-recomp
tools/build_ps5.sh <elf> <name>        # translate (bundle in runs/) + link via tools/build_manual.sh
tools/run_sandboxed.sh 10 build/<name>/native    # env -i, HOME/TMPDIR in ./home, bundles in ./runs
tools/diag_acceptance.sh               # hello_world, ps5_hello_k, four broken programs (tests/diag/crash_tests.c)
out/ps4-recomp last
```
`tools/build_manual.sh` is S2's helper (stubs GPU/Ajm modules the missing submodules would provide).

## Known gaps
- Reconstruction records (SPEC 3.10) are not emitted; guest PC is exact per guest instruction but
  register values are only those pinned in host registers (flags: raw NZCV, carry polarity unknown;
  PF/AF absent; YMM uppers, x87 not dumped).
- No per-thread ring buffer of guest branch targets; tier-1 miss log, arena map dump, GPU toggles and
  `tracediff` are not implemented. HLE trace prints raw register args, not typed args or return values.
- Uncaught C++ exceptions are only seen as SIGABRT (no `std::set_terminate` hook, no exception text).
- `imports.json` has no `stub` status yet (bound vs missing only). Exit code of the guest is not captured on
  normal exit paths (only via SIGTERM).
- Crash handler uses the faulting thread's stack (the alt stack is set up for the thread that installs it only).
- `addrmap.bin` binary form is `pcmap.bin` (dumped at crash/exit, host offsets relative to the image base).
- A guest null dereference does not fault today: the guest image is mapped at guest address 0. The acceptance
  memory fault uses an address beyond the arena. Expect a null-page guard from the loader/L2 work.
