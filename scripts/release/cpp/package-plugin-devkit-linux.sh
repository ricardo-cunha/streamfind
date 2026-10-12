#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../build" && pwd)/linux-common.sh"
require_tool cmake; require_tool ninja; require_tool cpack
VERSION="${1:?usage: package-plugin-devkit.sh VERSION}"
OUT="${STREAMFIND_RELEASE_DIR:-$REPO_ROOT/tmp/release-output}"
BUILD="$REPO_ROOT/tmp/build/release-plugin-devkit-linux"
INSTALL="$BUILD/install"
FRONTEND="$REPO_ROOT/frontend"
FRONTEND_DIST="$FRONTEND/dist"
mkdir -p "$OUT"
if [[ "${STREAMFIND_SKIP_FRONTEND_BUILD:-0}" == 0 ]]; then
    require_tool npm
    (cd "$FRONTEND" && npm ci && npm run build)
fi
test -f "$FRONTEND_DIST/index.html"
rm -rf "$BUILD"
cmake -G Ninja -S "$REPO_ROOT/cpp" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DSTREAMFIND_BUILD_TESTS=ON \
    -DSTREAMFIND_BUILD_SHARED=OFF \
    -DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON \
    -DSTREAMFIND_APP_DIR="$FRONTEND_DIST"
cmake --build "$BUILD" --parallel "${STREAMFIND_JOBS:-$(nproc)}"
[[ "${STREAMFIND_RUN_TESTS:-1}" == 0 ]] || (cd "$BUILD" && ctest --output-on-failure)
cmake --install "$BUILD"
PYTHON="${STREAMFIND_PYTHON:-$REPO_ROOT/.venv/bin/python}"
test -x "$PYTHON" || { echo "missing repository Python environment: $PYTHON" >&2; exit 1; }
"$PYTHON" "$REPO_ROOT/scripts/dev/test_plugin_consumer.py" \
    --source "$REPO_ROOT/cpp/sdk/examples/minimal_plugin" \
    --prefix "$INSTALL" \
    --build "$BUILD/consumer" \
    --cmake cmake --ninja ninja
(cd "$BUILD" && cpack -G TGZ -C Release -B "$BUILD/cpack")
archive="$BUILD/cpack/streamfind-plugin-dev-$VERSION-Linux-$(uname -m).tar.gz"
test -f "$archive"
cp -f "$archive" "$OUT/"
(cd "$OUT" && find . -maxdepth 1 -type f -name 'streamfind-plugin-dev-*' -printf '%f\n' | sort | xargs -r sha256sum > sha256sums.txt)
echo "Plugin developer kit: $OUT/$(basename "$archive")"
