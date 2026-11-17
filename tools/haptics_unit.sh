#!/bin/bash
# Host test of the controller-haptics logic (tests/haptics/haptics_unit.c): no hardware, mock sinks.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O1 -Wall -Wextra "$ROOT/tests/haptics/haptics_unit.c" -lm -lpthread -o "$T/hap" && "$T/hap"
