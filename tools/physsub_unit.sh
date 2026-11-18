#!/bin/bash
# Host test of the [physsub] decision logic (tests/physsub/physsub_unit.c).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O1 -Wall "$ROOT/tests/physsub/physsub_unit.c" -lm -o "$T/ps" && "$T/ps"
