#!/bin/bash
# Host-only unit test of the crash handler and run-bundle path redaction (tests/diag/diag_unit.c). Links
# pkg/runtime/core/nps5_diag.c with stubs; no game, no guest code, no GPU. Prints "diag_unit: all passed" on success.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
OUT="${TMPDIR:-/tmp}/diag_unit.$$"
mkdir -p "$OUT"
clang -O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC tests/diag/diag_unit.c $R/core/nps5_diag.c -o "$OUT/diag_unit" || { rm -rf "$OUT"; exit 1; }
NPS5_RUNS_DIR="$OUT/runs" NPS5_AUDIO_NULL=1 "$OUT/diag_unit"; rc=$?
rm -rf "$OUT"; exit $rc
