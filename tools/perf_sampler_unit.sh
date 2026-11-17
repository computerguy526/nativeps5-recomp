#!/bin/bash
# Stress test of the NPS5_PERF=2 per-thread sampler against thread churn (tests/perf/perf_sampler_stress.c).
# Native arm64 only. Arg 1: thread count (default 4000).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -I"$R/core" "$ROOT/tests/perf/perf_sampler_stress.c" "$R/core/nps5_perf.c" -lpthread \
  -Wl,-undefined,dynamic_lookup -o "$T/perf_sampler_stress"
NPS5_PERF=2 NPS5_PERF_HZ=10000 timeout 300 "$T/perf_sampler_stress" "${1:-4000}"
