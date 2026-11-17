#!/bin/bash
# m3-lift2 A/B: builds the UE-idiom micro-benchmark (work/ue-arm-study/scripts/uebench.c, own code, no game data)
# with the PS5 payload SDK, translates it once per variant, runs the variants RUNS times interleaved through
# tools/run_sandboxed.sh (CPU only) and prints the minimum ns per unit of each case and the ratio to the first variant.
#   usage: tools/lift2bench.sh [RUNS]      (default 3)
#   LIFT2_VARIANTS="name:flags|name:flags|..."  default: the three options off, then each on, then all on
# Only the arm64 output of this repo's translator is executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/lift2bench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
SRC="${UEBENCH_SRC:-$ROOT/../../work/ue-arm-study/scripts/uebench.c}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH" NPS5_JOBS="${NPS5_JOBS:-3}"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$B/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
"$SDK/bin/prospero-clang" -O2 -march=znver2 -fno-omit-frame-pointer -fstack-protector-strong -std=gnu11 -fplt -ffp-contract=off -Wno-everything \
  "$SRC" -o "$B/uebench.elf" || exit 2
OFF="-ro-loads=false -redzone-leaf=false -bmi-peep=false"
VARIANTS="${LIFT2_VARIANTS:-off:$OFF|ro:-redzone-leaf=false -bmi-peep=false|rz:-ro-loads=false -bmi-peep=false|bmi:-ro-loads=false -redzone-leaf=false|all:}"
names=""
IFS='|' read -ra VS <<< "$VARIANTS"
for v in "${VS[@]}"; do
  n="${v%%:*}"; f="${v#*:}"; d="$B/$n"; rm -rf "$d"; mkdir -p "$d"; names="$names $n"
  # shellcheck disable=SC2086
  "$BIN" -compile=false -tso-elide stack $f -o "$d/out" "$B/uebench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed ($n)"; exit 2; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed ($n)"; exit 2; }
done
for r in $(seq "$RUNS"); do
  for n in $names; do
    d="$B/$n"
    ( cd "$d/out" && NPS5_CPU_ONLY=1 "$ROOT/tools/run_sandboxed.sh" 600 "$d/native" 2> "$d/stderr.$r" | grep ns_per_unit > "$d/run.$r" )
  done
done
for n in $names; do
  awk -v mode="$n" '
    { name = $1; for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ns_per_unit_x100") v = kv[2] / 100; if (kv[1] == "check") c = kv[2] }
      if (!(name in mn) || v < mn[name]) mn[name] = v; chk[name] = c
      if (!(name in seen)) { seen[name] = 1; order[++k] = name } }
    END { for (i = 1; i <= k; i++) printf "%s %s %.2f %s\n", mode, order[i], mn[order[i]], chk[order[i]] }' "$B/$n"/run.*
done | awk '
  { v[$1, $2] = $3; c[$1, $2] = $4; if (!($1 in ms)) { ms[$1] = 1; mo[++nm] = $1 } if (!($2 in cs)) { cs[$2] = 1; co[++nc] = $2 } }
  END { printf "%-15s", "case (min ns)"; for (j = 1; j <= nm; j++) printf " %20s", mo[j]; printf "  checksums\n"
        for (i = 1; i <= nc; i++) { printf "%-15s", co[i]; same = "equal"
          for (j = 1; j <= nm; j++) { x = v[mo[j], co[i]]; b = v[mo[1], co[i]]
            if (j == 1) printf " %20.2f", x; else printf " %10.2f (%+6.1f%%)", x, (b > 0) ? (x - b) * 100 / b : 0
            if (c[mo[j], co[i]] != c[mo[1], co[i]]) same = "DIFFER" }
          printf "  %s\n", same } }'
