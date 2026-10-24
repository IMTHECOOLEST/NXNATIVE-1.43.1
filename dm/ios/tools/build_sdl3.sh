#!/bin/bash
# Build SDL3 as a static library for iOS devices (arm64) into a prefix that build_ipa.sh takes as SDL3_PREFIX (the optional SDL3 gamepad backend, Sources/DmSDL3Pad.c).
# Nothing is downloaded here: pass an extracted SDL3 source tree (a release of libsdl-org/SDL, 3.2 or newer).
# Usage: dm/ios/tools/build_sdl3.sh <SDL3 source dir> [prefix]   (prefix defaults to dm/third_party/sdl3-ios, which .gitignore covers)
# Then:  SDL3_PREFIX=dm/third_party/sdl3-ios GAME=... W=... dm/ios/tools/build_ipa.sh      and      DM_PAD_BACKEND=sdl3 in the app's Documents/env.txt
# Needs Xcode and cmake (brew install cmake). The options leave out what a gamepad backend never uses (HIDAPI, which on iOS would go through CoreBluetooth; Vulkan; OpenGL; GPU; camera; dialogs; tests)
set -eu
SRC=${1:?usage: $0 <SDL3 source dir> [prefix]}
PREFIX=${2:-$(cd "$(dirname "$0")/../.." && pwd)/third_party/sdl3-ios}
[ -f "$SRC/CMakeLists.txt" ] && [ -f "$SRC/include/SDL3/SDL.h" ] || { echo "FAIL: $SRC is not an SDL3 source tree (no CMakeLists.txt and include/SDL3/SDL.h)" >&2; exit 1; }
command -v cmake > /dev/null || { echo "FAIL: cmake is needed (brew install cmake)" >&2; exit 1; }
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
cmake -S "$SRC" -B "$B" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_INSTALL=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_HIDAPI=OFF -DSDL_VULKAN=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_GPU=OFF -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF
cmake --build "$B" --config Release -j "$(sysctl -n hw.ncpu)"
cmake --install "$B" --config Release
[ -f "$PREFIX/lib/libSDL3.a" ] && [ -f "$PREFIX/include/SDL3/SDL.h" ] || { echo "FAIL: the install has no lib/libSDL3.a and include/SDL3/SDL.h under $PREFIX" >&2; exit 1; }
echo "SDL3 for iOS installed in $PREFIX: SDL3_PREFIX=$PREFIX"
