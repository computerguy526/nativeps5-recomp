#!/bin/bash
# Rosetta reference runner for the M1 test corpus (SPEC §8 oracle; §8.1 "output identical to the Rosetta reference").
#
# Two routes:
#  (default)  source-level oracle: the SAME sources are compiled for x86_64-apple-macos
#             (-DNPS5_HOST shim: sceKernelWrite -> write(2)) and run under Rosetta with
#             `arch -x86_64`.  This executes the real x86-64 instruction forms (flags, x87,
#             AVX2/FMA, string ops) and checks that expected/*.txt (generated on arm64 from the
#             portable models) agree with x86 behaviour.  libc is macOS libc, not the PS5 one.
#  --elf      ELF-level oracle: the PS5 payload ELF (tests/ps5/build/<t>.<variant>.elf) is
#             post-processed by cmd/ps5nidify (plain imports -> NIDs, entry stub -> main, procparam),
#             relinked with AnyPS5's relinker (--macos --to-rosetta) and run under Rosetta.
#             Stdout is stored in tests/ps5/ref/<t>.<variant>.stdout.  See docs/m1/oracle.md.
#
# usage: tools/ref_rosetta.sh [--elf] [--variant plt|noplt|both] [--update-expected] [test ...]
# Safety: only these self-built programs are run; env -i, HOME/TMPDIR redirected, 30 s timeout, stdin /dev/null.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests/ps5"; B="$T/build"; SB="$B/rosetta_home"
ANYPS5="${ANYPS5_BUILD:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/s3/rosetta/AnyPS5-x86/build}"
TMO=/opt/homebrew/bin/gtimeout
mode=src; update=0; names=(); variants="plt noplt"
while [ $# -gt 0 ]; do case "$1" in
  --elf) mode=elf ;; --update-expected) update=1 ;;
  --variant) shift; case "$1" in both) variants="plt noplt" ;; plt|noplt) variants="$1" ;; *) echo "bad --variant"; exit 2 ;; esac ;;
  *) names+=("$1") ;; esac; shift; done
NOISE_RE='^\[(src/|ps4-|nps5-)'   # AnyPS5 runtime chatter on stdout
if [ "$mode" = elf ]; then
  mkdir -p "$B" "$T/ref"
  ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$B/ps5nidify" ./cmd/ps5nidify ) || { echo "ps5nidify build failed"; exit 2; }
fi
mkdir -p "$B/x86ref" "$B/ref" "$SB"
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "Rosetta not available (arch -x86_64 failed)"; exit 2; }

sandbox() { env -i PATH=/usr/bin:/bin HOME="$SB" TMPDIR="$SB" "$TMO" 30 arch -x86_64 "$@" </dev/null; }

pass=0; fail=0
while read -r name src flags; do
  case "$name" in ''|'#'*|broken_*) continue ;; esac
  [ "${#names[@]}" -gt 0 ] && [[ " ${names[*]} " != *" $name "* ]] && continue
  [ "$flags" = "-" ] && flags=""
  vlist="src"; [ "$mode" = elf ] && vlist="$variants"
  for v in $vlist; do
  tag="$name"; out="$B/ref/$name.rosetta.txt"; rc=0
  if [ "$mode" = src ]; then
    case "$src" in *.cpp) CC=clang++; STD=-std=c++17 ;; *) CC=clang; STD=-std=gnu11 ;; esac
    # shellcheck disable=SC2086
    $CC -target x86_64-apple-macos13 -O2 -ffp-contract=off -DNPS5_HOST $STD -Wno-everything $flags \
        "$T/src/$src" -o "$B/x86ref/$name" 2> "$B/x86ref/$name.log" || { echo "BUILD-FAIL $name"; fail=$((fail+1)); continue; }
    sandbox "$B/x86ref/$name" > "$out" 2> "$out.err"; rc=$?
  else
    tag="$name.$v"; out="$B/ref/$tag.rosetta.txt"
    d="$B/rosetta/$tag"; rm -rf "$d"; mkdir -p "$d" "$B/rosetta/libs"
    [ -f "$B/$tag.elf" ] || { echo "NO-ELF $tag (run tools/m1_build_tests.sh)"; fail=$((fail+1)); continue; }
    for f in "$ANYPS5"/core/libs/libs/*; do ln -sf "$f" "$B/rosetta/libs/$(basename "$f")"; done
    for n in libkernel_web libSceLibcInternal libSceNet; do ln -sf "$ANYPS5/core/libs/libs/$n.prx" "$B/rosetta/libs/$n.sprx"; done
    "$B/ps5nidify" "$B/$tag.elf" "$d/input.elf" > "$d/nidify.log" 2>&1 || { echo "NIDIFY-FAIL $tag (see $d/nidify.log)"; fail=$((fail+1)); continue; }
    (cd "$d" && sandbox "$ANYPS5/core/relinker/relinker" --macos --to-rosetta --skip-sce-module --rpath "$B/rosetta/libs" input.elf eboot) > "$d/relink.log" 2>&1 \
      || { echo "RELINK-FAIL $tag (see $d/relink.log)"; fail=$((fail+1)); continue; }
    (cd "$d" && sandbox ./eboot) > "$out" 2> "$out.err"; rc=$?
    # AnyPS5 aborts with "guest entry returned" (rc 134) when the entry stub returns: that is normal completion here.
    if [ "$rc" = 134 ] && grep -q "guest entry returned" "$out.err"; then rc=0; fi
    grep -v -E "$NOISE_RE" "$out" > "$T/ref/$tag.stdout"
    cp "$T/ref/$tag.stdout" "$out"
  fi
  if [ "$update" = 1 ]; then cp "$out" "$T/expected/$name.txt"; echo "UPDATED $tag (rc=$rc)"; continue; fi
  # known Rosetta deviations (tests/ps5/rosetta_known.txt: "<test>: <regex of lines to ignore>") are filtered from both sides
  known=$(cat "$T/rosetta_known.txt" $([ "$mode" = elf ] && echo "$T/ref/anyps5_known.txt") 2>/dev/null | grep "^$name:" | sed "s/^$name: *//" | paste -sd'|' -)
  if [ -n "$known" ]; then
    grep -v -E "$known" "$out" > "$out.f"; grep -v -E "$known" "$T/expected/$name.txt" > "$out.e"
  else cp "$out" "$out.f"; cp "$T/expected/$name.txt" "$out.e"; fi
  if [ "$rc" -le 1 ] && cmp -s "$out.f" "$out.e"; then echo "MATCH  $tag${known:+ (modulo known Rosetta deviations)}"; pass=$((pass+1))
  else echo "DIFF   $tag (rc=$rc)  diff $T/expected/$name.txt $out"; fail=$((fail+1)); fi
  done
done < "$T/tests.list"
echo "rosetta($mode): $pass match, $fail differ"
[ "$fail" -eq 0 ]
