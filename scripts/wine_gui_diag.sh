#!/bin/bash
# Diagnostic: watch the Any_Ps3 window appear on the Xvfb display.
W=/home/z/my-project/tools/wine-root
B=/home/z/my-project/windows/build
export LD_LIBRARY_PATH=$W/usr/lib/x86_64-linux-gnu:$W/usr/lib
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
$W/usr/bin/Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
XPID=$!
sleep 2
export DISPLAY=:99
export WINEDLLPATH=$W/usr/lib/x86_64-linux-gnu/wine
export WINEDATADIR=$W/usr/share/wine
export WINEPREFIX=/home/z/my-project/tools/wineprefix
export WINEARCH=win64
export WINEDEBUG=warn+module

$W/usr/lib/wine/wine64 "Z:\\home\\z\\my-project\\windows\\build\\Any_Ps3.exe" \
    > $B/wine_diag.log 2>&1 &
WPID=$!

for i in 1 2 3 4 5 6 7 8; do
    sleep 3
    echo "--- t=$((i*3))s ---"
    $W/usr/bin/xwininfo -root -children 2>&1 | grep -E "Any_Ps3|wine|absolute|children" | head -4
done

$W/usr/bin/xwd -root -silent > $B/shot.xwd 2>/dev/null
$W/usr/bin/xwdtopnm < $B/shot.xwd 2>/dev/null | $W/usr/bin/pnmtopng 2>/dev/null > $B/wine_screenshot.png
echo "--- final screenshot ---"
ls -la $B/wine_screenshot.png

kill $WPID $XPID 2>/dev/null
echo "--- last log lines ---"
tail -6 $B/wine_diag.log
