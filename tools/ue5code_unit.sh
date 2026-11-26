#!/bin/bash
# Host unit test of the UE5 shader library reader (tests/ue5code/ue5code_unit.c): synthetic IoStore containers (plain, split
# into partitions, gamepack .nps5seg packed) read through a test file layer and the real VFS, a stub codec as the decoder.
# No game bytes. Native arm64 only. Subsystems that are not linked get zero-returning stubs (as tools/segfile_unit.sh).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/ue5code_unit.$$"
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="${UE5_SAN:-} -O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC"
SRC="tests/ue5code/ue5code_unit.c $R/core/nps5_ue5core.c $R/core/nps5_ue5vfs.c $R/kernel/vfs/nps5_segfile.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/ps4_pak.c $R/kernel/syscalls/ps4_syscalls.c $R/kernel/aio/ps4_aio.c $R/modules/libSceAmpr/nps5_ampr.c $R/kernel/equeue/ps4_equeue.c $R/core/ps4_sfo.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then "$OUT"; rc=$?; fi
rm -f "$OUT" "$OUT.err" "$OUT.err2" "$OUT.stubs.c"
exit $rc
