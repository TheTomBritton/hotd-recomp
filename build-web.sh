#!/bin/sh
# Configure and build the web host: the lifted C on pcrecomp's runtime/win32hle,
# compiled to WebAssembly with Emscripten; the game runs in a browser tab.
#
#   ./build-web.sh                 # build-web/hotd1.js + hotd1.wasm
#
# Needs a lift (run_lift.py), cmake, ninja and Emscripten (an activated emsdk:
# source emsdk/emsdk_env.sh). The page that hosts the build is the site's.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-web}
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
command -v emcmake >/dev/null 2>&1 || { echo "emcmake not found: source emsdk/emsdk_env.sh" >&2; exit 1; }
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  emcmake cmake -S . -B "$BUILD_DIR" -G Ninja -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
