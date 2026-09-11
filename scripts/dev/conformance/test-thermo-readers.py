#!/usr/bin/env python3
"""Run Thermo RAW C++/Rust MCP parity checks on development fixtures.

This is deliberately outside the official C++ and Rust test suites. The RAW
fixtures are external development data; all assertions go through the public
MCP interface.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


def call(server: Path, requests: list[dict]) -> list[dict]:
    completed = subprocess.run([str(server)], input="".join(json.dumps(r) + "\n" for r in requests), text=True, capture_output=True)
    if completed.returncode:
        raise RuntimeError(f"{server} exited {completed.returncode}: {completed.stderr}")
    responses = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
    if len(responses) != len(requests):
        raise RuntimeError(f"{server} returned {len(responses)} responses for {len(requests)} requests")
    return responses


def comparable(response: dict) -> dict:
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
        if isinstance(payload, dict):
            if "created_at" in payload.get("columns", {}):
                payload["columns"].pop("created_at", None)
            if payload.get("columns", {}).get("workflow") == [[]]:
                payload["columns"]["workflow"] = [{"domain": "", "name": "", "steps": [], "version": 1}]
            item["text"] = json.dumps(payload, separators=(",", ":"), sort_keys=True)
    return result


def requests_for(database: Path, fixture: Path) -> list[dict]:
    common = {"database_path": str(database), "project_id": "thermo-mcp-development"}
    return [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "create", "arguments": {**common, "domain": "mass_spec"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "mass_spec.add_analyses", "arguments": {**common, "analyses": [{"path": str(fixture)}]}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "mass_spec.get_analysis_names", "arguments": common}},
        {"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "mass_spec.get_spectra_headers", "arguments": {**common, "analysis_names": [fixture.stem]}}},
        {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "mass_spec.get_raw_spectra", "arguments": {**common, "analysis_names": [fixture.stem], "indices": [0, 1], "targets": []}}},
        {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "mass_spec.get_chromatograms_headers", "arguments": {**common, "analysis_names": [fixture.stem]}}},
        {"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": "mass_spec.get_raw_chromatograms", "arguments": {**common, "analysis_names": [fixture.stem], "indices": [0, 1]}}},
    ]


def main() -> int:
    cpp = Path(os.environ.get("STREAMFIND_CPP_MCP", ""))
    rust = Path(os.environ.get("STREAMFIND_RUST_MCP", ""))
    if not cpp.is_file() or not rust.is_file():
        print("SKIP: set STREAMFIND_CPP_MCP and STREAMFIND_RUST_MCP to run Thermo MCP tests")
        return 0
    root = Path(__file__).resolve().parents[3]
    vendor_root = Path(os.environ.get("STREAMFIND_VENDOR_DATA_ROOT", r"E:\example_files\raw_vendor_files"))
    fixtures = sorted(vendor_root.joinpath("thermo").rglob("*.raw"))
    if not fixtures:
        print(f"SKIP: no Thermo RAW fixtures under {vendor_root}")
        return 0
    for fixture in fixtures:
        database = root / "tmp" / "projects" / f"thermo-mcp-{fixture.stem}.duckdb"
        database.unlink(missing_ok=True)
        requests = requests_for(database, fixture)
        left = call(cpp, requests)
        database.unlink(missing_ok=True)
        right = call(rust, requests)
        database.unlink(missing_ok=True)
        for request, cpp_response, rust_response in zip(requests, left, right):
            if comparable(cpp_response) != comparable(rust_response):
                print(json.dumps({"request": request, "cpp": cpp_response, "rust": rust_response}, indent=2), file=sys.stderr)
                return 1
        print(f"PASS {fixture.name}: {len(requests)} MCP requests matched exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
