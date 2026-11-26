#!/bin/bash
# Host unit test of the UE5 shader library reader (tests/ue5code/ue5code_unit.c): synthetic IoStore containers (plain, split
# into partitions, gamepack .nps5seg packed) read through a test file layer and the real VFS, a stub codec as the decoder.
# The guest decoder binding (nps5_ue5code.c) runs against a stand-in guest. No game bytes. Native arm64 only. Subsystems that are not linked get zero-returning stubs (as tools/segfile_unit.sh).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/ue5code_unit.$$"
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="${UE5_SAN:-} -O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC"
SRC="tests/ue5code/ue5code_unit.c $R/core/nps5_ue5core.c $R/core/nps5_ue5vfs.c $R/core/nps5_ue5code.c $R/kernel/vfs/nps5_segfile.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/ps4_pak.c $R/kernel/syscalls/ps4_syscalls.c $R/kernel/aio/ps4_aio.c $R/modules/libSceAmpr/nps5_ampr.c $R/kernel/equeue/ps4_equeue.c $R/core/ps4_sfo.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then "$OUT"; rc=$?; fi
# The developer probes (decoded-code dump, self-test) must not be in a normal build, and must be in a -DNPS5_UE5_DEV_PROBES one
# (so the check cannot pass for the wrong reason).
if [ $rc -eq 0 ]; then
  if grep -aq NPS5_UE5_EXTRACT_TEST "$OUT"; then echo "FAIL: the release build contains the decoded-code dump (NPS5_UE5_EXTRACT_TEST)"; rc=1; fi
  clang $FL -DNPS5_UE5_DEV_PROBES -c $R/core/nps5_ue5code.c -o "$OUT.dev.o" 2> /dev/null
  grep -aq NPS5_UE5_EXTRACT_TEST "$OUT.dev.o" || { echo "FAIL: the developer build lost the probes"; rc=1; }
  rm -f "$OUT.dev.o"
fi
[ -z "${UE5_KEEP:-}" ] || cp "$OUT" "$UE5_KEEP" # debugging: keep the test binary
rm -f "$OUT" "$OUT.err" "$OUT.err2" "$OUT.stubs.c"
exit $rc
