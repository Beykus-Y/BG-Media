#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/linux}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
CTEST_BIN="${CTEST_BIN:-ctest}"

"$CMAKE_BIN" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBGMEDIA_BUILD_DESKTOP=ON \
  -DBGMEDIA_BUILD_TESTS=ON \
  -DBGMEDIA_BUILD_TOOLS=ON \
  -DBGMEDIA_BUILD_MINIAUDIO=ON \
  -DBGMEDIA_FETCH_SDL3=ON

"$CMAKE_BIN" --build "$BUILD_DIR" --parallel
"$CTEST_BIN" --test-dir "$BUILD_DIR" --output-on-failure

echo
echo "BG Media built successfully."
echo "  build: $BUILD_DIR"
echo "  app:   $BUILD_DIR/bin/bgmedia_desktop"
