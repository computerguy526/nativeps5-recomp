# CI

Workflow: `.github/workflows/ci.yml`, on every pull request and push.

- **privacy and identity gate**: no game data, secrets, personal paths or non-noreply commit e-mails. Never relaxed.
- **workflow syntax (actionlint)**: pinned actionlint 1.7.7, checksum verified.
- **format/lint** and **tests**: gofmt, vet, `go test` (GOFLAGS=-mod=vendor), clang-format on changed lines, C unit tests `tools/*_unit.sh`.
- Every job has `timeout-minutes`, pinned tool versions and actions (by SHA), and retries (3x, backoff) around network installs (pip, brew, curl).
- The macOS job runs on `macos-15-intel` with `continue-on-error: true`: GitHub's macOS arm64 pool is capacity-starved (jobs were cancelled after 15 min in the queue), so a miss there shows as a warning and does not fail the PR. Rerun it from the Actions tab ("Re-run failed jobs"). `.github/actionlint.yaml` teaches actionlint that label.
- Tests that need game data, a GPU or local paths are skipped with a stated reason, not run on CI.

Rerun: Actions tab, open the run, "Re-run failed jobs"; or `gh run rerun <run-id> --failed`.
Run locally: `go test ./...` and `bash tools/<name>_unit.sh`.
- The C unit tests build the arm64 runtime, so on the Intel macOS runner the job reports a notice and skips (green); they run for real with `scripts/ci/run-local.sh` on Apple silicon.

Run the whole workflow locally before every push: `scripts/ci/run-local.sh` (commit first; clean `git worktree` of HEAD, no submodules, fresh venv, same `run:` steps as the workflow; `--job <id>`, `--x86` for the macOS job under Rosetta, `--act` for ubuntu jobs in nektos/act). Needs ruby, git and the job tools (shellcheck, go, cmake, ninja, actionlint as the job needs).
