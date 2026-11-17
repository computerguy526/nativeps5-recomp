#!/bin/bash
# FEX-Emu ASM oracle (SPEC 8.1): assemble each unittests/ASM test with nasm, lift it with this
# repo's translator, run the arm64 result sandboxed, compare with the test's RegData.
# Nothing x86 is executed: the tests are only assembled and lifted.
#
#   tools/fex_oracle.sh [filter-regexp] [extra fexasm flags]
#
# Env: FEX_DIR  (default <repo>/../../repos/FEX/unittests/ASM, the clone at commit ea43314f)
#      FEX_J    parallelism (default: cores)       FEX_TIMEOUT  seconds per test (default 20)
#      FEX_BASELINE results.json to diff against (default: the committed tests/fex/results.json)
# Output: tests/fex/results.json (filtered runs update only their entries), tests/fex/work/ (scratch).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FEX_DIR="${FEX_DIR:-$ROOT/../../repos/FEX/unittests/ASM}"
[ -d "$FEX_DIR" ] || { echo "FEX tests not found at $FEX_DIR (set FEX_DIR)" >&2; exit 2; }
command -v nasm >/dev/null || { echo "nasm missing: brew install nasm" >&2; exit 2; }
OUT="$ROOT/tests/fex"; mkdir -p "$OUT"
BASE="${FEX_BASELINE:-$OUT/results.json}"
cp "$BASE" "$OUT/.baseline.json" 2>/dev/null || echo '[]' > "$OUT/.baseline.json"
FILTER="${1:-}"; [ $# -gt 0 ] && shift
( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$OUT/fexasm" ./cmd/fexasm )
cd "$ROOT"
exec "$OUT/fexasm" -fex "$FEX_DIR" -root "$ROOT" -work "$OUT/work" -out "$OUT/results.json" \
  -baseline "$OUT/.baseline.json" -j "${FEX_J:-$(sysctl -n hw.ncpu)}" -timeout "${FEX_TIMEOUT:-20}" \
  ${FILTER:+-filter "$FILTER"} "$@"
