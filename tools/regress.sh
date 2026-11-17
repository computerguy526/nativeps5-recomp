#!/bin/bash
# Pre-merge regression gate: "is this branch safe to merge?"  One command, one PASS/FAIL table, nonzero exit on any failure.
#
#   tools/regress.sh [--quick|--full] [--ref <git rev>] [--rev <git rev>] [options]
#
#   --quick        (default) go test, m1_run (flat, -fplt variant), Calculator3D for 10 s.   target about 5 minutes
#   --full         go test, memtests --strict, m1_run (flat), tier1_test, mem_prove, diag_acceptance, and the homebrew titles
#                  Calculator3D (30 s), Syobon (60 s), Greenhollow (80 s) with scripted pad input and audio capture.
#   --rev <rev>    test <rev> (built in a temporary worktree) instead of the checkout this script lives in.
#   --ref <rev>    after the subject, run the same checks on <rev> (temporary worktree) and print the table of both plus the
#                  checks whose result differs: the tool for bisecting a regression.
#   --nested       also run m1_run with the nested call model.
#   --only a,b     run only these checks (build is always done): gotest memtests m1_flat m1_nested tier1 mem_prove diag
#                  calc3d syobon greenhollow
#   -j N           CPU budget (default 3): GOMAXPROCS, translator -j, clang workers.  The machine is shared.
#   --rebuild      rebuild the title binaries even if their inputs are unchanged.
#   --keep-wt      keep the temporary worktrees of --rev/--ref (they are reused next time).
#   --update-refs  after the title runs, store their checkpoint frames as the new reference frames (see docs/m3/regress.md);
#                  never fails the gate on frame comparisons of that run.
#   --seconds T=S  override a title's run length, e.g. --seconds syobon=30 (repeatable)
#
# Output: table on stdout, <REGRESS_DIR>/runs/regress-<timestamp>.json (all metrics), logs under <REGRESS_DIR>/runs/<timestamp>/.
# REGRESS_DIR defaults to $NPS5_ROOT/work/regress; references and thresholds live in <REGRESS_DIR>/refs
# (not in git).  Recompiled outputs are only executed through tools/run_sandboxed.sh; no x86 binary is ever run, scripts/build.sh is never used.
set -u
HARNESS="$(cd "$(dirname "$0")/.." && pwd)"          # the tree this script lives in: tools/, the harness version under test conditions
REG="${REGRESS_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/regress}"
NROOT=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}
ANYPS5_BUILD="${NPS5_ANYPS5_BUILD:-$NROOT/wt/anyps5-int/build-arm64}"
KK_ICD="${KK_ICD:-$NROOT/work/s3/kk-mesh/install-arm64/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json}"
CALC_ELF="${CALC_ELF:-$NROOT/work/s3/rosetta/extract/eboot.elf}"
SYOBON_ELF="${SYOBON_ELF:-$NROOT/testbed/bin/syobon/extracted/PPSA47402/eboot.bin}"
GH_ELF="${GH_ELF:-$NROOT/work/verify/gh_extract/eboot.bin}"
GTIMEOUT=/opt/homebrew/bin/gtimeout
MODE=quick; REF=""; REV=""; NESTED=0; ONLY=""; J=3; REBUILD=0; KEEPWT=0; UPDATE_REFS=0; SECS_OVERRIDE=""
while [ $# -gt 0 ]; do case "$1" in
  --quick) MODE=quick ;; --full) MODE=full ;;
  --ref) REF="$2"; shift ;; --rev) REV="$2"; shift ;;
  --nested) NESTED=1 ;; --only) ONLY="$(echo "$2" | tr ',' ' ')"; shift ;;
  -j) J="$2"; shift ;; --rebuild) REBUILD=1 ;; --keep-wt) KEEPWT=1 ;; --update-refs) UPDATE_REFS=1 ;;
  --seconds) SECS_OVERRIDE="$SECS_OVERRIDE $2"; shift ;;
  -h|--help) sed -n 2,26p "$0"; exit 0 ;;
  *) echo "unknown option $1 (see --help)"; exit 2 ;; esac; shift; done

