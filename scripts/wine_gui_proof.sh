#!/bin/bash
# One-shot Wine GUI proof: fresh Xvfb + Any_Ps3.exe + window tree + screenshot.
set -e
W=/home/z/my-project/tools/wine-root
B=/home/z/my-project/windows/build
export LD_LIBRARY_PATH=$W/usr/lib/x86_64-linux-gnu:$W/usr/lib
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

# fresh Xvfb, keep it for this script's lifetime
$W/usr/bin/Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
XPID=$!
sleep 2

export DISPLAY=:99
export WINEDLLPATH=$W/usr/lib/x86_64-linux-gnu/wine
export WINEDATADIR=$W/usr/share/wine
export WINEPREFIX=/home/z/my-project/tools/wineprefix
export WINEARCH=win64
export WINEDEBUG=-all

$W/usr/lib/wine/wine64 "Z:\\home\\z\\my-project\\windows\\build\\Any_Ps3.exe" \
    > $B/wine_gui.log 2>&1 &
WPID=$!
sleep 10

echo "=== window tree (proof of mapped window) ==="
$W/usr/bin/xwininfo -root -tree 2>/dev/null | sed -n '1,12p'

echo "=== screenshot capture ==="
$W/usr/bin/xwd -root -silent > $B/shot.xwd
$W/usr/bin/xwdtopnm < $B/shot.xwd 2>/dev/null | $W/usr/bin/pnmtopng 2>/dev/null \
    > $B/wine_screenshot.png
ls -la $B/wine_screenshot.png

kill $WPID $XPID 2>/dev/null || true
sleep 1
echo "=== app log (should only contain harmless wayland warning) ==="
head -3 $B/wine_gui.log
