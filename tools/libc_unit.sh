#!/bin/bash
# Build and run the host unit test of the HLE libc shims (tests/libc/libc_unit.c). Native arm64 only;
# no guest/x86 code is executed. The real VFS (ps4_vfs.c, with the pak and SFO readers) is linked in so
# that fopen/remove are tested against the real read-only /app0 guard; subsystems that are not linked get
# zero-returning stubs (generated from the linker errors, as in kernelfs_unit.sh).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
OUT="${TMPDIR:-/tmp}/libc_unit.$$"
INC=$(find "$R" -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="-O1 -g -Wall -Wno-unused-function $INC"
SRC="$ROOT/tests/libc/libc_unit.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/nps5_segfile.c $R/kernel/vfs/ps4_pak.c $R/core/ps4_sfo.c $R/modules/libc/*.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL -Wno-everything $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then "$OUT"; rc=$?; fi
rm -rf "$OUT" "$OUT".*; exit $rc
