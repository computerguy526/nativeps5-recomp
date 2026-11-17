#!/bin/bash
# HLE call-cost microbenchmark: builds tests/ps5/bench/shimbench.c with the PS5 payload SDK, translates
# it once (flat call model), and runs it RUNS times with the lean bridge (default) and with the full one
# (NPS5_LEAN=0), alternately, through tools/run_sandboxed.sh. Prints min/median ns per call.
#   usage: tools/shimbench.sh [RUNS]      (default 3)
# Only the arm64 output of this repo's translator is executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/shimbench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$T/build/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
"$SDK/bin/prospero-clang" -O2 -std=gnu11 -fplt -fno-optimize-sibling-calls -fno-builtin -ffp-contract=off -Wno-everything "$T/bench/shimbench.c" -o "$B/shimbench.elf" || exit 2
d="$B/out"; rm -rf "$d"; mkdir -p "$d"
"$BIN" -compile=false -call-model flat ${SHIMBENCH_XFLAGS:-} -o "$d/out" "$B/shimbench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed"; exit 2; }
"$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed"; exit 2; }
rm -f "$B"/run.*
for r in $(seq "$RUNS"); do
  for m in lean full; do
    lv=1; [ "$m" = full ] && lv=0
    ( cd "$d/out" && NPS5_EXTRA_ENV="NPS5_LEAN=$lv" "$ROOT/tools/run_sandboxed.sh" 300 "$d/native" 2> "$d/stderr.$m.$r" | grep ns_per_call > "$B/run.$m.$r" )
  done
done
awk '
  { split(FILENAME, p, "."); m = p[length(p)-1]; name = $1
    for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ns_per_call_x100") v = kv[2] / 100; if (kv[1] == "check") c = kv[2] }
    k = m SUBSEP name; cnt[k]++; val[k, cnt[k]] = v; chk[k] = c
    if (!(name in seen)) { seen[name] = 1; order[++n] = name } }
  function stat(k,   i, j, t, c) { c = cnt[k]; for (i = 1; i <= c; i++) a_[i] = val[k, i]
    for (i = 1; i <= c; i++) for (j = i + 1; j <= c; j++) if (a_[j] < a_[i]) { t = a_[i]; a_[i] = a_[j]; a_[j] = t }
    mn = a_[1]; md = a_[int((c + 1) / 2)] }
  END { printf "%-20s %19s %19s  %s\n", "case", "full min/median", "lean min/median", "checksums"
        for (i = 1; i <= n; i++) { stat("full" SUBSEP order[i]); fmn = mn; fmd = md; stat("lean" SUBSEP order[i])
          same = (chk["full" SUBSEP order[i]] == chk["lean" SUBSEP order[i]]) ? "equal" : "DIFFER"
          printf "%-20s %8.2f /%7.2f ns %8.2f /%7.2f ns  %s\n", order[i], fmn, fmd, mn, md, same } }
' "$B"/run.full.* "$B"/run.lean.*
