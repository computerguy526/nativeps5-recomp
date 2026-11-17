#!/bin/bash
# Cost of one engine Malloc+Free pair through the native UE allocator replacement, lean bridge vs full
# bridge. Needs the build tools/uemalloc_test.sh leaves in tests/ps5/build/uemalloc (run it first).
#   usage: tools/uembench.sh [RUNS]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/tests/ps5/build/uemalloc"; RUNS="${1:-5}"
d="$B/work.plt.replaced"; [ -x "$d/native" ] || { echo "run tools/uemalloc_test.sh first"; exit 2; }
for m in full lean; do
  lv=1; [ $m = full ] && lv=0
  for r in $(seq "$RUNS"); do
    ( cd "$d/out" && NPS5_WORKDIR="$B" NPS5_EXTRA_ENV="NPS5_LEAN=$lv" "$ROOT/tools/run_sandboxed.sh" 120 "$d/native" 2>/dev/null | grep bench_malloc_free_pair | sed 's/.*=//' )
  done | sort -n | awk -v m=$m '{a[NR]=$1/100} END {printf "%-5s min %.2f ns  median %.2f ns per Malloc+Free pair\n", m, a[1], a[int((NR+1)/2)]}'
done
