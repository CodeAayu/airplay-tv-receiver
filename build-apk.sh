#!/bin/bash
# Builds AirPlayTV.apk with the SDK tools directly (no Gradle download needed).
# Usage: ./build-apk.sh    (run ./fetch-deps.sh and ./build-openssl.sh first)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${JAVA_HOME:=$(/usr/libexec/java_home -v 17)}"
: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
NDK="$ANDROID_HOME/ndk/27.2.12479018"
BT="$ANDROID_HOME/build-tools/35.0.0"
ANDROID_JAR="$ANDROID_HOME/platforms/android-35/android.jar"
CMAKE_BIN="$ANDROID_HOME/cmake/3.22.1/bin"
ABI=armeabi-v7a
MIN_SDK=26
TARGET_SDK=34
PACKAGE=com.codeaayu.airplaytv

SRC="$HERE/app/src/main"
OUT="$HERE/build"
export PATH="$JAVA_HOME/bin:$PATH"

echo "== native library"
"$CMAKE_BIN/cmake" -S "$SRC/cpp" -B "$OUT/cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$CMAKE_BIN/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_SDK \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
"$CMAKE_BIN/cmake" --build "$OUT/cmake"
mkdir -p "$OUT/apk/lib/$ABI"
cp "$OUT/cmake/libairplaytv.so" "$OUT/apk/lib/$ABI/"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" "$OUT/apk/lib/$ABI/libairplaytv.so"

echo "== resources"
rm -rf "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/dex"
"$BT/aapt2" compile --dir "$SRC/res" -o "$OUT/res/compiled.zip"
sed "s|<manifest |<manifest package=\"$PACKAGE\" |" "$SRC/AndroidManifest.xml" > "$OUT/AndroidManifest.xml"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$ANDROID_JAR" \
    --manifest "$OUT/AndroidManifest.xml" --java "$OUT/gen" \
    --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK \
    --version-code 1 --version-name 0.1 \
    "$OUT/res/compiled.zip"

echo "== java"
# an argument file keeps paths with spaces intact
find "$SRC/java" "$OUT/gen" -name '*.java' | sed 's/.*/"&"/' > "$OUT/sources.txt"
javac -source 11 -target 11 -Xlint:-options -encoding UTF-8 -classpath "$ANDROID_JAR" -d "$OUT/classes" \
    @"$OUT/sources.txt"
(cd "$OUT/classes" && jar cf "$OUT/classes.jar" .)
"$BT/d8" --release --min-api $MIN_SDK --lib "$ANDROID_JAR" --output "$OUT/dex" "$OUT/classes.jar"

echo "== package"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
(cd "$OUT/dex" && zip -q "$OUT/unsigned.apk" classes.dex)
(cd "$OUT/apk" && zip -q -r "$OUT/unsigned.apk" lib)
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KEYSTORE="$HOME/.android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/AirPlayTV.apk" "$OUT/aligned.apk"
"$BT/apksigner" verify "$OUT/AirPlayTV.apk"
echo "built $OUT/AirPlayTV.apk ($(du -h "$OUT/AirPlayTV.apk" | cut -f1))"
