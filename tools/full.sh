#!/bin/bash
# usage: full.sh <elf> <builddir>  -> emit (CLI compile fails at Ajm/Gnm by design) + manual stub link
WT=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/wt/loader
mkdir -p "$2"; cd $WT
$WT/out/ps4-recomp -o "$2" "$1" >"$2.build.log" 2>&1
$WT/tools/build_manual.sh "$WT/$2" "$WT/$2/native" </dev/null
