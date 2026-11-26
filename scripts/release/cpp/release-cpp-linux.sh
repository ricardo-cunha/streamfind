#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../build" && pwd)/linux-common.sh"
sanitize_linux_build_environment
ensure_streamfind_example_data
require_tool cmake; require_tool ninja; require_tool tar; require_tool sha256sum
ABI_HEADER="$REPO_ROOT/cpp/sdk/include/streamfind/plugin_abi.h"
abi_major="$(sed -nE 's/^[[:space:]]*STREAMFIND_PLUGIN_ABI_MAJOR[[:space:]]*=[[:space:]]*([0-9]+),?[[:space:]]*$/\1/p' "$ABI_HEADER" | head -n1)"
abi_minor="$(sed -nE 's/^[[:space:]]*STREAMFIND_PLUGIN_ABI_MINOR[[:space:]]*=[[:space:]]*([0-9]+),?[[:space:]]*$/\1/p' "$ABI_HEADER" | head -n1)"
if [[ -z "$abi_major" || -z "$abi_minor" ]]; then
    echo "could not read plugin ABI version from $ABI_HEADER" >&2
    exit 2
fi
VERSION="${1:-$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")}"
if [[ -z "$VERSION" ]]; then
    echo "VERSION is empty" >&2
    exit 2
fi
canonical_version="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
if [[ "$VERSION" != "$canonical_version" ]]; then
    echo "release version $VERSION does not match canonical version $canonical_version in $REPO_ROOT/VERSION" >&2
    exit 2
fi
PLUGINS="${STREAMFIND_PLUGINS:-ALL}"
OUT="${STREAMFIND_RELEASE_DIR:-$REPO_ROOT/tmp/release-output}"
BUILD="$REPO_ROOT/tmp/build/linux/core"
reset_mixed_linux_build_tree "$BUILD"
FRONTEND="$REPO_ROOT/frontend"
FRONTEND_DIST="$FRONTEND/dist"
mkdir -p "$OUT"
if [[ "${STREAMFIND_SKIP_FRONTEND_BUILD:-0}" == 0 ]]; then
    require_tool npm
    (cd "$FRONTEND" && npm ci && npm run build)
fi
test -f "$FRONTEND_DIST/index.html"
cmake -G Ninja -S "$REPO_ROOT/cpp" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_DATA_ROOT="$STREAMFIND_DATA_REPOSITORY_ROOT" -DSTREAMFIND_BUILD_SHARED=OFF -DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON -DSTREAMFIND_ENABLED_PLUGINS="$PLUGINS" -DSTREAMFIND_APP_DIR="$FRONTEND_DIST"
cmake --build "$BUILD" --parallel "${STREAMFIND_JOBS:-$(nproc)}"
[[ "${STREAMFIND_RUN_TESTS:-1}" == 0 ]] || (cd "$BUILD" && ctest --output-on-failure)
(cd "$BUILD" && cpack -G TGZ -C Release -B "$OUT")
ARCH="$(uname -m)"; source_archive="$OUT/streamfind-core-cpp-$VERSION-Linux-$ARCH.tar.gz"; archive="$OUT/streamfind-core-cpp-$VERSION-Linux-$ARCH.tgz"
if [[ -f "$source_archive" ]]; then mv -f "$source_archive" "$archive"; fi
test -f "$archive"; assert_archive "$archive" licenses
runtime_root="$REPO_ROOT/tmp/scratch/linux-release-runtime"
rm -rf "$runtime_root"
mkdir -p "$runtime_root"
tar -xzf "$archive" -C "$runtime_root"
package_root="$(find "$runtime_root" -mindepth 1 -maxdepth 1 -type d -print -quit)"
test -n "$package_root"
reported_dev_version="$("$package_root/bin/streamfind-dev" --version)"
if [[ "$reported_dev_version" != "streamfind-dev $VERSION (protocol streamfind-dev/1)" ]]; then
    echo "embedded developer framework version mismatch: expected $VERSION, got '$reported_dev_version'" >&2
    exit 1
