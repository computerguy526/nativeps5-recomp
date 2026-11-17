# Notes for agents

nativeps5-recomp is the translator and runtime of nativeps5 (the `ps5recomp` component, a fork of PS4Recomp): it
statically lifts PS5 x86-64 code to arm64 and emits C that is built into a native macOS program, linked with
the system libraries of nativeps5-anyps5 or the GPU library of nativeps5-kyty. It is not an emulator and must not
become a JIT.

The project-wide rules are in https://github.com/computerguy526/nativeps5 [AGENTS.md](https://github.com/computerguy526/nativeps5/blob/main/AGENTS.md). Read it first. In short: the game-data rule (never game data or anything derived from a title in a commit, issue, PR or prompt; derived files live under `NPS5_SCRATCH`); the run safety rules (one GPU-using process on the machine, title runs only through `scripts/run-title.sh` of the entry repository, never raise `NPS5_SELF_MEM_LIMIT_MB`, no Metal counter heaps or GPU timestamp queries, no screenshots, kill only what you started); evidence for every speed or picture claim (`nps5 report`, A/B with interleaved runs, scene named, commits of every component); and git: the identity comes from git's configuration (never `--author`, `-c user.*` or `--no-verify`), no pushes to an upstream, and the pull request template states whether AI assisted.

## Upstream rules (still apply)

These are PS4Recomp's own rules; they hold for every change here:

- No cheating: no false tests, no passing a test by weakening what it checks, no running Python in an isolated
  directory to fake a result. Be strict, and tell the user the truth even when it is unwelcome.
- Not a JIT emulator. Do not build it so that it only fits one binary.
- No temporary solutions and no stubs that return 0: do it properly the first time. Do not do the whole project in
  one step, but do not take a step on a microtask that is not worth it.
- Comments, README and documentation in English only.
- Run the Go analysis tools (`go vet`, and what the change calls for) for purity of the code and the minimum number
  of allocations.
- If you need to download something, ask the user.

Where nativeps5 differs: the host is macOS arm64, the pull request template is ours, and evidence for a claim is
`nps5 report` output rather than prose.

## Where things live

| Path | What |
|---|---|
| `cmd/ps4-recomp`, `cmd/nps5` | the translator (`doctor` and the translate commands) and the `nps5` driver (`build`, `list`, `play`, `launcher`; `run`, `report` and the rest are in the entry repository's `scripts/nps5`); other `cmd/*` are analysis tools |
| `pkg/elfloader`, `pkg/disasm`, `pkg/analyzer` | loading a SELF/ELF, x86 decoding, function and call-graph analysis |
| `pkg/lifter`, `pkg/emitter`, `pkg/tcache` | the arm64 lifter, the C emitter (patch points: `pkg/emitter/patchpoints.go`), translation cache |
| `pkg/runtime` | the C runtime linked into every translated program (memory, threads, kernel and libc emulation, tier-1 miss path in `core/nps5_tier1.c`) |
| `pkg/diag` | run bundle, crash report, environment subset; `tests/diag/diag_unit.c` covers path redaction |
| `pkg/thunkgen`, `pkg/guestmem`, `pkg/pack` | import thunks, guest memory, packaging |
| `profiles/` | per-title translation profiles and patch lists (`profiles/patches/<title>.json`) |
| `tests/`, `tools/` | C and shell unit tests (`tools/*_unit.sh`), `tools/regress.sh` regression gate, benchmarks |
| `docs/m0`..`docs/m3`, `docs/SPEC.md` | design notes by milestone; read the one for the area before changing it |

## Build and test

```sh
GOFLAGS=-mod=vendor go build -o bin/ps4-recomp ./cmd/ps4-recomp   # modules are vendored; go.mod asks for the Go version the build prints
GOFLAGS=-mod=vendor go build -o bin/nps5 ./cmd/nps5               # or scripts/build.sh recomp in the entry repository
go test ./pkg/diag/ ./pkg/elfloader/ ./pkg/disasm/ ./pkg/cli/ ./pkg/thunkgen/   # quick; go test ./pkg/... is the full set (minutes)
bash tools/diag_unit.sh; bash tools/kernelfs_unit.sh; bash tools/hlesys_unit.sh; bash tools/framerate_unit.sh; bash tools/haptics_unit.sh; bash tools/avsync_unit.sh   # C unit tests, CPU only
tools/regress.sh --quick      # calc3d frame 0 exact, syobon, greenhollow; use a private REGRESS_DIR
```

`.nps5-check-tests` is the exact list the checker runs. Tests that need a payload build it from the C source next
to them when `PS5_SDK_DIR` is set and skip otherwise; `NPS5_TEST_HELLO_ELF` names upstream's sample ELF. Test
payloads are never committed.

## Rules specific to this repository

- New behaviour is off by default behind an `NPS5_*` environment switch unless proven neutral or better; list the
  switch in the entry repository's `docs/env_switches.tsv` (`scripts/gen_env_switches.py` there).
- A patch point (`profiles/patches/<title>.json`) must carry `func`, `func_size` and `func_sha256` of the image
  bytes it patches; the emitter refuses a site without them, so a patch cannot silently land on another build.
  Describe the patch (address, bytes before and after, game build); never commit patched or original binaries.
- The tier-1 miss fallback (a guest transfer to an address with no dispatch entry, `docs/m1/tier1.md`) spawns the
  translator `fragment` command against the build tree's `lifter_manifest.json`. Keep that file's format and the
  runtime that reads it in step; `NPS5_MISS=fatal` is the developer setting that fails instead.
- The crash report and run bundle redact the home folder from every path they write
  (`nps5_diag_redact_path_home`). A new field in the bundle gets the same treatment and a test in
  `tests/diag/diag_unit.c`.
- `lowering_src.go` only embeds the lowering sources so `emitter.LoweringID` names what the binary was built
  from; do not put logic there. Changing lifter or emitter output changes that id and invalidates translation caches.
- Anything the guest can read (a clock, a counter, a statistic) is an input to the game: do not fake it without
  knowing what the game does with it. No special-casing a value seen in one title's trace.
- Do not commit anything translated from a title; `3rdparty/` holds third-party code with its licence (`NOTICE`).

Pull requests use `.github/pull_request_template.md` (it points at the entry repository's CONTRIBUTING.md; fill the evidence block with `nps5 report`, see docs/agents/evidence.md there). The entry repository's `scripts/check.sh --tree`, run from inside this checkout, must report 0 failures; this repository's `.nps5-check-tests` lists the CPU-only tests it runs.
