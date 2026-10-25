#!/usr/bin/env bash
# Any_Ps3 — build the Android .apk (arm64-v8a, debug-signed so it installs
# directly; rename to Any_Ps3.apk for distribution).
# Requires on the build machine:
#   - Android SDK with: platforms;android-35 build-tools;35.0.0
#     ndk;27.2.12479018 cmake;3.22.1   (sdkmanager installs all of these)
#   - JDK 17+ and either system Gradle or ./gradlew (already committed)
#
# Env: ANDROID_HOME=<sdk root> (or write hosts/android/local.properties)
# Usage: scripts/build_android.sh [assembleDebug|assembleRelease]
set -euo pipefail
cd "$(dirname "$0")/../hosts/android"

MODE="${1:-assembleDebug}"

if [ ! -f gradlew ]; then
    if command -v gradle > /dev/null; then
        gradle wrapper --gradle-version 8.9
    else
        cat <<'EOF'
No gradle wrapper found and no system gradle. Either:
  1) open hosts/android in Android Studio (it generates the wrapper), or
  2) install Gradle and run: gradle wrapper --gradle-version 8.9
EOF
        exit 1
    fi
fi

./gradlew "$MODE" --no-daemon

if [ "$MODE" = "assembleDebug" ]; then
    APK=app/build/outputs/apk/debug/app-debug.apk
else
    APK=app/build/outputs/apk/release/app-release-unsigned.apk
fi
[ -f "$APK" ] || { echo "APK not found at $APK"; exit 1; }

mkdir -p ../../download
cp "$APK" ../../download/Any_Ps3.apk
echo "Packaged: download/Any_Ps3.apk ($(du -h "$APK" | cut -f1))"
if [ "$MODE" = "assembleDebug" ]; then
    echo "(debug-signed: installs directly via 'adb install' or sideloading)"
else
    echo "(unsigned release: sign with apksigner before installing)"
fi
