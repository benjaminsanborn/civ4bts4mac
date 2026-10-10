#!/bin/sh
# Link the host executable: runtime + recompiled guest code -> build/civ4
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/rt
CFLAGS="-O2 -g -Wall -Wno-unused-function -Wno-unused-but-set-variable -Iruntime"
for f in runtime/*.c; do
    o="build/rt/$(basename "$f" .c).o"
    [ "$o" -nt "$f" ] && [ "$o" -nt runtime/shim.h ] || clang -c $CFLAGS "$f" -o "$o"
done
for f in runtime/*.cpp; do
    o="build/rt/$(basename "$f" .cpp).o"
    [ "$o" -nt "$f" ] && [ "$o" -nt runtime/shim.h ] || clang++ -std=gnu++17 -c $CFLAGS "$f" -o "$o"
done
for f in runtime/*.m; do
    o="build/rt/$(basename "$f" .m).o"
    [ "$o" -nt "$f" ] && [ "$o" -nt runtime/shim.h ] || clang -c $CFLAGS -fobjc-arc "$f" -o "$o"
done
clang++ -O2 -g ${LDFLAGS--flto=thin -Wl,-cache_path_lto,build/lto-cache} build/rt/*.o ${LIBGAME:-build/libgame.a} -framework CoreFoundation -framework Cocoa -framework OpenGL \
    -framework CoreGraphics -framework CoreText -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework QuartzCore -framework ImageIO -framework OpenAL -Wl,-export_dynamic -o ${OUT:-build/civ4}
echo "linked ${OUT:-build/civ4}"
