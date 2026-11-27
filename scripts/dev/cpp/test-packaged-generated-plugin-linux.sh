#!/usr/bin/env bash
set -euo pipefail

package_root="${1:?usage: test-packaged-generated-plugin-linux.sh PACKAGE_ROOT PLUGIN_ROOT [PORT]}"
plugin_root="${2:?usage: test-packaged-generated-plugin-linux.sh PACKAGE_ROOT PLUGIN_ROOT [PORT]}"
port="${3:-8892}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
package="$repo_root/tmp/scratch/phase2-packaged-linux"
plugin="$package/plugins/custom/custom.plugin.phase2_example"
stdout="$package/phase2-service.stdout.log"
stderr="$package/phase2-service.stderr.log"
session="phase2-worker"
project_path="$repo_root/tmp/projects/phase2-worker-$port.sf"

test -f "$plugin_root/plugin.json"
test -f "$plugin_root/catalogue.duckdb"
test -f "$plugin_root/libstreamfind_phase2_example.so"
rm -rf -- "$package"
mkdir -p "$package"
cp -a "$package_root/." "$package/"
mkdir -p "$plugin"
cp "$plugin_root/plugin.json" "$plugin_root/catalogue.duckdb" "$plugin_root/libstreamfind_phase2_example.so" "$plugin/"
cat > "$package/bin/streamfind.json" <<EOF
{
  "server": {"port": $port, "automatic_port_fallback": false},
  "worker": {"memory_limit_mb": 0},
  "plugin_roots": {"development": [], "deployment": ["../plugins"]},
  "viewer_roots": {"development": [], "deployment": ["../viewers/native"]},
  "enabled_plugins": ["custom.plugin.phase2_example"],
  "enabled_viewers": []
}
EOF

cleanup() {
    if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then kill "$server_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT
"$package/bin/streamfind-server" "$port" >"$stdout" 2>"$stderr" &
server_pid=$!
for _ in $(seq 1 30); do
    if curl --silent --fail "http://127.0.0.1:$port/health" | grep -q 'healthy'; then break; fi
    sleep 0.5
done
curl --silent --fail "http://127.0.0.1:$port/capabilities/operations?plugin=phase2_example" | grep -q 'phase2_example.ping'
printf '{"session_id":"%s","project_path":"%s","mode":"create"}' "$session" "$project_path" |
    curl --silent --fail -X POST "http://127.0.0.1:$port/projects" -H 'Content-Type: application/json' --data-binary @- >/dev/null
printf '{"operation_instance":"phase2-worker-run","parameters":{},"inputs":null}' |
    curl --silent --fail -X POST "http://127.0.0.1:$port/projects/$session/operations/phase2_example.ping/run" -H 'Content-Type: application/json' --data-binary @- >/dev/null
for _ in $(seq 1 60); do
    state="$(curl --silent --fail "http://127.0.0.1:$port/projects/$session/operations/phase2-worker-run/state")"
    echo "$state" | grep -q '"state":"completed"' && break
    echo "$state" | grep -Eq '"state":"(failed|cancelled)"' && { echo "$state" >&2; exit 1; }
    sleep 0.5
done
curl --silent --fail "http://127.0.0.1:$port/projects/$session/results/current" | grep -q 'examplePingResult'
echo 'packaged generated plugin loaded, executed through the worker, and published its result'
