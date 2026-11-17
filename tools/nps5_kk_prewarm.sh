#!/bin/bash
# Builds the prebuilt Metal libraries of a title's GPU cache (docs/nps5-pipeline-cache.md in AnyPS5, "Prebuilt Metal libraries").
# A launch under -gpu anyps5 collects every Metal library the cache missed as MSL (APS5_KK_COLLECT_MSL=1 is the runtime default);
# this compiles them to <cache>/<version>/kk-metallib, which the next launch loads with KK_METALLIB_DIR (no MSL -> AIR compile,
# so even an evicted OS Metal cache costs nothing). Needs the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`).
#   tools/nps5_kk_prewarm.sh <titleId | cache dir>
# <titleId> means ~/Library/Application Support/nativeps5/<titleId>/gpu-cache.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARG="${1:?usage: nps5_kk_prewarm.sh <titleId | cache dir>}"
case "$ARG" in */*) CACHE="$ARG";; *) CACHE="$HOME/Library/Application Support/nativeps5/$ARG/gpu-cache";; esac
ANYPS5="${NPS5_ANYPS5_BUILD:-$ROOT/../anyps5-int/build-arm64}/.."
BUILD="${KK_METALLIB_BUILD_SCRIPT:-$ANYPS5/tools/kk-metallib/build_metallib.sh}"
[ -x "$BUILD" ] || { echo "build_metallib.sh not found at $BUILD (set KK_METALLIB_BUILD_SCRIPT)"; exit 2; }
n=0
for msl in "$CACHE"/*/kk-msl; do
  [ -d "$msl" ] || continue
  "$BUILD" "$msl" "${msl%/kk-msl}/kk-metallib"; n=$((n+1))
done
[ "$n" -gt 0 ] || { echo "nothing to build: no kk-msl directory below $CACHE (run the title once first)"; exit 1; }
