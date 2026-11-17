#!/bin/bash
# Build and run the host unit test of pkg/runtime/core/nps5_hostcall.c (tests/hostcall/hostcall_unit.c):
# a host library calling a guest function pointer. Native arm64 only; no guest/x86 code is executed.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
OUT="${TMPDIR:-/tmp}/hostcall_unit.$$"
clang -O1 -g -Wall -Wno-unused-function -I"$R/core" "$ROOT/tests/hostcall/hostcall_unit.c" "$R/core/nps5_hostcall.c" -o "$OUT"
"$OUT"; rc=$?; rm -f "$OUT"; exit $rc
