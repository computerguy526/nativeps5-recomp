#!/bin/bash
# NPS5_TITLE_GATE: while the PPSA30552 title runs (6-10 GB, GPU), other GPU runs wait (max 5 min) so two windowed GPU loads never overlap.
case "${NPS5_CPU_ONLY:-0}:$*" in 1:*|*/tests/fex/*|*/tests/ps5/build/*) ;; *ppsa30552*) _L=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/guard/title.lock; _ok=0; for _i in $(seq 1 120); do if mkdir "$_L" 2>/dev/null; then echo $$ > "$_L/pid"; trap 'rm -rf "$_L"' EXIT; _ok=1; break; fi; _p=$(cat "$_L/pid" 2>/dev/null); if [ -n "$_p" ] && ! kill -0 "$_p" 2>/dev/null; then rm -rf "$_L"; continue; fi; [ $_i = 1 ] && echo "[run_sandboxed] title lock held by pid $_p; waiting" >&2; sleep 5; done; [ $_ok = 1 ] || { echo "[run_sandboxed] title lock not acquired in 10 min; refusing to run a second title" >&2; exit 75; };; *) for _i in $(seq 1 60); do pgrep -qf "ppsa30552/.*/native" || break; [ $_i = 1 ] && echo "[run_sandboxed] waiting: PPSA30552 title run active" >&2; sleep 5; done;; esac
# usage: run_sandboxed.sh <secs> <binary> [args]
# Runs a recompiled arm64 program with a scrubbed env; HOME/TMPDIR confined to <worktree>/home,
# runs/ bundles go to <worktree>/runs, no dialogs. Never use this for x86 binaries.
W="${NPS5_WORKDIR:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"; T=$1; shift
mkdir -p $W/home $W/runs
env -i PATH=/usr/bin:/bin HOME=$W/home TMPDIR=$W/home NPS5_RUNS_DIR=$W/runs ${NPS5_TEST_AUDIO_ENV-NPS5_AUDIO_NULL=1} ${NPS5_EXTRA_ENV:-} PS4_RECOMP_HEADLESS=1 PS4_NO_DIALOGS=1 PS4_LANG=en PS4RECOMP_DATA_DIR=$W/home/data PS4RECOMP_CACHE_DIR=$W/home/cache /opt/homebrew/bin/gtimeout $T "$@" </dev/null
