#!/bin/sh
# Download the third-party sources that are not checked in:
#   libstdc++ 4.2.1 (Apple libstdcxx-52), GCC 4.2.1 libsupc++/unwinder headers/demangler, CPython 2.6.9.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ ! -d "$TP/libstdcxx" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 --branch libstdcxx-52 \
        https://github.com/apple-oss-distributions/libstdcxx "$TP/libstdcxx"
    rm -rf "$TP/libstdcxx/.git"
fi
if [ ! -d "$TP/gcc421" ]; then
    mkdir -p "$TP/gcc421"
    curl -sSfL -o "$TMP/gpp.tar.bz2" https://ftp.gnu.org/gnu/gcc/gcc-4.2.1/gcc-g++-4.2.1.tar.bz2
    curl -sSfL -o "$TMP/core.tar.bz2" https://ftp.gnu.org/gnu/gcc/gcc-4.2.1/gcc-core-4.2.1.tar.bz2
    tar -xjf "$TMP/gpp.tar.bz2" -C "$TP/gcc421" --strip-components=1 \
        gcc-4.2.1/libstdc++-v3/libsupc++ gcc-4.2.1/libstdc++-v3/config/cpu/generic/cxxabi_tweaks.h
    tar -xjf "$TMP/core.tar.bz2" -C "$TP/gcc421" --strip-components=1 \
        gcc-4.2.1/gcc/unwind-pe.h gcc-4.2.1/gcc/unwind-generic.h gcc-4.2.1/gcc/unwind-dw2.c gcc-4.2.1/gcc/unwind-dw2.h \
        gcc-4.2.1/gcc/unwind-dw2-fde.c gcc-4.2.1/gcc/unwind-dw2-fde.h gcc-4.2.1/gcc/unwind-dw2-fde-darwin.c \
        gcc-4.2.1/gcc/unwind.inc gcc-4.2.1/gcc/dwarf2.h gcc-4.2.1/libiberty/cp-demangle.c gcc-4.2.1/libiberty/cp-demangle.h \
        gcc-4.2.1/include/demangle.h gcc-4.2.1/include/libiberty.h gcc-4.2.1/include/ansidecl.h \
        gcc-4.2.1/COPYING gcc-4.2.1/COPYING.LIB
fi
if [ ! -d "$TP/Python-2.6.9" ]; then
    curl -sSfL -o "$TMP/py.tgz" https://www.python.org/ftp/python/2.6.9/Python-2.6.9.tgz
    tar -xzf "$TMP/py.tgz" -C "$TP"
fi
echo "third_party ready"
