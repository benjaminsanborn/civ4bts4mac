#!/bin/sh
# Build the guest (i386) CPython 2.6.9 objects and stage its standard library.
# Output: build/guest/python/*.o, build/pylib/lib/python2.6
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/Python-2.6.9"
OUT="$ROOT/build/guest/python"
mkdir -p "$OUT"
CC="clang -target i386-apple-macos10.6 -O2 -mdynamic-no-pic -fno-stack-protector -fno-strict-aliasing -fwrapv -w"
DEFS="-DPy_BUILD_CORE -DNDEBUG"
INC="-I$ROOT/guest/python -I$SRC/Include -I$SRC/Python"

cc1() { # cc1 SOURCE [extra flags]
    src=$1; shift
    obj="$OUT/$(echo "$src" | sed "s#$SRC/##; s#/#_#g; s#\.c\$#.o#")"
    [ "$obj" -nt "$src" ] && return 0
    $CC $DEFS $INC "$@" -c "$src" -o "$obj"
}

for f in "$SRC"/Parser/*.c; do
    case $(basename "$f") in pgenmain.c|tokenizer_pgen.c|printgrammar.c|intrcheck.c|tokenizer.c) continue;; esac
    cc1 "$f"
done
# Non-ASCII bytes in a source file without a coding declaration are accepted as
# they are, as in the Python 2.4 that Civ4 mods were written for (2.5+ makes it a
# SyntaxError; map scripts such as Planet Generator have them in comments).
sed 's/^    if (badchar) {$/    if (0 \&\& badchar) {/' "$SRC/Parser/tokenizer.c" > "$OUT/tokenizer_lenient.c.new"
grep -q 'if (0 && badchar)' "$OUT/tokenizer_lenient.c.new"
if cmp -s "$OUT/tokenizer_lenient.c.new" "$OUT/tokenizer_lenient.c"; then rm "$OUT/tokenizer_lenient.c.new"
else mv "$OUT/tokenizer_lenient.c.new" "$OUT/tokenizer_lenient.c"; fi
[ "$OUT/Parser_tokenizer.o" -nt "$OUT/tokenizer_lenient.c" ] || $CC $DEFS $INC -I"$SRC/Parser" -c "$OUT/tokenizer_lenient.c" -o "$OUT/Parser_tokenizer.o"
for f in "$SRC"/Objects/*.c; do cc1 "$f"; done
for f in "$SRC"/Python/*.c; do
    case $(basename "$f") in dynload_*|mactoolboxglue.c|sigcheck.c|frozenmain.c|atof.c|dup2.c|getcwd.c|strdup.c|strtod.c) continue;; esac
    # sys.platform: "darwin", as the Mac game's own Python reported (mods such as BUG check it).
    case $(basename "$f") in getplatform.c) cc1 "$f" -DPLATFORM='"darwin"'; continue;; esac
    cc1 "$f"
done
for m in getbuildinfo gcmodule posixmodule signalmodule threadmodule errnomodule _sre _codecsmodule zipimport \
         symtablemodule mathmodule timemodule _randommodule operator itertoolsmodule _struct arraymodule binascii \
         cPickle cStringIO _collectionsmodule _functoolsmodule _weakref stropmodule datetimemodule _bisectmodule \
         _heapqmodule md5 md5module shamodule sha256module sha512module; do
    cc1 "$SRC/Modules/$m.c"
done
# Paths are only defaults; the runtime sets PYTHONHOME.
cc1 "$SRC/Modules/getpath.c" -DPYTHONPATH='":plat-darwin"' -DPREFIX='"/civ4py"' -DEXEC_PREFIX='"/civ4py"' \
    -DVERSION='"2.6"' -DVPATH='""'
$CC $DEFS $INC -c "$ROOT/guest/python/config.c" -o "$OUT/config.o"

# Standard library (pure Python), as the system Python 2.6 framework used to provide it.
mkdir -p "$ROOT/build/pylib/lib"
rsync -a --delete --exclude test --exclude '*.pyc' "$SRC/Lib/" "$ROOT/build/pylib/lib/python2.6/"
# Stand-ins for C modules the guest doesn't have, so socket/urllib/webbrowser import.
cp "$ROOT"/guest/python/*.py "$ROOT/build/pylib/lib/python2.6/"
echo "built $(ls "$OUT"/*.o | wc -l | tr -d ' ') objects in $OUT"
