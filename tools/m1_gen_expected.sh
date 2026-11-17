#!/bin/bash
# Regenerate tests/ps5/expected/<test>.txt on the host (arm64 macOS) from the portable models in the SAME
# sources (-DNPS5_HOST).  Cross-check afterwards with tools/ref_rosetta.sh (x86-64 under Rosetta).
# The broken_* expected files and *.report token files are hand-written and not touched.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; T="$ROOT/tests/ps5"; mkdir -p "$T/build"
while read -r name src flags; do
  case "$name" in ''|'#'*|broken_*) continue ;; esac
  [ "$#" -gt 0 ] && [[ " $* " != *" $name "* ]] && continue
  case "$src" in *.cpp) CC=clang++; STD=-std=c++17 ;; *) CC=clang; STD=-std=gnu11 ;; esac
  $CC -target arm64-apple-macos -O2 -ffp-contract=off -DNPS5_HOST $STD -Wno-everything "$T/src/$src" -o "$T/build/host_$name" || { echo "FAIL build $name"; continue; }
  ( cd "$T/build" && ./host_$name > "$T/expected/$name.txt" ); echo "expected/$name.txt rc=$? ($(wc -l < "$T/expected/$name.txt") lines)"
done < "$T/tests.list"
