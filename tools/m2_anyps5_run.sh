#!/bin/bash
# M2: translate a PS5 title with -gpu anyps5, build it against AnyPS5's arm64 libraries and run it sandboxed
# (it opens a window) with KosmicKrisp, dumping the first presented frames.
#
#   tools/m2_anyps5_run.sh <eboot.elf> [secs] [frames]        (default: 30 s, 8 frames)
#
# Environment:
#   NPS5_ANYPS5_BUILD   AnyPS5 build-arm64 dir built with -DAPS5_HOST_ARM64=ON (targets: libSceAgc libSceAgcDriver libkernel
#                       libc nps5_gpu).  Default: ../anyps5-int/build-arm64 beside the worktree.
#   KK_ICD              KosmicKrisp ICD json (default: work/s3/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json)
#   OUT                 work directory (default tests/ps5/build/m2/<name>)
#   DUMP_SCALE          APS5_DUMP_SCALE divisor for the dumped frames (default 1)
#   M2_EXTRA_ENV        extra space-separated VAR=value pairs for the guest (e.g. NPS5_VPAD_SCRIPT=..., NPS5_PAD_LOG=1)
# Frames land in $OUT/out/frame_NNN.bmp (the AGC driver writes them to the working directory, the application root).
set -eu
export NPS5_AUDIO_NULL="${NPS5_AUDIO_NULL:-1}"   # tests are silent (headless clock-paced audio device)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; SECS="${2:-30}"; FRAMES="${3:-8}"
NAME="$(basename "$(dirname "$ELF")")"
OUT="${OUT:-$ROOT/tests/ps5/build/m2/$NAME}"
export NPS5_ANYPS5_BUILD="${NPS5_ANYPS5_BUILD:-$(cd "$ROOT/../anyps5-int/build-arm64" && pwd)}"
KK_ICD="${KK_ICD:-$ROOT/../../work/s3/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json}"
mkdir -p "$OUT"
( cd "$ROOT" && GOFLAGS="-mod=vendor -buildvcs=false" go build -o "$OUT/ps4-recomp" ./cmd/ps4-recomp )
rm -rf "$OUT/out"
"$OUT/ps4-recomp" -compile=false -gpu anyps5 -o "$OUT/out" "$ELF" > "$OUT/translate.log" 2>&1 || { tail -20 "$OUT/translate.log"; exit 1; }
"$ROOT/tools/m1_native_build.sh" "$OUT/out" "$OUT/native" 2>&1 | grep -v "stub:" || true
cd "$OUT/out"
NPS5_WORKDIR="$OUT/w" NPS5_EXTRA_ENV="VK_DRIVER_FILES=$KK_ICD NPS5_ANYPS5_TRACE=1 APS5_DUMP_FRAMES=$FRAMES APS5_DUMP_SCALE=${DUMP_SCALE:-1} ${M2_EXTRA_ENV:-}" \
  "$ROOT/tools/run_sandboxed.sh" "$SECS" "$OUT/native" > "$OUT/run.out" 2> "$OUT/run.err" || true
ls "$OUT"/out/frame_*.bmp 2>/dev/null | head -"$FRAMES"
