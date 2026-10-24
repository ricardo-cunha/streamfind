# How streamfind works

streamfind uses DuckDB for project metadata and a service-managed Result
Runtime for execution data. The native service owns project sessions,
persistence, validation, workflow scheduling, result publication, and the
public HTTP/WebSocket boundary. Short-lived workers execute one operation at
a time through the service data proxy. Domain plugins provide catalogued
operations through the SDK. The semantic catalogue describes the operations,
parameters, typed ports, results, and usage guidance exposed to applications
and AI agents.

```text
                 Shared semantic catalogue
       operations • parameters • typed ports • results
                         │
          ┌────────────────────────────┐
          ▼                            ▼
   Service control plane          MCP over stdio
          │                            │
    ┌─────┴─────┐                      │
    ▼           ▼                      │
 Workers   Result Runtime              │
    └─────┬─────┘                      │
          └──────────────┬─────────────┘
                         ▼
                    React web app
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
executes each operation, and publishes immutable typed results. Every required
input must be connected explicitly. Intermediate data can be streamed between
workers or spilled to the project result store according to the run mode.

## Project usage model

A typical application or agent follows this sequence:

1. create or open a project with `database_path`;
2. discover operations and inspect their schemas;
3. add operation nodes and persist their parameters;
4. connect compatible output and input ports;
5. validate the operation graph;
6. run the graph and inspect its results and execution history.

The C++ MCP server is the application boundary and current runtime contract.

The React web app uses the C++ public API and service boundary. It does not
access DuckDB files or plugin internals directly. Both native archives contain
the built app and a launcher that starts the local service and opens it in a
browser. The [Web app](web-app.md) page documents the platform paths.

## C++ plugin framework

The C++ backend is divided into a generic host core, an SDK boundary, and domain
plugins:

- **Service** owns project sessions, DuckDB metadata connections and
  transactions, workflow scheduling, validation, result publication, caching,
  and audit state.
- **SDK** defines the versioned generic plugin ABI, host callbacks, manifest
  validation, and semantic catalogue integration.
- **Workers and plugins** own domain schemas, native readers, processing
  algorithms, and catalogue-declared Operations. They receive typed inputs and
  deliver typed output batches through the SDK; they do not receive project or
  DuckDB handles.

Plugins are loaded from allowlisted packages. They receive an opaque,
transaction-scoped host access context rather than `Project` or DuckDB handles,
and they use the service data proxy plus generic table/schema/batch callbacks.
Arrow IPC is used for bounded tabular transport. This keeps persistence
policy, result validation, and transaction control in the service while
allowing domain capabilities to be deployed selectively.

## Data and runtime assets

Native packages include the runtime data needed by the MCP servers, especially
the semantic catalogue under `core/`. Keep the catalogue with the package.
Optional scientific tools remain separate user-installed
components.

See [Releases](releases.md) for package layouts and
[Availability](status.md) for compatibility scope.
