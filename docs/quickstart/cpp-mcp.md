# C++ MCP server

The C++ MCP server is included in the
[Windows x64 and Linux x86_64 C++ packages](../releases.md). It communicates
with MCP clients using JSON-RPC messages over standard input/output.

## Package paths

After extracting a release package, launch:

```text
Windows: <package>\bin\streamfind_mcp.exe
Linux:   <package>/bin/streamfind_mcp
```

Keep the package's `share/streamfind/catalogue.duckdb` available. It is required
runtime data for the server.

## Project and Operation flow

A typical client or AI agent uses this sequence:

1. call `initialize`;
2. call `tools/list` to discover callable Operations;
3. call `create` for a new project;
4. call `describe`, `get_domain`, or `get_metadata`;
5. call a domain Operation with `database_path`.

Example requests, one JSON object per line:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"create","arguments":{"database_path":"demo.duckdb","domain":"mass_spec"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"mass_spec.get_analyses_info","arguments":{"database_path":"demo.duckdb"}}}
```

Direct domain Operations are stateless. Include `database_path` on every
applicable request; operation-specific parameters are documented by
`tools/list`. `connect` is not required for this path.

## Workflow Methods

Workflow Methods are processing steps, not MCP tools. To use them:

1. call `connect` with an existing `database_path`;
2. call `get_available_methods` to discover Methods and their complete input
   schemas;
3. use `add_method` or `set_workflow` to create the ordered workflow;
4. call `validate_workflow`;
5. call `run_workflow` or `run_method`;
6. call `close` when finished.

Do not look for Methods in `tools/list`.

## MCP client configuration

Configure an MCP client to launch the extracted executable over stdio. The
C++ package is appropriate when the client should use the C++ implementation;
the Rust package provides the equivalent Rust implementation.

If the catalogue is stored separately, set `STREAMFIND_CATALOGUE` to the path
of `catalogue.duckdb`. Otherwise keep the packaged catalogue in place.
