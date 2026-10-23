#!/bin/bash
# Any_Ps3 Windows build — static x64 exe with icon via mingw-w64.
set -e

ROOT=/home/z/my-project
MINGW=$ROOT/tools/mingw-root/usr/bin
CXX=$MINGW/x86_64-w64-mingw32-g++-posix
WINDRES=$MINGW/x86_64-w64-mingw32-windres
IMGUI=$ROOT/tools/imgui-master
W=$ROOT/windows
BUILD=$W/build

rm -rf $BUILD
mkdir -p $BUILD

echo "== resources (icon + embedded logo) =="
cp $ROOT/assets/logo_full.png $W/logo_full.png
cp $ROOT/assets/icon_256.png $W/logo_small.png 2>/dev/null || cp $ROOT/assets/logo_full.png $W/logo_small.png
"$WINDRES" $W/anyps3.rc -O coff -o $BUILD/anyps3.res

echo "== core + shell =="
SRC="$ROOT/core/util.cpp $ROOT/core/bytesource.cpp $ROOT/core/minijson.cpp \
$ROOT/core/sfo.cpp $ROOT/core/iso9660.cpp $ROOT/core/thumbs.cpp \
$ROOT/core/game_import.cpp $ROOT/core/cache_manifest.cpp \
$ROOT/core/recompiler.cpp $ROOT/core/lv2.cpp $ROOT/core/library.cpp \
$W/main.cpp \
$IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
$IMGUI/backends/imgui_impl_win32.cpp $IMGUI/backends/imgui_impl_dx11.cpp"

"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Wno-missing-field-initializers \
    -I$ROOT -I$IMGUI -I$IMGUI/backends \
    $SRC $BUILD/anyps3.res \
    -o $BUILD/Any_Ps3.exe \
    -static -static-libgcc -static-libstdc++ \
    -luuid -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi

ls -la $BUILD/Any_Ps3.exe
file $BUILD/Any_Ps3.exe 2>/dev/null || true
echo "== done =="
