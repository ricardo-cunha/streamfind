# How streamfind works

streamfind is a DuckDB-backed analytical runtime. The native C++ core owns
projects, persistence, validation, and execution. Domain plugins provide
catalogued operations through the SDK. The semantic catalogue describes the
operations, parameters, typed ports, results, and usage guidance exposed to
applications and AI agents.

```text
                 Shared semantic catalogue
       operations • parameters • typed ports • results
                         │
          ┌──────────────┴──────────────┐
          ▼                             ▼
   C++ core + plugins       Rust backend (stale development)
          ▼
     C++ public API / MCP
          │
          ▼
   Future React frontend
```

## Operations and operation graphs

**Operations** are the units of computation and project interaction. Each
operation has an identifier, parameters, typed input ports, typed output
ports, and a catalogue description.

A workflow is a persisted operation graph:

```text
operations[]   = node id + operation identifier + parameters
connections[]  = output port -> input port
```

The runtime validates the graph, resolves dependencies from typed connections,
executes each operation, and persists execution records and artifacts. Every
required input must be connected explicitly.

## Project usage model

A typical application or agent follows this sequence:

1. create or open a project with `database_path`;
2. discover operations and inspect their schemas;
3. add operation nodes and persist their parameters;
4. connect compatible output and input ports;
5. validate the operation graph;
6. run the graph and inspect its artifacts and results.

The C++ MCP server is the application boundary. The Rust MCP server is a stale
development backend and is not the current runtime contract.

The future React frontend will use the C++ public API and service boundary,
including MCP or a later HTTP adapter. It will not access DuckDB files or plugin
internals directly.

## C++ plugin framework

The C++ backend is divided into a generic host core, an SDK boundary, and domain
plugins:

- **Core** owns project handles, DuckDB connections, transactions, table
  lifecycle, workflow execution, validation, caching, and audit state.
- **SDK** defines the versioned generic plugin ABI, host callbacks, manifest
  validation, and semantic catalogue integration.
- **Plugins** own domain schemas, native readers, processing algorithms, and
  catalogue-declared Operations and operation graphs.

Plugins are loaded from allowlisted packages. They receive an opaque,
transaction-scoped host access context rather than `Project` or DuckDB handles,
and they use generic table/schema/batch callbacks supplied by the core. This
keeps persistence policy and transaction control in the host while allowing
domain capabilities to be deployed selectively.

## Data and runtime assets

Native packages include the runtime data needed by the MCP servers, especially
the semantic catalogue under `share/streamfind/`. Keep the catalogue with the
server executable. Optional scientific tools remain separate user-installed
components.

See [Releases](releases.md) for package layouts and
[Availability](status.md) for compatibility scope.
