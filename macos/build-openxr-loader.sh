#!/bin/bash
# Build the Khronos OpenXR loader (libopenxr_loader.dylib) for testing the EF_VR build on a Mac,
# for example against the Meta XR Simulator. Not shipped: desktop releases are built without EF_VR.
#
#   macos/build-openxr-loader.sh <OpenXR-SDK-Source dir> <build dir>
#
# Source: https://github.com/KhronosGroup/OpenXR-SDK-Source (Apache-2.0), tag release-1.1.65,
# which matches the headers in deps/openxr. The loader ends up at <build dir>/src/loader/libopenxr_loader.dylib;
# point the game at it with EF_OPENXR_LOADER=<that path> and pick the runtime with XR_RUNTIME_JSON.
set -euo pipefail
SRC="${1:?OpenXR-SDK-Source directory}"
BUILD="${2:?build directory}"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DDYNAMIC_LOADER=ON \
	-DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$BUILD" --target openxr_loader -j "$(sysctl -n hw.ncpu)" >/dev/null
find "$BUILD/src/loader" -name 'libopenxr_loader*.dylib' -maxdepth 1 -print
