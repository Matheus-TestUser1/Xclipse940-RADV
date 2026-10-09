#!/usr/bin/env bash
set -euo pipefail

: "${ANDROID_CROSS_FILE:?Set ANDROID_CROSS_FILE to your Mesa/NDK aarch64 Android Meson cross file}"

x940_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
x940_build_dir="$x940_script_dir/build-xclipse940-android"
x940_api="${ANDROID_API:-34}"

if [[ ! "$x940_api" =~ ^[1-9][0-9]*$ ]]; then
  printf 'ANDROID_API must be a positive integer, got: %s\n' "$x940_api" >&2
  exit 1
fi
if [[ ! -f "$ANDROID_CROSS_FILE" || ! -r "$ANDROID_CROSS_FILE" ]]; then
  printf 'Cross file is not a readable file: %s\n' "$ANDROID_CROSS_FILE" >&2
  exit 1
fi

# Resolve caller-relative paths before changing to the source directory.
x940_cross_file="$(realpath -- "$ANDROID_CROSS_FILE")"
x940_prefix="$(realpath -m -- "${PREFIX:-$x940_script_dir/out-xclipse940-android}")"
cd -- "$x940_script_dir"

# Mesa requires Meson >= 1.1: --reconfigure also accepts a fresh build directory.
# Keep existing build outputs and unrelated files in the installation prefix.
meson setup --reconfigure "$x940_build_dir" "$x940_script_dir" \
  --cross-file "$x940_cross_file" \
  --prefix="$x940_prefix" \
  --libdir=lib \
  -Dplatforms=android \
  -Dplatform-sdk-version="$x940_api" \
  -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled \
  -Dvulkan-drivers=amd \
  -Dgallium-drivers= \
  -Dllvm=disabled \
  -Dgbm=disabled \
  -Dglx=disabled \
  -Dbuildtype=release

ninja -C "$x940_build_dir"
if [[ ! -s "$x940_build_dir/src/amd/vulkan/libvulkan_radeon.so" ]]; then
  printf 'Build did not produce libvulkan_radeon.so in %s\n' "$x940_build_dir" >&2
  exit 1
fi
ninja -C "$x940_build_dir" install
if [[ ! -s "$x940_prefix/lib/libvulkan_radeon.so" ]]; then
  printf 'Install did not produce %s/lib/libvulkan_radeon.so\n' "$x940_prefix" >&2
  exit 1
fi

echo
printf 'Android RADV library: %s/lib/libvulkan_radeon.so\n' "$x940_prefix"
