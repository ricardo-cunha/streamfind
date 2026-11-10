# How streamfind works

streamfind separates project coordination from operation computation. Each
project's DuckDB file stores workflow and metadata; a sibling `<project>.data`
directory stores published result payloads and their query indexes. The native
service is the single control plane: it owns project sessions, database
transactions, workflow scheduling, worker supervision, and result publication.
The browser app connects over HTTP and WebSocket, while the MCP server connects
over its local service client and exposes JSON-RPC over stdio. Both clients see
the same project and runtime state.

```text
                 Shared semantic catalogue
       operations • parameters • typed ports • results
                         │
       ┌─────────────────┴──────────────────┐
       ▼                                    ▼
 Browser app                          MCP client
 HTTP + WebSocket                   JSON-RPC over stdio
       │                                    │
       │                              MCP server
       │                                    │
       └─────────────────┬──────────────────┘
                         ▼
                 Native streamfind service
                 ├── Project DuckDB metadata
                 ├── Supervised operation workers
                 └── Project .data result store
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
executes ready operations, and publishes immutable typed results. Every
required input must be connected explicitly. The service passes intermediate
data between workers using bounded IPC streams or disk-backed result handles,
so large tables do not need to be copied as one in-memory object.

Both workflow modes retain completed results. `detailed` publishes each
operation's results as it completes. `pipeline` defers publication until the
workflow finishes. The mode changes publication timing, not whether results
remain available for inspection.

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

## Backend plugin framework

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

## Frontend plugin framework

The browser application has a separate TypeScript plugin surface. Frontend
plugins extend the UI through manifests and registered capabilities such as
result viewers and editors. They use shared frontend contracts and the service
API to retrieve data; they do not access DuckDB or backend plugin internals.
The backend and frontend plugin systems have different runtimes and packaging
contracts, while sharing the operation catalogue and result schemas. See
[Plugin development](plugin-development.md) for both extension paths.

## Data and runtime assets

Native packages include the runtime data needed by the MCP servers, especially
the semantic catalogue under `core/`. Keep the catalogue with the package.
Optional scientific tools remain separate user-installed
components.

See [Releases](releases.md) for package layouts and
[Availability](status.md) for compatibility scope.
