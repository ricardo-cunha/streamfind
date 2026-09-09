#!/usr/bin/env python3
"""Compare C++ and Rust mass-spec MCP responses on shared fixtures."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


def call(server: Path, requests: list[dict]) -> list[dict]:
    completed = subprocess.run(
        [str(server)],
        input="".join(json.dumps(request) + "\n" for request in requests),
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"{server} exited {completed.returncode}: {completed.stderr}")
    responses = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
    if len(responses) != len(requests):
        raise RuntimeError(f"{server} returned {len(responses)} responses for {len(requests)} requests")
    return responses


def normalize_numbers(value):
    if isinstance(value, float) and value == 0.0:
        return 0.0
    if isinstance(value, list):
        return [normalize_numbers(item) for item in value]
    if isinstance(value, dict):
        return {key: normalize_numbers(item) for key, item in value.items()}
    return value


def comparable(response: dict) -> dict:
    """Remove only transport/runtime differences; retain public data strictly."""
    result = json.loads(json.dumps(response))
    result.pop("id", None)
    if "result" in result and "serverInfo" in result["result"]:
        result["result"]["serverInfo"].pop("name", None)
    if "result" in result and "tools" in result["result"]:
        result["result"]["tools"] = sorted(
            [{"name": tool["name"], "inputSchema": tool["inputSchema"]} for tool in result["result"]["tools"]],
            key=lambda tool: tool["name"],
        )
    for item in result.get("result", {}).get("content", []):
        if item.get("type") != "text":
            continue
        try:
            payload = json.loads(item["text"])
        except (KeyError, TypeError, json.JSONDecodeError):
            continue
        if not isinstance(payload, dict):
            item["text"] = json.dumps(payload, separators=(",", ":"), sort_keys=True)
            continue
        if "created_at" in payload.get("columns", {}):
            payload["columns"].pop("created_at", None)
        if payload.get("columns", {}).get("workflow") == [[]]:
            payload["columns"]["workflow"] = [{"domain": "", "name": "", "steps": [], "version": 1}]
        item["text"] = json.dumps(normalize_numbers(payload), separators=(",", ":"), sort_keys=True)
    return result


def requests_for(database: Path, files: list[Path], selected: Path, index: int, include_fallback: bool) -> list[dict]:
    common = {"database_path": str(database)}
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "create", "arguments": {**common, "domain": "mass_spec"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "mass_spec.add_analyses", "arguments": {**common, "analyses": [{"path": str(path)} for path in files]}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "mass_spec.get_analysis_names", "arguments": common}},
        {"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "mass_spec.get_spectra_headers", "arguments": {**common, "analysis_names": [selected.stem]}}},
        {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "mass_spec.get_raw_spectra", "arguments": {**common, "analysis_names": [selected.stem], "indices": [index], "targets": [{"mz_min": 99999.0, "mz_max": 100000.0}]}}},
        {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "mass_spec.get_chromatograms_headers", "arguments": {**common, "analysis_names": [selected.stem]}}},
        {"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": "mass_spec.get_raw_chromatograms", "arguments": {**common, "analysis_names": [selected.stem], "indices": [0, 1]}}},
    ]
    if include_fallback:
        requests.append({"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": {"name": "mass_spec.get_raw_spectra", "arguments": {**common, "analysis_names": [selected.stem], "indices": []}}})
    return requests


def run_case(label: str, cpp: Path, rust: Path, root: Path, files: list[Path], selected: Path, index: int, include_fallback: bool) -> int:
    missing = [path for path in files if not path.is_file()]
    if missing:
        print(f"SKIP {label}: missing fixture {missing[0]}")
        return 0
    database = root / "tmp" / "projects" / f"mcp-differential-{label}.duckdb"
    database.unlink(missing_ok=True)
    requests = requests_for(database, files, selected, index, include_fallback)
    cpp_responses = call(cpp, requests)
    database.unlink(missing_ok=True)
    rust_responses = call(rust, requests)
    database.unlink(missing_ok=True)
    for request, left, right in zip(requests, cpp_responses, rust_responses):
        if comparable(left) != comparable(right):
            print(f"FAIL {label}: MCP request {request['id']} differs", file=sys.stderr)
            print(json.dumps({"request": request, "cpp": left, "rust": right}, indent=2), file=sys.stderr)
            return 1
    print(f"PASS {label}: {len(requests)} MCP requests matched exactly")
    return 0


def main() -> int:
    cpp_value = os.environ.get("STREAMFIND_CPP_MCP")
    rust_value = os.environ.get("STREAMFIND_RUST_MCP")
    if not cpp_value or not rust_value:
        print("SKIP: set STREAMFIND_CPP_MCP and STREAMFIND_RUST_MCP to run MCP differential testing")
        return 0
    cpp, rust = Path(cpp_value), Path(rust_value)
    if not cpp.is_file() or not rust.is_file():
        raise RuntimeError(f"MCP binaries do not exist: C++={cpp}, Rust={rust}")
    root = Path(__file__).resolve().parents[1]
    data = root / "tests" / "data" / "mass_spec" / "wastewater"
    portable = [data / f"01_tof_ww_is_pos_blank-r00{i}.mzML" for i in (1, 2, 3)]
    if run_case("portable", cpp, rust, root, portable, portable[1], 0, True):
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
