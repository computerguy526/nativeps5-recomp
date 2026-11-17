#!/bin/bash
# usage: verify_run.sh <title> <secs> [pad-file]   (arm64 native run of a translated homebrew title)
# Runs <V>/<title>/native in <V>/<title>/out under run_sandboxed.sh with NPS5_AUDIO_CAPTURE=<V>/<title>/audio
# (headless NPS5_AUDIO_NULL clock device), the pad timeline injected when frame_000.bmp appears,
# and presented frames dumped (APS5_DUMP_FRAMES_EVERY) into <V>/<title>/frames_arm.
# V defaults to work/verify next to the repo; override with VERIFY_DIR. The run ends by timeout.
T=$1; S=$2; PAD=${3:-}
R="$(cd "$(dirname "$0")/.." && pwd)"
V=${VERIFY_DIR:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/verify}
export NPS5_ANYPS5_BUILD=${NPS5_ANYPS5_BUILD:-${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/wt/anyps5-int/build-arm64}
KK=${NPS5_ROOT:?set NPS5_ROOT (see BUILDING.md)}/work/s3/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
D=$V/$T; rm -rf $D/w $D/audio $D/frames_arm $D/out/frame_*.bmp $D/padcmd.txt; mkdir -p $D/w $D/audio
[ -n "${SEED_DATA:-}" ] && { mkdir -p $D/w/home; cp -R "$SEED_DATA" $D/w/home/data; } # optional pre-existing save data
# the pad timeline starts when the first frame is dumped (same sync point as verify_run_x86.sh)
if [ -n "$PAD" ]; then ( while [ ! -f $D/out/frame_000.bmp ]; do sleep 0.05; done; python3 -I -c 'import time;print(time.time())' > $D/pad_start.txt; cp "$PAD" $D/padcmd.txt ) & fi
cd $D/out; s=$(date +%s)
NPS5_WORKDIR=$D/w NPS5_EXTRA_ENV="VK_DRIVER_FILES=$KK NPS5_AUDIO_NULL=1 NPS5_AUDIO_CAPTURE=$D/audio PS4_AUDIO_LOG=1 APS5_DUMP_FRAMES=${FRAMES:-60} APS5_DUMP_SCALE=${DSCALE:-1} APS5_DUMP_FRAMES_EVERY=${EVERY:-60} ${PAD:+APS5_PAD_INPUT_FILE=$D/padcmd.txt} $EXTRA" \
  $R/tools/run_sandboxed.sh $S $D/native > $D/run.out 2> $D/run.err
echo "rc=$? elapsed=$(( $(date +%s)-s ))s"; wait
mkdir -p $D/frames_arm; mv $D/out/frame_*.bmp $D/frames_arm/ 2>/dev/null; ls $D/frames_arm | wc -l
