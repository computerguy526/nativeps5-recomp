#!/bin/bash
# Host test of the [framerate] decision logic (tests/framerate/framerate_unit.c).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O1 -Wall "$ROOT/tests/framerate/framerate_unit.c" -lm -o "$T/fr" && "$T/fr"
