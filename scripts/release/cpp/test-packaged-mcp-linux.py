#!/usr/bin/env python3
"""Smoke-test the extracted Linux MCP runtime over newline-delimited JSON."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def request(process: subprocess.Popen[str], payload: dict, expected_id: int) -> dict:
    process.stdin.write(json.dumps(payload) + "\n")
    process.stdin.flush()
    line = process.stdout.readline()
    if not line:
        error = process.stderr.read()
        raise RuntimeError(f"MCP exited before response: {error}")
    response = json.loads(line)
    if response.get("id") != expected_id:
        raise RuntimeError(f"unexpected MCP response: {response}")
    if "error" in response:
        raise RuntimeError(f"MCP request failed: {response['error']}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package_root", type=Path)
    args = parser.parse_args()
    root = args.package_root.resolve()
    executable = root / "bin" / "streamfind_mcp"
    if not executable.is_file():
        raise SystemExit(f"missing packaged MCP executable: {executable}")
    env = os.environ.copy()
    env.pop("STREAMFIND_CATALOGUE", None)
    process = subprocess.Popen([str(executable)], cwd=root / "bin", env=env,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        initialized = request(process, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}, 1)
        if initialized["result"]["serverInfo"]["name"] != "streamfind-cpp":
            raise RuntimeError(f"unexpected server: {initialized}")
        tools = request(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}, 2)
        names = {tool["name"] for tool in tools["result"]["tools"]}
        for required in ("create", "describe", "get_operations", "run_operation"):
            if required not in names:
                raise RuntimeError(f"missing packaged MCP tool: {required}")
        print(f"Linux packaged MCP smoke passed ({len(names)} tools)")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        if process.returncode not in (0, -15):
            sys.stderr.write(process.stderr.read())
            raise RuntimeError(f"MCP exited with {process.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
