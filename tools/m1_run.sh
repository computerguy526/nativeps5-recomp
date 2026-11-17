#!/bin/bash
# M1 test harness: translate -> compile/link -> run (sandboxed) -> compare, for every test and both
# import variants (-fplt / -fno-plt), then print a PASS/FAIL table.
#
#   tools/m1_run.sh [--variant plt|noplt|both] [--call-model nested|flat] [--x87 f80|f64] [--timeout SECS] [--rebuild] [--no-elf-build] [--keep-going] [test ...]
#   --call-model passes -call-model to the translator and keeps the outputs apart (build/work-<model>,
#   results in build/m1_results-<model>.txt); without it the translator's default model is used.
#
# Stages per (test, variant):  elf (SDK build)  translate (ps4-recomp)  build (clang+link)  run  compare
# Verdicts: PASS, FAIL-OUTPUT (ran, stdout differs), FAIL-EXIT (nonzero exit, stdout ok), CRASH (signal),
#           TIMEOUT, TRANSLATE-FAIL, BUILD-FAIL, ELF-FAIL, and for the §8.2 broken_* set:
#           PASS (nonzero exit/crash + stdout prefix + every token of expected/<n>.report found in
#           stderr or run bundle) / PARTIAL (crashed as intended but report misses tokens) / FAIL.
# --x87 f64 translates with the double-precision x87 model (-x87 f64; default f80, the translator's default) and
#   ignores the lines of tests/ps5/x87_f64_known.txt ("<test>: <regex>") on both sides: the documented f64 deviations.
# Environment: M1_NOISE_RE (stdout lines to drop before comparing; default runtime tags like "[ps4-vfs]")
#              PS4RECOMP_BIN (use an existing translator instead of building one)
#              M1_RUN_TIMEOUT, OBJ_CACHE, M1_EXTRA_ENV ("K=V K=V" passed into the sandbox)
#              M1_XLATE_FLAGS (extra translator flags) with M1_WORK_TAG (suffix of the work directory and results file)
# Nothing but the arm64 output of this repo's translator is executed; env -i, HOME/TMPDIR confined to
# tests/ps5/build/home, stdin /dev/null, timeout.  Never runs x86 code, never runs scripts/build.sh.
set -u
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests/ps5"; B="$T/build"; HOME_DIR="$B/home"; WORK="$B/work"
TMO=/opt/homebrew/bin/gtimeout
variants="plt noplt"; timeout="${M1_RUN_TIMEOUT:-30}"; rebuild=0; buildelf=1; names=(); x87="${M1_X87:-f80}"; model="${M1_CALL_MODEL:-}"
while [ $# -gt 0 ]; do case "$1" in
  --variant) case "$2" in both) variants="plt noplt" ;; *) variants="$2" ;; esac; shift ;;
  --timeout) timeout="$2"; shift ;;
  --call-model) model="$2"; shift ;;
  --rebuild) rebuild=1 ;;
  --x87) x87="$2"; shift ;;
  --no-elf-build) buildelf=0 ;;
  -h|--help) sed -n 2,21p "$0"; exit 0 ;;
  *) names+=("$1") ;; esac; shift; done
case "$x87" in f80|f64) ;; *) echo "--x87 must be f80 or f64"; exit 2 ;; esac
XFLAGS="${M1_XLATE_FLAGS:-}"; RES="$B/m1_results.txt"
if [ -n "$model" ]; then WORK="$B/work-$model"; XFLAGS="$XFLAGS -call-model $model"; RES="$B/m1_results-$model.txt"; fi
# M1_WORK_TAG keeps the outputs of a run with other translator flags (M1_XLATE_FLAGS) apart.
if [ -n "${M1_WORK_TAG:-}" ]; then WORK="$WORK-$M1_WORK_TAG"; RES="${RES%.txt}-$M1_WORK_TAG.txt"; fi
mkdir -p "$B" "$HOME_DIR" "$WORK"

# 1. translator
if [ -n "${PS4RECOMP_BIN:-}" ]; then BIN="$PS4RECOMP_BIN"; else
  BIN="$B/ps4-recomp"
  ( cd "$ROOT" && GOFLAGS=-mod=vendor go build -o "$BIN" ./cmd/ps4-recomp ) || { echo "translator build failed"; exit 2; }
fi
REV=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)

# 2. PS5 ELFs
if [ "$buildelf" = 1 ]; then "$ROOT/tools/m1_build_tests.sh" ${names[@]+"${names[@]}"} > "$B/elf_build.log" 2>&1 || echo "note: some ELF builds failed, see $B/elf_build.log"; fi

sandbox() { # $1=cwd ; rest = command
  local cwd="$1"; shift
  ( cd "$cwd" && env -i PATH=/usr/bin:/bin HOME="$HOME_DIR" TMPDIR="$HOME_DIR" PS4_RECOMP_HEADLESS=1 PS4_NO_DIALOGS=1 PS4_LANG=en \
      PS4RECOMP_DATA_DIR="$HOME_DIR/data" PS4RECOMP_CACHE_DIR="$HOME_DIR/cache" NPS5_TRACE=1 ${M1_EXTRA_ENV:-} \
      "$TMO" -s KILL "$timeout" "$@" </dev/null )
}

