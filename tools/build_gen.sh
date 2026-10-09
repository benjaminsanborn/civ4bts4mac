#!/bin/sh
# Compile a generated directory into a static library: build_gen.sh GENDIR OUT.a
set -e
GEN=$(cd "$1" && pwd); OUT=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$GEN/obj"
export GEN ROOT TOTAL=$(ls "$GEN"/*.c | wc -l | tr -d ' ')
ls "$GEN"/*.c | xargs -P "$(sysctl -n hw.ncpu)" -n 1 sh -c '
  o="$GEN/obj/$(basename "$1" .c).o"
  [ "$o" -nt "$1" ] || clang -c -w -O1 -gline-tables-only ${CFLAGS:-} -I"$ROOT/runtime" "$1" -o "$o" || exit 255
  echo "@@progress {\"done\": $(ls "$GEN"/obj/*.o | wc -l | tr -d " "), \"total\": $TOTAL}"' sh
rm -f "$OUT"; ar rcs "$OUT" "$GEN"/obj/*.o
echo "built $OUT"
