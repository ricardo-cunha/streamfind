#!/usr/bin/env python3
"""Compare native C++ and Rust SCIEX MCP output on development WIFF files.

External vendor files are intentionally opt-in and are not part of official tests.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


def call(server: Path, requests: list[dict]) -> list[dict]:
    root = Path(__file__).resolve().parents[2]
    runtime = root / "tmp" / "build" / "core-default" / "tests"
    path = os.environ.get("PATH", "")
    completed = subprocess.run(
        [str(server)],
        input="".join(json.dumps(request) + "\n" for request in requests),
        text=True,
        capture_output=True,
        check=False,
        env={**os.environ, "PATH": f"{runtime};{path}", "STREAMFIND_TRACE_PAYLOAD_DECODES": "1"},
    )
    if completed.returncode:
        raise RuntimeError(f"{server} exited {completed.returncode}: {completed.stderr}")
    responses = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
    if len(responses) != len(requests):
        raise RuntimeError(f"{server} returned {len(responses)} responses for {len(requests)} requests")
    return responses


NUMBER = re.compile(r"^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$")


def normalize_table_values(value):
    if isinstance(value, str) and NUMBER.fullmatch(value):
        return float(value) if any(character in value for character in ".eE") else int(value)
    if isinstance(value, list):
        return [normalize_table_values(item) for item in value]
    if isinstance(value, dict):
        return {key: normalize_table_values(item) for key, item in value.items()}
    return value


def comparable(response: dict) -> dict:
    result = json.loads(json.dumps(response))
    result.pop("id", None)
    server_info = result.get("result", {}).get("serverInfo")
    if isinstance(server_info, dict):
        server_info.pop("name", None)
    tools = result.get("result", {}).get("tools")
    if isinstance(tools, list):
        result["result"]["tools"] = sorted(
            [{"name": tool["name"], "inputSchema": tool["inputSchema"]} for tool in tools],
            key=lambda tool: tool["name"],
        )
    for item in result.get("result", {}).get("content", []):
        if item.get("type") != "text":
            continue
        if result.get("result", {}).get("isError"):
            item["text"] = item["text"].replace(
                "mass_spec.get_raw_spectra: mass spectrometry ", ""
            ).replace("InvalidArgument: ", "")
            continue
        try:
            payload = json.loads(item["text"])
        except (KeyError, TypeError, json.JSONDecodeError):
            continue
        if isinstance(payload, dict):
            payload.get("columns", {}).pop("created_at", None)
            if payload.get("columns", {}).get("workflow") == [[]]:
                payload["columns"]["workflow"] = [{"domain": "", "name": "", "steps": [], "version": 1}]
            payload = normalize_table_values(payload)
            item["text"] = json.dumps(payload, separators=(",", ":"), sort_keys=True)
    return result


def requests_for(database: Path, fixture: Path) -> list[dict]:
    common = {"database_path": str(database), "project_id": "sciex-mcp-development"}
    selected = fixture.stem
    return [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "create", "arguments": {**common, "domain": "mass_spec"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "mass_spec.add_analyses", "arguments": {**common, "analyses": [{"path": str(fixture)}]}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "mass_spec.get_analysis_names", "arguments": common}},
        {"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "mass_spec.get_analyses_info", "arguments": common}},
        {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "mass_spec.get_spectra_headers", "arguments": {**common, "analysis_names": [selected]}}},
        # Keep corpus validation bounded: headers and persistence are exhaustive;
        # payload parity samples one public index from each family.
        {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "mass_spec.get_raw_spectra", "arguments": {**common, "analysis_names": [selected], "indices": [0], "targets": []}}},
        {"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": "mass_spec.get_chromatograms_headers", "arguments": {**common, "analysis_names": [selected]}}},
        {"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": {"name": "mass_spec.get_raw_chromatograms", "arguments": {**common, "analysis_names": [selected], "indices": [0]}}},
    ]


def run_fixture(cpp: Path, rust: Path, root: Path, fixture: Path) -> str:
    database = root / "tmp" / "projects" / f"sciex-mcp-{fixture.stem}.duckdb"
    database.unlink(missing_ok=True)
    requests = requests_for(database, fixture)
    left = call(cpp, requests)
    database.unlink(missing_ok=True)
    right = call(rust, requests)
    database.unlink(missing_ok=True)
    for request, cpp_response, rust_response in zip(requests, left, right):
        if comparable(cpp_response) != comparable(rust_response):
            raise RuntimeError(
                f"{fixture.name}: MCP request {request['id']} differs\n"
                + json.dumps({"request": request, "cpp": cpp_response, "rust": rust_response}, indent=2)
            )
    return f"PASS {fixture.name}: {len(requests)} MCP requests matched exactly"


def main() -> int:
    cpp = Path(os.environ.get("STREAMFIND_CPP_MCP", ""))
    rust = Path(os.environ.get("STREAMFIND_RUST_MCP", ""))
    if not cpp.is_file() or not rust.is_file():
        print("SKIP: set STREAMFIND_CPP_MCP and STREAMFIND_RUST_MCP")
        return 0
    root = Path(__file__).resolve().parents[2]
    vendor_root = Path(os.environ.get("STREAMFIND_VENDOR_DATA_ROOT", r"E:\example_files\raw_vendor_files"))
    fixtures = sorted(vendor_root.joinpath("sciex").rglob("*.wiff"))
    if not fixtures:
        print(f"SKIP: no SCIEX WIFF fixtures under {vendor_root}")
        return 0
    workers = max(1, int(os.environ.get("STREAMFIND_SCIEX_DIFFERENTIAL_WORKERS", "4")))
    completed = 0
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(run_fixture, cpp, rust, root, fixture) for fixture in fixtures]
        for future in as_completed(futures):
            try:
                print(future.result())
                completed += 1
            except Exception as error:
                print(f"FAIL: {error}", file=sys.stderr)
                return 1
    if completed != len(fixtures):
        raise RuntimeError(f"completed {completed} SCIEX fixtures, expected {len(fixtures)}")
    print(f"PASS SCIEX corpus: {completed} fixtures")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
