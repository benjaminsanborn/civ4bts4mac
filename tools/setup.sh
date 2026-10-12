#!/bin/sh
# Setup, run before the game is built and linked: choose the game installation
# (it must be the Aspyr Mac port of Beyond the Sword 3.19, the exact build the
# recompiler targets) and whether to install BUG Mod. Shows a window:
#   tools/setup.sh
# or, without a window (scripts, CI):
#   tools/setup.sh --game DIR [--bug none|customassets|mod]
# Writes build/setup.json, points build/game at the installation, and installs,
# switches or removes BUG Mod in the game's user folder (DOCS below). The window
# also checks the build tools (Xcode command line tools, Python, disk space).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SHA256=fec6efc11f8e6065c3e0bd0e5b06d58335fa6c0ce842e505c3cd470bb36143ae
EXE="Beyond the Sword/Civilization IV Beyond the Sword.app/Contents/MacOS/Civilization IV Beyond the Sword"
# The game's user folder; the runtime keeps it here rather than in Documents,
# and moves one found in ~/Documents over once (as the setup app does).
DOCS="$HOME/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword"
if [ ! -e "$DOCS" ] && [ -d "$HOME/Documents/Civilization IV Beyond the Sword" ]; then
    mkdir -p "$(dirname "$DOCS")"
    mv "$HOME/Documents/Civilization IV Beyond the Sword" "$DOCS"
fi
mkdir -p build/setup

GAME= BUG=
while [ $# -gt 0 ]; do
    case $1 in
        --game) GAME=$2; shift 2;;
        --bug) BUG=$2; shift 2;;
        *) echo "usage: $0 [--game DIR [--bug none|customassets|mod]]" >&2; exit 2;;
    esac
done

# The build needs Apple Silicon and Xcode's command line tools (clang, swiftc);
# the setup window checks the rest.
[ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] || { echo "this build needs a Mac with Apple Silicon" >&2; exit 1; }
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find swiftc >/dev/null 2>&1; then
    echo "Xcode's command line tools are needed to build: install them (a window opens), then run this again." >&2
    xcode-select --install 2>/dev/null || true
    exit 1
fi

if [ -z "$GAME" ]; then
    APP=build/setup/Civ4Setup
    if [ ! -x "$APP" ] || [ tools/setup/Setup.swift -nt "$APP" ] || [ tools/setup/BugMod.swift -nt "$APP" ] || [ tools/setup/Movies.swift -nt "$APP" ]; then
        echo "building the setup window..."
        swiftc -parse-as-library -O tools/setup/Setup.swift tools/setup/BugMod.swift tools/setup/Movies.swift -o "$APP"
    fi
    rm -f build/setup/choice.json
    "$APP" --out build/setup/choice.json --root "$ROOT" --current "$(readlink build/game 2>/dev/null || true)" --docs "$DOCS" || true
    [ -s build/setup/choice.json ] || { echo "setup cancelled" >&2; exit 1; }
    GAME=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["game"])' build/setup/choice.json)
    BUG=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["bug"])' build/setup/choice.json)
fi
BUG=${BUG:-none}

[ -f "$GAME/$EXE" ] || { echo "not a Beyond the Sword installation: $GAME" >&2; exit 1; }
got=$(shasum -a 256 "$GAME/$EXE" | cut -d' ' -f1)
[ "$got" = "$SHA256" ] || { echo "unsupported game build (SHA-256 $got); need the Aspyr Mac port, BtS 3.19" >&2; exit 1; }
ln -sfn "$GAME" build/game
echo "game: $GAME"

current=$(python3 tools/bugmod.py status --docs "$DOCS" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("mode", "none"))')
if [ "$BUG" != "$current" ]; then
    [ "$current" = none ] || python3 tools/bugmod.py uninstall --docs "$DOCS"
    [ "$BUG" = none ] || python3 tools/bugmod.py install --docs "$DOCS" --mode "$BUG"
fi
echo "BUG Mod: $BUG"
printf '{"game": "%s", "bug": "%s"}\n' "$GAME" "$BUG" > build/setup.json
