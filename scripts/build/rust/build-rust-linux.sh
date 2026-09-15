#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/linux-common.sh"
require_tool cargo
CATALOGUE="${STREAMFIND_CATALOGUE:-$REPO_ROOT/tmp/build/linux-cpp/semantic_catalogue/catalogue.duckdb}"
test -f "$CATALOGUE" || { echo "C++ catalogue not found: $CATALOGUE" >&2; exit 1; }
export STREAMFIND_CATALOGUE="$CATALOGUE"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$REPO_ROOT/tmp/build/linux-rust-target}"
ARGS=(build --manifest-path "$REPO_ROOT/rust/Cargo.toml" --workspace)
[[ "${STREAMFIND_BUILD_TYPE:-Debug}" == Release ]] && ARGS+=(--release)
cargo "${ARGS[@]}"
if [[ "${STREAMFIND_RUN_TESTS:-0}" != 0 ]]; then
  TEST=(test --manifest-path "$REPO_ROOT/rust/Cargo.toml" --workspace)
  [[ "${STREAMFIND_BUILD_TYPE:-Debug}" == Release ]] && TEST+=(--release)
  cargo "${TEST[@]}"
fi
echo "Rust Linux build complete: $CARGO_TARGET_DIR"
