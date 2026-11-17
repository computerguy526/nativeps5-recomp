#!/bin/bash
# usage: build_ps5.sh <elf> <name>  -> translate with the CLI (diag bundle in runs/), then link via build_manual.sh
# Output: build/<name>/native. The CLI compile step fails at Ajm/Gnm (missing submodules); build_manual.sh finishes it.
W=${W:-$(cd "$(dirname "$0")/.." && pwd)}
cd $W && mkdir -p build runs
env NPS5_RUNS_DIR=$W/runs out/ps4-recomp -o build/$2 "$1" > build/$2.build.log 2>&1
tail -3 build/$2.build.log
tools/build_manual.sh $W/build/$2 $W/build/$2/native </dev/null
