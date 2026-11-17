#!/bin/bash
# Build and run tests/hlesys/ajm_unit.c: the Ajm batch builder and worker over the real minimp3
# decoder (3rdparty submodules must be initialised). Native arm64 only; no guest code runs.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
OUT="${TMPDIR:-/tmp}/ajm_unit.$$"
INC="-I$R/core -I$R/kernel/syscalls -I$R/kernel/memory -I$R/kernel/threading -I$R/kernel/sync -I$R/modules/libSceAjm -I$ROOT/3rdparty/minimp3 -I$ROOT/3rdparty/libatrac9/C/src"
clang -O1 -g -Wall -Wno-unused-function -Wno-unused-variable $INC \
  "$ROOT/tests/hlesys/ajm_unit.c" "$R/modules/libSceAjm/ps4_ajm.c" "$ROOT"/3rdparty/libatrac9/C/src/*.c -lm -o "$OUT"
"$OUT"; rc=$?; rm -f "$OUT"; exit $rc
