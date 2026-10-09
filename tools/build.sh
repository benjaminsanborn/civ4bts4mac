#!/bin/sh
# Full build: guest libraries -> linked modules -> lifted C -> static library.
#   tools/build.sh [GAME_BINARY]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME=${1:-"$ROOT/build/game/Beyond the Sword/Civilization IV Beyond the Sword.app/Contents/MacOS/Civilization IV Beyond the Sword"}
PY="$ROOT/.venv/bin/python"
cd "$ROOT"
tools/fetch_third_party.sh
echo "== guest libraries"
guest/build_libstdcxx.sh
guest/build_python.sh
echo "== link modules"
(cd tools/recomp && "$PY" world.py "$GAME" "$ROOT/build/guest/libstdcxx" "$ROOT/build/guest/libstdcxx" 0x02000000)
(cd tools/recomp && "$PY" world.py "$GAME" "$ROOT/build/guest/python" "$ROOT/build/guest/python" 0x02100000)
echo "== lift"
(cd tools/recomp && "$PY" gen.py "$GAME" "$ROOT/build/gen" --all \
    --module "$ROOT/build/guest/libstdcxx" --module "$ROOT/build/guest/python")
echo "== compile"
tools/build_gen.sh build/gen build/libgame.a
