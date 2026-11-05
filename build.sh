#!/usr/bin/env bash
# Configure + build calligrus on Linux in one go (counterpart of build.ps1).
#   ./build.sh                       -> Release build in build/
#   ./build.sh Debug                 -> Debug build
#   BUILD_DIR=build-test ./build.sh
#   BUILD_DIR=~/calligrus-build/cpp bash build.sh   -> build outside the source tree (WSL on /mnt/d)
#   ./build.sh Release --test        -> also run every build/calligrus-*-test
# Output: <BUILD_DIR>/calligrus. The libraries are downloaded and built by CMake (FetchContent,
# cmake/deps.cmake) and linked statically; the exe only links the system's libc, libstdc++,
# X11/GL. Downloads are cached in $CALLIGRUS_DOWNLOAD_DIR (default <BUILD_DIR>/_downloads).
#   CALLIGRUS_USE_SYSTEM_LIBS=ON ./build.sh   -> use the distribution's libraries instead
#   CALLIGRUS_COMPILER_LAUNCHER=sccache ./build.sh -> wrap the C/C++ compiler (CI uses this; unset by default)
# Needs: a C/C++ compiler, cmake, ninja (or make), pkg-config (system libs only), network
# access for the first configure, and the X11/GL dev packages GLFW builds against, e.g. on Ubuntu:
#   sudo apt install build-essential cmake ninja-build pkg-config libx11-dev libxrandr-dev \
#       libxinerama-dev libxcursor-dev libxi-dev libgl-dev libxkbcommon-dev libwayland-dev \
#       libxext-dev wayland-protocols
set -euo pipefail

CONFIG="${1:-Release}"
RUN_TESTS=0
[ "${2:-}" = "--test" ] && RUN_TESTS=1
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# BUILD_DIR is relative to this folder unless it is an absolute path.
BUILD="${BUILD_DIR:-build}"
[[ "$BUILD" = /* ]] || BUILD="$SRC/$BUILD"
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
LAUNCH=()
if [ -n "${CALLIGRUS_COMPILER_LAUNCHER:-}" ]; then
    LAUNCH=(-DCMAKE_C_COMPILER_LAUNCHER="$CALLIGRUS_COMPILER_LAUNCHER" -DCMAKE_CXX_COMPILER_LAUNCHER="$CALLIGRUS_COMPILER_LAUNCHER")
fi
cmake -S "$SRC" -B "$BUILD" "${GEN[@]}" "${LAUNCH[@]}" -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCALLIGRUS_USE_SYSTEM_LIBS="${CALLIGRUS_USE_SYSTEM_LIBS:-OFF}"
cmake --build "$BUILD" --parallel "${JOBS:-$(nproc)}"
echo "Built: $BUILD/calligrus"

if [ "$RUN_TESTS" = 1 ]; then
    fail=0
    for t in "$BUILD"/calligrus-*-test; do
        echo "== $(basename "$t")"
        "$t" > "$BUILD/$(basename "$t").log" 2>&1 && echo "   ok" || { echo "   FAILED (see $BUILD/$(basename "$t").log)"; fail=1; }
    done
    exit $fail
fi
