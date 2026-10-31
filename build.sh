#!/usr/bin/env bash
# Configure + build calligrus on Linux in one go (counterpart of build.ps1).
#   ./build.sh                       -> Release build in build/
#   ./build.sh Debug                 -> Debug build
#   BUILD_DIR=build-test ./build.sh
#   BUILD_DIR=~/calligrus-build/cpp bash build.sh   -> build outside the source tree (WSL on /mnt/d)
#   ./build.sh Release --test        -> also run every build/calligrus-*-test
# Output: <BUILD_DIR>/calligrus. Dependencies come from vcpkg in manifest mode
# (vcpkg.json, triplet x64-linux: static libraries; the exe only links the
# system's libc, libstdc++, X11/GL). vcpkg is looked up in $VCPKG_ROOT, ~/vcpkg,
# /opt/vcpkg, /usr/local/vcpkg and cloned into ~/vcpkg if missing.
# Needs: a C/C++ compiler, cmake, ninja (or make), git, curl, zip, unzip, tar,
# pkg-config, and the X11/GL dev packages GLFW builds against, e.g. on Ubuntu:
#   sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config \
#       libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
#       libxkbcommon-dev libwayland-dev libxext-dev
set -euo pipefail

CONFIG="${1:-Release}"
RUN_TESTS=0
[ "${2:-}" = "--test" ] && RUN_TESTS=1
TRIPLET="${TRIPLET:-x64-linux}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# BUILD_DIR is relative to this folder unless it is an absolute path.
BUILD="${BUILD_DIR:-build}"
[[ "$BUILD" = /* ]] || BUILD="$SRC/$BUILD"
# Find (or fetch) vcpkg.
VCPKG="${VCPKG_ROOT:-}"
if [ -z "$VCPKG" ]; then
    for d in "$HOME/vcpkg" /opt/vcpkg /usr/local/vcpkg; do
        [ -f "$d/scripts/buildsystems/vcpkg.cmake" ] && VCPKG="$d" && break
    done
fi
if [ -z "$VCPKG" ]; then
    VCPKG="$HOME/vcpkg"
    echo "vcpkg not found: cloning into $VCPKG"
    git clone https://github.com/microsoft/vcpkg "$VCPKG"
fi
[ -x "$VCPKG/vcpkg" ] || "$VCPKG/bootstrap-vcpkg.sh" -disableMetrics

# A build folder configured for another triplet keeps stale library paths: start it over.
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q "^VCPKG_TARGET_TRIPLET:STRING=$TRIPLET$" "$BUILD/CMakeCache.txt"; then
    echo "Triplet changed: clearing $BUILD/CMakeCache.txt"
    rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
fi

GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
cmake -S "$SRC" -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE="$CONFIG" \
    "-DCMAKE_TOOLCHAIN_FILE=$VCPKG/scripts/buildsystems/vcpkg.cmake" \
    "-DVCPKG_TARGET_TRIPLET=$TRIPLET"
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
