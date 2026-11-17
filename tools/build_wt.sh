#!/bin/bash
# usage: build_wt.sh <elf> <name> [recompiler flags...]
# Translate with this worktree's out/ps4-recomp, then link via build_manual.sh.
# Output: build/<name>/native. Worktree-relative (no hardcoded checkout path).
# The CLI's own compile step is expected to stop at the Ajm/Gnm modules (missing third-party
# submodules); any other compile error is a real failure and stops this script, because
# build_manual.sh would otherwise stub the missing symbols and produce a broken binary.
W="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
ELF="$1"; NAME="$2"; shift 2
cd "$W" && mkdir -p build runs
env NPS5_RUNS_DIR="$W/runs" out/ps4-recomp "$@" -o "build/$NAME" "$ELF" > "build/$NAME.build.log" 2>&1
if grep -E '^[^ ]+\.(c|m|mm|cpp|s|h):[0-9]+:[0-9]+: error' "build/$NAME.build.log" | grep -v 'libSceAjm\|libSceGnmDriver' | head -20 | grep .; then
  echo "build_wt: compile errors outside the Ajm/Gnm modules (see build/$NAME.build.log)"; exit 1
fi
grep -E 'Memory model|Error: ' "build/$NAME.build.log" | head -5
if [ ! -f "build/$NAME/main.o" ]; then
  echo "build_wt: translation did not reach the compile step:"; tail -8 "build/$NAME.build.log"; exit 1
fi
tools/build_manual.sh "$W/build/$NAME" "$W/build/$NAME/native" </dev/null
