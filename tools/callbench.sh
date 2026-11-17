#!/bin/bash
# Call-model A/B microbenchmark: builds tests/ps5/bench/callbench.c with the PS5 payload SDK,
# translates it once per call model, runs them RUNS times alternately through
# tools/run_sandboxed.sh and prints the minimum and median ns/call per case.
#   usage: tools/callbench.sh [RUNS]      (default 3)
# Only the arm64 output of this repo's translator is executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/bench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$T/build/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
"$SDK/bin/prospero-clang" -O2 -std=gnu11 -fplt -fno-optimize-sibling-calls -ffp-contract=off -Wno-everything "$T/bench/callbench.c" -o "$B/callbench.elf" || exit 2
MODELS="${CALLBENCH_MODELS:-nested flat}"
for m in $MODELS; do
  d="$B/$m"; rm -rf "$d"; mkdir -p "$d"
  "$BIN" -compile=false -call-model $m ${CALLBENCH_XFLAGS:-} -o "$d/out" "$B/callbench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed ($m)"; exit 2; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed ($m)"; exit 2; }
done
# Interleaved, so that load on the machine hits both models alike.
for r in $(seq "$RUNS"); do
  for m in $MODELS; do
    d="$B/$m"
    # the runtime loads guest_image.bin from its cwd
    ( cd "$d/out" && "$ROOT/tools/run_sandboxed.sh" 300 "$d/native" 2> "$d/stderr.$r" | grep ns_per_call > "$d/run.$r" )
  done
done
awk '
  { split(FILENAME, p, "/"); m = p[length(p)-1]; name = $1
    for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ns_per_call_x100") v = kv[2] / 100; if (kv[1] == "check") c = kv[2] }
    k = m SUBSEP name; cnt[k]++; val[k, cnt[k]] = v; chk[k] = c
    if (!(name in seen)) { seen[name] = 1; order[++n] = name } }
  function stat(k,   i, j, t, c) { c = cnt[k]; for (i = 1; i <= c; i++) a_[i] = val[k, i]
    for (i = 1; i <= c; i++) for (j = i + 1; j <= c; j++) if (a_[j] < a_[i]) { t = a_[i]; a_[i] = a_[j]; a_[j] = t }
    mn = a_[1]; md = a_[int((c + 1) / 2)] }
  END { printf "%-10s %19s %19s %15s  %s\n", "case", "nested min/median", "flat min/median", "delta min/med", "checksums"
        for (i = 1; i <= n; i++) { stat("nested" SUBSEP order[i]); amn = mn; amd = md; stat("flat" SUBSEP order[i])
          d1 = (amn > 0) ? (mn - amn) * 100 / amn : 0; d2 = (amd > 0) ? (md - amd) * 100 / amd : 0
          same = (chk["nested" SUBSEP order[i]] == chk["flat" SUBSEP order[i]]) ? "equal" : "DIFFER"
          printf "%-10s %8.2f /%7.2f ns %8.2f /%7.2f ns %+6.1f%% /%+6.1f%%  %s\n", order[i], amn, amd, mn, md, d1, d2, same } }
' "$B"/nested/run.* "$B"/flat/run.*
