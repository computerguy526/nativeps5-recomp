#!/bin/bash
# Host test of the AvPlayer frame selection (tests/avsync/avsync_unit.cpp).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang++ -std=c++17 -O1 -Wall "$ROOT/tests/avsync/avsync_unit.cpp" -o "$T/av" && "$T/av"