rows=(); np=0; nf=0; ntot=0
while read -r name src flags; do
  case "$name" in ''|'#'*) continue ;; esac
  [ "${#names[@]}" -gt 0 ] && [[ " ${names[*]} " != *" $name "* ]] && continue
  broken=0; case "$name" in broken_*) broken=1 ;; esac
  for v in $variants; do
    ntot=$((ntot+1)); id="$name.$v"; elf="$B/$id.elf"
    # f80 is the default model and keeps the old directory names; f64 builds live beside them
    d="$WORK/$id"; [ "$x87" = f64 ] && d="$WORK/$id.f64"
    t_tr="-"; t_bd="-"; t_run="-"; note=""; verdict=""
    if [ ! -f "$elf" ]; then verdict="ELF-FAIL"; note="no $elf"
    else
      if [ "$rebuild" = 1 ] || [ ! -x "$d/native" ] || [ "$elf" -nt "$d/native" ] || [ "$BIN" -nt "$d/native" ]; then
        rm -rf "$d"; mkdir -p "$d"
        s=$(date +%s); "$BIN" -compile=false -x87 "$x87" $XFLAGS -o "$d/out" "$elf" > "$d/translate.log" 2>&1; trc=$?; t_tr=$(( $(date +%s) - s ))s
        if [ $trc -ne 0 ]; then verdict="TRANSLATE-FAIL"; note="$(grep -m1 -iE 'error|fail|unsupported' "$d/translate.log" | cut -c1-90)"
        else
          s=$(date +%s); "$ROOT/tools/m1_native_build.sh" "$d/out" "$d/native" > "$d/build.log" 2>&1; brc=$?; t_bd=$(( $(date +%s) - s ))s
          [ $brc -ne 0 ] && { verdict="BUILD-FAIL"; note="$(tail -1 "$d/build.log" | cut -c1-90)"; }
        fi
      else t_tr="cached"; t_bd="cached"; fi
      if [ -z "$verdict" ]; then
        rdir="$d/out"; rm -rf "$rdir/runs"  # runtime loads guest_image.bin from its cwd
        s=$(date +%s); sandbox "$rdir" "$d/native" > "$d/stdout.txt" 2> "$d/stderr.txt"; rc=$?; rs=$(( $(date +%s) - s )); t_run="${rs}s"
        # runtime chatter such as "[ps4-vfs] ..." may reach stdout; test output never starts with '[' tags
        cp "$d/stdout.txt" "$d/stdout.raw"; grep -v -E "${M1_NOISE_RE:-^\[(ps4|nps5|recomp)[^]]*\]}" "$d/stdout.raw" > "$d/stdout.txt"
        exp="$T/expected/$name.txt"
        if [ "$rc" = 137 ] && [ "$rs" -ge "$timeout" ]; then verdict="TIMEOUT"
        elif [ "$broken" = 1 ]; then
          bad=""
          [ "$rc" -eq 0 ] && bad="exit 0"
          cmp -s "$d/stdout.txt" "$exp" || bad="$bad stdout-differs"
          miss=""
          if [ -f "$T/expected/$name.report" ]; then
            blob="$d/report_blob.txt"; { cat "$d/stderr.txt"; find "$rdir" "$d" -path '*/runs/*' -type f 2>/dev/null | head -50 | xargs cat 2>/dev/null; } > "$blob"
            while read -r tok; do [ -z "$tok" ] && continue; grep -qF -- "$tok" "$blob" || miss="$miss $tok"; done < "$T/expected/$name.report"
          fi
          if [ -n "$bad" ]; then verdict="FAIL"; note="$bad (rc=$rc)"
          elif [ -n "$miss" ]; then verdict="PARTIAL"; note="crash rc=$rc; report lacks:$miss"
          else verdict="PASS"; note="rc=$rc, report ok"; fi
        else
          cmpa="$exp"; cmpb="$d/stdout.txt"
          if [ "$x87" = f64 ] && [ -f "$T/x87_f64_known.txt" ]; then
            kn=$(grep "^$name:" "$T/x87_f64_known.txt" | sed "s/^$name: *//" | paste -sd'|' -)
            if [ -n "$kn" ]; then
              grep -v -E "$kn" "$exp" > "$d/exp.filtered"; grep -v -E "$kn" "$d/stdout.txt" > "$d/stdout.filtered"
              cmpa="$d/exp.filtered"; cmpb="$d/stdout.filtered"
            fi
          fi
          if cmp -s "$cmpb" "$cmpa"; then
            if [ "$rc" -eq 0 ]; then verdict="PASS"; else verdict="FAIL-EXIT"; note="rc=$rc"; fi
          elif [ "$rc" -gt 128 ]; then verdict="CRASH"; note="rc=$rc $(grep -m1 -iE 'unresolved|unsupported|abort|fault|signal|SIG' "$d/stderr.txt" | cut -c1-80)"
          else verdict="FAIL-OUTPUT"; note="rc=$rc $(diff "$cmpa" "$cmpb" | grep -m1 '^[<>]' | cut -c1-60)"; fi
        fi
      fi
    fi
    case "$verdict" in PASS) np=$((np+1)) ;; *) nf=$((nf+1)) ;; esac
    rows+=("$(printf '%-19s %-6s %-15s %-7s %-7s %-6s %s' "$name" "$v" "$verdict" "$t_tr" "$t_bd" "$t_run" "$note")")
  done
done < "$T/tests.list"

{
  echo "M1 test run  translator=$BIN  x87=$x87  repo=$REV  call-model=${model:-default}  $(date -u +%Y-%m-%dT%H:%MZ)"
  printf '%-19s %-6s %-15s %-7s %-7s %-6s %s\n' TEST VARIANT VERDICT XLATE BUILD RUN NOTE
  printf '%s\n' "${rows[@]}"
  echo "-- $np PASS, $nf not passing, $ntot total"
} | tee "$RES"
[ "$nf" -eq 0 ]
