#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../build" && pwd)/linux-common.sh"
require_tool cargo; require_tool tar; require_tool sha256sum
VERSION="${1:?usage: release-rust-linux.sh VERSION CPP_CATALOGUE}"
CATALOGUE="${2:?usage: release-rust-linux.sh VERSION CPP_CATALOGUE}"
test -f "$CATALOGUE" || { echo "C++ catalogue not found: $CATALOGUE" >&2; exit 1; }
export STREAMFIND_CATALOGUE="$CATALOGUE"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$REPO_ROOT/tmp/build/release-rust-linux-target}"
(cd "$REPO_ROOT/rust" && cargo build --release --workspace --exclude streamfind-rust-test-support)
if [[ "${STREAMFIND_RUN_TESTS:-1}" != 0 ]]; then (cd "$REPO_ROOT/rust" && cargo test --workspace); fi
ARCH="$(uname -m)"; OUT="${STREAMFIND_RELEASE_DIR:-$REPO_ROOT/tmp/release-output}"; STAGING="$REPO_ROOT/tmp/scratch/streamfind-rust-$VERSION-Linux-$ARCH"; mkdir -p "$OUT" "$STAGING/bin" "$STAGING/share/streamfind"
cp "$CARGO_TARGET_DIR/release/streamfind-rust-cli" "$CARGO_TARGET_DIR/release/streamfind-rust-mcp" "$STAGING/bin/"
cp "$CATALOGUE" "$STAGING/share/streamfind/catalogue.duckdb"; cp "$REPO_ROOT/LICENSE.md" "$REPO_ROOT/NOTICE.md" "$REPO_ROOT/rust/LICENSES.md" "$STAGING/"; assert_payload "$STAGING" LICENSES.md
archive="$OUT/streamfind-rust-$VERSION-Linux-$ARCH.tgz"; (cd "$(dirname "$STAGING")" && tar czf "$archive" "$(basename "$STAGING")"); assert_archive "$archive" LICENSES.md
(cd "$OUT" && find . -maxdepth 1 -type f \( -name 'streamfind-*.tgz' -o -name 'streamfind-*.tar.gz' \) -printf '%f\n' | sort | xargs -r sha256sum > sha256sums.txt)
echo "Rust Linux release: $archive"
