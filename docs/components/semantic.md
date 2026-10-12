# Semantic catalogue and operation discovery

The semantic catalogue is the machine-readable description of streamfind's
public interface. It defines operation names, domains, parameters, typed input
and output ports, constraints, result fields, units, nullability, and usage
guidance.

## What the catalogue provides

The catalogue allows an application or AI agent to discover:

- which Operations can be called directly;
- which operations are available;
- required and optional parameters;
- nested object and array schemas;
- defaults, examples, constraints, and units;
- result shapes and project effects;
- suggested next operations and whether a connection is required.

The browser application, C++ API, and MCP server use the catalogue to present
tool names, descriptions, input schemas, ports, and result contracts.

## Operations and operation graphs

- **Operations** are callable project or domain actions. They expose typed
  ports and operation-specific parameters.
- **Operation graphs** persist operation nodes and typed connections. A node
  stores its operation identifier and JSON parameters.

Typical project entry points are:

```text
create -> describe -> get_domain/get_metadata
      -> get_operations -> get_operation
      -> add_operation -> connect_operations
      -> validate_workflow -> run_workflow
      -> get_artifact_inventory
        -> close
```

## Runtime catalogue

The native packages include the catalogue under `core/`. The MCP server needs
`catalogue.duckdb` at runtime. An explicit `STREAMFIND_CATALOGUE`
path can be used when an application manages the catalogue separately.

See [Releases](../releases.md) for the package contents and
[How streamfind works](../architecture.md) for the relationship between the
catalogue, native backends, and MCP.
