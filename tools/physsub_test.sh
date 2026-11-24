#!/bin/bash
# End-to-end test of wrapped function replacements ("wrap": true, docs/m3/function-hle.md section 6) and the [physsub]
# shims (pkg/runtime/modules/libSceAnyPS5/nps5_physsub.c) with a synthetic program (tests/physsub/t_physsub.c): its
# world_step / cloth_step are wrapped by nps5_physsub_world_step / nps5_physsub_cloth_step; the program is translated,
# built and run (tools/run_sandboxed.sh only) with the switch off, on, and on with substepped cloth, and the call counts,
# dt sums and largest dt are checked. Needs the payload SDK and clang (as tools/uemalloc_test.sh).
#   tools/physsub_test.sh [--timeout SECS]
# Nothing but the arm64 output of this repo's translator is executed.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/tests/ps5/build/physsub"; TMO=60
[ "${1:-}" = "--timeout" ] && TMO="$2"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$B/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || { echo "translator build failed"; exit 2; }
elf="$B/t_physsub.elf"
"$SDK/bin/prospero-clang" -O1 -ffp-contract=off -Wno-everything "$ROOT/tests/physsub/t_physsub.c" -o "$elf" 2> "$elf.log" || { echo "ELF build failed, see $elf.log"; exit 2; }
fa() { local l; l=$(llvm-nm -S "$elf" | awk -v s="$2" '$4==s {print $1, $2; exit}'); [ -n "$l" ] || return
  printf '{"name":"%s","addr":"0x%s","size":%d,"shim":"%s","wrap":true}' "$1" "${l% *}" "$((16#${l#* }))" "$3"; }
W=$(fa world_step world_step nps5_physsub_world_step); C=$(fa cloth_step cloth_step nps5_physsub_cloth_step)
[ -n "$W" ] && [ -n "$C" ] || { echo "symbols not found"; exit 2; }
cat > "$B/table.json" <<JSON
{"name":"t_physsub","funcs":[ $W, $C ], "params":[{"key":"physsub.cloth.skip_flag","value":"0x195"}]}
JSON
d="$B/work"; rm -rf "$d"; mkdir -p "$d"
"$BIN" -compile=false -hle-replace "$B/table.json" -hle-replace-unverified -o "$d/out" "$elf" > "$d/translate.log" 2>&1 || { echo "TRANSLATE-FAIL ($(grep -m1 -iE 'error' "$d/translate.log" | cut -c1-200))"; exit 1; }
grep -q "Function replacements" "$d/translate.log" || { echo "translator did not report the replacements"; exit 1; }
"$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "BUILD-FAIL ($(tail -1 "$d/build.log" | cut -c1-150))"; exit 1; }
fail=0
# expected: off = unchanged; on = world 3 calls of 50 ms -> 2 steps each (world default 25 Hz; +1 short call), cloth clamped to 1/60; substep = cloth 3 steps each
check() { # mode env expectations...
  local mode="$1" env="$2"; shift 2
  ( cd "$d/out" && NPS5_WORKDIR="$B" NPS5_EXTRA_ENV="$env" "$ROOT/tools/run_sandboxed.sh" "$TMO" "$d/native" > "$d/stdout.$mode" 2> "$d/stderr.$mode" ); local rc=$? bad=""
  grep -qx "result=DONE" "$d/stdout.$mode" || bad="no result line (rc=$rc)"
  for kv in "$@"; do
    k=${kv%%=*}; v=${kv#*=}; got=$(sed -n "s/^$k=//p" "$d/stdout.$mode")
    case "$v" in ~*) t=${v#\~}; [ -n "$got" ] && [ $(( got > t ? got - t : t - got )) -le 2 ] || bad="$bad $k=$got(want ~$t)";;
                *) [ "$got" = "$v" ] || bad="$bad $k=$got(want $v)";; esac
  done
  if [ -z "$bad" ]; then echo "$mode: PASS $(grep -m1 '^\[physsub\]' "$d/stderr.$mode" | cut -c1-120)"; else echo "$mode: FAIL$bad $(grep -m1 -E 'FATAL|fault' "$d/stderr.$mode" | cut -c1-160)"; fail=1; fi
}
check off  "NPS5_PHYS_SUBSTEP=0" world_calls=4 world_sum_us=~160000 world_max_us=~50000 world_bad=0 cloth_calls=2 cloth_sum_us=~100000 posed_calls=1
check on   "NPS5_PHYS_SUBSTEP=1" world_calls=7 world_sum_us=~160000 world_max_us=~25000 world_bad=0 cloth_calls=2 cloth_sum_us=~33333 cloth_max_us=~16667 posed_calls=1 posed_sum_us=~50000
check sub  "NPS5_PHYS_SUBSTEP=1 NPS5_PHYS_SUBSTEP_CLOTH=substep NPS5_PHYS_SUBSTEP_HZ=30" world_calls=7 world_sum_us=~160000 world_max_us=~25000 cloth_calls=4 cloth_sum_us=~100000 cloth_max_us=~25000 posed_calls=1
# world clamp (F1): one call per frame, dt = min(dt, 1/25 s), no split (4 calls, 3 x 50 ms -> 40 ms + the 10 ms one)
check clamp "NPS5_PHYS_SUBSTEP=1 NPS5_PHYS_SUBSTEP_WORLD_CLAMP=1" world_calls=4 world_sum_us=~130000 world_max_us=~40000 world_bad=0 cloth_calls=2
exit $fail
