#!/bin/bash
# m3-liftmisc A/B microbenchmark: builds tests/ps5/bench/liftbench.c with the PS5 payload SDK, translates it once per
# configuration (all three features off; each alone; all on), runs them RUNS times interleaved through
# tools/run_sandboxed.sh and prints the minimum ns per unit per case.
#   usage: tools/liftbench.sh [RUNS]      (default 3)     env: LIFTBENCH_CFGS="off fp pp jmp all"
# Only the arm64 output of this repo's translator is executed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; B="$T/build/liftbench"; RUNS="${1:-3}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$B/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || exit 2
# -fno-optimize-sibling-calls is NOT given: tail_* cases need jmp reg / jmp [mem]
"$SDK/bin/prospero-clang" -O2 -std=gnu11 -fplt -ffp-contract=off -Wno-everything "$T/bench/liftbench.c" -o "$B/liftbench.elf" || exit 2
CFGS="${LIFTBENCH_CFGS:-off fp pp jmp all}"
flags() { case "$1" in
  off) echo "-fp-cmp-fuse=false -pushpop-fuse=false -jmp-ic=false" ;;
  fp)  echo "-fp-cmp-fuse=true -pushpop-fuse=false -jmp-ic=false" ;;
  pp)  echo "-fp-cmp-fuse=false -pushpop-fuse=true -jmp-ic=false" ;;
  jmp) echo "-fp-cmp-fuse=false -pushpop-fuse=false -jmp-ic=true" ;;
  all) echo "-fp-cmp-fuse=true -pushpop-fuse=true -jmp-ic=true" ;;
  nocold) echo "-cold-outline=false" ;; cold) echo "-cold-outline=true" ;; esac; }
for c in $CFGS; do
  d="$B/$c"; rm -rf "$d"; mkdir -p "$d"
  "$BIN" -compile=false -j 3 $(flags $c) -o "$d/out" "$B/liftbench.elf" > "$d/translate.log" 2>&1 || { echo "translate failed ($c)"; exit 2; }
  "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "build failed ($c)"; exit 2; }
done
for r in $(seq "$RUNS"); do
  for c in $CFGS; do
    d="$B/$c"
    ( cd "$d/out" && "$ROOT/tools/run_sandboxed.sh" 300 "$d/native" 2> "$d/stderr.$r" | grep ns_per_call > "$d/run.$r" )
  done
done
awk -v cfgs="$CFGS" '
  BEGIN { nc = split(cfgs, C, " ") }
  { split(FILENAME, p, "/"); m = p[length(p)-1]; name = $1
    for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "ns_per_call_x100") v = kv[2] / 100; if (kv[1] == "check") c = kv[2] }
    k = m SUBSEP name; if (!(k in mn) || v < mn[k]) mn[k] = v; chk[k] = c
    if (!(name in seen)) { seen[name] = 1; order[++n] = name } }
  END { printf "%-16s", "case (ns/unit, min)"; for (j = 1; j <= nc; j++) printf " %9s", C[j]; printf "  %s\n", "checks"
        for (i = 1; i <= n; i++) { printf "%-16s", order[i]; same = "equal"
          for (j = 1; j <= nc; j++) { k = C[j] SUBSEP order[i]; printf " %9.3f", mn[k]; if (chk[k] != chk[C[1] SUBSEP order[i]]) same = "DIFFER" }
          printf "  %s\n", same } }
' $(for c in $CFGS; do ls "$B"/$c/run.* ; done)
