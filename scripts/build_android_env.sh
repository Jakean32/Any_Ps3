#!/usr/bin/env bash
# Any_Ps3 — build the Android APK with the sandbox toolchains.
# Uses the pre-staged SDK/NDK/Gradle/JDK under /tmp/my-project/tools (no root).
# Usage: scripts/build_android_env.sh [assembleDebug] [output-name.apk]
set -euo pipefail
cd "$(dirname "$0")/../hosts/android"

MODE="${1:-assembleDebug}"
OUT="${2:-}"

TOOLS=/tmp/my-project/tools
export ANDROID_HOME="$TOOLS/android-sdk"
export ANDROID_SDK_ROOT="$TOOLS/android-sdk"
export JAVA_HOME="$TOOLS/jdk-21.0.13+9"
export GRADLE_USER_HOME="$TOOLS/gradle-home"
export PATH="$JAVA_HOME/bin:$TOOLS/gradle-8.11.1/bin:$PATH"

gradle "$MODE" --no-daemon -Dorg.gradle.jvmargs="-Xmx2g -XX:MaxMetaspaceSize=768m"

APK=app/build/outputs/apk/debug/app-debug.apk
[ -f "$APK" ] || { echo "APK not found at $APK"; exit 1; }
if [ -n "$OUT" ]; then
    cp "$APK" "$OUT"
    echo "Packaged: $OUT ($(du -h "$OUT" | cut -f1))"
else
    echo "Built: $APK ($(du -h "$APK" | cut -f1))"
fi
