#!/bin/bash
# Builds a static libcrypto for the TV's 32-bit ARM ABI into third_party/openssl-android.
# Usage: ./build-openssl.sh    (run ./fetch-deps.sh first)
set -euo pipefail

: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
: "${ANDROID_NDK_ROOT:=$ANDROID_HOME/ndk/27.2.12479018}"
export ANDROID_NDK_ROOT
API=24
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/third_party/openssl"
OUT="$HERE/third_party/openssl-android/armeabi-v7a"

export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"

cd "$SRC"
make distclean >/dev/null 2>&1 || true
./Configure android-arm -D__ANDROID_API__=$API no-shared no-tests no-engine \
    no-ui-console no-async --prefix="$OUT"
make -j"$(sysctl -n hw.ncpu)" build_libs
mkdir -p "$OUT/lib"
cp libcrypto.a "$OUT/lib/"
rm -rf "$OUT/include"
cp -R include "$OUT/include"
echo "libcrypto ready in $OUT"
