#!/bin/bash
# Memory-model proof runs (docs/m1/memory.md "What runs"): translate, link and run, sandboxed,
#   hello_ps4    PS4Recomp hello_world.elf            base
#   hello_k      PS5 payload-SDK sceKernelWrite test  base and identity
#   t_fixedmap   fixed guest maps at 0x2_0000_0000, 0x30_0020_0000, 0x70_0000_0000, top  base
# and compare stdout with tests/ps5/memory/expected/. usage: tools/mem_prove.sh [name ...]
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
W="$(cd "$(dirname "$0")/.." && pwd)"; cd "$W"
HELLO_K="${HELLO_K_ELF:-${NPS5_ROOT:-/nonexistent}/work/s2/ps5/hello_k/hello_k.elf}"
HELLO_PS4="${NPS5_TEST_HELLO_ELF:-$W/hello_world.elf}"   # upstream PS4Recomp sample, not committed here
E="$W/tests/ps5/memory/expected"; fail=0
mkdir -p build out
# PS4RECOMP_BIN (tools/regress.sh): use that translator (a wrapper that caps clang workers) instead of building one
if [ -n "${PS4RECOMP_BIN:-}" ]; then mkdir -p out && cp "$PS4RECOMP_BIN" out/ps4-recomp || exit 2
else rm -f out/ps4-recomp; GOFLAGS=-mod=vendor go build -o out/ps4-recomp ./cmd/ps4-recomp || exit 2; fi
tools/mem_tests.sh >/dev/null; mrc=$?; [ $mrc = 0 ] || [ $mrc = 3 ] || exit 2   # 3 = no payload SDK: t_fixedmap is skipped below
run() { # name elf secs expected flags...
  local name="$1" elf="$2" secs="$3" exp="$4"; shift 4
  [ -n "${ONLY:-}" ] && [[ " $ONLY " != *" $name "* ]] && return
  [ -f "$elf" ] || { echo "SKIP  $name: $elf not present (payloads are built from source or supplied, never committed)"; return; }
  rm -rf "build/$name"
  if ! tools/build_wt.sh "$elf" "$name" "$@" > "build/$name.link.log" 2>&1; then
    echo "FAIL  $name: build (build/$name.link.log)"; fail=1; return
  fi
  NPS5_EXTRA_ENV="NPS5_LOG=vm=warn,sys=warn,mem=warn" tools/run_sandboxed.sh "$secs" "build/$name/native" > "build/$name.stdout" 2> "build/$name.stderr"
  local rc=$?
  grep -v -E '^\[(ps4|nps5|recomp|runtime|vmm|mem)[^]]*\]' "build/$name.stdout" > "build/$name.out"
  local arena; arena="$(grep -h -m1 'guest memory:' "build/$name.stdout" "build/$name.stderr" | sed 's/.*guest memory: //')"
  if cmp -s "build/$name.out" "$exp"; then echo "PASS  $name (rc=$rc)  [$arena]"
  else echo "FAIL  $name (rc=$rc): output differs from $exp  [$arena]"; diff "$exp" "build/$name.out" | head -5; fail=1; fi
}
ONLY="$*"
run hello_ps4      "$HELLO_PS4" 5  "$E/hello_ps4.txt"  -allow-unsupported
run hello_k        "$HELLO_K"           20 "$E/hello_k.txt"
run hello_k_ident  "$HELLO_K"           20 "$E/hello_k.txt"    -addr-mode identity
run hello_k_auto   "$HELLO_K"           20 "$E/hello_k.txt"    -addr-mode auto
run t_fixedmap     "$W/tests/ps5/memory/build/t_fixedmap.elf" 20 "$E/t_fixedmap.txt"
exit $fail
