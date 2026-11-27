# C++ runtime and API

The C++ runtime is the native foundation of streamfind. It provides the
project and workflow engine, shared service, CLI, MCP server, worker supervisor,
and dynamic backend plugin host. Windows x64 and Linux x86_64 packages are
listed on [Releases](../releases.md).

!!! note "Compatibility"
    The native C++ package is versioned. Use the native and viewer SDKs shipped
    in the same archive when building extensions; cross-version C++ ABI stability
    is not guaranteed.

## Package contents

The C++ package includes:

- `streamfind-mcp` / `streamfind-mcp.exe`;
- the native runtime libraries and platform dependencies;
- `streamfind-dev`, the native SDK and the viewer SDK;
- DuckDB and other native runtime dependencies;
- `core/catalogue.duckdb` and packaged plugin catalogues.

The runtime archive contains the built-in plugin and viewer packages, app assets
and the matching development payload. The catalogue is required by the MCP server and
must remain with the package. Use the SDK from the same archive to build an
out-of-tree backend plugin.

## Extension model

The service owns project persistence, DuckDB metadata connections,
transactions, schema lifecycle, operation-graph execution, validation, the
Result Runtime, cache, and audit state. Workers use the service data proxy and
never open the project DuckDB. The SDK defines the generic versioned plugin ABI
and typed input/output callbacks. Backend plugins own their catalogues, schemas,
native readers, algorithms, and Operations.

Dynamic plugins receive an opaque transaction-scoped host access context. They do
not receive `streamfind::Project` or DuckDB handles, and they do not create
project tables outside the host lifecycle. Use the [plugin development guide](../plugin-development.md)
and the matching SDK from the same runtime archive when building an out-of-tree plugin.

## Project API

`streamfind::Project` provides persistence inside a service-managed `.sf`
folder. The service owns the project session and metadata connection;
clients and workers use the public service/API boundary rather than opening
the file independently:

```cpp
streamfind::ProjectOptions options{
    "project.duckdb",
    streamfind::Json{{"name", "demo"}},
    streamfind::Json::object()
};
auto project = streamfind::Project::create(options);
```

The user-visible project path is the `.sf` folder, containing a fixed
`project.duckdb` file and a `data/` result directory. `ProjectOptions` is an
internal persistence-layer configuration and contains the database path,
project metadata, and initial workflow metadata. A new project starts with an empty workflow. The project's
`get_plugins()` result is inferred from the plugin prefixes of operation IDs in
that workflow; it is not a setting supplied at project creation.

The MCP exposes these project tools:

```text
create, describe, validate
get_project_metadata, set_project_metadata
get_workflow_metadata, set_workflow_metadata, get_project_plugins
get_workflow, get_workflow_execution
get_result_inventory, get_current_result_inventory, request_result
get_audit_trail, close
```

Catalogue discovery is separate from project inspection:

```text
get_plugins, get_modules, get_operations, get_operation
```

The C++ `Project` API also provides workflow editing, result inventory queries,
audit access, and project copying. These are C++ API methods and should not be
confused with MCP tool names. The project API exposes project metadata,
workflow-derived plugins, published results, and audit information.

## MCP

The [C++ MCP quickstart](../quickstart/cpp-mcp.md) documents the stdio server
and persisted operation-graph workflow. The browser application consumes this
public boundary and does not access DuckDB or plugin internals directly.
