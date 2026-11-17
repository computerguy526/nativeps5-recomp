#!/bin/bash
# End-to-end test of the function-replacement facility (docs/m3/function-hle.md) with a synthetic program:
# tests/uemalloc/t_fake_malloc.cpp builds an "engine allocator" object (vtable, constructor, Malloc/Realloc/Free/
# QuantizeSize/GetAllocationSize/Trim) whose original implementation counts its calls; the program is translated
# twice, without and with a -hle-replace table that binds those functions to the native UE allocator shims
# (shim_ue5_malloc_*, pkg/runtime/modules/libSceUeMalloc), built and run (tools/run_sandboxed.sh only) in both
# import variants. The control run must report replaced=no, the replaced run replaced=yes, both result=PASS.
#   tools/uemalloc_test.sh [--timeout SECS]       needs the payload SDK, clang and the 3rdparty submodules (as m1_run.sh)
# Nothing but the arm64 output of this repo's translator is executed.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/tests/ps5/build/uemalloc"; TMO=60
[ "${1:-}" = "--timeout" ] && TMO="$2"
SDK="${PS5_SDK_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/testbed/bin/ps5-payload-sdk/extracted/ps5-payload-sdk}"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
mkdir -p "$B"
BIN="${PS4RECOMP_BIN:-$B/ps4-recomp}"
[ -n "${PS4RECOMP_BIN:-}" ] || ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || { echo "translator build failed"; exit 2; }
fail=0
for v in plt noplt; do
  [ "$v" = plt ] && VF="-fplt" || VF=""
  elf="$B/t_fake_malloc.$v.elf"
  "$SDK/bin/prospero-clang++" -O1 -std=c++17 $VF -ffp-contract=off -Wno-everything -fno-rtti -fno-exceptions -pthread \
      "$ROOT/tests/uemalloc/t_fake_malloc.cpp" -o "$elf" 2> "$elf.log" || { echo "ELF build failed ($v), see $elf.log"; exit 2; }
  nmv() { llvm-nm "$elf" | awk -v s="$1" '$3==s {print "0x" $1; exit}'; }
  VT=$(nmv _ZTV10FakeBinned); [ -n "$VT" ] || { echo "no vtable symbol"; exit 2; }
  VT=$(printf '0x%x' $((VT + 16)))
  fa() { printf '{"name":"%s","addr":"%s","size":%d,"shim":"%s"}' "$1" "$(nmv "$2")" 16 "$3"; }
  cat > "$B/table.$v.json" <<JSON
{"name":"t_fake_malloc","funcs":[
 $(fa FakeBinned::FakeBinned _ZN10FakeBinnedC2Ev shim_ue5_malloc_ctor),
 $(fa FakeBinned::~FakeBinned _ZN10FakeBinnedD2Ev shim_ue5_malloc_nop),
 $(fa FakeBinned::Malloc _ZN10FakeBinned6MallocEmj shim_ue5_malloc_malloc),
 $(fa FakeBinned::Realloc _ZN10FakeBinned7ReallocEPvmj shim_ue5_malloc_realloc),
 $(fa FakeBinned::Free _ZN10FakeBinned4FreeEPv shim_ue5_malloc_free),
 $(fa FakeBinned::QuantizeSize _ZN10FakeBinned12QuantizeSizeEmj shim_ue5_malloc_quantize),
 $(fa FakeBinned::GetAllocationSize _ZN10FakeBinned17GetAllocationSizeEPvRm shim_ue5_malloc_getsize),
 $(fa FakeBinned::Trim _ZN10FakeBinned4TrimEb shim_ue5_malloc_nop)],
 "vtable_checks":[
 {"vtable":"$VT","slot":3,"func":"$(nmv _ZN10FakeBinned6MallocEmj)"},
 {"vtable":"$VT","slot":5,"func":"$(nmv _ZN10FakeBinned7ReallocEPvmj)"},
 {"vtable":"$VT","slot":7,"func":"$(nmv _ZN10FakeBinned4FreeEPv)"},
 {"vtable":"$VT","slot":8,"func":"$(nmv _ZN10FakeBinned12QuantizeSizeEmj)"},
 {"vtable":"$VT","slot":9,"func":"$(nmv _ZN10FakeBinned17GetAllocationSizeEPvRm)"},
 {"vtable":"$VT","slot":10,"func":"$(nmv _ZN10FakeBinned4TrimEb)"}],
 "params":[{"key":"ue5m.vtable","addr":"$VT"}]}
JSON
  for mode in control replaced; do
    d="$B/work.$v.$mode"; rm -rf "$d"; mkdir -p "$d"
    extra=""; [ $mode = replaced ] && extra="-hle-replace $B/table.$v.json"
    "$BIN" -compile=false $extra -o "$d/out" "$elf" > "$d/translate.log" 2>&1 || { echo "$v/$mode: TRANSLATE-FAIL ($(grep -m1 -iE 'error' "$d/translate.log" | cut -c1-150))"; fail=1; continue; }
    if [ $mode = replaced ] && ! grep -q "Function replacements" "$d/translate.log"; then echo "$v/$mode: translator did not report the replacements"; fail=1; continue; fi
    "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1 || { echo "$v/$mode: BUILD-FAIL ($(tail -1 "$d/build.log" | cut -c1-150))"; fail=1; continue; }
    ( cd "$d/out" && NPS5_WORKDIR="$B" NPS5_EXTRA_ENV="NPS5_UEMALLOC_STATS=1 NPS5_UEMALLOC_POISON=1" "$ROOT/tools/run_sandboxed.sh" "$TMO" "$d/native" > "$d/stdout.txt" 2> "$d/stderr.txt" ); rc=$?
    want=no; [ $mode = replaced ] && want=yes
    if [ $rc -eq 0 ] && grep -qx "replaced=$want" "$d/stdout.txt" && grep -qx "result=PASS" "$d/stdout.txt" && grep -q "^thread_ops=" "$d/stdout.txt"; then
      echo "$v/$mode: PASS $(grep -m1 '^\[ue5-malloc\] allocs' "$d/stderr.txt" | cut -c1-110)"
    else
      echo "$v/$mode: FAIL rc=$rc: $(grep -E 'FAIL|replaced=|result=' "$d/stdout.txt" | tr '\n' ' ' | cut -c1-200) $(grep -m1 -E 'FATAL|fault|abort' "$d/stderr.txt" | cut -c1-120)"; fail=1
    fi
  done
done
exit $fail
