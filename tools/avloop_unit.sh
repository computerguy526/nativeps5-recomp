#!/bin/bash
# Host stress test of the AvPlayer loop path (tests/avloop/avloop_unit.cpp): a generated clip looped NPS5_AVLOOP_COUNT
# (default 50) times with 4 decode threads, no guest, no game data. Needs ffmpeg (libx264) and its development files
# via pkg-config; skips (exit 0) when they are missing.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
LIBS="libavcodec libavformat libswscale libswresample libavutil"
command -v ffmpeg >/dev/null && pkg-config --exists $LIBS || { echo "avloop_unit: SKIP (ffmpeg development files not found)"; exit 0; }
INC=$(find pkg/runtime -type d | sed 's/^/-I/' | tr '\n' ' ')
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang++ -std=c++17 -O1 -g -w $INC $(pkg-config --cflags $LIBS) tests/avloop/avloop_unit.cpp -o "$T/avloop" $(pkg-config --libs $LIBS) || exit 1
"$T/avloop"; rc=$?
[ $rc -ne 0 ] && echo "avloop_unit: exit status $rc"
exit $rc
