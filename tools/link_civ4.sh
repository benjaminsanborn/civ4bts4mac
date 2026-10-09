#!/bin/sh
# Link the host executable: runtime + recompiled guest code -> build/civ4
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/rt
for f in runtime/*.c; do
    clang -c -O2 -g -Wall -Wno-unused-function -Iruntime "$f" -o "build/rt/$(basename "$f" .c).o"
done
clang -O2 -g build/rt/*.o build/libgame.a -framework CoreFoundation -Wl,-export_dynamic -o build/civ4
echo "linked build/civ4"
