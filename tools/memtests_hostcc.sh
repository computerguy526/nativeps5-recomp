#!/bin/bash
# usage: memtests_hostcc.sh <out> <mode:base|identity> <extra clang flags and sources...>
# Builds a host test against the real runtime sources (core + kernel memory/syscalls + generated
# libkernel thunks). Undefined symbols from subsystems that are not linked (video, vfs, gnm ...)
# get zero-returning stubs, as tools/build_manual.sh does. Native arm64 only.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
OUT="$1"; MODE="$2"; shift 2
INC=$(find pkg/runtime -type d | sed 's/^/-I/' | tr '\n' ' ')
FL="-O1 -g -Wno-everything -DRECOMP_ARM64_ASM=1 -DNPS5_ADDR_MODE_IDENTITY=$([ "$MODE" = identity ] && echo 1 || echo 0) -Itests/memtests $INC"
RT="pkg/runtime/core/recomp_runtime.c pkg/runtime/core/nps5_mem.c pkg/runtime/core/nps5_vm.c pkg/runtime/core/nps5_diag.c pkg/runtime/core/nps5_shadow.c pkg/runtime/core/ps4_log.c pkg/runtime/kernel/memory/ps4_direct_mem.c pkg/runtime/kernel/syscalls/ps4_syscalls.c pkg/runtime/modules/libkernel/nps5_thunks_sys.c pkg/runtime/modules/libkernel/nps5_thunks_mem.c"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/memtests.XXXXXX")"
clang $FL "$@" $RT -o "$OUT" 2> "$TMP/link.err"; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' "$TMP/link.err" | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > "$TMP/stubs.c"
  if [ -s "$TMP/stubs.c" ]; then clang $FL "$@" $RT "$TMP/stubs.c" -o "$OUT" 2> "$TMP/link2.err"; rc=$?; else cp "$TMP/link.err" "$TMP/link2.err"; fi
  [ $rc -ne 0 ] && head -30 "$TMP/link2.err"
fi
rm -rf "$TMP"; exit $rc
