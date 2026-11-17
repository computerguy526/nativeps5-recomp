#!/bin/bash
# Build the M1 PS5 test corpus with the PS5 payload SDK, in two variants:
#   <name>.plt.elf    (-fplt)            <name>.noplt.elf (SDK default -fno-plt, GOT-style imports)
# usage: tools/m1_build_tests.sh [test-name ...]     (default: every test in tests/ps5/tests.list)
# env:   PS5_SDK_DIR  payload SDK root (default: testbed copy)   OPT  optimisation flags (default -O2 -g0)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests/ps5"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"   # clang 23 + lld, as in S2
OPT="${OPT:--O2}"
mkdir -p "$T/build"
want=" $* "; fail=0
while read -r name src flags; do
  case "$name" in ''|'#'*) continue ;; esac
  [ "$#" -gt 0 ] && [[ "$want" != *" $name "* ]] && continue
  [ "$flags" = "-" ] && flags=""
  case "$src" in *.cpp) CC="$SDK/bin/prospero-clang++"; STD="-std=c++17" ;; *) CC="$SDK/bin/prospero-clang"; STD="-std=gnu11" ;; esac
  for v in plt noplt; do
    [ "$v" = plt ] && VF="-fplt" || VF=""
    out="$T/build/$name.$v.elf"
    if $CC $OPT $STD $VF -ffp-contract=off -Wno-everything $flags "$T/src/$src" -o "$out" 2> "$out.log"; then
      printf 'built  %-20s %-6s %s\n' "$name" "$v" "$(wc -c < "$out") bytes"
    else
      printf 'FAILED %-20s %-6s (see %s.log)\n' "$name" "$v" "$out"; fail=1
    fi
  done
done < "$T/tests.list"
exit $fail
