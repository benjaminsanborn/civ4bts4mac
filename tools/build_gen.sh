#!/bin/sh
# Compile a generated directory into a static library: build_gen.sh GENDIR OUT.a
# Objects are ThinLTO bitcode: link_civ4.sh then inlines across the generated
# files (tiny getters dominate the AI's time; ~25% faster late-game turns, same
# game state). OBJ=dir keeps objects elsewhere (e.g. for different CFLAGS).
set -e
GEN=$(cd "$1" && pwd); OUT=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OBJ=${OBJ:-$GEN/obj_lto}
CFLAGS=${CFLAGS--flto=thin}
mkdir -p "$OBJ"
export GEN OBJ ROOT CFLAGS TOTAL=$(ls "$GEN"/*.c | wc -l | tr -d ' ')
ls "$GEN"/*.c | xargs -P "$(sysctl -n hw.ncpu)" -n 1 sh -c '
  o="$OBJ/$(basename "$1" .c).o"
  case "$(basename "$1")" in part_*) LANG="-x c++ -std=gnu++17";; *) LANG="";; esac
  [ "$o" -nt "$1" ] || clang $LANG -c -w -O1 -gline-tables-only ${CFLAGS:-} -I"$ROOT/runtime" "$1" -o "$o" || exit 255
  echo "@@progress {\"done\": $(ls "$OBJ"/*.o | wc -l | tr -d " "), \"total\": $TOTAL}"' sh
rm -f "$OUT"; ar rcs "$OUT" "$OBJ"/*.o
echo "built $OUT"
