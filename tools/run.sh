#!/bin/sh
# Relink the runtime and run, printing only the interesting tail.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
tools/link_civ4.sh >/dev/null
./build/civ4 "$@" 2>&1 | grep -vE "^    (rt_call|main|start|backtrace_host|on_trap|on_crash|rt_unimpl|rt_import|_sigtramp)\+" | tail -${TAIL:-25}
