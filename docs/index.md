# streamfind

<p align="center">
  <img src="assets/streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed framework for analytical data processing. Its
active native backend is C++, with mass-spectrometry data access, a shared
semantic catalogue, a dynamic plugin framework, and MCP servers for applications
and AI agents. A Rust backend is preserved from an earlier development phase.

## Start here

- [Download the current native packages](releases.md).
- [Use the C++ MCP server](quickstart/cpp-mcp.md).
- [Use the Rust MCP server](quickstart/rust-mcp.md).
- [Check supported interfaces and limitations](status.md).
- [Use the existing R package](components/bindings-r.md).

!!! note "Version {{ streamfind_version }}"
    The native C++ and Rust project version is now **{{ streamfind_version }}**. The latest
    downloadable archives are currently the `v0.1.0` Windows and Linux preview
    packages; the [Releases](releases.md) page will be updated when the `v{{ streamfind_version }}`
    archives are published. Cross-version API and ABI stability is not yet
    guaranteed.

## Interfaces

| Interface | Provides | Availability |
| --- | --- | --- |
| C++ core | Native project API, mass-spectrometry operations, and MCP server | Available as a native package |
| Rust backend | Native project API, CLI, mass-spectrometry operations, and MCP server | Preserved preview; development paused |
| MCP | JSON-RPC over stdio for applications and AI agents | Available through both native backends |
| R package | Existing R workflows, non-target screening, and Shiny application | Preserved and functional |
| Python package | Public Python API | Not released |
| Cogniflow integration | Cogniflow adapter | Separate future integration path |
| React frontend | Planned user interface over the C++ backend | Future; not released |

## MCP at a glance

The C++ and Rust MCP servers expose the same catalogue-backed interface.

1. Call `initialize` to receive the server capabilities and usage guidance.
2. Call `tools/list` to discover callable Operations.
3. Use `create`, then `describe`, `get_domain`, or `get_metadata` for a new
   project.
4. Use domain Operations with explicit `database_path` and `project_id`.
5. Use `connect` only when running workflow Methods.
6. Call `get_available_methods` to discover workflow Methods and their schemas.
7. Build and validate a workflow, then run it with `run_workflow` or
   `run_method`.

Operations are stateless. Workflow Methods are session-bound and are not
advertised as MCP tools. See the [MCP quickstarts](quickstart/cpp-mcp.md) for
request examples.

## Shared contract

The semantic catalogue defines operation names, parameters, nested input
schemas, results, units, constraints, and agent-facing guidance. C++ and Rust
implement the contract independently but expose the same public concepts.

The [architecture](architecture.md) page explains the relationship between
the catalogue, native APIs, and MCP.

The active implementation path is the C++ plugin framework. Rust remains a
preserved, stale development backend while C++ domain plugins and native readers
are completed. A future React frontend will use the C++ backend through its
public service boundary; it will not access project DuckDB files directly.
