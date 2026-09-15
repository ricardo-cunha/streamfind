#!/usr/bin/env bash
# Run the complete native C++ NTA workflow on Linux.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
if ! command -v pwsh >/dev/null 2>&1; then
  printf 'error: pwsh (PowerShell 7) is required for the shared NTA harness\n' >&2
  exit 2
fi

if [[ "${1:-}" != "-SkipBuild" ]]; then
  bash "$ROOT/scripts/build/cpp/build-cpp-linux.sh"
fi

exec pwsh -NoProfile -File "$ROOT/scripts/dev/cpp/test-nta.ps1" -Backend Cpp -RunPipeline "$@"
