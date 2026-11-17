#!/bin/bash
# Host test and micro-benchmark of the guest clocks (tests/clock/clock_unit.c, docs/m3/clock.md).
# Native arm64 only; no guest code runs. `tools/clock_unit.sh bench` runs the benchmark only.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
clang -O2 -g ${CLOCK_UNIT_CFLAGS:-} -Wall -Wno-unused-function -Wno-unused-variable -ffp-contract=off -I"$R/core" -I"$R/kernel/syscalls" -I"$R/kernel/vfs" \
  -I"$R/modules/libkernel" -I"$R/modules/libc" -I"$R/modules/libSceFiber" -I"$R/kernel/threading" "$ROOT/tests/clock/clock_unit.c" -lpthread \
  ${CLOCK_UNIT_LD:--Wl,-undefined,dynamic_lookup} -o "$T/clock_unit"
timeout 120 "$T/clock_unit" "$@"
