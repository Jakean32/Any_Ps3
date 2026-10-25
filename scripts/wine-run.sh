#!/bin/sh
# Local wine 10 launcher (extracted Debian packages, no root needed).
# Usage: scripts/wine-run.sh <exe> [args...]
# Environment:
#   WINEPREFIX  prefix dir (default: /tmp/my-project/tools/wine-prefix)
#   DISPLAY     X display for GUI runs (Xvfb)
ROOT=/tmp/my-project/tools/wine-root/usr
export WINEPREFIX="${WINEPREFIX:-/tmp/my-project/tools/wine-prefix2}"
export WINESERVER="$ROOT/lib/wine/wineserver64"
# extra X libs (libxkbcommon-x11, libxcb-xkb, libxcb-cursor) needed by winex11
export LD_LIBRARY_PATH="/tmp/my-project/tools/xlibs-root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export WINEDLLOVERRIDES="mscoree,mshtml="  # skip gecko/mono install prompts
export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;winemenubuilder="  # no file associations
export WINEDEBUG="${WINEDEBUG:--all}"
mkdir -p "$WINEPREFIX"
exec "$ROOT/lib/wine/wine64" "$@"
