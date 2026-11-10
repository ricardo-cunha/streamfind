#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TMP_ROOT="$REPO_ROOT/tmp"

require_tool() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 1; }
}

ensure_streamfind_example_data() {
    local managed_home repository_root data_root
    if [[ -n "${STREAMFIND_EXAMPLE_DATA_ROOT:-}" ]]; then
        data_root="$(realpath -m "$STREAMFIND_EXAMPLE_DATA_ROOT")"
        [[ -d "$data_root" ]] || { echo "STREAMFIND_EXAMPLE_DATA_ROOT is not a directory: $data_root" >&2; return 1; }
        export STREAMFIND_DATA_REPOSITORY_ROOT="$(dirname "$data_root")"
        return 0
    fi
    managed_home="${STREAMFIND_HOME:-$HOME/.streamfind}"
    repository_root="$managed_home/streamfind.data"
    data_root="$repository_root/data"
    if [[ ! -d "$data_root" ]]; then
        [[ ! -e "$repository_root" ]] || { echo "managed streamfind.data path exists without data/: $repository_root" >&2; return 1; }
        require_tool git
        mkdir -p "$managed_home"
        echo "Downloading streamfind.data to $repository_root"
        if ! git clone --depth 1 https://git.uni-due.de/odea-project/streamfind/streamfind.data "$repository_root"; then
            rm -rf -- "$repository_root"
            return 1
        fi
    fi
    [[ -d "$data_root" ]] || { echo "streamfind.data checkout has no data directory: $data_root" >&2; return 1; }
    export STREAMFIND_DATA_REPOSITORY_ROOT="$repository_root"
}

sanitize_linux_build_environment() {
    local entry filtered_path='' variable value
    local -a path_entries=()

    IFS=: read -r -a path_entries <<< "${PATH:-}"
    for entry in "${path_entries[@]}"; do
        [[ -z "$entry" ]] && continue
        if [[ "$entry" =~ ^/mnt/[a-zA-Z]/ || "$entry" =~ ^[a-zA-Z]:[/\\] ]]; then
            continue
        fi
        filtered_path+="${filtered_path:+:}$entry"
    done
    export PATH="$filtered_path"

    # WSL imports Windows process variables. Remove imported Windows package
    # hints as a unit so CMake and pkg-config cannot mix Windows libraries into
    # a Linux build. Linux-only entries remain untouched when no Windows path is
    # present.
    for variable in CMAKE_PREFIX_PATH CMAKE_MODULE_PATH CMAKE_LIBRARY_PATH \
        CMAKE_INCLUDE_PATH CMAKE_TOOLCHAIN_FILE PKG_CONFIG_PATH PKG_CONFIG_LIBDIR \
        PKG_CONFIG_SYSROOT_DIR; do
        value="${!variable-}"
        if [[ "$value" =~ /mnt/[a-zA-Z]/msys64(/|$) || "$value" =~ [a-zA-Z]:[/\\] || "$value" =~ (^|[/;:])/(ucrt64|mingw64|clang64)(/|$) ]]; then
            unset "$variable"
        fi
    done

    while IFS= read -r variable; do
        [[ "$variable" =~ (_DIR|_ROOT)$ ]] || continue
        value="${!variable-}"
        if [[ "$value" =~ /mnt/[a-zA-Z]/msys64(/|$) || "$value" =~ [a-zA-Z]:[/\\] || "$value" =~ (^|[/;:])/(ucrt64|mingw64|clang64)(/|$) ]]; then
            unset "$variable"
        fi
    done < <(compgen -e)

    unset MSYSTEM MINGW_PREFIX MSYS2_PATH_TYPE
}

reset_mixed_linux_build_tree() {
    local build_dir="$1" cache_file resolved_build managed_build_root
    cache_file="$build_dir/CMakeCache.txt"
    [[ -f "$cache_file" ]] || return 0

    if ! grep -qiE 'msys64|(^|[=;])/(ucrt64|mingw64|clang64)(/|$)|^(CMAKE_(C|CXX)_COMPILER|CMAKE_MAKE_PROGRAM|[^#[:space:]]+_DIR):[^=]*=.*[a-zA-Z]:[/\\]|^CMAKE_(C|CXX)_COMPILER:[^=]*=[^[:space:]]+\.exe|^CMAKE_MAKE_PROGRAM:[^=]*=[^[:space:]]+\.exe' "$cache_file"; then
        return 0
    fi

    resolved_build="$(realpath -m "$build_dir")"
    managed_build_root="$(realpath -m "$TMP_ROOT/build")/"
    case "$resolved_build/" in
        "$managed_build_root"*) ;;
        *)
            echo "refusing to remove contaminated build tree outside $TMP_ROOT/build: $resolved_build" >&2
            return 1
            ;;
    esac

    echo "Removing Linux CMake build tree containing Windows/MSYS2 paths: $resolved_build" >&2
    rm -rf -- "$resolved_build"
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
