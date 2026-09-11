#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/linux-common.sh"
require_tool cmake; require_tool ninja
BUILD_DIR="${STREAMFIND_CPP_BUILD_DIR:-$REPO_ROOT/tmp/build/linux-cpp}"
cmake -G Ninja -S "$REPO_ROOT/cpp" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="${STREAMFIND_BUILD_TYPE:-Release}" \
  -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_BUILD_SHARED=OFF
cmake --build "$BUILD_DIR" --parallel "${STREAMFIND_JOBS:-$(nproc)}"
if [[ "${STREAMFIND_RUN_TESTS:-0}" != 0 ]]; then
  (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
echo "C++ Linux build complete: $BUILD_DIR"
