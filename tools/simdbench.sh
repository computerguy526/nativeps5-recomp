#!/bin/bash
# SSE/AVX state microbenchmark (docs/m3/simdregs.md): builds tests/ps5/bench/simdbench.c with the
# PS5 payload SDK (AVX2 + FMA, as Unreal titles are built), translates it once per
# -simd-liveness mode, builds the same source natively for the arm64 host, runs them RUNS times
# alternately and prints the minimum ns per kernel call and the ratios.
#   usage: tools/simdbench.sh [RUNS]      (default 3)
#   env:   SIMDBENCH_MODES ("off abi")  SIMDBENCH_XFLAGS (extra translator flags)  SIMDBENCH_CFLAGS ("-mavx2 -mfma")
# Only the arm64 output of this repo's translator and the native arm64 build are executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/simdbench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$T/build/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
"$SDK/bin/prospero-clang" -O2 -std=gnu11 -fplt -ffp-contract=off ${SIMDBENCH_CFLAGS:--mavx2 -mfma} -Wno-everything "$T/bench/simdbench.c" -o "$B/simdbench.elf" || exit 2
/usr/bin/clang -target arm64-apple-macos -O2 -std=gnu11 -ffp-contract=off -DNPS5_HOST -Wno-everything "$T/bench/simdbench.c" -o "$B/native_arm64" || exit 2
MODES="${SIMDBENCH_MODES:-off abi}"
for m in $MODES; do
  d="$B/$m"; rm -rf "$d"; mkdir -p "$d"
  "$BIN" -compile=false -simd-liveness $m ${SIMDBENCH_XFLAGS:-} -o "$d/out" "$B/simdbench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed ($m)"; exit 2; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed ($m)"; exit 2; }
done
mkdir -p "$B/native"
# Interleaved, so that load on the machine hits all builds alike.
for r in $(seq "$RUNS"); do
  ( cd "$B" && "$ROOT/tools/run_sandboxed.sh" 300 "$B/native_arm64" 2> "$B/native/stderr.$r" | grep ps_per_call > "$B/native/run.$r" )
  for m in $MODES; do
    d="$B/$m"
    ( cd "$d/out" && "$ROOT/tools/run_sandboxed.sh" 300 "$d/native" 2> "$d/stderr.$r" | grep ps_per_call > "$d/run.$r" )
  done
done
files=("$B"/native/run.*); for m in $MODES; do files+=("$B/$m"/run.*); done
awk -v modes="native $MODES" '
  { split(FILENAME, p, "/"); m = p[length(p)-1]; name = $1
    for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ps_per_call") v = kv[2] / 1000; if (kv[1] == "check") c = kv[2] }
    k = m SUBSEP name; if (!(k in mn) || v < mn[k]) mn[k] = v; chk[k] = c
    if (!(name in seen)) { seen[name] = 1; order[++n] = name } }
  END { nm = split(modes, M, " ")
        printf "%-15s", "kernel"; for (j = 1; j <= nm; j++) printf " %10s", M[j] " ns"
        for (j = 2; j <= nm; j++) printf " %12s", M[j] "/native"; printf "  %s\n", "checksums"
        for (i = 1; i <= n; i++) { printf "%-15s", order[i]; same = "equal"
          for (j = 1; j <= nm; j++) { printf " %10.2f", mn[M[j] SUBSEP order[i]]; if (chk[M[j] SUBSEP order[i]] != chk[M[1] SUBSEP order[i]]) same = "DIFFER" }
          for (j = 2; j <= nm; j++) { b = mn[M[1] SUBSEP order[i]]; r = (b > 0) ? mn[M[j] SUBSEP order[i]] / b : 0; printf " %11.2fx", r }
          printf "  %s\n", same } }
' "${files[@]}"
