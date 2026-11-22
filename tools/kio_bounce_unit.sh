#!/bin/bash
# Host unit test: VFS reads into guest memory whose kernel copy keeps failing with EFAULT must not come back short
# (tests/kio/kio_bounce_unit.c). Runs twice: the fix on (whole file, right bytes, file position) and NPS5_KIO_BOUNCE=0
# (the old short read, so the test is known to catch it). Native arm64 only; files live under a temp dir.
# Subsystems of ps4_vfs.c that are not linked get zero-returning stubs (as tools/kernelfs_unit.sh).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/kio_bounce_unit.$$"
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="-O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC"
SRC="tests/kio/kio_bounce_unit.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/nps5_segfile.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then "$OUT" && "$OUT" off; rc=$?; fi
rm -rf "$OUT" "$OUT".*; exit $rc
