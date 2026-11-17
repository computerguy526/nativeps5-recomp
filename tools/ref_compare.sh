#!/bin/bash
# Compare a native (arm64 translated) run's stdout against the Rosetta ELF-level reference.
#   tools/ref_compare.sh [--variant plt|noplt] [--results-dir DIR] [test ...]
# Reads  DIR/<test>.<variant>/stdout.txt   (default DIR = tests/ps5/build/work, the m1_run.sh layout;
#        or DIR/<test>.<variant>.stdout)
# against tests/ps5/ref/<test>.<variant>.stdout (from tools/ref_rosetta.sh --elf).
# Lines matching tests/ps5/rosetta_known.txt and tests/ps5/ref/anyps5_known.txt are ignored on both sides.
# Exit 0 iff every compared test matches; tests with no reference or no run are reported SKIP.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"
dir="$T/build/work"; variants="plt noplt"; names=()
while [ $# -gt 0 ]; do case "$1" in
  --variant) shift; variants="$1" ;; --results-dir) shift; dir="$1" ;; *) names+=("$1") ;; esac; shift; done
ok=0; bad=0; skip=0
for ref in "$T"/ref/*.stdout; do
  [ -e "$ref" ] || { echo "no references in $T/ref (run tools/ref_rosetta.sh --elf)"; exit 2; }
  b=$(basename "$ref" .stdout); name=${b%.*}; v=${b##*.}
  [[ " $variants " == *" $v "* ]] || continue
  [ "${#names[@]}" -gt 0 ] && [[ " ${names[*]} " != *" $name "* ]] && continue
  run="$dir/$name.$v/stdout.txt"; [ -f "$run" ] || run="$dir/$name.$v.stdout"
  [ -f "$run" ] || { printf 'SKIP   %-24s no run output (%s)\n' "$b" "$run"; skip=$((skip+1)); continue; }
  known=$(cat "$T/rosetta_known.txt" "$T/ref/anyps5_known.txt" 2>/dev/null | grep "^$name:" | sed "s/^$name: *//" | paste -sd'|' -)
  if [ -n "$known" ]; then a=$(grep -v -E "$known" "$ref"); c=$(grep -v -E "$known" "$run"); else a=$(cat "$ref"); c=$(cat "$run"); fi
  if [ "$a" = "$c" ]; then printf 'MATCH  %s\n' "$b"; ok=$((ok+1))
  else printf 'DIFF   %-24s first: %s\n' "$b" "$(diff <(echo "$a") <(echo "$c") | grep -m1 '^[<>]' | cut -c1-70)"; bad=$((bad+1)); fi
done
echo "ref_compare: $ok match, $bad differ, $skip skipped"
[ "$bad" -eq 0 ]
