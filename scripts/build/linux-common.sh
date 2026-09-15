#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TMP_ROOT="$REPO_ROOT/tmp"

require_tool() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 1; }
}

assert_payload() {
    local root="$1" license_payload="${2:-licenses}"
    test -f "$root/NOTICE.md" || { echo "missing NOTICE.md" >&2; exit 1; }
    test -f "$root/LICENSE.md" || { echo "missing LICENSE.md" >&2; exit 1; }
    if [[ "$license_payload" == licenses ]]; then test -d "$root/licenses"; else test -f "$root/$license_payload"; fi
}

assert_archive() {
    local archive="$1" license_payload="${2:-licenses}"
    local listing="${archive}.list"
    tar -tzf "$archive" > "$listing"
    grep -Eq '(^|/)NOTICE[.]md$' "$listing"
    grep -Eq '(^|/)LICENSE[.]md$' "$listing"
    if [[ "$license_payload" == licenses ]]; then grep -Eq '(^|/)licenses/' "$listing"; else grep -Eq "(^|/)$license_payload$" "$listing"; fi
    rm -f "$listing"
}
