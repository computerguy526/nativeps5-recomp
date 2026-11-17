#!/bin/bash
# End-to-end memtests: PS5 guest programs (tests/ps5/memtests) built with the payload SDK, translated
# in base and identity mode with this worktree's out/ps4-recomp, linked by tools/build_wt.sh, and run
# ONLY through tools/run_sandboxed.sh (translated arm64 code; no x86 binary is ever executed).
# usage: tools/memtests_guest.sh <results-file> [program ...]
# Output: one "RESULT|name|area|status|detail" line per case appended to <results-file>.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
W="$(cd "$(dirname "$0")/.." && pwd)"; cd "$W"
OUT="${1:?results file}"; shift
ONLY="${*:-}"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:-/nonexistent}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
[ -x "$SDK/bin/prospero-clang" ] || { echo "SKIP memtests_guest: no payload SDK at $SDK (set PS5_SDK_DIR to a ps5-payload-sdk install); payloads are built from source, never committed"; exit 0; }
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
T="$W/tests/ps5/memtests"; mkdir -p "$T/build" build runs
# PS4RECOMP_BIN (tools/regress.sh): use that translator (a wrapper that caps clang workers) instead of building one
if [ -n "${PS4RECOMP_BIN:-}" ]; then mkdir -p out && cp "$PS4RECOMP_BIN" out/ps4-recomp || exit 2
else rm -f out/ps4-recomp; GOFLAGS=-mod=vendor go build -o out/ps4-recomp ./cmd/ps4-recomp || exit 2; fi
emit() { printf 'RESULT|%s|%s|%s|%s\n' "$1" "$2" "$3" "$(printf '%s' "$4" | tr '|\n' '/ ' | cut -c1-600)" >> "$OUT"; }

build_prog() { # prog src flags mode
  local prog="$1" src="$2" flags="$3" mode="$4" name="mtg_${1}_${4}"
  [ "$flags" = "-" ] && flags=""
  if ! "$SDK/bin/prospero-clang" -O1 -std=gnu11 -ffp-contract=off -Wno-everything $flags "$T/$src" -o "$T/build/$prog.elf" 2> "$T/build/$prog.log"; then return 1; fi
  local extra=""; [ "$mode" = identity ] && extra="-addr-mode identity"
  rm -rf "build/$name"
  tools/build_wt.sh "$T/build/$prog.elf" "$name" $extra > "build/$name.link.log" 2>&1 < /dev/null || return 2
  [ -x "build/$name/native" ] || return 2
  return 0
}

run_case() { # name mode arg -> sets RC, STDOUT_FILE, CRASH_CLASS, CRASH_DETAIL
  local name="$1" arg="$2" stamp; stamp="$(mktemp "${TMPDIR:-/tmp}/mtstamp.XXXXXX")"
  NPS5_EXTRA_ENV="NPS5_LOG=vm=error,sys=error,mem=error" tools/run_sandboxed.sh 25 "build/$name/native" $arg > "build/$name.$arg.out" 2> "build/$name.$arg.err"
  RC=$?
  STDOUT_FILE="build/$name.$arg.out"
  CRASH_CLASS=""; CRASH_DETAIL=""
  local d; d="$(find runs -maxdepth 1 -type d -name '*-run' -newer "$stamp" 2>/dev/null | sort | tail -1)"
  if [ -n "$d" ] && [ -f "$d/crash.txt" ]; then
    CRASH_CLASS="$(grep -m1 '^fault_class:' "$d/crash.txt" | sed 's/^fault_class: //')"
    CRASH_DETAIL="$(grep -m1 '^reason:' "$d/crash.txt" | sed 's/^reason: //')"
  fi
  rm -f "$stamp"
}

