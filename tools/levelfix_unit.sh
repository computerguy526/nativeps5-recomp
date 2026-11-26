#!/bin/bash
# Host unit test of the read-time level fix (pkg/runtime/kernel/vfs/nps5_levelfix.h, tests/levelfix/levelfix_unit.c):
# synthetic data + fake file layer. Header-only, no runtime linked. Also checks ps4_vfs.c still compiles with the hook.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/levelfix_unit.$$"
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
clang -O1 -g -Wall -Wno-unused-function $INC tests/levelfix/levelfix_unit.c -o "$OUT" || exit 1
"$OUT"; rc=$?
clang -O1 -Wno-everything -DRECOMP_ARM64_ASM=1 $INC -c $R/kernel/vfs/ps4_vfs.c -o "$OUT.o" || rc=1
rm -f "$OUT" "$OUT.o"; rm -rf "$OUT.dSYM"; exit $rc
