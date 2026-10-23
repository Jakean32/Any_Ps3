#!/bin/bash
# Build the core test suite as a Windows x64 console exe (mingw, static)
# and run it under Wine. Verifies the exact Windows build of the core.
set -e
ROOT=/home/z/my-project
M=$ROOT/tools/mingw-root/usr/bin
CXX=$M/x86_64-w64-mingw32-g++-posix
OUT=$ROOT/windows/build
mkdir -p $OUT

$CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -Wno-missing-field-initializers -mconsole \
    -I$ROOT \
    $ROOT/core/util.cpp $ROOT/core/bytesource.cpp $ROOT/core/minijson.cpp \
    $ROOT/core/sfo.cpp $ROOT/core/iso9660.cpp $ROOT/core/thumbs.cpp \
    $ROOT/core/game_import.cpp $ROOT/core/cache_manifest.cpp \
    $ROOT/core/recompiler.cpp $ROOT/core/lv2.cpp $ROOT/core/library.cpp \
    $ROOT/tests/core_tests.cpp \
    -o $OUT/anyps3_tests.exe \
    -static -static-libgcc -static-libstdc++
ls -la $OUT/anyps3_tests.exe

W=$ROOT/tools/wine-root
export LD_LIBRARY_PATH=$W/usr/lib/x86_64-linux-gnu:$W/usr/lib
export WINEPREFIX=$ROOT/tools/wineprefix
export WINEARCH=win64
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG=-all
echo "== running test suite under wine =="
$W/usr/lib/wine/wine64 $OUT/anyps3_tests.exe 2>/dev/null
