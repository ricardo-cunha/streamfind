#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/linux-common.sh"
sanitize_linux_build_environment
ensure_streamfind_example_data
require_tool cmake; require_tool ninja
BUILD_DIR="${STREAMFIND_CPP_BUILD_DIR:-$REPO_ROOT/tmp/build/linux/core}"
reset_mixed_linux_build_tree "$BUILD_DIR"
PLUGINS="${STREAMFIND_PLUGINS:-ALL}"
cmake -G Ninja -S "$REPO_ROOT/cpp" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="${STREAMFIND_BUILD_TYPE:-Release}" \
  -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_BUILD_SHARED=OFF \
  -DSTREAMFIND_DATA_ROOT="$STREAMFIND_DATA_REPOSITORY_ROOT" \
  -DSTREAMFIND_ENABLED_PLUGINS="$PLUGINS"
cmake --build "$BUILD_DIR" --parallel "${STREAMFIND_JOBS:-$(nproc)}"
if [[ "${STREAMFIND_RUN_TESTS:-0}" != 0 ]]; then
  (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
echo "C++ Linux build complete: $BUILD_DIR"
