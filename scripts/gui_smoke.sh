#!/bin/bash
# Any_Ps3 — Windows GUI smoke test under Wine + Xvfb.
# This is verification scaffolding for the sandbox this project was built in
# (wine 10 unpacked from Debian .debs without root); on real Windows you can
# simply run Any_Ps3.exe.
#
# Usage: scripts/gui_smoke.sh <path-to-Any_Ps3.exe> [frames]
# Exit code 0 = window + D3D11 + Dear ImGui rendered `frames` frames.
#
# Environment (override if your layout differs):
#   WINE          wine64 binary                    (default: tools/wine-root)
#   WINEPREFIX    wine prefix                      (default: tools/wine-prefix)
#   XLIB_DIRS     ':'-list with libvulkan.so.1 etc (default: tools/mesa-root)
#   VK_ICD        Vulkan ICD json (winex11 wants one even for X11 apps)
#
# Sandbox notes that cost us debugging time (kept for posterity):
#  - Debian wine's winex11.so links libvulkan.so.1 and refuses to load without
#    a usable Vulkan ICD — hence XLIB_DIRS/VK_ICD.
#  - The first window creation after a prefix update races explorer/service
#    init; we warm the server with `cmd /c exit` + 2s settle.
#  - Redirecting the app's stdout/stderr straight to a file makes wine attach a
#    console session and window creation lands on the wrong desktop; keep the
#    app's stdout on a PIPE (as done below).
set -u
EXE="${1:?usage: gui_smoke.sh <Any_Ps3.exe> [frames]}"
FRAMES="${2:-120}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HERE/tools/wine-prefix}"
WINE_BIN="${WINE:-$HERE/tools/wine-root/usr/lib/wine/wine64}"
WINE_SERVER="$(dirname "$WINE_BIN")/wineserver64"
XLIB_DIRS="${XLIB_DIRS:-$HERE/tools/mesa-root/usr/lib/x86_64-linux-gnu}"
VK_ICD="${VK_ICD:-$HERE/tools/mesa-root/usr/share/vulkan/icd.d/lvp_icd.json}"
export LD_LIBRARY_PATH="$XLIB_DIRS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VK_ICD_FILENAMES="$VK_ICD"

# Fresh-ish server state avoids stale desktop-window poison.
"$WINE_SERVER" -k 2>/dev/null || true
sleep 1

DISP=$(( (RANDOM % 80) + 10 ))
Xvfb ":$DISP" -screen 0 1440x900x24 >/tmp/xvfb_anyps3.log 2>&1 &
XVFB_PID=$!
sleep 2
if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    echo "SMOKE FAIL: Xvfb did not start on :$DISP"
    head -3 /tmp/xvfb_anyps3.log
    exit 1
fi
echo "Xvfb on :$DISP (pid $XVFB_PID)"

# Warm the prefix + wineserver (see notes above).
DISPLAY=":$DISP" "$WINE_BIN" cmd /c exit >/dev/null 2>&1
sleep 2

DISPLAY=":$DISP" ANYPS3_GUI_FRAMES="$FRAMES" "$WINE_BIN" "$EXE" 2>&1 |
    grep -vE "trace:" | head -40 > /tmp/anyps3_gui.log
RC=${PIPESTATUS[0]}
kill "$XVFB_PID" 2>/dev/null
echo "GUI exit code: $RC (expected 0 after $FRAMES frames)"
head -5 /tmp/anyps3_gui.log
exit $RC
