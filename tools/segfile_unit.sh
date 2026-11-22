#!/bin/bash
# Host unit test of gamepack segmented files (tests/segfile/segfile_unit.c): every guest read path of ps4_vfs, readv/preadv,
# aio, AMPR and directory listings over a <file>.nps5seg/ directory must read exactly like the original file. Runs with
# segments written by the test, then with NPS5_VFS_SEGFILES=0 (not visible). With GAMEPACK=<path to gamepack.py> it also
# packs a file with the real tool (tiny segments, APFS compression where the tool can) and runs the test on that.
# Native arm64 only; files live under a temp dir. Subsystems that are not linked get zero-returning stubs.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/segfile_unit.$$"
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="-O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC"
SRC="tests/segfile/segfile_unit.c $R/kernel/vfs/nps5_segfile.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/ps4_pak.c $R/kernel/syscalls/ps4_syscalls.c $R/kernel/aio/ps4_aio.c $R/modules/libSceAmpr/nps5_ampr.c $R/kernel/equeue/ps4_equeue.c $R/core/ps4_sfo.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then SEGFILE_TMP="${TMPDIR:-/tmp}" "$OUT" && SEGFILE_TMP="${TMPDIR:-/tmp}" "$OUT" off; rc=$?; fi
if [ $rc -eq 0 ] && [ -n "${GAMEPACK:-}" ]; then
  # the real tool: pack, run the reader test on the packed tree, then unpack and compare bit for bit
  T=$(mktemp -d "${TMPDIR:-/tmp}/segfile_gp.XXXXXX"); mkdir -p "$T/app/Paks" "$T/app.orig"
  head -c 3000017 /dev/urandom > "$T/app.orig/Big-ps5.ucas"
  cp "$T/app.orig/Big-ps5.ucas" "$T/app/Paks/Big-ps5.ucas"
  python3 -I "$GAMEPACK" pack "$T/app/Paks/Big-ps5.ucas" --seg 65537 --min-size 0 --whole-max 0 --margin 0 --ratio-skip 2 --no-game-check > "$T/pack.log" 2>&1 \
    || { cat "$T/pack.log"; rc=1; }
  if [ $rc -eq 0 ]; then
    [ -e "$T/app/Paks/Big-ps5.ucas" ] && { echo "FAIL: gamepack left the original"; rc=1; }
    SEGFILE_TMP="${TMPDIR:-/tmp}" NPS5_VFS_SEGFILES_TRACE=200 "$OUT" dir "$T/app" 2> "$T/trace.err" || rc=1
    grep -c '^\[vfs\] segread' "$T/trace.err" | sed 's/^/segfile: trace lines: /'
    python3 -I "$GAMEPACK" checktrace "$T/trace.err" || rc=1
    python3 -I "$GAMEPACK" verify "$T/app/Paks/Big-ps5.ucas" --no-game-check || rc=1
    python3 -I "$GAMEPACK" unpack "$T/app/Paks/Big-ps5.ucas" --no-game-check > "$T/unpack.log" 2>&1 || { cat "$T/unpack.log"; rc=1; }
    cmp "$T/app/Paks/Big-ps5.ucas" "$T/app.orig/Big-ps5.ucas" && echo "segfile: gamepack pack -> VFS read -> unpack round trip bit-exact" || rc=1
  fi
  rm -rf "$T"
fi
rm -rf "$OUT" "$OUT".*; exit $rc
