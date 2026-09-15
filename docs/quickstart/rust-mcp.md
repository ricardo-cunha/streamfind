# Rust MCP server

The Rust MCP server is included in the
[Windows x64 and Linux x86_64 Rust packages](../releases.md). It communicates
with MCP clients using JSON-RPC messages over standard input/output.

!!! warning "Preserved backend"
    Rust development is currently paused. This quickstart documents the
    preserved Rust package for existing compatibility work; new capabilities
    target the C++ backend and its plugin framework.

## Package paths

After extracting a release package, launch:

```text
Windows: <package>\bin\streamfind-rust-mcp.exe
Linux:   <package>/bin/streamfind-rust-mcp
```

Keep `share/streamfind/catalogue.duckdb` and `catalogue.json` with the
package. The DuckDB catalogue is required runtime data for the MCP server.

## Project and Operation flow

A typical client or AI agent uses this sequence:

1. call `initialize` and read its usage instructions;
2. call `tools/list` to discover callable Operations;
3. call `create` for a new project;
4. call `describe`, `get_domain`, or `get_metadata`;
5. call domain Operations with explicit `database_path` and `project_id`.

Example requests, one JSON object per line:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"create","arguments":{"database_path":"demo.duckdb","project_id":"demo","domain":"mass_spec"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"mass_spec.get_analyses_info","arguments":{"database_path":"demo.duckdb","project_id":"demo"}}}
```

Direct domain Operations are stateless and do not require `connect` or
`close`.

## Workflow Methods

Workflow Methods are processing steps, not MCP tools. To use them:

1. call `connect` with an existing `database_path` and `project_id`;
2. call `get_available_methods` to discover Methods and their complete schemas;
3. use `add_method` or `set_workflow` to create the ordered workflow;
4. call `validate_workflow`;
5. call `run_workflow` or `run_method`;
6. call `close` when finished.

`tools/list` exposes Operations before connection; it is not the Method
discovery endpoint.

## Rust CLI

The Rust package also includes a CLI for project creation and inspection:

```text
streamfind-rust-cli create --help
streamfind-rust-cli describe --help
```

## MCP client configuration

Configure an MCP client to launch the extracted Rust executable over stdio.
If the catalogue is stored separately, set `STREAMFIND_CATALOGUE` to the path
of `catalogue.duckdb`; otherwise keep the packaged catalogue in place.

Optional tools such as Open Babel, Java, and MetFrag are separate dependencies
and are not installed automatically by the native package.
