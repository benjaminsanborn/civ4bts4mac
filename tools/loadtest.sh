#!/bin/bash
# Long AI-autoplay load test: a Huge custom game (sandboxed profile), every seat
# played by the AI, for up to $HOURS hours. Writes build/loadtest/:
#   turns.log    "turn epoch" every 30 s (from the game's Python)
#   run.log      the harness log (fps, memory, hitches)
#   sample_*.txt process samples every $SAMPLE_EVERY seconds
# Autosaves (every 25 turns, set in the sandbox CivilizationIV.ini) land in
# build/home/Documents/Civilization IV Beyond the Sword/Saves/single/auto.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
HOURS=${HOURS:-8}
SAMPLE_EVERY=${SAMPLE_EVERY:-600}
OUT=build/loadtest
mkdir -p "$OUT"
rm -f "$OUT"/turns.log "$OUT"/sample_*.txt
TURNS="$ROOT/$OUT/turns.log"

LOG="py open('$TURNS','a').write('%d %.0f\\n'%(__import__('CvPythonExtensions').CyGame().getGameTurn(),__import__('time').time()))"
# Single Player > Custom Game (keyboard), Launch (Return), dismiss Dawn of Man, AI autoplay.
SCRIPT="16:click 720,445;20:keycode 125;21:keycode 125;22:keycode 36;30:keycode 36;80:keycode 36;85:py __import__('CvPythonExtensions').CyGame().setAIAutoPlay(1000)"
end=$((HOURS * 3600))
for ((t = 90; t < end; t += 30)); do SCRIPT="$SCRIPT;$t:$LOG"; done

tools/link_civ4.sh > build/link.log 2>&1 || { grep -A3 error build/link.log | head; exit 1; }
CIV4_LENIENT=1 CIV4_FPS=1 CIV4_MEMSTATS=1 CIV4_MUTE=1 CIV4_PREFS_DOMAIN=civ4.test HOME="$ROOT/build/home" \
    CIV4_INPUT="$SCRIPT" ./build/civ4 > "$OUT/run.log" 2>&1 &
PID=$!
echo "pid $PID"
start=$(date +%s)
last_sample=$start
last_turn=-1
last_change=$start
while kill -0 $PID 2>/dev/null; do
    sleep 15
    now=$(date +%s)
    turn=$(tail -1 "$TURNS" 2>/dev/null | cut -d' ' -f1)
    turn=${turn:-0}
    rss=$(ps -o rss= -p $PID 2>/dev/null | tr -d ' ')
    if [ "$turn" != "$last_turn" ]; then last_turn=$turn; last_change=$now; fi
    echo "@@progress {\"done\": $turn, \"total\": 460, \"msg\": \"rss $((${rss:-0} / 1024)) MB, $(( (now - start) / 60 )) min\", \"ts\": $now}"
    if [ $((now - last_sample)) -ge "$SAMPLE_EVERY" ]; then
        sample $PID 10 -f "$ROOT/$OUT/sample_t${turn}.txt" > /dev/null 2>&1
        last_sample=$now
    fi
    # No turn progress for 30 minutes (game over, or stuck): sample and stop.
    if [ $((now - last_change)) -ge 1800 ] && [ "$turn" -gt 0 ]; then
        sample $PID 10 -f "$ROOT/$OUT/sample_stalled_t${turn}.txt" > /dev/null 2>&1
        echo "no progress for 30 min at turn $turn; stopping"
        break
    fi
    [ $((now - start)) -ge $end ] && { echo "time limit reached at turn $turn"; break; }
done
kill $PID 2>/dev/null
wait $PID 2>/dev/null
echo "exit; last turn $last_turn"
tail -5 "$OUT/run.log"
