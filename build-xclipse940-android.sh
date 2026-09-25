#!/usr/bin/env bash
set -euo pipefail

: "${ANDROID_CROSS_FILE:?Set ANDROID_CROSS_FILE to your Mesa/NDK aarch64 Android Meson cross file}"

PREFIX="${PREFIX:-$PWD/out-xclipse940-android}"
rm -rf build-xclipse940-android "$PREFIX"

meson setup build-xclipse940-android \
  --cross-file "$ANDROID_CROSS_FILE" \
  --prefix="$PREFIX" \
  -Dplatforms=android \
  -Dplatform-sdk-version=34 \
  -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled \
  -Dvulkan-drivers=amd \
  -Dgallium-drivers= \
  -Dllvm=disabled \
  -Dgbm=disabled \
  -Dglx=disabled \
  -Dbuildtype=release

ninja -C build-xclipse940-android
ninja -C build-xclipse940-android install

echo
echo "Android RADV output: $PREFIX"