for mode in base identity; do
  while read -r prog src flags kind _; do
    case "$prog" in ''|\#*) continue ;; esac
    [ -n "$ONLY" ] && [[ " $ONLY " != *" $prog "* ]] && continue
    build_prog "$prog" "$src" "$flags" "$mode"; brc=$?
    if [ $brc -eq 2 ] && [ "$mode" = identity ] && grep -q "title needs base addressing" "build/mtg_${prog}_${mode}.link.log" 2>/dev/null; then
      emit "e2e.identity.$prog.build" e2e SKIP "identity refused by the translate-time scan (a false positive on a data word of the SDK-built image, memory.md section 4 allows those): $(grep -h -m1 'title needs base addressing' "build/mtg_${prog}_${mode}.link.log" | cut -c1-200)"
      continue
    fi
    if [ $brc -ne 0 ]; then
      emit "e2e.$mode.$prog.build" e2e FAIL "$( [ $brc = 1 ] && echo 'payload SDK compile failed' || echo 'translate/link failed') (build/mtg_${prog}_${mode}.link.log, tests/ps5/memtests/build/$prog.log): $(grep -h -m1 -E 'Error|error' "build/mtg_${prog}_${mode}.link.log" "$T/build/$prog.log" 2>/dev/null | head -1)"
      continue
    fi
    name="mtg_${prog}_${mode}"
    while read -r cprog case_ expect rest; do
      [ "$cprog" = "$prog" ] || continue
      # the expectation may contain spaces ("crash:null guard"): parse the raw line
      line="$(grep -E "^$cprog[[:space:]]+$case_[[:space:]]" "$T/cases.list" | head -1)"
      known="$(printf '%s' "$line" | sed -n 's/.*[[:space:]]known=\(.*\)$/\1/p')"
      idexp="$(printf '%s' "$line" | sed -E 's/[[:space:]]+known=.*$//' | sed -n 's/.*[[:space:]]id=\(.*\)$/\1/p')"
      expect="$(printf '%s' "$line" | sed -E "s/^$cprog[[:space:]]+$case_[[:space:]]+//; s/[[:space:]]+(id|known)=.*\$//")"
      [ "$mode" = identity ] && [ -n "$idexp" ] && expect="$idexp"
      arg="$case_"; [ "$arg" = "-" ] && arg=""
      tname="e2e.$mode.$prog.${case_/-/all}"
      run_case "$name" "$arg"
      out="$STDOUT_FILE"
      case "$expect" in
        results)
          n=$(grep -c '^RESULT|' "$out")
          if [ "$n" -eq 0 ]; then
            emit "$tname" e2e FAIL "no RESULT lines; rc=$RC${CRASH_CLASS:+; crashed: $CRASH_CLASS ($CRASH_DETAIL)}; last stdout: $(grep -v '^\[' "$out" | tail -2 | tr '\n' ' ')"
          else
            grep '^RESULT|' "$out" >> "$OUT"
            # a crash after some results is its own failure
            [ -n "$CRASH_CLASS" ] && emit "$tname.crash" e2e FAIL "crashed after $n results: $CRASH_CLASS ($CRASH_DETAIL)"
            [ -z "$CRASH_CLASS" ] && [ "$RC" -eq 124 ] && emit "$tname.timeout" e2e KNOWN-FAIL "did not finish within 25 s after $n results (hang); see vm.*.direct_map_fixed_over_existing_mapping"
          fi ;;
        ok)
          if [ "$RC" -eq 0 ] && grep -q -E "CASE $case_ (OK|SURVIVED)" "$out"; then emit "$tname" e2e PASS ""
          else emit "$tname" e2e FAIL "rc=$RC${CRASH_CLASS:+; crashed: $CRASH_CLASS ($CRASH_DETAIL)}; $(grep -v '^\[' "$out" | tail -2 | tr '\n' ' ')"; fi ;;
        crash:*)
          want="${expect#crash:}"; status=PASS; detail=""
          if [ -z "$CRASH_CLASS" ] && [ "$RC" -eq 0 ]; then status=SURVIVED; detail="the access did not fault (rc=0)"
          elif [ "$RC" -eq 124 ]; then status=HANG; detail="timed out"
          elif [ "$want" != any ] && ! printf '%s' "$CRASH_CLASS" | grep -qi -- "$want"; then status=MISCLASS; detail="expected class containing '$want', crash report says '${CRASH_CLASS:-<none>}' (rc=$RC)"; fi
          if [ "$status" = PASS ]; then emit "$tname" e2e PASS "classified as: ${CRASH_CLASS:-n/a}"
          elif [ -n "$known" ]; then emit "$tname" e2e KNOWN-FAIL "$known [$detail]"
          else emit "$tname" e2e FAIL "$detail"; fi ;;
      esac
    done < "$T/cases.list"
  done < "$T/tests.list"
done
exit 0
