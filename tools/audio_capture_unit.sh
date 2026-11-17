#!/bin/bash
# Host unit test of the NPS5_AUDIO_CAPTURE writer. Native arm64 only; temp dir under /tmp.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; M="$ROOT/pkg/runtime/modules/libSceAudioOut"
OUT="${TMPDIR:-/tmp}/audio_capture_unit.$$"
clang -O1 -g -Wall -I"$M" "$ROOT/tests/hlesys/audio_capture_unit.c" "$M/nps5_audio_capture.c" -o "$OUT" -lm
"$OUT"; rc=$?; rm -f "$OUT"; exit $rc