now() { /opt/homebrew/bin/gdate +%s.%N; }
TS="$(date +%Y%m%dT%H%M%S)"; RUNS="$REG/runs"; RUN="$RUNS/$TS"; T_ALL=$(date +%s)
mkdir -p "$REG"/{refs,runs,state,objcache,wt,builds} "$RUN"
export NPS5_ANYPS5_BUILD="$ANYPS5_BUILD"

if [ "$MODE" = full ]; then CHECKS="gotest memtests m1_flat tier1 mem_prove diag calc3d syobon greenhollow"
else CHECKS="gotest m1_flat calc3d"; fi
[ "$NESTED" = 1 ] && CHECKS="$CHECKS m1_nested"
[ -n "$ONLY" ] && CHECKS="$ONLY"
want() { case " $CHECKS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

BGPIDS=""
# kill_runs <prefix>: SIGKILL the title processes of THIS invocation only. Concurrent gates share $REG/builds, so matching the binary
# path would kill other agents' runs; a run is identified by open files below its own run directory (its HOME, cwd of the runs/ bundle, audio capture).
kill_runs() { local p; for p in $(pgrep -f "$REG/builds/.*/native" 2>/dev/null); do
    lsof -nP -p "$p" 2>/dev/null | grep -qF "$1" && kill -9 "$p" 2>/dev/null; done; return 0; }
cleanup() { for p in $BGPIDS; do kill "$p" 2>/dev/null; done; kill_runs "$RUN/"; true; }
trap cleanup EXIT INT TERM

# ----------------------------------------------------------------------------------------------- one suite (subject or reference)
# run_suite <label> <source root> <results.tsv>
run_suite() {
  local LABEL="$1" S="$2" RES="$3"; local OUT="$RUN/$LABEL"; local LOGS="$OUT/logs"
  mkdir -p "$LOGS" "$OUT/bin"; : > "$RES"
  export GOMAXPROCS="$J" M1_JOBS="$J" NPS5_BUILD_J="$J" OBJ_CACHE="$REG/objcache" GOFLAGS="-mod=vendor -buildvcs=false"
  local REVSHORT; REVSHORT="$(git -C "$S" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  [ -n "$(git -C "$S" status --porcelain --untracked-files=no 2>/dev/null | grep -v '^.. runs/')" ] && REVSHORT="$REVSHORT+dirty"
  echo "== [$LABEL] $S @ $REVSHORT"

  # record <check> <status> <secs> <detail> [metrics.json]
  record() { printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "${5:-}" >> "$RES"; printf '   %-14s %-5s %4ss  %s\n' "$1" "$2" "$3" "$4" | cut -c1-170; }
  # chk <name> <timeout-secs> <detail-fn> <cmd...>    (runs in $S; the command's exit status decides)
  chk() {
    local name="$1" tmo="$2" dfn="$3"; shift 3
    local s0; s0=$(date +%s)
    ( cd "$S" && "$GTIMEOUT" -k 20 "$tmo" "$@" ) > "$LOGS/$name.log" 2>&1; local rc=$?
    local secs=$(( $(date +%s) - s0 )) st=PASS; [ $rc -ne 0 ] && st=FAIL
    local det; det="$($dfn "$LOGS/$name.log" $rc)"
    [ $rc -eq 124 ] && det="TIMEOUT after ${tmo}s; $det"
    record "$name" "$st" "$secs" "$det"
  }
  d_last() { tail -1 "$1" | cut -c1-150; }
  d_gotest() { local ok bad; ok=$(grep -c '^ok' "$1"); bad=$(grep -E '^(FAIL|--- FAIL)' "$1" | head -3 | tr '\n' ' '); echo "$ok packages ok${bad:+; failing: $bad}"; }
  d_memtests() { local l; l=$(grep -E '^PASS [0-9]+ FAIL' "$1" | tail -1); echo "${l:-no report}"; }
  d_m1() { local l; l=$(grep -E '^-- ' "$1" | tail -1); local f; f=$(grep -E '^[a-z_0-9]+ +(plt|noplt) +[A-Z-]+' "$1" | grep -vE ' PASS ' | head -3 | awk '{printf "%s.%s=%s ", $1,$2,$3}'); echo "$l${f:+; $f}"; }
  d_tier1() { grep -E '^tier1_test:|^  FAIL' "$1" | head -4 | tr '\n' ' '; }
  d_prove() { echo "$(grep -c '^PASS' "$1") PASS, $(grep -c '^FAIL' "$1") FAIL$(grep '^FAIL' "$1" | head -2 | tr '\n' ' ' | cut -c1-100)"; }
  d_diag() { echo "$(grep -c '^PASS ' "$1") PASS, $(grep -c '^FAIL ' "$1") FAIL$(grep '^FAIL ' "$1" | head -2 | tr '\n' ' ')"; }

  # ---- translator, built once; a wrapper caps its clang workers at -j N
  local s0; s0=$(date +%s)
  if ( cd "$S" && go build -o "$OUT/bin/ps4-recomp.real" ./cmd/ps4-recomp ) > "$LOGS/build.log" 2>&1; then
    # wrapper: translate-only calls get -j N; calls that also compile (mem_prove, diag_acceptance, memtests_guest go through the CLI's ninja
    # step, which stops at the first error of the known Ajm/Gnm modules and relies on the other objects being done by then, so a small -j
    # there leaves objects missing) keep the CLI's default worker count but run at nice 15 so the shared machine stays responsive
    printf '#!/bin/bash\nfor a in "$@"; do [ "$a" = "-compile=false" ] && exec "%s" -j %s "$@"; done\nexec nice -n 15 "%s" "$@"\n' \
      "$OUT/bin/ps4-recomp.real" "$J" "$OUT/bin/ps4-recomp.real" > "$OUT/bin/ps4-recomp"; chmod +x "$OUT/bin/ps4-recomp"
    record build PASS $(( $(date +%s) - s0 )) "translator built once: $OUT/bin/ps4-recomp.real"
  else
    record build FAIL $(( $(date +%s) - s0 )) "go build ./cmd/ps4-recomp failed: $(tail -2 "$LOGS/build.log" | tr '\n' ' ' | cut -c1-120)"
    return 1
  fi
  local WRAP="$OUT/bin/ps4-recomp" REAL="$OUT/bin/ps4-recomp.real"
  mkdir -p "$S/out" && cp "$WRAP" "$S/out/ps4-recomp"            # tools that expect out/ps4-recomp (diag_acceptance, build_ps5.sh)
  export PS4RECOMP_BIN="$WRAP"

  want gotest && chk gotest 1500 d_gotest env LIFTER_NO_X86=1 GOFLAGS=-mod=vendor go test -p "$J" ./pkg/...
  want memtests && chk memtests 2400 d_memtests tools/memtests.sh --strict --no-doc
  # --quick runs the -fplt variant only (half the suite, the other variant differs in import binding); --full runs both
  local M1V=""; [ "$MODE" = quick ] && M1V="--variant plt"
  want m1_flat && chk m1_flat 1500 d_m1 env PS4RECOMP_BIN="$REAL" tools/m1_run.sh --call-model flat $M1V
  want m1_nested && chk m1_nested 1500 d_m1 env PS4RECOMP_BIN="$REAL" tools/m1_run.sh --call-model nested
  want tier1 && chk tier1 1500 d_tier1 env PS4RECOMP_BIN="$REAL" tools/tier1_test.sh --call-model flat
  want mem_prove && chk mem_prove 1500 d_prove tools/mem_prove.sh
  want diag && chk diag 1200 d_diag tools/diag_acceptance.sh

  # ---- homebrew titles
  want calc3d && run_title calc3d "$CALC_ELF" "$([ "$MODE" = full ] && echo 30 || echo 10)" "$S" "$OUT" "$REAL" "$WRAP" "$REVSHORT" "$RES"
  want syobon && run_title syobon "$SYOBON_ELF" 60 "$S" "$OUT" "$REAL" "$WRAP" "$REVSHORT" "$RES"
  want greenhollow && run_title greenhollow "$GH_ELF" 80 "$S" "$OUT" "$REAL" "$WRAP" "$REVSHORT" "$RES"
  return 0
}

# ----------------------------------------------------------------------------------------------- homebrew title
# run_title <name> <elf> <secs> <src root> <out dir> <real translator> <wrapper> <rev> <results.tsv>
run_title() {
  local T="$1" ELF="$2" DUR="$3" S="$4" OUT="$5" REAL="$6" WRAP="$7" REVS="$8" RES="$9"
  local o; for o in $SECS_OVERRIDE; do [ "${o%%=*}" = "$T" ] && DUR="${o#*=}"; done
  local REFS="$REG/refs/$T" LOGS="$OUT/logs" R="$OUT/$T"
  local EVERY=10 SCALE=8 PADF="" PHASE0=0
  case "$T" in
    calc3d) EVERY=10; SCALE=8; PHASE0=1 ;;
    syobon) EVERY=6; SCALE=8; PADF="$REG/refs/inputs/syobon.pad" ;;
    greenhollow) EVERY=12; SCALE=8; PADF="$REG/refs/inputs/greenhollow.pad" ;;
  esac
  [ -f "$REFS/thresholds.json" ] && EVERY="$(python3 -I -c "import json,sys;print(json.load(open('$REFS/thresholds.json')).get('dump_every',$EVERY))")"
  rm -rf "$R"; mkdir -p "$R"
  local s0; s0=$(date +%s)
  fail_title() { record "$T" FAIL $(( $(date +%s) - s0 )) "$1"; return 0; }
  [ -f "$ELF" ] || { fail_title "input $ELF missing"; return 0; }

  # ---- translate + build (cached by the hash of everything that goes into the binary)
  local KEY; KEY="$( { shasum "$REAL"; git -C "$S" rev-parse HEAD; git -C "$S" diff HEAD -- . ':!runs' 2>/dev/null | shasum; shasum "$ELF"; shasum "$S/tools/m1_native_build.sh"
                       stat -f %m "$ANYPS5_BUILD/core/libs/libs/unpatched/libSceAgcDriver.prx" 2>/dev/null; } | shasum | cut -c1-12)"
  local B="$REG/builds/$T-$KEY"
  if [ "$REBUILD" = 1 ] || [ ! -x "$B/native" ]; then
    rm -rf "$B"; mkdir -p "$B"
    ( cd "$B" && "$WRAP" -compile=false -gpu anyps5 -o "$B/out" "$ELF" ) > "$LOGS/$T.translate.log" 2>&1 \
      || { rm -rf "$B"; fail_title "translate failed: $(grep -aiE 'error|fatal' "$LOGS/$T.translate.log" | head -1 | cut -c1-110)"; return 0; }
    ( cd "$S" && "$GTIMEOUT" -k 20 1500 tools/m1_native_build.sh "$B/out" "$B/native" ) > "$LOGS/$T.build.log" 2>&1
    [ -x "$B/native" ] || { rm -rf "$B"; fail_title "native build failed: $(grep -a '!!' "$LOGS/$T.build.log" | head -1 | cut -c1-110)"; return 0; }
    grep -aq "FATAL" "$LOGS/$T.build.log" && { rm -rf "$B"; fail_title "build: $(grep -a FATAL "$LOGS/$T.build.log" | head -1 | cut -c1-110)"; return 0; }
    ls -dt "$REG/builds/$T-"* 2>/dev/null | tail -n +3 | while read -r old; do rm -rf "$old"; done   # keep the two newest
  fi
  local tbuild=$(( $(date +%s) - s0 ))

  # does this runtime have the audio capture hook?
  local CAP=false; grep -q NPS5_AUDIO_CAPTURE "$S/pkg/runtime/modules/libSceAudioOut/ps4_audioout.c" 2>/dev/null && CAP=true

  # exec_title <dir> <secs> <every> <scale> <dump-limit> <pad file|"">    runs <B>/native sandboxed, leaves timing in <dir>/timing.json
  exec_title() {
    local D="$1" SECS="$2" EV="$3" SC="$4" LIM="$5" PAD="$6"
    rm -rf "$D"; mkdir -p "$D/w/home"; rm -f "$B"/out/frame_*.bmp
    ln -s "$REG/state/cache" "$D/w/home/cache" 2>/dev/null; mkdir -p "$REG/state/cache"
    local t0 t1 tf="null"; t0=$(now)
    ( while [ ! -f "$B/out/frame_000.bmp" ]; do sleep 0.05; done; now > "$D/first_dump.txt"; [ -n "$PAD" ] && cp "$PAD" "$D/padcmd.txt" ) &
    local pp=$!; BGPIDS="$BGPIDS $pp"
    ( cd "$B/out" && NPS5_WORKDIR="$D/w" NPS5_EXTRA_ENV="VK_DRIVER_FILES=$KK_ICD NPS5_AUDIO_NULL=1 NPS5_AUDIO_CAPTURE=$D/audio APS5_DUMP_FRAMES=$LIM APS5_DUMP_FRAMES_EVERY=$EV APS5_DUMP_SCALE=$SC ${PAD:+APS5_PAD_INPUT_FILE=$D/padcmd.txt}" \
        "$S/tools/run_sandboxed.sh" "$SECS" "$B/native" > "$D/run.out" 2> "$D/run.err" ); local rc=$?
    t1=$(now); kill "$pp" 2>/dev/null; wait "$pp" 2>/dev/null
    kill_runs "$D/"                                                      # terminate this run, whatever it ignored (not other gates' runs)
    [ -f "$D/first_dump.txt" ] && tf="$(cat "$D/first_dump.txt")"
    printf '{"rc":%s,"t_start":%s,"t_first_dump":%s,"t_end":%s,"seconds":%s,"capture":%s}\n' "$rc" "$t0" "$tf" "$t1" "$SECS" "$CAP" > "$D/timing.json"
    mkdir -p "$D/frames"; mv "$B"/out/frame_*.bmp "$D/frames/" 2>/dev/null
    return 0
  }

  # warm-up: the first frame of a title with an empty shader/pipeline cache can exceed the title's own flip timeout (docs/m2/anyps5-integration.md
  # finding 7), so a cold cache is warmed by one unmeasured run; the cache lives in <REGRESS_DIR>/state/cache and survives between gate runs
  local WSTAMP; WSTAMP="$(stat -f %m "$ANYPS5_BUILD/core/libs/libs/unpatched/libSceAgcDriver.prx" 2>/dev/null)"
  local twarm=0
  if [ "$(cat "$REG/state/warm-$T" 2>/dev/null)" != "$WSTAMP" ]; then
    local w0; w0=$(date +%s); exec_title "$OUT/$T-warm" 30 100000 16 0 ""; twarm=$(( $(date +%s) - w0 ))
    printf '%s' "$WSTAMP" > "$REG/state/warm-$T"; rm -rf "$OUT/$T-warm"
  fi

  # ---- phase 0 (Calculator3D): frame 0 at full size, for the exact comparison with the Rosetta x86 reference
  local P0="$OUT/$T-p0"
  [ "$PHASE0" = 1 ] && exec_title "$P0" 12 1 1 1 ""
  # ---- the measured run
  local PADARG=""; [ -n "$PADF" ] && [ -f "$PADF" ] && PADARG="$PADF"
  local m0; m0=$(date +%s)
  exec_title "$R" "$DUR" "$EVERY" "$SCALE" 100000 "$PADARG"
  [ "$PHASE0" = 1 ] && { mkdir -p "$R/frame0"; cp "$P0/frames/frame_000.bmp" "$R/frame0/" 2>/dev/null; rm -rf "$P0/frames"; }
  local trun=$(( $(date +%s) - m0 ))

  # ---- evaluate
  local ev="ok"
  python3 -I "$HARNESS/tools/regress_report.py" title --title "$T" --run-dir "$R" --refs "$REFS" --out "$R/metrics.json" --quiet > "$R/eval.txt" 2>&1; local erc=$?
  [ -f "$R/metrics.json" ] || { fail_title "evaluation failed: $(tail -1 "$R/eval.txt")"; return 0; }
  if [ "$UPDATE_REFS" = 1 ]; then
    python3 -I "$HARNESS/tools/regress_report.py" refs-update --title "$T" --run-dir "$R" --refs "$REFS" --meta "rev=$REVS dur=$DUR every=$EVERY scale=$SCALE date=$TS" >> "$LOGS/$T.refs.log" 2>&1
    ev="refs updated"
  fi
  local fl=$(python3 -I -c "import json;m=json.load(open('$R/metrics.json'));print(m['metrics'].get('frames_presented_est'),'frames,',m['metrics'].get('max_flip_gap_s'),'s max gap')" 2>/dev/null)
  record "$T" "$([ $erc -eq 0 ] && echo PASS || echo FAIL)" $(( $(date +%s) - s0 )) "$fl; build ${tbuild}s warm ${twarm}s run ${trun}s; $ev" "$R/metrics.json"
  return 0
}

