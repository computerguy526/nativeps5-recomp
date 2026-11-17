#!/bin/bash
# Base vs identity run time on memory-bound guest loops (tests/ps5/memory/bench/b_mem.c).
# usage: tools/mem_bench.sh [runs]   -> best-of-N wall time per (benchmark, mode), in ms
set -u
W="$(cd "$(dirname "$0")/.." && pwd)"; cd "$W"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
RUNS="${1:-5}"; B="$W/tests/ps5/memory/build"; mkdir -p "$B"
for bench in STREAM CHASE STACK; do
  "$SDK/bin/prospero-clang" -O1 -std=gnu11 -Wno-everything -DBENCH_$bench tests/ps5/memory/bench/b_mem.c -o "$B/b_$bench.elf" || exit 2
  for mode in base identity; do
    name="b_${bench}_$mode"; rm -rf "build/$name"
    tools/build_wt.sh "$B/b_$bench.elf" "$name" -addr-mode $mode > "build/$name.link.log" 2>&1 || { echo "$name: build failed"; continue; }
    best=999999; out=""
    for i in $(seq 1 "$RUNS"); do
      s=$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%d", time*1000')
      out=$(NPS5_EXTRA_ENV="NPS5_LOG=vm=warn,sys=warn,mem=warn" tools/run_sandboxed.sh 120 "build/$name/native" 2>/dev/null | grep -E '^(stream|chase|fib)=')
      e=$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%d", time*1000')
      t=$((e - s)); [ "$t" -lt "$best" ] && best=$t
    done
    printf '%-8s %-9s best %6d ms   %s\n' "$bench" "$mode" "$best" "$out"
  done
done
