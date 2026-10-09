#!/bin/sh
# Build the guest (i386) libstdc++ 4.2.1 + libsupc++ objects the game imports.
# Output: build/guest/libstdcxx/*.o
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/guest/libstdcxx"
mkdir -p "$OUT"
LSC="$ROOT/third_party/libstdcxx"
SUPC="$ROOT/third_party/gcc421/libstdc++-v3/libsupc++"
CXX="clang++ -target i386-apple-macos10.6 -std=gnu++98 -O2 -mdynamic-no-pic -fno-stack-protector -nostdinc++ -w"
INC="-I$ROOT/guest/include -I$ROOT/third_party/gcc421/gcc -I$SUPC -I$LSC/include/c++/4.2.1"

# libstdc++ proper: only the translation units that define symbols the game uses.
for f in string-inst wstring-inst tree list sstream-inst ostream-inst istream-inst istream ios-inst ios ios_init ios_locale \
         ios_failure streambuf-inst streambuf locale locale_init locale-inst localename locale_facets ctype \
         ctype_members codecvt codecvt_members c++locale functexcept stdexcept globals_io misc-inst \
         allocator-inst wlocale-inst numeric_members collate_members messages_members monetary_members \
         time_members basic_file fstream-inst iostream-inst limits; do
    $CXX $INC -c "$LSC/src/$f.cc" -o "$OUT/$f.o"
done
# libsupc++: C++ ABI runtime (exceptions, RTTI, guards, new/delete).
for f in "$SUPC"/*.cc; do
    b=$(basename "$f" .cc)
    [ "$b" = eh_arm ] && continue
    src="$f"
    if [ "$b" = eh_call ]; then
        # clang rejects the (void*) vs (_Unwind_Exception*) redeclaration GCC accepted.
        src="$OUT/eh_call.cc"
        sed '/^__cxa_call_terminate(_Unwind_Exception\* ue_header)$/{s/.*/__cxa_call_terminate(void* ue_header_)/;n;s/^{$/{ _Unwind_Exception* ue_header = (_Unwind_Exception*)ue_header_;/;}' "$f" > "$src"
    fi
    $CXX $INC -I"$SUPC" -c "$src" -o "$OUT/supc_$b.o"
done
# __cxa_demangle
clang -target i386-apple-macos10.6 -O2 -mdynamic-no-pic -fno-stack-protector -w -DHAVE_STDLIB_H -DHAVE_STRING_H -DIN_GLIBCPP_V3 \
    -I"$ROOT/third_party/gcc421/include" -c "$ROOT/third_party/gcc421/libiberty/cp-demangle.c" -o "$OUT/cp-demangle.o"
echo "built $(ls "$OUT"/*.o | wc -l | tr -d ' ') objects in $OUT"
