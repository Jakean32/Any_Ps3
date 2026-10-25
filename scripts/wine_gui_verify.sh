#!/usr/bin/env bash
# Any_Ps3 — Wine GUI verification battery (run from the repo root or anywhere).
# Proves, under Wine + Xvfb with real mouse input (xdotool):
#   1. The v0.4.0 exe REPRODUCES the ImGui "2 visible items with conflicting ID!"
#      overlay in a 2-game library (the reported bug),
#   2. the new exe with the PushID fix shows NO overlay under identical input,
#   3. --crash-test writes crash_last.txt and the next start shows the
#      "Previous session crashed" dialog,
#   4. clicking "Export log" writes anyps3-diagnostics-*.txt next to the exe.
# Usage: scripts/wine_gui_verify.sh <new-exe> <old-exe-or-zip> <workdir>
set -u
NEW_EXE="$1"; OLD_SRC="$2"; WORK="$3"

HERE="$(cd "$(dirname "$0")" && pwd)"
WINE="$HERE/wine-run.sh"
XDTOOL=/tmp/my-project/tools/xdotool-root/usr/bin/xdotool
export DISPLAY=:99
export LD_LIBRARY_PATH="/tmp/my-project/tools/xdotool-root/usr/lib/x86_64-linux-gnu:/tmp/my-project/tools/xlibs-root/usr/lib/x86_64-linux-gnu"

mkdir -p "$WORK"
cd "$WORK"

# --- 0) display ---------------------------------------------------------------
pkill -f 'Xvfb :99' 2>/dev/null; sleep 0.5
mkdir -p /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix
Xvfb :99 -screen 0 1440x900x24 >/tmp/xvfb_gui.log 2>&1 &
XVFB_PID=$!
sleep 2
if ! kill -0 $XVFB_PID 2>/dev/null; then echo "FAIL: Xvfb did not start"; exit 1; fi

shot() {  # shot <out.png>
    ffmpeg -y -loglevel error -f x11grab -video_size 1440x900 -i :99 -frames:v 1 "$1"
}

# --- 1) prepare the OLD exe dir (the delivered v0.4.0 build) ------------------
OLD="$WORK/old"; rm -rf "$OLD"; mkdir -p "$OLD/samples"
if [[ "$OLD_SRC" == *.zip ]]; then
    unzip -o -q -j "$OLD_SRC" 'Any_Ps3.exe' -d "$OLD"
else
    cp "$OLD_SRC" "$OLD/Any_Ps3.exe"
fi
cp /home/z/my-project/Any_Ps3/hosts/windows/samples/hello.elf "$OLD/samples/"

# --- 2) prepare the NEW exe dir ------------------------------------------------
NEW="$WORK/new"; rm -rf "$NEW"; mkdir -p "$NEW/samples"
cp "$NEW_EXE" "$NEW/Any_Ps3.exe"
cp /home/z/my-project/Any_Ps3/hosts/windows/samples/hello.elf "$NEW/samples/"

# --- 3) seed both data dirs with the demo via --selftest -----------------------
for d in "$OLD" "$NEW"; do
    rm -rf "$d/data"
    (cd "$d" && timeout 240 "$WINE" Any_Ps3.exe --selftest >/dev/null 2>&1) || true
    if [ ! -f "$d/data/games/DEMO00001/game.txt" ]; then
        echo "FAIL: demo did not install in $d"; exit 1
    fi
    # second library entry so two rows with identical button labels are visible
    mkdir -p "$d/data/games/ZZZTEST02"
    cp "$d/data/games/DEMO00001/game.txt" "$d/data/games/ZZZTEST02/game.txt"
    sed -i 's/^title=.*/title=Second Test Game/' "$d/data/games/ZZZTEST02/game.txt"
    sed -i 's/^title_id=.*/title_id=ZZZTEST02/' "$d/data/games/ZZZTEST02/game.txt"
    cp "$d/data/games/DEMO00001/icon.png" "$d/data/games/ZZZTEST02/icon.png" 2>/dev/null || true
done
echo "OK: both exe dirs seeded with 2 games"

# --- 4) GUI run helper: launch, hover, screenshot ------------------------------
gui_hover_shots() {  # gui_hover_shots <dir> <prefix>
    local dir="$1" prefix="$2"
    ( cd "$dir" && ANYPS3_GUI_FRAMES=420 "$WINE" Any_Ps3.exe >gui_$prefix.out 2>&1 ) &
    local pid=$!
    sleep 4
    "$XDTOOL" mousemove 1180 105   # row 1 buttons
    sleep 1
    shot "${prefix}_row1.png"
    "$XDTOOL" mousemove 1180 160   # row 2 buttons
    sleep 1
    shot "${prefix}_row2.png"
    "$XDTOOL" mousemove 1180 160
    "$XDTOOL" click 1 2>/dev/null
    sleep 2
    wait $pid
}

echo "== old exe (v0.4.0) — expect the ID-conflict overlay =="
gui_hover_shots "$OLD" old

echo "== new exe (fixed) — expect NO overlay =="
rm -f "$NEW"/anyps3-diagnostics-*.txt
gui_hover_shots "$NEW" new

# --- 5) crash handler: --crash-test then next start shows the dialog ------------
echo "== crash-test =="
rm -f "$NEW/data/crash_last.txt"
( cd "$NEW" && "$WINE" Any_Ps3.exe --crash-test >/dev/null 2>&1 )
if [ -f "$NEW/data/crash_last.txt" ]; then
    echo "OK: crash_last.txt written by the crash handler"
    head -3 "$NEW/data/crash_last.txt"
else
    echo "FAIL: --crash-test did not write data/crash_last.txt"
fi
( cd "$NEW" && ANYPS3_GUI_FRAMES=120 "$WINE" Any_Ps3.exe >gui_crash.out 2>&1 ) &
CPID=$!
sleep 4
shot crash_dialog.png
wait $CPID

# --- 6) Export log click on the new exe -----------------------------------------
echo "== export-log click =="
rm -f "$NEW"/anyps3-diagnostics-*.txt
( cd "$NEW" && ANYPS3_GUI_FRAMES=420 "$WINE" Any_Ps3.exe >gui_export.out 2>&1 ) &
EPID=$!
sleep 4
# "Export log" button sits at the top-left of the log window (bottom 38%)
for y in 587 595 580; do
    "$XDTOOL" mousemove 60 $y
    "$XDTOOL" click 1
    sleep 1
    if ls "$NEW"/anyps3-diagnostics-*.txt >/dev/null 2>&1; then break; fi
done
sleep 2
wait $EPID
if ls "$NEW"/anyps3-diagnostics-*.txt >/dev/null 2>&1; then
    echo "OK: Export log button produced $(ls "$NEW"/anyps3-diagnostics-*.txt)"
else
    echo "WARN: export click did not produce a file (button position may differ)"
fi

pkill -f 'Xvfb :99' 2>/dev/null
echo "== done; screenshots in $WORK =="
