#!/usr/bin/env bash
# Final Wine verification pass on the shipping Windows exe.
# Usage: scripts/wine_final_verify.sh <exe> <outdir>
set -u
EXE="$1"
OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$HERE/wine-run.sh"
export DISPLAY=:99
export LD_LIBRARY_PATH="/tmp/my-project/tools/xdotool-root/usr/lib/x86_64-linux-gnu:/tmp/my-project/tools/xlibs-root/usr/lib/x86_64-linux-gnu"
X=/tmp/my-project/tools/xdotool-root/usr/bin/xdotool

mkdir -p "$OUT"; cd "$OUT"
rm -rf "$OUT/pkg" "$OUT"/*.png "$OUT"/selftest.log "$OUT"/*.txt
mkdir -p pkg/samples
cp "$EXE" pkg/Any_Ps3.exe
cp "$(dirname "$HERE")/hosts/windows/samples/hello.elf" pkg/samples/hello.elf

pkill -f 'Xvfb :99' 2>/dev/null; pkill -f Any_Ps3.exe 2>/dev/null; sleep 0.5
Xvfb :99 -screen 0 1440x900x24 >/tmp/xvfb_fv.log 2>&1 &
sleep 2

echo "== 1) --selftest =="
(cd pkg && timeout 240 "$WINE" Any_Ps3.exe --selftest >/dev/null 2>&1)
SELFTEST=$?
echo "selftest exit: $SELFTEST (expect 0)"
grep -c 'Hello from Any_Ps3' pkg/selftest.log | xargs echo "TTY 'Hello' lines:"
tail -1 pkg/selftest.log

echo "== 2) --diag-test =="
(cd pkg && timeout 240 "$WINE" Any_Ps3.exe --diag-test >/tmp/fv_diag.txt 2>&1)
DIAG=$?
echo "diag-test exit: $DIAG (expect 0)"; cat /tmp/fv_diag.txt | head -1
mv pkg/anyps3-diagnostics-*.txt diag_report.txt 2>/dev/null && echo "diag report saved: $(wc -l < diag_report.txt) lines"

echo "== 3) --crash-test + crash dialog =="
rm -f pkg/data/crash_last.txt
(cd pkg && "$WINE" Any_Ps3.exe --crash-test >/dev/null 2>&1)
if [ -f pkg/data/crash_last.txt ]; then
    echo "crash_last.txt written: $(head -1 pkg/data/crash_last.txt)"
else
    echo "FAIL: no crash_last.txt"
fi
(cd pkg && ANYPS3_GUI_FRAMES=3000 "$WINE" Any_Ps3.exe >/tmp/fv_crashgui.out 2>&1) &
sleep 18
ffmpeg -y -loglevel error -f x11grab -video_size 1440x900 -i :99 -frames:v 1 crash_dialog.png
pkill -f Any_Ps3.exe 2>/dev/null; sleep 1

echo "== 4) 2-game library screenshot (fixed build) =="
# second library entry so identical button labels repeat (the bug scenario)
mkdir -p pkg/data/games/ZZZTEST02
sed 's/^title=.*/title=Second Test Game/; s/^title_id=.*/title_id=ZZZTEST02/' \
    pkg/data/games/DEMO00001/game.txt > pkg/data/games/ZZZTEST02/game.txt
cp pkg/data/games/DEMO00001/icon.png pkg/data/games/ZZZTEST02/icon.png 2>/dev/null || true
rm -f pkg/data/crash_last.txt
$X mousemove 1180 216   # hover parked on row-2 buttons (screen coords incl. title bar)
(cd pkg && ANYPS3_GUI_FRAMES=3000 "$WINE" Any_Ps3.exe >/tmp/fv_lib.out 2>&1) &
sleep 18
ffmpeg -y -loglevel error -f x11grab -video_size 1440x900 -i :99 -frames:v 1 library_2games_hover.png
pkill -f Any_Ps3.exe 2>/dev/null; sleep 1
pkill -f 'Xvfb :99' 2>/dev/null

echo "== summary =="
echo "selftest_exit=$SELFTEST diagtest_exit=$DIAG"
ls -la "$OUT"/*.png | awk '{print $5, $9}'
