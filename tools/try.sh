#!/bin/sh
# Relink and run the game for up to $SECS seconds (default 60); log to build/run.log.
# Set CIV4_HOME=$HOME to run against the real profile.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
tools/link_civ4.sh > build/link.log 2>&1 || { grep -A3 error build/link.log | head -30; exit 1; }
# Test runs use a sandboxed profile (build/home: intro off, logging on), not ~/Documents.
CIV4_PREFS_DOMAIN=${CIV4_PREFS_DOMAIN-civ4.test} CIV4_MUTE=${CIV4_MUTE-1} HOME="${CIV4_HOME:-$ROOT/build/home}" ./build/civ4 "$@" > build/run.log 2>&1 &
PID=$!
for i in $(seq 1 ${SECS:-60}); do
    kill -0 $PID 2>/dev/null || break
    [ $((i % 10)) -eq 0 ] && echo "@@progress {\"done\": $i, \"total\": ${SECS:-60}, \"ts\": $(date +%s)}"
    sleep 1
done
kill $PID 2>/dev/null && echo "[try] killed after ${SECS:-60}s" >> build/run.log
wait $PID 2>/dev/null
grep -vE "CFBundleGetFunctionPointer|NSIsSymbolNameDefined|^    (rt_call|main|start|backtrace_host|on_trap|on_crash|rt_unimpl|rt_import|_sigtramp|rt_fatal|dispatch_from|sh_RunApplicationEventLoop)\+" build/run.log | tail -${TAIL:-30}
