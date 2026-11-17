#!/bin/bash
# Ordering litmus tests through an escaped stack object (docs/m3/tsoelide.md):
# tests/ps5/tsoelide/t_tsolitmus.c is built with the PS5 payload SDK, translated once per
# -tso-elide mode, linked (tools/m1_native_build.sh) and run RUNS times in the sandbox.
#
#   tools/tsoelide_test.sh [--runs N] [--timeout SECS] [--modes "off stack stack-region"]
#
# Verdict: with -tso-elide stack and stack-region every *_violations line must be 0 in every run (exit 1
# otherwise). With -tso-elide off the counts are reported only: that mode emits the owner's
# RSP/RBP-relative accesses without ordering, so outcomes x86 forbids are possible there.
# Nothing but the arm64 output of this repo's translator is executed (env -i, timeout).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests/ps5"; B="$T/build/tsoelide"; HOME_DIR="$T/build/home"
TMO=/opt/homebrew/bin/gtimeout
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
runs=5; timeout=120; modes="off stack stack-region"
while [ $# -gt 0 ]; do case "$1" in
  --runs) runs="$2"; shift ;;
  --timeout) timeout="$2"; shift ;;
  --modes) modes="$2"; shift ;;
  -h|--help) sed -n 2,12p "$0"; exit 0 ;;
  *) echo "unknown argument $1"; exit 2 ;; esac; shift; done
mkdir -p "$B" "$HOME_DIR"
export NPS5_JOBS="${NPS5_JOBS:-3}"

if [ -n "${PS4RECOMP_BIN:-}" ]; then BIN="$PS4RECOMP_BIN"; else
  BIN="$B/ps4-recomp"
  ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || { echo "translator build failed"; exit 2; }
fi
ELF="$B/t_tsolitmus.elf"
PATH="/opt/homebrew/opt/llvm/bin:$PATH" "$SDK/bin/prospero-clang" -O2 -std=gnu11 -ffp-contract=off -Wno-everything -pthread \
  "$T/tsoelide/t_tsolitmus.c" -o "$ELF" 2> "$ELF.log" || { echo "ELF build failed, see $ELF.log"; exit 2; }

fail=0
for mode in $modes; do
  d="$B/$mode"; rm -rf "$d"; mkdir -p "$d"
  "$BIN" -compile=false -tso-elide "$mode" -o "$d/out" "$ELF" > "$d/translate.log" 2>&1 || { echo "$mode: translation failed, see $d/translate.log"; fail=1; continue; }
  grep -q "\"tso_elide\": \"$mode\"" "$d/out/lifter_manifest.json" || { echo "$mode: lifter_manifest.json does not record the mode"; fail=1; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "$mode: build failed, see $d/build.log"; fail=1; continue; }
  total=0; bad_runs=0
  for i in $(seq 1 "$runs"); do
    rm -rf "$d/out/runs"
    ( cd "$d/out" && env -i PATH=/usr/bin:/bin HOME="$HOME_DIR" TMPDIR="$HOME_DIR" PS4_RECOMP_HEADLESS=1 PS4_NO_DIALOGS=1 PS4_LANG=en \
        PS4RECOMP_DATA_DIR="$HOME_DIR/data" PS4RECOMP_CACHE_DIR="$HOME_DIR/cache" NPS5_AUDIO_NULL=1 \
        "$TMO" -s KILL "$timeout" "$d/native" </dev/null ) > "$d/stdout.$i.txt" 2> "$d/stderr.$i.txt"; rc=$?
    n=$(grep -c '_violations=' "$d/stdout.$i.txt")
    if [ "$rc" -ne 0 ] || [ "$n" -ne 3 ]; then echo "$mode run $i: rc=$rc, $n result lines (see $d/stderr.$i.txt)"; fail=1; bad_runs=$((bad_runs+1)); continue; fi
    v=$(grep '_violations=' "$d/stdout.$i.txt" | awk -F= '{s+=$2} END {print s+0}')
    total=$((total+v))
    printf '%-5s run %d: %s\n' "$mode" "$i" "$(grep -E '_violations=|_helper_' "$d/stdout.$i.txt" | tr '\n' ' ')"
  done
  if [ "$mode" != off ]; then
    if [ "$total" -eq 0 ] && [ "$bad_runs" -eq 0 ]; then echo "$mode: PASS (0 forbidden outcomes in $runs runs)"; else echo "$mode: FAIL ($total forbidden outcomes)"; fail=1; fi
  else
    echo "$mode: $total forbidden outcomes in $runs runs (reported, not a verdict)"
  fi
done
exit $fail
