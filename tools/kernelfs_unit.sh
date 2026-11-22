#!/bin/bash
# Host unit test of the libkernel file family SCE error codes, the pak locale-fallback gate and the
# PS5 param.json reader (tests/kernelfs/kernelfs_unit.c). Native arm64 only; no guest/x86 code runs;
# files live under a temp dir. Subsystems that are not linked get zero-returning stubs.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
R=pkg/runtime
OUT="${TMPDIR:-/tmp}/kernelfs_unit.$$"
# Lint: the guest-facing file shims must not call a host mutator directly. Every path-taking mutating call goes through the
# ps4_vfs_* write gate (ps4_vfs.h), so that no shim can bypass the read-only /app0 guard. A line may opt out with "vfs-ok".
BAD=$(grep -nE '\b(chmod|fchmod|lchmod|chown|fchown|lchown|utimes|futimes|lutimes|utimensat|futimens|unlink|unlinkat|rmdir|mkdir|mkdirat|rename|renameat|truncate|ftruncate|link|linkat|symlink|symlinkat|setxattr|removexattr|fsetxattr|fremovexattr|chflags|fchflags|mkfifo|mknod|remove|fopen|freopen|creat|open|openat|exchangedata|copyfile|clonefile)\(' \
  $R/kernel/syscalls/ps4_syscalls.c $R/modules/libc/*.c | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -v 'vfs-ok')
if [ -n "$BAD" ]; then echo "kernelfs_unit: shim calls a host mutator directly (use the ps4_vfs_* gate):"; echo "$BAD"; exit 1; fi
INC=$(find $R -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="-O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 $INC"
# the pak is included into the test TU's helper so the test can insert an index entry without a .pak file
cat > "$OUT.pakhelper.c" <<EOF
#include "$ROOT/$R/kernel/vfs/ps4_pak.c"
void nps5_test_pak_insert(const char *path) { hash_table_insert_file(fnv1a_64_lower(path), -1, 0, 1, 0); g_entry_count = g_entry_count ? g_entry_count : 1; }
EOF
SRC="tests/kernelfs/kernelfs_unit.c $OUT.pakhelper.c $R/kernel/syscalls/ps4_syscalls.c $R/kernel/vfs/ps4_vfs.c $R/kernel/vfs/nps5_segfile.c $R/core/ps4_sfo.c $R/modules/libSceAppContent/ps4_app_content.c"
clang $FL $SRC -o "$OUT" 2> "$OUT.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$OUT.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$OUT.stubs.c"
  if [ -s "$OUT.stubs.c" ]; then clang $FL $SRC "$OUT.stubs.c" -o "$OUT" 2> "$OUT.err2"; rc=$?; else cp "$OUT.err" "$OUT.err2"; fi
  [ $rc -ne 0 ] && head -30 "$OUT.err2"
fi
if [ $rc -eq 0 ]; then "$OUT"; rc=$?; fi
rm -rf "$OUT" "$OUT".*; exit $rc
