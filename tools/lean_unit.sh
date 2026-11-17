#!/bin/bash
# Host differential test of every lean HLE entry against its full shim (tests/lean/lean_unit.c).
# Native arm64 only; no guest/x86 code is executed. The full memcpy-family shims live in
# kernel/syscalls/ps4_syscalls.c, which is too big to link here: they are extracted verbatim.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
T="${TMPDIR:-/tmp}/lean_unit.$$"; mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
{ echo '#include "recomp_runtime.h"'; echo '#include <string.h>'
  awk '/^void shim_memcpy\(/{p=1} p{print} /^void shim_strcmp\(/{q=1} q&&/^}/{exit}' "$R/kernel/syscalls/ps4_syscalls.c"; } > "$T/full_string.c"
grep -q "shim_strcmp" "$T/full_string.c"
clang -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -I"$R/core" -I"$R/kernel/syscalls" -I"$R/kernel/vfs" \
  -I"$R/modules/libc" -I"$R/modules/libkernel" \
  "$ROOT/tests/lean/lean_unit.c" "$T/full_string.c" "$R/core/nps5_lean.c" "$R"/modules/libc/*.c "$R/modules/libkernel/nps5_thunks_sync.c" \
  -Wl,-undefined,dynamic_lookup -o "$T/lean_unit"
"$T/lean_unit" "$@"
