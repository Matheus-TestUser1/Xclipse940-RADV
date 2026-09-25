#!/bin/sh
set -eu

ROOT="${1:-$HOME/Xclipse940-RADV-v0.2.1-source}"
NDK="${NDK:-$HOME/android-ndk-r29}"
API="${API:-34}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"

SRC="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/radv_device_probe_safe.c"
OUT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/radv_device_probe_safe"

echo "Mesa root : $ROOT"
echo "NDK       : $NDK"
echo "API       : $API"
echo "Compiler  : $CC"

"$CC" \
  -O2 -Wall -Wextra \
  -I"$ROOT/include/android_stub" \
  "$SRC" \
  -o "$OUT" \
  -ldl

echo
echo "Built: $OUT"
file "$OUT" || true
echo
echo "Push with:"
echo "  adb push \"$OUT\" /data/local/tmp/xclipse940/"
