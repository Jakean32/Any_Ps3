#!/bin/bash
# Any_Ps3 Android build script — no Gradle, direct toolchain:
#   NDK clang++ -> libanyps3_core.so (arm64)
#   aapt2 compile/link, javac, d8, zip, zipalign, apksigner
set -e

ROOT=/home/z/my-project
SDK=$ROOT/tools/android-sdk
BT=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-34/android.jar
NDKTC=$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin
JDK=$ROOT/tools/jdk-21.0.13+9
A=$ROOT/android
BUILD=$A/build
JRE=$JDK/bin

export JAVA_HOME=$JDK
export PATH=$JDK/bin:$PATH

rm -rf $BUILD
mkdir -p $BUILD/gen $BUILD/classes $BUILD/stage/lib/arm64-v8a $BUILD/dex

echo "== 1/7 native core (arm64-v8a) =="
CLANG=$NDKTC/aarch64-linux-android26-clang++
$CLANG -std=c++17 -O2 -fPIC -shared \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -DANDROID -I$ROOT \
    $ROOT/core/util.cpp $ROOT/core/bytesource.cpp $ROOT/core/minijson.cpp \
    $ROOT/core/sfo.cpp $ROOT/core/iso9660.cpp $ROOT/core/thumbs.cpp \
    $ROOT/core/game_import.cpp $ROOT/core/cache_manifest.cpp \
    $ROOT/core/recompiler.cpp $ROOT/core/lv2.cpp $ROOT/core/library.cpp \
    $A/jni/anyps3_jni.cpp \
    -static-libstdc++ \
    -Wl,--exclude-libs,ALL \
    -Wl,-soname,libanyps3_core.so \
    -o $BUILD/stage/lib/arm64-v8a/libanyps3_core.so
ls -la $BUILD/stage/lib/arm64-v8a/

echo "== 2/7 aapt2 compile resources =="
$BT/aapt2 compile --dir $A/res -o $BUILD/res.zip

echo "== 3/7 aapt2 link =="
$BT/aapt2 link -o $BUILD/stage/base.apk \
    -I "$PLATFORM" \
    --manifest $A/AndroidManifest.xml \
    --min-sdk-version 26 --target-sdk-version 34 \
    --java $BUILD/gen \
    --auto-add-overlay \
    $BUILD/res.zip
ls $BUILD/gen/com/anyps3/app/R.java

echo "== 4/7 javac =="
find $A/src -name "*.java" > $BUILD/sources.txt
echo "$BUILD/gen/com/anyps3/app/R.java" >> $BUILD/sources.txt
$JRE/javac -source 8 -target 8 -nowarn -Xlint:-options \
    -bootclasspath "$PLATFORM:$BT/core-lambda-stubs.jar" \
    -d $BUILD/classes \
    @$BUILD/sources.txt 2>&1 | grep -v "^Note:" || true
if [ ! -f $BUILD/classes/com/anyps3/app/MainActivity.class ]; then
  echo "FATAL: javac failed — MainActivity.class missing"; exit 1
fi
find $BUILD/classes -name "*.class" | head -3

echo "== 5/7 d8 dex =="
cd $BUILD/classes && $JDK/bin/jar cf $BUILD/classes.jar . && cd $ROOT
$BT/d8 --release --min-api 26 --lib "$PLATFORM" --classpath "$PLATFORM" \
    --output $BUILD/dex $BUILD/classes.jar
ls $BUILD/dex

echo "== 6/7 assemble APK =="
cp $BUILD/dex/classes.dex $BUILD/stage/classes.dex
cd $BUILD/stage
zip -q -r $BUILD/unsigned.apk classes.dex lib
zip -q -r $BUILD/unsigned.apk base.apk 2>/dev/null || true  # (no-op guard)
cd $ROOT
# base.apk already contains resources + manifest; merge:
cd $BUILD
cp stage/base.apk unsigned.apk
cd stage && zip -q $BUILD/unsigned.apk classes.dex lib/arm64-v8a/libanyps3_core.so
cd $ROOT

echo "== 7/7 zipalign + sign =="
$BT/zipalign -f 4 $BUILD/unsigned.apk $BUILD/aligned.apk
if [ ! -f $ROOT/tools/debug.keystore ]; then
  keytool -genkeypair -keystore $ROOT/tools/debug.keystore \
    -alias androiddebugkey -storepass android -keypass android \
    -dname "CN=AnyPs3 Debug,O=AnyPs3,C=DE" -keyalg RSA -keysize 2048 \
    -validity 10000 >/dev/null 2>&1
fi
$BT/apksigner sign --ks $ROOT/tools/debug.keystore --ks-pass pass:android \
    --min-sdk-version 26 \
    $BUILD/aligned.apk
cp $BUILD/aligned.apk $ROOT/download/Any_Ps3.apk
echo "APK written: $ROOT/download/Any_Ps3.apk"
ls -la $ROOT/download/Any_Ps3.apk
