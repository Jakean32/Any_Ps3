#!/usr/bin/env bash
# Any_Ps3 — cross-build the Windows host (Any_Ps3.exe) with MinGW-w64.
# Produces: build/windows-mingw/Any_Ps3.exe (fully static C++ runtime).
#
# Toolchain discovery:
#   1. x86_64-w64-mingw32-g++ on PATH
#   2. tools/mingw64/bin (unpacked from Debian mingw packages, see README)
#
# Usage: scripts/build_windows_mingw.sh [build-dir]
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build/windows-mingw}"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 \
   && [ ! -x tools/mingw64/usr/bin/x86_64-w64-mingw32-g++ ]; then
  cat <<'EOF'
No MinGW-w64 toolchain found. To install without root on Debian/Ubuntu:

    mkdir -p tools/dl && cd tools/dl
    apt-get download mingw-w64-common mingw-w64-x86-64-dev \
        gcc-mingw-w64-base gcc-mingw-w64-x86-64 \
        g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64
    mkdir -p ../mingw64
    for d in *.deb; do dpkg-deb -x "$d" ../mingw64; done
EOF
  exit 1
fi

if [ -x tools/mingw64/usr/bin/x86_64-w64-mingw32-g++ ] \
   && ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  export PATH="$PWD/tools/mingw64/usr/bin:$PATH"
fi

cmake -S hosts/windows -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/scripts/mingw-w64-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "== built: $BUILD_DIR/Any_Ps3.exe =="
file "$BUILD_DIR/Any_Ps3.exe"
