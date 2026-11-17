#!/bin/bash
# Launch a built .app with `open` for N seconds, report whether it presented frames, kill it, and check
# that the game dump was not touched.
#   tools/nps5_app_smoke.sh <App.app> <titleId> <dump dir> [secs] [frames]
# NPS5_HOME (support root, default ~/Library/Application Support/nativeps5) is passed through.
set -u
APP="$1"; TID="$2"; DUMP="$3"; SECS="${4:-10}"; FRAMES="${5:-4}"
HOMEDIR="${NPS5_HOME:-$HOME/Library/Application Support/nativeps5}"; T="$HOMEDIR/$TID"
snap() { find "$DUMP" -print0 | xargs -0 stat -f '%N %z %m %p' | shasum | cut -c1-16; }
before=$(snap)
rm -rf "$T/run" "$T/logs"
open -n --env NPS5_HOME="$HOMEDIR" --env APS5_DUMP_FRAMES="$FRAMES" "$APP"
sleep "$SECS"
pid=$(pgrep -f "$APP/Contents/MacOS/nps5-title" | head -1)
echo "running: ${pid:-NO}"
nf=$(ls "$T"/run/frame_*.bmp 2>/dev/null | wc -l | tr -d ' ')
echo "frames dumped: $nf"; ls "$T"/run 2>/dev/null | head -5
[ -n "$pid" ] && { kill "$pid"; sleep 1; kill -9 "$pid" 2>/dev/null; }
after=$(snap)
[ "$before" = "$after" ] && echo "dump unchanged ($after)" || echo "!!! DUMP CHANGED"
tail -5 "$T/logs/last.log" 2>/dev/null
[ "${nf:-0}" -gt 0 ] && [ "$before" = "$after" ]
