#!/bin/bash
# Host unit test of the NPS5_PTHREAD_EXIT_SCE routing (core/nps5_pthread_exit.h). Native arm64, no guest code.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="${TMPDIR:-/tmp}/pthread_exit_unit.$$"
clang -O1 -g -Wall -I"$ROOT/pkg/runtime/core" "$ROOT/tests/threading/pthread_exit_unit.c" -o "$T" -lpthread
"$T"; rc=$?; rm -f "$T"; exit $rc