# ----------------------------------------------------------------------------------------------- main
make_wt() { # rev label -> prints the worktree path (a temporary worktree of the harness repo with this tree's tools/ overlaid)
  local rev="$1" sha wt; sha="$(git -C "$HARNESS" rev-parse --short=8 "$rev" 2>/dev/null)" || { echo "unknown rev $rev" >&2; return 1; }
  wt="$REG/wt/$sha"
  if [ ! -d "$wt" ]; then git -C "$HARNESS" worktree add --detach "$wt" "$sha" > "$RUN/wt-$sha.log" 2>&1 || { cat "$RUN/wt-$sha.log" >&2; return 1; }; fi
  # submodules (not populated in a fresh worktree) come from the harness tree
  for m in minimp3 libatrac9 spirv-cross; do
    if [ -d "$HARNESS/3rdparty/$m" ] && [ -z "$(ls -A "$wt/3rdparty/$m" 2>/dev/null)" ]; then rm -rf "$wt/3rdparty/$m"; ln -s "$HARNESS/3rdparty/$m" "$wt/3rdparty/$m"; fi
  done
  # the harness (tools/) is the one of this checkout, so every rev is judged by the same checks
  mkdir -p "$wt/tools"; cp -R "$HARNESS/tools/." "$wt/tools/"   # includes the silent run_sandboxed.sh
  # the opt-in audio capture (m3-verify) is test instrumentation, not behaviour: overlay it on revs that predate it
  if ! grep -q NPS5_AUDIO_CAPTURE "$wt/pkg/runtime/modules/libSceAudioOut/ps4_audioout.c" 2>/dev/null && [ -f "$HARNESS/pkg/runtime/modules/libSceAudioOut/nps5_audio_capture.c" ]; then
    cp "$HARNESS"/pkg/runtime/modules/libSceAudioOut/nps5_audio_capture.[ch] "$wt/pkg/runtime/modules/libSceAudioOut/"
    ( cd "$wt" && git -C "$HARNESS" diff ae1075d~1 ae1075d -- pkg/runtime/modules/libSceAudioOut/ps4_audioout.c | git apply 2>"$RUN/capture-overlay-$sha.log" ) \
      || echo "note: audio capture overlay does not apply to $sha; the audio check is skipped there" >&2
  fi
  echo "$wt"
}
rm_wt() { [ "$KEEPWT" = 1 ] || git -C "$HARNESS" worktree remove --force "$1" > /dev/null 2>&1; }

