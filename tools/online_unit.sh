#!/bin/bash
# Build and run the host unit test of the offline online/system stubs (tests/online/online_unit.c).
# Native arm64 only; no guest/x86 code is executed.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
OUT="${TMPDIR:-/tmp}/online_unit.$$"
clang -O1 -g -Wall -Wno-unused-function -I"$R/core" -I"$R/modules/libSceNpOnlineStubs" \
  "$ROOT/tests/online/online_unit.c" "$R"/modules/libSceNpOnlineStubs/*.c "$R"/modules/libSceNpCppWebApi/*.c -o "$OUT"
"$OUT"; rc=$?; rm -f "$OUT"; exit $rc
