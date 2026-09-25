#!/bin/sh
set -eu

ROOT="${1:-$HOME/Xclipse940-RADV-v0.2.1-source}"
NDK="${NDK:-$HOME/android-ndk-r29}"
API="${API:-34}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SRC="$HERE/radv_push_descriptor_probe_diag.c"
OUT="$HERE/radv_push_descriptor_probe_diag"

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
