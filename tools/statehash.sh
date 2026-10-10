#!/bin/bash
# Determinism check: load SAVE, let the AI play, and write a per-player state
# summary when game turn TARGET begins to build/bench/state_NAME_TAG.txt (one
# run at a time). Builds that play identically produce identical files.
#   tools/statehash.sh SAVE TARGET TAG     (CIV4_BIN picks the binary)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SAVE=$1; TARGET=$2; TAG=${3:-x}; TURNS=3
NAME=$(basename "$SAVE" .CivBeyondSwordSave)
cp "$SAVE" "build/home/Documents/Civilization IV Beyond the Sword/Saves/single/"
mkdir -p build/bench
OUTF="$ROOT/build/bench/state_${NAME}_$TAG.txt"
rm -f "$OUTF" build/bench/statehash.cfg.err
printf "%s\n%s\n" "$OUTF" "$TARGET" > build/bench/statehash.cfg  # read by statehash.py
SCRIPT="16:click 720,445;20:keycode 126;21:keycode 36;25:key $NAME.CivBeyondSwordSave;29:keycode 36"
for ((t = 40; t < 120 + 120 * TURNS; t += 3)); do SCRIPT="$SCRIPT;$t:keycode 36;$((t + 1)):py execfile('$ROOT/tools/statehash.py',{'CFG':'$ROOT/build/bench/statehash.cfg'})"; done
CIV4_LENIENT=1 CIV4_MUTE=1 CIV4_PREFS_DOMAIN=civ4.test \
    HOME="$ROOT/build/home" CIV4_INPUT="$SCRIPT" "${CIV4_BIN:-./build/civ4}" > "$ROOT/build/bench/state_run_$TAG.log" 2>&1 &
PID=$!
start=$(date +%s)
while kill -0 $PID 2>/dev/null && [ ! -s "$OUTF" ]; do
    sleep 3
    [ $(( $(date +%s) - start )) -gt $((150 + 150 * TURNS)) ] && { echo "timed out"; break; }
done
kill $PID 2>/dev/null; wait $PID 2>/dev/null
[ -s build/bench/statehash.cfg.err ] && tail -3 build/bench/statehash.cfg.err
md5 -q "$OUTF" 2>/dev/null; head -2 "$OUTF" 2>/dev/null
