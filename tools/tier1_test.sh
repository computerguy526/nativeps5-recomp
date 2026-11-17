#!/bin/bash
# Tier-1 miss fallback test (M1 exit criterion, docs/m1/tier1.md).
#   tools/tier1_test.sh [--mode base|identity|both] [--call-model flat|nested] [--keep]   (default call model: flat)
# Builds tests/ps5/tier1/t_tier1_hidden.c with the PS5 payload SDK (-O1): three functions reachable
# only through a runtime-computed pointer (naked asm in a custom section, no FDE/reloc/FUNC symbol).
# For each address mode:
#   1. translate + build; run 1: output correct, every hidden function appears in
#      tier1_misses.jsonl as compiled/waited (dedupe: 1 compile for the 4-thread race);
#   2. run 2: same output, the misses are served from the disk cache (cache_hit, no compile);
#   3. cache corruption: a damaged cached dylib is recompiled, never loaded;
#   4. NPS5_MISS=fatal: crash.txt with the structured tier1_* fields;
#   5. retranslate with -seeds-from <run 1 bundle>: run 3 has no tier-1 miss at all.
# Runs only arm64 output of this repo's translator through tools/run_sandboxed.sh.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
B="$ROOT/tests/ps5/build/tier1"; SRC="$ROOT/tests/ps5/tier1"
modes="base identity"; keep=0; CM=flat
while [ $# -gt 0 ]; do case "$1" in
  --mode) case "$2" in both) modes="base identity" ;; *) modes="$2" ;; esac; shift ;;
  --call-model) CM="$2"; shift ;;
  --keep) keep=1 ;; *) echo "usage: $0 [--mode base|identity|both]"; exit 2 ;; esac; shift; done
mkdir -p "$B"
fail=0; ok() { echo "  ok   $*"; }; bad() { echo "  FAIL $*"; fail=1; }

BIN="${PS4RECOMP_BIN:-$B/ps4-recomp}"
if [ -z "${PS4RECOMP_BIN:-}" ]; then ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || { echo "translator build failed"; exit 2; }; fi
ELF="$B/t_tier1_hidden.elf"
PATH="/opt/homebrew/opt/llvm/bin:$PATH" "$SDK/bin/prospero-clang" -O1 -std=gnu11 -pthread -ffp-contract=off -Wno-everything \
  "$SRC/t_tier1_hidden.c" -o "$ELF" || { echo "ELF build failed"; exit 2; }
export NPS5_WORKDIR="$B"   # run_sandboxed.sh: HOME, TMPDIR and runs/ below $B

