# How streamfind works

streamfind presents one public contract through several interfaces. The shared
semantic catalogue describes operations, workflow Methods, parameters, schemas,
results, and usage guidance. Native backends implement that contract, and MCP
adapters make it available to applications and AI agents.

```text
                 Shared semantic catalogue
       operations • methods • parameters • results
                         │
          ┌──────────────┴──────────────┐
          ▼                             ▼
   C++ core + plugins       Rust backend (preserved)
          │                    development paused
          ▼
     C++ public API / MCP
          │
          ▼
   Future React frontend
```

## Operations and workflow Methods

The public contract distinguishes two kinds of capability:

- **Operations** are callable project or domain actions. Domain Operations are
  stateless and include their project selection in every request.
- **Workflow Methods** are ordered processing steps. They are discovered with
  `get_available_methods` and executed in a connected project session.

Methods are not MCP tools. `tools/list` is the discovery endpoint for callable
Operations; `get_available_methods` is the discovery endpoint for workflow
Methods.

## Project usage model

A typical application or agent follows this sequence:

1. create or open a project;
2. inspect its identity, domain, metadata, and available analyses;
3. invoke stateless domain Operations for direct queries;
4. connect when a workflow is required;
5. discover Methods and their schemas;
6. validate and execute the workflow;
7. close the connected session.

The C++ MCP server is the active application boundary. The Rust MCP server is a
preserved implementation of the shared catalogue contract; Rust development is
currently paused and it should not be treated as the target for new capabilities.

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
  catalogue-declared Operations and workflow Methods.

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
