#!/bin/bash
# Load-test benchmark: load a save (sandboxed profile), let the AI play every
# seat for $TURNS turns, and report seconds per turn.
#   tools/bench.sh saves/huge12_AD1700.CivBeyondSwordSave [TURNS]
# The save is copied into the sandbox's Saves/single and loaded through the
# Load Game screen. Results go to build/bench/.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SAVE=$1
TURNS=${2:-10}
[ -f "$SAVE" ] || { echo "usage: $0 SAVE [TURNS]"; exit 1; }
NAME=$(basename "$SAVE" .CivBeyondSwordSave)
cp "$SAVE" "build/home/Documents/Civilization IV Beyond the Sword/Saves/single/"
OUT=build/bench
mkdir -p "$OUT"
TL="$ROOT/$OUT/turns_$NAME.log"
rm -f "$TL"
LOG="py open('$TL','a').write('%d %.2f\\n'%(__import__('CvPythonExtensions').CyGame().getGameTurn(),__import__('time').time()))"
# Single Player > Load Game, type the name, Return; then AI autoplay.
SCRIPT="16:click 720,445;20:keycode 126;21:keycode 36;25:key $NAME.CivBeyondSwordSave;29:keycode 36"
START="py (lambda g: g.getGameTurn() > 0 and g.getAIAutoPlay() == 0 and g.setAIAutoPlay($TURNS))(__import__('CvPythonExtensions').CyGame())"
# Dismiss any popup once loaded, then start autoplay (only once a game is running).
for ((t = 60; t < 240; t += 10)); do SCRIPT="$SCRIPT;$t:keycode 36;$((t + 2)):$START"; done
for ((t = 61; t < 240 + 60 * TURNS; t += 2)); do SCRIPT="$SCRIPT;$t:$LOG"; done

CIV4_LENIENT=1 CIV4_FPS=1 CIV4_MUTE=1 CIV4_PREFS_DOMAIN=civ4.test HOME="$ROOT/build/home" \
    CIV4_INPUT="$SCRIPT" "${CIV4_BIN:-./build/civ4}" > "$OUT/run_$NAME.log" 2>&1 &
PID=$!
start=$(date +%s)
first=""
while kill -0 $PID 2>/dev/null; do
    sleep 2
    [ -s "$TL" ] || { [ $(( $(date +%s) - start )) -gt 240 ] && { echo "save did not load"; break; }; continue; }
    first=${first:-$(head -1 "$TL" | cut -d' ' -f1)}
    cur=$(tail -1 "$TL" | cut -d' ' -f1)
    echo "@@progress {\"done\": $((cur - first)), \"total\": $TURNS, \"ts\": $(date +%s)}"
    [ $((cur - first)) -ge "$TURNS" ] && break
    [ $(( $(date +%s) - start )) -gt $((120 + 120 * TURNS)) ] && { echo "timed out"; break; }
done
kill $PID 2>/dev/null
wait $PID 2>/dev/null
# Per-turn seconds (time between the first log of consecutive turns).
awk '$1 != t {if (t != "") printf "turn %d: %.1f s\n", t, $2 - s; t = $1; s = $2}' "$TL"
