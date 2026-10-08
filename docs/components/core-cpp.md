# C++ API

The C++ backend provides a native project API, a dynamic plugin framework, and a C++ MCP server. The current
Windows x64 and Linux x86_64 packages are listed on [Releases](../releases.md).

!!! note "Compatibility"
    The native C++ package is a versioned preview release. It is suitable
    for applications and integration testing, but does not yet promise a stable
    cross-version ABI.

## Package contents

The C++ package includes:

- `streamfind_mcp` / `streamfind_mcp.exe`;
- the C++ libraries and public headers;
- DuckDB and other native runtime dependencies;
- `share/streamfind/catalogue.duckdb`.

The catalogue is required by the MCP server and must remain with the package.

## Plugin framework

The C++ host owns project persistence, DuckDB connections, transactions, schema
lifecycle, operation-graph execution, validation, cache, and audit state. The
SDK defines the generic versioned plugin ABI and host callbacks. Domain plugins
own their catalogues, schemas, native readers, algorithms, and Operations.

Dynamic plugins receive an opaque transaction-scoped host access context. They do
not receive `streamfind::Project` or DuckDB handles, and they do not create
project tables outside the host lifecycle. This is the active extension boundary
for new C++ capabilities.

## Project API

`streamfind::Project` owns one DuckDB-backed project selected by
`ProjectOptions`:

```cpp
streamfind::ProjectOptions options{
    "project.duckdb", "demo", std::nullopt, false, false, "mass_spec"
};
auto project = streamfind::Project::create(options);
```

Common project operations include:

```text
create, describe, validate
get_metadata, set_metadata
get_domain
get_workflow, set_workflow, validate_workflow, run_workflow
get_operations, get_operation, add_operation, connect_operations
get_artifact_inventory, get_current_artifact_inventory, request_artifact
get_audit_trail
copy, close
```

Domains are assigned when a project is created and are immutable afterward.

## MCP

The [C++ MCP quickstart](../quickstart/cpp-mcp.md) documents the stdio server
and the persisted operation-graph workflow. The current React development/
preview frontend consumes this C++ public boundary and does not access DuckDB or
plugin internals directly.
