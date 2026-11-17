#!/bin/bash
# Host unit + stress test of the native UE allocator core (tests/uemalloc/uemalloc_unit.c): alignment, size classes,
# realloc, large blocks and their cache, OOM, multi-threaded alloc/free/realloc with cross-thread frees and pattern
# checks. Native arm64 only; nothing from a guest runs. SAN=thread|address builds with that sanitizer.
# usage: tools/uemalloc_unit.sh [iterations-per-thread]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/pkg/runtime/modules/libSceUeMalloc"
OUT="${TMPDIR:-/tmp}/uemalloc_unit.$$"
SAN=${SAN:+-fsanitize=$SAN}
clang -O1 -g -Wall -Wextra -Wno-unused-function -Wno-gnu-case-range $SAN -I"$D" \
  "$ROOT/tests/uemalloc/uemalloc_unit.c" "$D/nps5_uemalloc_core.c" -o "$OUT" -lpthread
"$OUT" "${1:-150000}"; rc=$?; rm -rf "$OUT" "$OUT.dSYM"; exit $rc
