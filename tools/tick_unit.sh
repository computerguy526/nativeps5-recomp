#!/bin/bash
# Host test of the F2 tick-split plan (tests/tick/tick_unit.c).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O1 -Wall "$ROOT/tests/tick/tick_unit.c" -lm -o "$T/tk" && "$T/tk"
