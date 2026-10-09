#!/bin/sh
# Map a host PC inside recompiled code to the original x86 instruction:
#   tools/guestpc.sh PC SLIDE
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOAD=$(printf '0x%x' $((0x100000000 + 0x$2)))
LOC=$(atos -o "$ROOT/build/civ4" -l "$LOAD" "$1")
echo "$LOC"
FILE=$(echo "$LOC" | sed -nE 's/.*\((part_[0-9]+\.c):([0-9]+)\).*/\1/p')
LINE=$(echo "$LOC" | sed -nE 's/.*\((part_[0-9]+\.c):([0-9]+)\).*/\2/p')
[ -n "$FILE" ] || exit 0
START=$((LINE > 12 ? LINE - 12 : 1))
sed -n "${START},${LINE}p" "$ROOT/build/gen/$FILE" | grep -E '^\s*/\* [0-9a-f]+:' | tail -6
sed -n "${LINE}p" "$ROOT/build/gen/$FILE"
