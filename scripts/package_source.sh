#!/usr/bin/env bash
# Any_Ps3 — package the source zip for a release.
# Usage: scripts/package_source.sh <output.zip> [tree-dir]
#
# Guarantees (asserted after packaging, script fails otherwise):
#   * NO build outputs (build dirs, *.o, *.so, *.a, *.exe, *.apk, *.zip)
#   * NO key material (*.dkey / *.key / key blobs)
#   * NO local machine config (local.properties), no caches, no logs
set -euo pipefail

TREE="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="$1"
[ -n "$OUT" ] || { echo "usage: $0 <output.zip> [tree-dir]"; exit 1; }

cd "$(dirname "$TREE")"
NAME="$(basename "$TREE")"

# Exclusion list (relative paths + glob patterns, handed to zip -x).
EXCLUDES=(
    "*.git/*" "*.gitignore"
    # build outputs / caches / IDE
    "*/build/*" "*/build-*/*" "*/.gradle/*" "*/.idea/*" "*/.cxx/*" "*/.externalNativeBuild/*"
    "*.o" "*.obj" "*.so" "*.a" "*.dll" "*.exe" "*.apk" "*.zip" "*.jar"
    "*.ninja" "*.ninja_*" "CMakeCache.txt" "cmake_install.cmap" "cmake_install.cmake"
    "*.log" "imgui.ini"
    # machine-local config (SDK paths differ per machine)
    "*/local.properties"
    # key material must never be inside a shipped zip
    "*.dkey" "*.key" "*dkey.bin"
    # runtime data dirs created by the app itself
    "*/data/*" "*/.externalNativeBuild/*"
)

rm -f "$OUT"
zip -r -q -X "$OUT" "$NAME" \
    -x "${EXCLUDES[@]}" \
    || { echo "zip failed"; exit 1; }

echo "== audit: forbidden files inside $OUT =="
BAD=$(unzip -l "$OUT" | awk '{print $4}' | grep -E \
    '(^|/)(build|build-[a-z0-9]+|\.cxx|\.gradle|\.git|data)(/|$)' \
    || true)
BAD2=$(unzip -l "$OUT" | awk '{print $4}' | grep -Ei \
    '\.(o|obj|so|a|dll|exe|apk|zip|dkey|key|log)$' \
    || true)
if [ -n "$BAD" ] || [ -n "$BAD2" ]; then
    echo "AUDIT FAILED — forbidden entries:"; echo "$BAD"; echo "$BAD2"
    rm -f "$OUT"; exit 1
fi
echo "audit OK ($(unzip -l "$OUT" | tail -1 | awk '{print $2}') files)"
echo "packaged: $OUT ($(du -h "$OUT" | cut -f1))"
