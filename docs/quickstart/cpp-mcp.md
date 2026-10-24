# Use the C++ MCP server

The C++ MCP server is included in the
[Windows x64 and Linux x86_64 C++ packages](../releases.md). It communicates
with MCP clients using JSON-RPC messages over standard input/output.

## Start the server

After extracting a release package, configure your MCP client to launch:

```text
Windows: <package>\bin\streamfind-mcp.exe
Linux:   <package>/bin/streamfind-mcp
```

Keep the extracted package layout intact. The catalogue and native libraries
are required runtime data for the server.

The package also contains one `streamfind.json` beside the service binaries.
It controls the enabled plugins and the default local service port:

```json
{
  "server": {"port": 8787},
  "plugin_roots": ["../plugins"],
  "enabled_plugins": ["mass_spec", "raman", "sensors", "statistics"]
}
```

The CLI, app host, and MCP runtime use this same configuration. An explicit
CLI port option overrides `server.port`; keep the service bound to loopback
unless remote access is deliberately configured and secured.

## Create and run a project

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
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"mass_spec.get_analyses","arguments":{"database_path":"demo.duckdb"}}}
```

Include `database_path` on every applicable request. Operation-specific
parameters and constraints are returned by `tools/list` and `get_operation`.

## Build a workflow

Use a persisted operation graph for every multi-step workflow:

1. call `get_operations` to discover operations in a domain;
2. call `get_operation` to inspect parameters and typed ports;
3. call `add_operation` with a stable node ID and a JSON `parameters` object;
4. call `connect_operations` for each output-to-input dependency;
5. call `validate_workflow`;
6. call `run_workflow`;
7. call `get_result_registry` to inspect published results.

The workflow is stored in the project database, so it can be reopened,
validated, rerun, and inspected by another client. Use direct operation calls
for independent queries; use a persisted graph when downstream steps depend on
upstream results.

The MCP server uses the same service runtime as the browser application. It
does not open the project DuckDB itself: the service serializes metadata
updates, supervises workers, and publishes validated results for both clients.

## MCP client configuration

Configure an MCP client to launch `streamfind-mcp.exe` or `streamfind-mcp` from
the extracted package over stdio. The executable resolves its catalogue,
plugins, worker, and native runtime paths relative to its own package location;
the package layout must remain intact.

If the catalogue is stored separately, set `STREAMFIND_CATALOGUE` to the path
of `catalogue.duckdb`. Otherwise keep the packaged catalogue in place.
