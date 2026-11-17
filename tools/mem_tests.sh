#!/bin/bash
# Build the memory-model PS5 tests (tests/ps5/memory/*.c) with the payload SDK.
# usage: tools/mem_tests.sh        -> tests/ps5/memory/build/<name>.elf
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:-/nonexistent}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
# No prebuilt payloads are committed: they are built here from the C sources, or the step is skipped (exit 3).
[ -x "$SDK/bin/prospero-clang" ] || { echo "SKIP mem_tests: no payload SDK at $SDK (set PS5_SDK_DIR to a ps5-payload-sdk install)"; exit 3; }
T="$ROOT/tests/ps5/memory"; mkdir -p "$T/build"; fail=0
for src in "$T"/*.c; do
  name="$(basename "$src" .c)"
  if "$SDK/bin/prospero-clang" -O1 -std=gnu11 -ffp-contract=off -Wno-everything "$src" -o "$T/build/$name.elf" 2> "$T/build/$name.log"; then
    echo "built  $name ($(wc -c < "$T/build/$name.elf") bytes)"
  else
    echo "FAILED $name (see $T/build/$name.log)"; fail=1
  fi
done
exit $fail
