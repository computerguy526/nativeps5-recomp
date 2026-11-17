#!/bin/bash
# -tso-elide A/B microbenchmark: builds tests/ps5/bench/tsobench.c with the PS5 payload SDK,
# translates it once per mode (TSOBENCH_MODES, default off stack stack-region), runs them RUNS times alternately through
# tools/run_sandboxed.sh and prints the minimum ns per iteration of each case and the difference to the first mode.
#   usage: tools/tsobench.sh [RUNS]      (default 3)
# Only the arm64 output of this repo's translator is executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/tsobench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH" NPS5_JOBS="${NPS5_JOBS:-3}"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$T/build/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
"$SDK/bin/prospero-clang" -O2 -std=gnu11 -fplt -fno-optimize-sibling-calls -ffp-contract=off -Wno-everything "$T/bench/tsobench.c" -o "$B/tsobench.elf" || exit 2
MODES="${TSOBENCH_MODES:-off stack stack-region}"
for m in $MODES; do
  d="$B/$m"; rm -rf "$d"; mkdir -p "$d"
  "$BIN" -compile=false -tso-elide $m -o "$d/out" "$B/tsobench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed ($m)"; exit 2; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed ($m)"; exit 2; }
done
# Interleaved, so that load on the machine hits every mode alike. CPU-only: no title gate.
for r in $(seq "$RUNS"); do
  for m in $MODES; do
    d="$B/$m"
    ( cd "$d/out" && NPS5_CPU_ONLY=1 "$ROOT/tools/run_sandboxed.sh" 300 "$d/native" 2> "$d/stderr.$r" | grep ns_per_iter > "$d/run.$r" )
  done
done
for m in $MODES; do
  awk -v mode="$m" '
    { name = $1; for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ns_per_iter_x100") v = kv[2] / 100; if (kv[1] == "check") c = kv[2] }
      if (!(name in mn) || v < mn[name]) mn[name] = v; chk[name] = c
      if (!(name in seen)) { seen[name] = 1; order[++n] = name } }
    END { for (i = 1; i <= n; i++) printf "%s %s %.2f %s\n", mode, order[i], mn[order[i]], chk[order[i]] }' "$B/$m"/run.*
done | awk '
  { v[$1, $2] = $3; c[$1, $2] = $4; if (!($1 in ms)) { ms[$1] = 1; mo[++nm] = $1 } if (!($2 in cs)) { cs[$2] = 1; co[++nc] = $2 } }
  END { printf "%-15s", "case (min ns)"; for (j = 1; j <= nm; j++) printf " %22s", mo[j]; printf "  checksums\n"
        for (i = 1; i <= nc; i++) { printf "%-15s", co[i]; same = "equal"
          for (j = 1; j <= nm; j++) { x = v[mo[j], co[i]]; b = v[mo[1], co[i]]
            if (j == 1) printf " %22.2f", x; else printf " %12.2f (%+6.1f%%)", x, (b > 0) ? (x - b) * 100 / b : 0
            if (c[mo[j], co[i]] != c[mo[1], co[i]]) same = "DIFFER" }
          printf "  %s\n", same } }'