fi
PYTHON="${STREAMFIND_PYTHON:-$REPO_ROOT/.venv/bin/python}"
test -x "$PYTHON"
"$PYTHON" "$REPO_ROOT/scripts/dev/validate_native_dependencies.py" "$package_root" --platform linux
"$PYTHON" "$REPO_ROOT/scripts/release/cpp/test-packaged-mcp-linux.py" "$package_root"
listing="${archive}.list"
tar -tzf "$archive" > "$listing"
grep -Eq '(^|/)app/index\.html$' "$listing"
grep -Eq '(^|/)streamfind$' "$listing"
grep -Eq '(^|/)bin/streamfind-dev$' "$listing"
grep -Eq '(^|/)dev/streamfind-dev-setup\.ps1$' "$listing"
grep -Eq '(^|/)sdk/native/sdk-manifest\.json$' "$listing"
grep -Eq '(^|/)sdk/viewer/@streamfind/viewer-sdk/package\.json$' "$listing"
plugin_manifests=$(grep -E '(^|/)plugins/[^/]+/[^/]+/plugin\.json$' "$listing")
test -n "$plugin_manifests"
if grep -Eq '^[^/]+/core/vendors(/|$)' "$listing"; then
    echo "runtime archive must not contain the removed core/vendors directory" >&2
    exit 1
fi
native_outside_bin=$(grep -E '\.(so|dylib)$' "$listing" | grep -Ev '(^|/)(bin|sdk/native/lib|plugins/[^/]+)/' || true)
if [[ -n "$native_outside_bin" ]]; then
    echo "runtime archive contains native libraries outside bin or plugin packages:" >&2
    printf '%s\n' "$native_outside_bin" >&2
    exit 1
fi
grep -Eq '(^|/)core/catalogue\.duckdb$' "$listing"
while IFS= read -r manifest_path; do
    plugin_id="$(dirname "$manifest_path" | xargs basename)"
    plugin_root="$(dirname "$(dirname "$manifest_path")" | xargs basename)"
    manifest_file="${archive}.${plugin_id}.manifest"
    tar -xOf "$archive" "$manifest_path" > "$manifest_file"
    grep -q "\"plugin_id\": \"$plugin_id\"" "$manifest_file"
    grep -q '"version": "'"$VERSION"'"' "$manifest_file"
    grep -Eq '"abi_version":[[:space:]]*\{[[:space:]]*"major":[[:space:]]*'"$abi_major"'[[:space:]]*,[[:space:]]*"minor":[[:space:]]*'"$abi_minor"'[[:space:]]*\}' "$manifest_file"
    grep -q '"static_composition": false' "$manifest_file"
    grep -q '"semantic_catalogue": "catalogue.duckdb"' "$manifest_file"
    library_name="$(grep -m1 '"linux-x86_64":' "$manifest_file" | sed -E 's/.*"linux-x86_64": "([^"]+)".*/\1/')"
    test -n "$library_name"
    grep -Eq '(^|/)plugins/'"$plugin_root"'/'"$plugin_id"'/'"$library_name"'$' "$listing"
    grep -q '"linux-x86_64": "'"$library_name"'"' "$manifest_file"
    rm -f "$manifest_file"
done <<< "$plugin_manifests"
rm -f "$listing"
rm -rf "$runtime_root"
(cd "$OUT" && {
    release_files=()
    for candidate in streamfind-*-"$VERSION"-*.zip streamfind-*-"$VERSION"-*.tgz streamfind-*-"$VERSION"-*.tar.gz; do
        [[ -f "$candidate" ]] && release_files+=("$candidate")
    done
    ((${#release_files[@]} > 0))
    printf '%s\n' "${release_files[@]}" | sort | xargs -r sha256sum > sha256sums.txt
})
echo "C++ Linux release: $archive"
