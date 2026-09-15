# Semantic catalogue

The semantic catalogue is the shared public description of streamfind's
interfaces. It defines operation names, workflow Methods, domains, parameters,
input constraints, result fields, units, nullability, and agent-facing usage
guidance.

## What the catalogue provides

The catalogue allows an application or AI agent to discover:

- which Operations can be called directly;
- which workflow Methods are available;
- required and optional parameters;
- nested object and array schemas;
- defaults, examples, constraints, and units;
- result shapes and project effects;
- suggested next operations and whether a connection is required.

The C++ and Rust MCP servers use the same catalogue so that their tool names,
descriptions, and input schemas remain equivalent.

## Operations and Methods

- **Operations** are callable project or domain actions. Domain Operations are
  stateless and require `database_path`; additional parameters are
  operation-specific.
- **Methods** are workflow steps. Use `get_available_methods` to discover them
  and their schemas; Methods are not advertised as MCP tools.

Typical project entry points are:

```text
create -> describe -> get_domain/get_metadata
      -> domain Operations

connect -> get_available_methods
        -> set_workflow/validate_workflow
        -> run_workflow or run_method
        -> close
```

## Runtime catalogue

The native packages include the catalogue under `share/streamfind/`. The MCP
server needs `catalogue.duckdb` at runtime. An explicit `STREAMFIND_CATALOGUE`
path can be used when an application manages the catalogue separately.

See [Releases](../releases.md) for the package contents and
[How streamfind works](../architecture.md) for the relationship between the
catalogue, native backends, and MCP.
