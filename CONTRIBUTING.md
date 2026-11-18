# Contributing

This repository is a component of [nativeps5](https://github.com/computerguy526/nativeps5). The contribution rules (never any
game data, one change per PR, evidence format, run rules) are in that repository's `CONTRIBUTING.md` and `AGENTS.md`; read them first.

## CI

Before pushing, run `scripts/ci/run-local.sh` (commit first): it runs every job of `.github/workflows/ci.yml` from a clean checkout of HEAD (same steps, fresh venv, no submodules); `--job <id>` runs one, `--x86` runs the macOS job under Rosetta like the Intel hosted runner. See docs/ci.md.

GitHub Actions (`.github/workflows/ci.yml`) runs on every pull request and push to `main`, with no secrets, game data or GPU.
Each check is one command locally (from the repository root):

| Job | What | Locally |
|---|---|---|
| go | `gofmt` on the Go files the change touches, `go vet`, `go test` | `export GOFLAGS=-mod=vendor; gofmt -l <your .go files>; go vet ./... && go test ./pkg/cli ./pkg/diag ./pkg/disasm ./pkg/elfloader ./pkg/thunkgen` |
| unit | the standalone C host tests, GitHub macOS runner (macos-15-intel: the arm64 pool is capacity-starved) | `bash tools/kernelfs_unit.sh` (one per `tools/*_unit.sh`) |
| clang-format | C/C++ lines the change touches (`.clang-format`); the existing code is never reformatted | `git clang-format origin/main -- pkg tests cmd` (or `--diff` to look) |
| privacy | game data, secrets, personal paths, commit e-mails (a copy of the entry repository's gate, `.github/nps5-ci/`) | `BASE=origin/main TIP=HEAD .github/nps5-ci/privacy.sh` |
| workflows | workflow syntax | `actionlint` |

Not in CI: the regression gates, title runs, `tools/ajm_unit.sh` (links the minimp3 and libatrac9 submodules; run it in a checkout with `3rdparty/` initialised), the full
`pkg/lifter`, `pkg/runtime` and `pkg/emitter` test suites (minutes; run `go test ./pkg/lifter ./pkg/runtime ./pkg/emitter` yourself when you touch them). A PR needs green CI; it does not replace the evidence.
