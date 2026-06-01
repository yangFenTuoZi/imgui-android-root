#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/imgui-root-build}"
ABI="${ABI:-arm64-v8a}"
API="${API:-29}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if [[ -z "${NDK_HOME:-}" ]]; then
    echo "error: NDK_HOME is not set" >&2
    exit 1
fi

if [[ ! -f "$NDK_HOME/build/cmake/android.toolchain.cmake" ]]; then
    echo "error: invalid NDK_HOME: $NDK_HOME" >&2
    exit 1
fi

generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]] && grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$BUILD_DIR/CMakeCache.txt" && ! command -v ninja >/dev/null 2>&1; then
    cat >&2 <<EOF
error: $BUILD_DIR was configured with Ninja, but ninja is not installed.

Use a fresh build directory, for example:
  BUILD_DIR=/tmp/imgui-root-build-make $0
EOF
    exit 1
fi

cmake -S "$SCRIPT_DIR" \
      -B "$BUILD_DIR" \
      "${generator_args[@]}" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" \
      -DANDROID_PLATFORM="android-$API"

cmake --build "$BUILD_DIR" --target imgui_root_android

echo
echo "built: $BUILD_DIR/imgui_root_android"
echo "push/run:"
echo "  adb push '$BUILD_DIR/imgui_root_android' /data/local/tmp/"
echo "  adb shell su -c 'chmod 755 /data/local/tmp/imgui_root_android'"
echo "  adb shell su -c '/data/local/tmp/imgui_root_android --width 1080 --height 2400'"
