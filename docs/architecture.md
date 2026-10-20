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
          ┌────────────────────────────┐
          ▼                            ▼
   C++ core + plugins             C++ public API / MCP
          │                            │
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
executes each operation, and persists execution records and artifacts. Every
required input must be connected explicitly.

## Operation cancellation

Cancellation is an operation-level runtime contract, not only a visual action
on a workflow node. The browser sends the operation instance identifier to the
service. The service owns a cancellation token for that operation and passes
the token through the SDK access object used by the plugin.

Long-running operations must check the token at bounded intervals and stop at a
safe boundary. This applies to analysis and sample loops, table processing,
statistics, file and network work, nested workflows, and parallel workers. A
plugin should use the SDK cancellation checkpoint rather than maintaining a
separate cancellation flag.

Operations that delegate work need an explicit interruption mechanism as well:

- DuckDB work should use an interruptible connection or query boundary.
- External programs must run in an owned process group or Windows Job Object;
  cancellation must terminate the complete child process tree and wait for it
  to exit.
- Parallel workers must share the operation token and be joined before the
  operation is reported as cancelled.

The observable state transition is:

```text
running -> cancelling -> cancelled
running -> completed
running -> failed
```

The service should scope cancellation by operation instance, make repeated
stop requests idempotent, and report `cancelled` only after workers and child
processes have stopped. Partial output must not be published as a successful
artifact. The cancellation contract should be covered by native tests for
cooperative loops, nested/parallel work, DuckDB interruption, external
process termination, repeated cancellation, and normal failure handling.

## Project usage model

A typical application or agent follows this sequence:

1. create or open a project with `database_path`;
2. discover operations and inspect their schemas;
3. add operation nodes and persist their parameters;
4. connect compatible output and input ports;
5. validate the operation graph;
6. run the graph and inspect its artifacts and results.

The C++ MCP server is the application boundary and current runtime contract.

The React web app uses the C++ public API and service boundary. It does not
access DuckDB files or plugin internals directly. Both native archives contain
the built app and a launcher that starts the local service and opens it in a
browser. The [Web app](web-app.md) page documents the platform paths.

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
the semantic catalogue under `core/`. Keep the catalogue with the package.
Optional scientific tools remain separate user-installed
components.

See [Releases](releases.md) for package layouts and
[Availability](status.md) for compatibility scope.
