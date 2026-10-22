#!/usr/bin/env bash
# Any_Ps3 — build the Android .apk (arm64-v8a primary, x86_64 secondary).
# Requires on the build machine:
#   - Android Studio SDK + NDK 27 (ndkVersion in app/build.gradle.kts)
#   - JDK 17 and Gradle (or ./gradlew after a one-time `gradle wrapper`)
set -euo pipefail
cd "$(dirname "$0")/../hosts/android"

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

./gradlew assembleRelease
APK=app/build/outputs/apk/release/app-release-unsigned.apk
if [ ! -f "$APK" ]; then
    echo "APK not found at $APK"
    exit 1
fi
mkdir -p ../../package
cp "$APK" ../../package/Any_Ps3.apk
echo "Packaged: package/Any_Ps3.apk (unsigned; sign with apksigner for install)"