# count of jsonl lines with outcome $2 in bundle dir $1
cnt() { f="$1/tier1_misses.jsonl"; if [ -f "$f" ]; then grep -c "\"outcome\":\"$2\"" "$f"; else echo 0; fi; }
newest_bundle() { ls -1dt "$B"/runs/*-run 2>/dev/null | head -1; }
run_once() { # $1 outdir, $2 label; sets BUNDLE, RC; stdout in $B/$2.out
  rm -f "$B/$2.out"
  ( cd "$1" && "$ROOT/tools/run_sandboxed.sh" 90 "$B/native_$MODE" > "$B/$2.raw" 2> "$B/$2.err" ); RC=$?
  grep -v -E '^\[(ps4|nps5|recomp)[^]]*\]' "$B/$2.raw" > "$B/$2.out"   # runtime chatter, as tools/m1_run.sh
  BUNDLE="$(newest_bundle)"
}
build() { # $1 outdir, rest: extra translate flags
  d="$1"; shift
  rm -rf "$d"
  "$BIN" -compile=false -addr-mode "$MODE" -call-model "$CM" ${TIER1_XFLAGS:-} "$@" -o "$d" "$ELF" > "$B/translate_$MODE.log" 2>&1 || { bad "translate ($MODE) failed: see $B/translate_$MODE.log"; return 1; }
  "$ROOT/tools/m1_native_build.sh" "$d" "$B/native_$MODE" > "$B/build_$MODE.log" 2>&1 || { bad "native build ($MODE) failed: see $B/build_$MODE.log"; return 1; }
}

for MODE in $modes; do
  echo "== address mode: $MODE"
  OUT="$B/out_$MODE"
  build "$OUT" || continue
  rm -rf "$B/runs"

  run_once "$OUT" run1; B1="$BUNDLE"
  if cmp -s "$SRC/t_tier1_hidden.expected" "$B/run1.out"; then ok "run 1 output matches (rc=$RC)"; else bad "run 1 output differs (rc=$RC), see $B/run1.out $B/run1.err"; diff "$SRC/t_tier1_hidden.expected" "$B/run1.out" | head -5; fi
  L="$B1/tier1_misses.jsonl"
  served=$(( $(cnt "$B1" compiled) + $(cnt "$B1" waited) ))
  [ -f "$L" ] && ok "miss log written: $L" || bad "no tier1_misses.jsonl in $B1"
  [ "$(cnt "$B1" compiled)" -ge 3 ] && ok "3 hidden functions compiled by tier 1 ($(cnt "$B1" compiled) compiles, $(cnt "$B1" waited) waited, $(cnt "$B1" raced) raced)" || bad "expected >=3 compiled misses, got $(cnt "$B1" compiled)"
  [ "$(cnt "$B1" failed)" = 0 ] && ok "no failed misses" || bad "failed misses in $L"
  # dedupe: hidden_thr is missed by 4 threads at once: exactly one compile among those records
  nthr=$(grep -c '"outcome":"compiled"' "$L"); [ "$nthr" -le 3 ] && ok "concurrent misses deduplicated (compiled records: $nthr)" || bad "more compiles than distinct hidden entries ($nthr)"
  grep -h '"outcome"' "$L" | sed 's/^/         /'

  # run 2: cache
  run_once "$OUT" run2; B2="$BUNDLE"
  cmp -s "$SRC/t_tier1_hidden.expected" "$B/run2.out" && ok "run 2 output matches" || bad "run 2 output differs"
  [ "$(cnt "$B2" compiled)" = 0 ] && [ "$(cnt "$B2" cache_hit)" -ge 3 ] && ok "run 2 served from the disk cache ($(cnt "$B2" cache_hit) cache hits, 0 compiles)" || bad "run 2: compiled=$(cnt "$B2" compiled) cache_hit=$(cnt "$B2" cache_hit)"

  # cache corruption: damage one cached dylib; it must be recompiled
  victim="$(ls -1 "$OUT"/tier1_cache/*/f_*.dylib | head -1)"
  printf 'garbage' >> "$victim"
  run_once "$OUT" run2b; B2b="$BUNDLE"
  cmp -s "$SRC/t_tier1_hidden.expected" "$B/run2b.out" && ok "damaged cache entry: output still correct" || bad "damaged cache entry broke the run"
  [ "$(cnt "$B2b" compiled)" -ge 1 ] && ok "damaged cache entry was recompiled, not loaded" || bad "damaged entry not recompiled"

  # MISS=fatal: structured crash
  NPS5_EXTRA_ENV="NPS5_MISS=fatal" run_once "$OUT" fatal; BF="$BUNDLE"
  if [ "$RC" -ne 0 ] && [ -f "$BF/crash.txt" ] && grep -q '^tier1_kind: miss_fatal' "$BF/crash.txt" && grep -q '^tier1_guest_rip:' "$BF/crash.txt" && grep -q '^tier1_module:' "$BF/crash.txt"; then
    ok "NPS5_MISS=fatal -> crash.txt with tier1_kind/tier1_guest_rip/tier1_module (rc=$RC)"; grep -E '^(reason|tier1_)' "$BF/crash.txt" | sed 's/^/         /'
  else bad "NPS5_MISS=fatal did not produce the structured crash.txt (rc=$RC) $BF"; fi

  # failure paths: each must stop with a structured crash.txt (never silently)
  for spec in "1 outside_guest_code data address" "2 undecodable undecodable bytes" "3 outside_guest_code null pointer"; do
    set -- $spec; m=$1; kind=$2; shift 2
    BELF="$B/t_tier1_bad$m.elf"
    PATH="/opt/homebrew/opt/llvm/bin:$PATH" "$SDK/bin/prospero-clang" -O1 -std=gnu11 -DMODE=$m -Wno-everything "$SRC/t_tier1_bad.c" -o "$BELF" || { bad "ELF build bad$m"; continue; }
    BOUT="$B/outbad${m}_$MODE"; rm -rf "$BOUT"
    "$BIN" -compile=false -addr-mode "$MODE" -call-model "$CM" -o "$BOUT" "$BELF" > "$B/translate_bad.log" 2>&1 && "$ROOT/tools/m1_native_build.sh" "$BOUT" "$B/native_bad${m}_$MODE" > "$B/build_bad.log" 2>&1 || { bad "build bad$m"; continue; }
    ( cd "$BOUT" && "$ROOT/tools/run_sandboxed.sh" 60 "$B/native_bad${m}_$MODE" > "$B/bad$m.raw" 2> "$B/bad$m.err" ); RC=$?
    BF="$(newest_bundle)"
    if [ "$RC" -ne 0 ] && grep -q '^before' "$B/bad$m.raw" && ! grep -q '^after' "$B/bad$m.raw" && grep -q "^tier1_kind: $kind" "$BF/crash.txt" 2>/dev/null; then
      ok "$* -> crash.txt tier1_kind=$kind (rc=$RC)"; grep -E '^tier1_detail' "$BF/crash.txt" | cut -c1-170 | sed 's/^/         /'
    else bad "$* : expected a crash with tier1_kind=$kind (rc=$RC, bundle $BF)"; fi
    [ -s "$BF/tier1_misses.jsonl" ] && grep -q "\"kind\":\"$kind\"" "$BF/tier1_misses.jsonl" && ok "  miss log records the failure ($kind)" || bad "  miss log lacks the $kind record"
  done

  # a compile time cap: the translator is killed/aborted and the failure is structured
  rm -rf "$OUT"/tier1_cache
  NPS5_EXTRA_ENV="NPS5_TIER1_TIMEOUT_MS=1" run_once "$OUT" tmo; BT="$BUNDLE"
  if [ "$RC" -ne 0 ] && grep -q '^tier1_kind: timeout' "$BT/crash.txt" 2>/dev/null; then ok "time cap (NPS5_TIER1_TIMEOUT_MS=1) -> crash.txt tier1_kind=timeout"; else bad "time cap did not produce tier1_kind=timeout (rc=$RC; $(grep '^tier1_kind' "$BT/crash.txt" 2>/dev/null))"; fi

  # caps: fragment count, cache size
  rm -rf "$OUT"/tier1_cache
  NPS5_EXTRA_ENV="NPS5_TIER1_MAX_FRAGMENTS=1" run_once "$OUT" capn; BC="$BUNDLE"
  if [ "$RC" -ne 0 ] && grep -q '^tier1_kind: cap_exceeded' "$BC/crash.txt" 2>/dev/null && [ "$(cnt "$BC" compiled)" = 1 ]; then ok "fragment cap (NPS5_TIER1_MAX_FRAGMENTS=1): 1 compile, then crash.txt tier1_kind=cap_exceeded"; else bad "fragment cap not enforced (rc=$RC compiled=$(cnt "$BC" compiled))"; fi
  rm -rf "$OUT"/tier1_cache
  NPS5_EXTRA_ENV="NPS5_TIER1_CACHE_MB=0" run_once "$OUT" capc; BC="$BUNDLE"
  nd=$(ls -1 "$OUT"/tier1_cache/*/f_*.dylib 2>/dev/null | wc -l | tr -d ' ')
  if cmp -s "$SRC/t_tier1_hidden.expected" "$B/capc.out" && [ "$nd" -le 1 ]; then ok "cache cap (NPS5_TIER1_CACHE_MB=0): output correct, $nd dylib kept"; else bad "cache cap: $nd dylibs kept"; fi
  rm -rf "$OUT"/tier1_cache

  # seeds: retranslate folding the run-1 misses in
  OUT3="$B/out3_$MODE"
  build "$OUT3" -seeds-from "$B1" || continue
  grep -h "Tier-1 seeds" "$B/translate_$MODE.log" | sed 's/^ */         /'
  run_once "$OUT3" run3; B3="$BUNDLE"
  cmp -s "$SRC/t_tier1_hidden.expected" "$B/run3.out" && ok "seeded build: output matches" || bad "seeded build output differs"
  n3=0; [ -f "$B3/tier1_misses.jsonl" ] && n3=$(wc -l < "$B3/tier1_misses.jsonl" | tr -d ' ')
  [ "$n3" = 0 ] && ok "-seeds-from removed every miss (no tier-1 miss in the third run)" || bad "seeded build still missed $n3 time(s)"
done
echo
[ "$fail" = 0 ] && echo "tier1_test: PASS" || echo "tier1_test: FAIL"
exit $fail
