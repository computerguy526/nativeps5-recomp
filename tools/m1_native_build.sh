#!/bin/bash
# Compile and link a PS4Recomp output directory (made with `ps4-recomp -compile=false -o DIR x.elf`)
# into an arm64 binary.  Adapted from S2's build_manual.sh (work/s2/tools).
#  * Compile flags, source list and link flags are read from DIR/build.ninja, so it follows the emitter.
#  * Runtime objects are cached across tests under $OBJ_CACHE (key = flags + runtime headers + file content).
#  * Ajm needs the real third-party libs: minimp3 (header-only, MINIMP3_IMPLEMENTATION lives in ps4_ajm.c) and
#    libatrac9 (git submodule, C sources compiled here and linked). Init them first:
#      git submodule update --init --depth 1 3rdparty/minimp3 3rdparty/libatrac9
#  * Sources that still fail to compile (GPU .mm, ps4_vfs.c) are skipped; unresolved symbols are filled with
#    zero-returning stubs and listed.  These are bring-up concessions: GPU/audio are out of scope for M1.
# env: NPS5_JOBS|M1_JOBS|NPS5_BUILD_J=<n> compile parallelism (incremental relink is automatic, see below)
# usage: m1_native_build.sh <outdir> <binary>      exit 0 = binary produced
set -u
D="$(cd "$1" && pwd)"; OUT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/.." && pwd)"
CACHE="${OBJ_CACHE:-$HERE/../tests/ps5/build/objcache}"; mkdir -p "$CACHE"
[ -f "$REPO/3rdparty/minimp3/minimp3.h" ] && [ -f "$REPO/3rdparty/libatrac9/C/src/libatrac9.h" ] \
  || { echo "!! FATAL: 3rdparty submodules not initialised: git submodule update --init --depth 1 3rdparty/minimp3 3rdparty/libatrac9"; exit 4; }
cd "$D" || exit 2
[ -f build.ninja ] || { echo "no build.ninja in $D"; exit 2; }
CFLAGS=$(sed -n 's/^cflags = //p' build.ninja | head -1)
LDFLAGS=$(sed -n 's/^ldflags = //p' build.ninja | head -1)
LDFLAGS="$LDFLAGS -lc++ ${NPS5_LDEXTRA:-}"
# NPS5_KEEP_SYMS=1: keep local symbols (drop -Wl,-x) so the performance trace and crash reports can name shims and runtime functions.
[ -n "${NPS5_KEEP_SYMS:-}" ] && LDFLAGS="${LDFLAGS//-Wl,-x/}"
THIRD="-I$REPO/3rdparty/minimp3 -I$REPO/3rdparty/libatrac9/C/src"
GKEY=$( { echo "$CFLAGS $THIRD"; find . \( -name '*.h' -o -name '*.hpp' \) | sort | xargs shasum; } | shasum | cut -c1-16)
export CFLAGS THIRD CACHE GKEY
# Incremental relink: an object that exists, is newer than its source and was built under the same
# flags + headers (GKEY, recorded in .nps5_gkey) is kept. The translator rewrites a generated source
# only when its text changed (code_NNN_reg.c, dispatch.c, ...), so a relink compiles what changed.
KEEP=0; if [ "$(cat .nps5_gkey 2>/dev/null)" = "$GKEY" ]; then KEEP=1; fi
echo "$GKEY" > .nps5_gkey
export KEEP
cc_one() { # $1=src ; prints "FAIL src" on compile failure ; object left next to the source
  src="$1"; obj="${src%.*}.o"; ext="${src##*.}"
  case "$src" in runtime/*) ;; *) if [ "$KEEP" = 1 ] && [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then return 0; fi ;; esac
  case "$ext" in
    c)   cc="clang";   xf="" ;;
    s|S) cc="clang";   xf="" ;;
    m)   cc="clang";   xf="-fobjc-arc" ;;
    mm)  cc="clang++"; xf="-fobjc-arc -std=c++17" ;;
    cpp|cc) cc="clang++"; xf="-std=c++17" ;;
    *) return 0 ;;
  esac
  case "$src" in
    runtime/*) k=$( { echo "$GKEY $src"; cat "$src"; } | shasum | cut -c1-24)
               if [ -f "$CACHE/$k.o" ]; then cp "$CACHE/$k.o" "$obj"; return 0; fi ;;
    *) k="" ;;
  esac
  # shellcheck disable=SC2086
  if $cc $CFLAGS $THIRD $xf -c "$src" -o "$obj" 2> "$obj.err"; then
    [ -n "$k" ] && cp "$obj" "$CACHE/$k.o"; rm -f "$obj.err"
  else echo "SKIP $src"; rm -f "$obj"; fi
}
export -f cc_one
find . -path ./tier1_cache -prune -o -type f \( -name '*.c' -o -name '*.s' -o -name '*.S' -o -name '*.m' -o -name '*.mm' -o -name '*.cpp' \) -print | sed 's|^\./||' | sort \
  | xargs -P "${NPS5_JOBS:-${M1_JOBS:-${NPS5_BUILD_J:-$(sysctl -n hw.ncpu)}}}" -I{} bash -c 'cc_one "$@"' _ {} > compile.skipped
echo "skipped $(wc -l < compile.skipped | tr -d ' ') source file(s) (see $D/compile.skipped)"
rm -f nps5_stubs.o nps5_stubs.c
# libatrac9 (ATRAC9 decoder) from the submodule: one object per source, cached by content.
LIB9=()
for s in "$REPO"/3rdparty/libatrac9/C/src/*.c; do
  o="$CACHE/lib9_$(basename "$s" .c)_$(shasum "$s" | cut -c1-16).o"
  if [ ! -f "$o" ]; then
    clang -target arm64-apple-darwin -O2 -std=c99 -c "$s" -o "$o.tmp" 2> "$o.err" && mv "$o.tmp" "$o" \
      || { echo "!! FATAL: libatrac9 failed to compile: $s (see $o.err)"; exit 4; }
  fi
  LIB9+=("$o")
done
# tier1_cache/ holds run-time fragments (and work directories of interrupted compiles): not part of the image.
OBJS="$(find . -path ./tier1_cache -prune -o -name '*.o' -print | sort) ${LIB9[*]}"
# shellcheck disable=SC2086
clang -target arm64-apple-darwin $LDFLAGS $OBJS -o "$OUT" 2> link.err; rc=$?
if [ $rc -ne 0 ]; then
  grep '^  "_' link.err | sort -u | sed -E 's/^  "_([^"]+)".*/\1/' \
    | awk '/^g_/{print "long long " $1 "[64];"; next} {print "long long " $1 "(void){return 0;}"}' > nps5_stubs.c
  clang -target arm64-apple-darwin -O1 -c nps5_stubs.c -o nps5_stubs.o \
    && clang -target arm64-apple-darwin $LDFLAGS $OBJS nps5_stubs.o -o "$OUT" 2> link2.err; rc=$?
  names=$(sed -E 's/^long long ([A-Za-z0-9_]+).*/\1/' nps5_stubs.c | sort -u)
  echo "!! STUBBED $(echo "$names" | grep -c .) unresolved symbol(s) with zero-returning stubs; relink rc=$rc"
  echo "$names" | sed 's/^/!!   stub: /'
  bad=$(echo "$names" | grep -E '^(recomp_|nps5_|shim_)' || true)
  if [ -n "$bad" ]; then
    echo "!! FATAL: runtime symbols (recomp_*/nps5_*/shim_*) must never be stubbed; they are missing from the link:"
    echo "$bad" | sed 's/^/!!   MISSING: /'
    exit 3
  fi
fi
exit $rc
