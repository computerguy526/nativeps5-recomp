#!/bin/bash
# Build and run the host unit test of the m2-hlesys system libraries (tests/hlesys/hlesys_unit.c):
# SaveData round trip, AudioOut2 on the headless device (no audio hardware), Ampr/Apr reads and
# events. Native arm64 only; no guest/x86 code is executed; all files live under a temp dir.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/pkg/runtime"
OUT="${TMPDIR:-/tmp}/hlesys_unit.$$"
INC="-I$R/core -I$R/kernel/vfs -I$R/kernel/equeue -I$R/kernel/syscalls -I$R/kernel/threading -I$R/kernel/sync -I$R/kernel/memory -I$R/modules/libSceSaveData -I$R/modules/libSceAudioOut -I$R/modules/libSceHlesys -I$R/modules/libSceAmpr -I$R/modules/libSceJson2 -I$R/modules/libSceAnyPS5"
clang -O1 -g -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-label $INC \
  "$ROOT/tests/hlesys/hlesys_unit.c" \
  "$R/modules/libSceSaveData/ps4_savedata.c" "$R/modules/libSceAudioOut/ps4_audioout.c" \
  "$R/modules/libSceAudioOut/nps5_audioout2.c" "$R/modules/libSceAudioOut/nps5_audio_capture.c" "$R/modules/libSceAmpr/nps5_ampr.c" "$R/modules/libSceAstroStubs/nps5_astro_stubs.c" "$R/modules/libSceJson2/ps4_json2.c" \
  "$R/modules/libSceHlesys/nps5_hlesys.c" \
  "$R/kernel/vfs/ps4_vfs.c" "$R/kernel/vfs/ps4_pak.c" "$R/core/ps4_sfo.c" "$R/core/ps4_log.c" \
  "$R/kernel/equeue/ps4_equeue.c" \
  -framework AudioToolbox -framework CoreFoundation -o "$OUT"
NPS5_AUDIO_NULL=1 "$OUT"; rc=$?; rm -f "$OUT"; exit $rc
