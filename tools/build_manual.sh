#!/bin/bash
# S2 helper: finish a PS4Recomp output dir whose GPU runtime needs 3rdparty libs we do not have.
# Compiles missing objects (Ajm against the real minimp3/libatrac9 submodules, libatrac9 C sources), skips GPU
# .mm files, links, and auto-generates stubs for unresolved symbols. Local file work only.
# The submodules must be initialised: git submodule update --init --depth 1 3rdparty/minimp3 3rdparty/libatrac9
set -u
D="$1"; OUT="$2"; REPO="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$REPO/3rdparty/minimp3/minimp3.h" ] && [ -f "$REPO/3rdparty/libatrac9/C/src/libatrac9.h" ] \
  || { echo "!! FATAL: 3rdparty submodules not initialised (see header)"; exit 4; }
cd "$D"
INC=$(find runtime -type d | sed 's/^/-I/' | tr '\n' ' ')
FT="-I/opt/homebrew/opt/freetype/include/freetype2 -I/opt/homebrew/opt/libpng/include/libpng16 -I/opt/homebrew/Cellar/brotli/1.2.1/include"
FF="-I/opt/homebrew/Cellar/ffmpeg/9.0.2_1/include"
CF="-target arm64-apple-darwin -mcpu=apple-m1 -DRECOMP_ARM64_ASM=1 -O2 -ffp-contract=off -fvisibility=hidden -Wno-parentheses-equality -I. $INC $FT $FF -I$REPO/3rdparty/minimp3 -I$REPO/3rdparty/libatrac9/C/src"
# The addressing mode is a build flag of the runtime (nps5_mem.h); take it from the manifest.
grep -q '"addr_mode": "identity"' memory_manifest.json 2>/dev/null && CF="$CF -DNPS5_ADDR_MODE_IDENTITY=1"
# nps5: make sure the diagnostics sources have objects (the CLI compile can stop at Ajm/Gnm first)
for c in nps5_buildinfo.c runtime/core/nps5_diag.c runtime/core/recomp_runtime.c runtime/modules/libSceAjm/ps4_ajm.c; do
  o="${c%.c}.o"; [ -f "$c" ] && { [ ! -f "$o" ] || [ "$c" -nt "$o" ]; } && clang $CF -c "$c" -o "$o"
done
for s in "$REPO"/3rdparty/libatrac9/C/src/*.c; do
  clang -target arm64-apple-darwin -O2 -std=c99 -c "$s" -o "lib9_$(basename "$s" .c).o" || exit 4
done
OBJS=$(find . -name '*.o' | sort)
FW="-framework Metal -framework Cocoa -framework QuartzCore -framework GameController -framework AudioToolbox -framework ImageIO -framework MetalFX -framework VideoToolbox -framework CoreMedia -framework CoreVideo -lz"
LIBS="-L/opt/homebrew/opt/freetype/lib -lfreetype -L/opt/homebrew/Cellar/ffmpeg/9.0.2_1/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lc++"
clang -target arm64-apple-darwin -Wl,-dead_strip $FW $LIBS $OBJS -o "$OUT" 2> link.err; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' link.err | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > s2_stubs.c
  clang -target arm64-apple-darwin -O1 -c s2_stubs.c -o s2_stubs.o && clang -target arm64-apple-darwin -Wl,-dead_strip $FW $LIBS $OBJS s2_stubs.o -o "$OUT" 2> link2.err; rc=$?
  echo "stubbed $(wc -l < s2_stubs.c) symbols (GPU modules not built); relink rc=$rc"
fi
echo "link rc=$rc; undefined symbols: $(grep -c '^  "' link.err)"
