#!/bin/bash
# Time one guest leaf function lifted with vector lane liveness off / strict / abi (docs/m3/simdregs.md).
#   usage: tools/simdkern/run.sh <elf> <function address, hex, as cmd/simdstat prints it> [iterations] [repeats]
# The function must be a leaf that takes up to three pointers (rdi, rsi, rdx) to arrays of doubles and
# up to two floats (xmm0, xmm1); the harness does not know its real signature, it only gives every
# mode the same inputs and checks that all modes leave the same memory behind.
# Nothing x86 is executed: the function is lifted (cmd/simdstat -emit), assembled and run as arm64.
# Work files go to $SIMDKERN_DIR (default: a temporary directory, removed afterwards).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; H="$ROOT/tools/simdkern"
ELF="$1"; A="${2#0x}"; ITERS="${3:-2000000}"; REPS="${4:-7}"
D="${SIMDKERN_DIR:-$(mktemp -d)}"; mkdir -p "$D"
[ -n "${SIMDKERN_DIR:-}" ] || trap 'rm -rf "$D"' EXIT
( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$D/simdstat" ./cmd/simdstat )
"$D/simdstat" -emit "$D" "$ELF" "0x$A" 2>/dev/null
{ echo "static const struct { unsigned long long addr; unsigned char b[32]; } consts[] = {"
  while read -r addr hex; do printf '  {0x%s, {%s}},\n' "$addr" "$(echo "$hex" | sed 's/../0x&,/g')"; done < "$D/fn_$A.consts"
  echo "  {0x60000, {0}}"; echo "};"; } > "$D/consts.h"
clang -O2 -w -I "$D" -DKOFF="kern_${A}_off" -DKSTRICT="kern_${A}_strict" -DKABI="kern_${A}_abi" \
  "$H/harness.c" "$H/glue.s" "$D/fn_${A}_off.s" "$D/fn_${A}_strict.s" "$D/fn_${A}_abi.s" -o "$D/kern"
/opt/homebrew/bin/gtimeout 300 "$D/kern" "$ITERS" "$REPS"
