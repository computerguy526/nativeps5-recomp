#!/bin/bash
# Offline test of NPS5_AVPLAYER_VT (tests/avplayer_vt/avplayer_vt_unit.cpp): generates small SYNTHETIC H.264 clips with the
# ffmpeg CLI (test pattern, B-frames; one 1080p clip whose coded height is cropped from 1088), decodes each with software
# and with VideoToolbox, compares frames. Needs ffmpeg with libx264 and pkg-config. No game content.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="${TMPDIR:-/tmp}/avplayer_vt_unit.$$"; mkdir -p "$T"; trap 'rm -rf "$T"' EXIT
FFM=$(command -v ffmpeg) || { echo "avplayer_vt_unit: no ffmpeg, skipped"; exit 0; }
"$FFM" -hide_banner -encoders 2>/dev/null | grep -q libx264 || { echo "avplayer_vt_unit: ffmpeg has no libx264, skipped"; exit 0; }
clang++ -std=c++17 -O1 -Wall -Wno-deprecated-declarations $(pkg-config --cflags libavformat libavcodec libswscale libavutil) \
  "$ROOT/tests/avplayer_vt/avplayer_vt_unit.cpp" $(pkg-config --libs libavformat libavcodec libswscale libavutil) -o "$T/t"
rc=0
for spec in "1280x720 high" "1920x1080 high" "640x360 main"; do
  set -- $spec
  "$FFM" -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$1:rate=30:duration=2" -c:v libx264 -profile:v "$2" -pix_fmt yuv420p \
    -bf 2 -g 24 -preset veryfast -movflags +faststart "$T/c.mp4"
  echo "== $1 $2"; "$T/t" "$T/c.mp4" 2 || rc=1
done
exit $rc
