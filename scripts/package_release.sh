#!/bin/bash
# Package the Any_Ps3 release artifacts into download/:
#   Any_Ps3.apk            (Android arm64, debug-signed)
#   Any_Ps3_Windows.zip    (x64 static exe + logo + docs)
#   Any_Ps3_source.zip     (complete source; ships with every release)
set -e
ROOT=/home/z/my-project
D=$ROOT/download
mkdir -p $D

echo "== 1/3 Windows zip =="
STAGE=$ROOT/windows/package
rm -rf $STAGE && mkdir -p $STAGE
cp $ROOT/windows/build/Any_Ps3.exe $STAGE/
cp $ROOT/assets/logo_full.png $STAGE/
cp $ROOT/windows/build/wine_screenshot.png $STAGE/verification_wine_screenshot.png 2>/dev/null || true
cp $ROOT/windows/README.txt $STAGE/README.txt
(cd $STAGE && zip -q -r $D/Any_Ps3_Windows.zip .)
ls -la $D/Any_Ps3_Windows.zip

echo "== 2/3 source zip =="
SRC=$ROOT/package_src
rm -rf $SRC && mkdir -p $SRC/anyps3
rsync -a --exclude build --exclude '*.o' --exclude '*.so' --exclude '*.exe' \
    --exclude tools --exclude package_src \
    $ROOT/core $ROOT/android $ROOT/windows $ROOT/tests $ROOT/scripts $ROOT/fixtures \
    $SRC/anyps3/ 2>/dev/null || {
  # no rsync? plain cp
  for d in core android windows tests scripts fixtures; do
    mkdir -p $SRC/anyps3/$d
    (cd $ROOT/$d && find . -type f \
       -not -path "./build/*" -not -name "*.o" -not -name "*.so" -not -name "*.exe" \
       -exec cp --parents {} $SRC/anyps3/$d/ \;)
  done
}
cp $ROOT/README.md $SRC/anyps3/
cp $ROOT/PHASE_REPORTS.md $SRC/anyps3/ 2>/dev/null || true
cp $ROOT/worklog.md $SRC/anyps3/ 2>/dev/null || true
cp $ROOT/assets/palette.json $SRC/anyps3/assets_palette.json 2>/dev/null || true
(cd $SRC && zip -q -r $D/Any_Ps3_source.zip anyps3)
ls -la $D/Any_Ps3_source.zip

echo "== 3/3 final listing =="
ls -la $D/
