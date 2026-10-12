#!/bin/sh
# Full build: guest libraries -> linked modules -> lifted C -> static library.
#   tools/build.sh [--setup] [GAME_BINARY]
# Runs tools/setup.sh first (choose the game installation, BUG Mod) if it
# hasn't been run yet, or with --setup.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ "$1" = --setup ] || [ ! -f "$ROOT/build/setup.json" ] || [ ! -e "$ROOT/build/game" ]; then
    [ "$1" = --setup ] && shift
    "$ROOT/tools/setup.sh"
fi
GAME=${1:-"$ROOT/build/game/Beyond the Sword/Civilization IV Beyond the Sword.app/Contents/MacOS/Civilization IV Beyond the Sword"}
PY="$ROOT/.venv/bin/python"
cd "$ROOT"
# The recompiler's Python environment (capstone disassembles the game).
if ! "$PY" -c 'import capstone' 2>/dev/null; then
    echo "== python environment (.venv)"
    python3 -m venv .venv
    "$PY" -m pip install -q capstone==5.0.7
fi
tools/fetch_third_party.sh
echo "== guest libraries"
guest/build_libstdcxx.sh
guest/build_python.sh
echo "== link modules"
(cd tools/recomp && "$PY" world.py "$GAME" "$ROOT/build/guest/libstdcxx" "$ROOT/build/guest/libstdcxx" 0x02000000)
(cd tools/recomp && "$PY" world.py "$GAME" "$ROOT/build/guest/python" "$ROOT/build/guest/python" 0x02100000)
echo "== lift"
(cd tools/recomp && "$PY" gen.py "$GAME" "$ROOT/build/gen" --all \
    --module "$ROOT/build/guest/libstdcxx" --module "$ROOT/build/guest/python" --overrides "$ROOT/runtime")
echo "== compile"
tools/build_gen.sh build/gen build/libgame.a
