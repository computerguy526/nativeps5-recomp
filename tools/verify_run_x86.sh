#!/bin/bash
# usage: verify_run_x86.sh <label> <secs> [pad-file]
# Rosetta reference run of a relinked title under the x86_64 AnyPS5 build. POLICY (owner-approved): x86 code may run
# ONLY for the open-source homebrew references (Calculator3D, Syobon, Greenhollow) and self-built test ELFs. This
# script refuses anything else: the real path of <V>/<label>/run/eboot must be under one of the allowlisted
# directories below, and a path (or label) that mentions the PPSA30552 dump is refused outright.
# Expects <V>/<label>/run/{eboot,libs,app0} (see VERIFY.md). env -i, HOME/TMPDIR redirected to <V>/<label>/home,
# killed by timeout. Frames: <V>/<label>/frames_x86 (every EVERY-th present).
# The pad timeline is injected when frame_000.bmp appears, exactly as verify_run.sh does.
# X86_CHECK_ONLY=1: only run the policy check (exit 0 = allowed), execute nothing.
set -u
[ $# -ge 2 ] || { echo "usage: verify_run_x86.sh <label> <secs> [pad-file]" >&2; exit 2; }
T=$1; S=$2; PAD=${3:-}
NPS=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}
V=${VERIFY_DIR:-$NPS/work/verify}
R=$NPS/work/s3/rosetta
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- policy: explicit allowlist of directories an x86 eboot may live under (resolved real paths)
ALLOW=(
  "$V/calculator3d" "$V/calc3d" "$V/syobon" "$V/greenhollow"        # open-source homebrew references
  "$NPS/testbed"                                                      # the homebrew testbed sources/builds
  "$ROOT/tests/ps5/build"                                             # self-built test ELFs
)
refuse() { echo "verify_run_x86: REFUSED: $1" >&2; exit 3; }
shopt -s nocasematch
case "$T" in */*|*..*|"") refuse "label '$T' must be a plain directory name" ;; esac
[[ "$T" == *ppsa30552* ]] && refuse "label mentions the PPSA30552 dump (never run x86 for it)"
EB="$V/$T/run/eboot"
[ -f "$EB" ] || refuse "no eboot at $EB"
# Resolve the FINAL file, following every symlink (directory components and the file itself, chains included).
resolve_final() {
  local p=$1 n=0 d t
  while :; do
    d=$(cd "$(dirname "$p")" 2>/dev/null && pwd -P) || return 1
    p=$d/$(basename "$p")
    if [ -L "$p" ]; then
      t=$(readlink "$p") || return 1
      case "$t" in /*) p=$t ;; *) p=$d/$t ;; esac
      n=$((n+1)); [ $n -le 40 ] || return 1
    else
      printf '%s\n' "$p"; return 0
    fi
  done
}
[ -L "$EB" ] && refuse "eboot is a symlink: $EB"
REAL=$(resolve_final "$EB") || refuse "cannot resolve $EB"
for sub in libs app0; do
  if [ -e "$V/$T/run/$sub" ]; then
    RS=$(resolve_final "$V/$T/run/$sub") || refuse "cannot resolve run/$sub"
    [[ "$RS" == *ppsa30552* ]] && refuse "run/$sub resolves into the PPSA30552 dump: $RS"
  fi
done
[[ "$REAL" == *ppsa30552* || "$EB" == *ppsa30552* ]] && refuse "path mentions the PPSA30552 dump: $REAL"
OK=0
for a in "${ALLOW[@]}"; do
  ra=$(cd "$a" 2>/dev/null && pwd -P) || continue
  if [[ "$REAL" == "$ra"/* ]]; then OK=1; break; fi
done
# the run directory is normally a copy under $V/<label>; the label itself must be an approved homebrew name then
if [ $OK = 0 ]; then
  case "$(echo "$T" | tr 'A-Z' 'a-z')" in calculator3d|calc3d|syobon|greenhollow|ps5_test_*|selftest_*) [[ "$REAL" == "$(cd "$V" && pwd -P)"/* ]] && OK=1 ;; esac
fi
[ $OK = 1 ] || refuse "$REAL is not under an approved homebrew/self-built directory (allowlist: ${ALLOW[*]})"
shopt -u nocasematch
[ "${X86_CHECK_ONLY:-}" = 1 ] && { echo "verify_run_x86: allowed: $REAL"; exit 0; }

D=$V/$T; rm -rf "$D/home" "$D/frames_x86" "$D/padcmd.txt" "$D"/run/frame_*.bmp; mkdir -p "$D/home"
[ -n "${SEED_DATA:-}" ] && cp -R "$SEED_DATA" "$D/home/data"
if [ -n "$PAD" ]; then ( while [ ! -f "$D/run/frame_000.bmp" ]; do sleep 0.05; done; python3 -I -c 'import time;print(time.time())' > "$D/pad_start.txt"; cp "$PAD" "$D/padcmd.txt" ) & fi
cd "$D/run" || exit 1; s=$(date +%s)
env -i PATH=/usr/bin:/bin HOME="$D/home" TMPDIR="$D/home" \
  SDL_VULKAN_LIBRARY="$R/vkloader/build/loader/libvulkan.1.dylib" VK_DRIVER_FILES="$R/kk-x86/build/outputs/kosmickrisp_mesa_icd.json" \
  ANYPS5_LANGUAGE=en-US APS5_DUMP_FRAMES="${FRAMES:-60}" APS5_DUMP_SCALE="${DSCALE:-1}" APS5_DUMP_FRAMES_EVERY="${EVERY:-60}" ${PAD:+APS5_PAD_INPUT_FILE="$D/padcmd.txt"} ${EXTRA:-} \
  /opt/homebrew/bin/gtimeout "$S" arch -x86_64 ./eboot </dev/null > "$D/run.out" 2> "$D/run.err"
echo "rc=$? elapsed=$(( $(date +%s)-s ))s"; wait
mkdir -p "$D/frames_x86"; mv "$D"/run/frame_*.bmp "$D/frames_x86/" 2>/dev/null; ls "$D/frames_x86" | wc -l
