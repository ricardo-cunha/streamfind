#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../build" && pwd)/linux-common.sh"
require_tool cmake; require_tool ninja; require_tool tar; require_tool sha256sum
VERSION="${1:?usage: release-cpp-linux.sh VERSION}"
OUT="${STREAMFIND_RELEASE_DIR:-$REPO_ROOT/tmp/release-output}"
BUILD="$REPO_ROOT/tmp/build/release-cpp-linux"
mkdir -p "$OUT"
cmake -G Ninja -S "$REPO_ROOT/cpp" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_BUILD_SHARED=OFF
cmake --build "$BUILD" --parallel "${STREAMFIND_JOBS:-$(nproc)}"
[[ "${STREAMFIND_RUN_TESTS:-1}" == 0 ]] || (cd "$BUILD" && ctest --output-on-failure)
(cd "$BUILD" && cpack -G TGZ -C Release -B "$OUT")
ARCH="$(uname -m)"; source_archive="$OUT/streamfind-core-cpp-$VERSION-Linux-$ARCH.tar.gz"; archive="$OUT/streamfind-core-cpp-$VERSION-Linux-$ARCH.tgz"
if [[ -f "$source_archive" ]]; then mv -f "$source_archive" "$archive"; fi
test -f "$archive"; assert_archive "$archive" licenses
listing="${archive}.list"
tar -tzf "$archive" > "$listing"
grep -Eq '(^|/)share/streamfind/plugins/mass_spec/plugin\.json$' "$listing"
grep -Eq '(^|/)share/streamfind/plugins/raman/plugin\.json$' "$listing"
grep -Eq '(^|/)share/streamfind/plugins/sensors/plugin\.json$' "$listing"
grep -Eq '(^|/)lib/libduckdb_static\.a$' "$listing"
for domain in mass_spec raman sensors; do
    manifest_path="$(grep -m1 "/share/streamfind/plugins/$domain/plugin\.json$" "$listing")"
    test -n "$manifest_path"
    manifest_file="${archive}.${domain}.manifest"
    tar -xOf "$archive" "$manifest_path" > "$manifest_file"
    grep -q "\"plugin_id\": \"$domain\"" "$manifest_file"
    grep -q '"version": "0.2.0"' "$manifest_file"
    grep -q '"static_composition": true' "$manifest_file"
    grep -q '"semantic_catalogue": "catalogue.duckdb"' "$manifest_file"
    rm -f "$manifest_file"
done
rm -f "$listing"
(cd "$OUT" && find . -maxdepth 1 -type f \( -name 'streamfind-*.tgz' -o -name 'streamfind-*.tar.gz' \) -printf '%f\n' | sort | xargs -r sha256sum > sha256sums.txt)
echo "C++ Linux release: $archive"