SUBJ="$HARNESS"; SUBJ_WT=""
if [ -n "$REV" ]; then SUBJ_WT="$(make_wt "$REV")" || exit 2; SUBJ="$SUBJ_WT"; fi
RES_A="$RUN/results-subject.tsv"; RES_B="$RUN/results-ref.tsv"
run_suite subject "$SUBJ" "$RES_A"
REF_WT=""
if [ -n "$REF" ]; then
  REF_WT="$(make_wt "$REF")" || exit 2
  run_suite ref "$REF_WT" "$RES_B"
fi
SUBJ_REV="$(git -C "$SUBJ" rev-parse --short HEAD 2>/dev/null)"; REF_REV=""; [ -n "$REF" ] && REF_REV="$(git -C "$REF_WT" rev-parse --short HEAD)"
JSON="$RUNS/regress-$TS.json"
echo
python3 -I "$HARNESS/tools/regress_report.py" final --results "$RES_A" --out "$JSON" --ts "$TS" --mode "$MODE" --rev "$SUBJ_REV" --subject "$SUBJ" --jobs "$J" \
  --wall "$(( $(date +%s) - T_ALL ))" ${REF:+--ref-results "$RES_B" --ref-label "$REF" --ref-rev "$REF_REV"}
RC=$?
[ -n "$SUBJ_WT" ] && rm_wt "$SUBJ_WT"; [ -n "$REF_WT" ] && rm_wt "$REF_WT"
exit $RC
