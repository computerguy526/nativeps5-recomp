#!/bin/bash
# Host unit test of the adaptive yield back-off (core/nps5_yield.h). Native arm64, no guest code.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="${TMPDIR:-/tmp}/yield_unit.$$"
clang -O1 -g -Wall -I"$ROOT/pkg/runtime/core" "$ROOT/tests/threading/yield_unit.c" -o "$T"
"$T"; rc=$?; rm -f "$T"; exit $rc
