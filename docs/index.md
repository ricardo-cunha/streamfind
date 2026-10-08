# streamfind

<p align="center">
  <img src="assets/streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed framework for analytical data processing. Its
active native backend is C++, with mass-spectrometry data access, a shared
semantic catalogue, a dynamic plugin framework, and MCP servers for applications
and AI agents.

## Start here

- [Download the current native packages](releases.md).
- [Develop backend and frontend plugins](plugin-development.md).
- [Use the C++ MCP server](quickstart/cpp-mcp.md).
- [Check supported interfaces and limitations](status.md).
- [Use the R interface](components/bindings-r.md).

!!! note "Version {{ streamfind_version }}"
    The native C++ project version is **{{ streamfind_version }}**. Native
    packages target Windows x64 and Linux x86_64. Cross-version API and ABI
    stability is not guaranteed for this preview platform.

## Interfaces

| Interface | Provides | Availability |
| --- | --- | --- |
| C++ core | Native project API, mass-spectrometry operations, and MCP server | Available as a native package |
| MCP | JSON-RPC over stdio for applications and AI agents | Available through the native C++ package |
| R package | Existing R workflows, non-target screening, and Shiny application | Separate R interface |
| Python package | — | No public package |
| Cogniflow integration | — | Not included in the native distribution |
| React web app | Browser application served by the packaged C++ service; also available from the Vite development server | Included in the Windows and Linux native archives |

## MCP at a glance

The C++ MCP server exposes the catalogue-backed interface.

1. Call `initialize` to receive the server capabilities and usage guidance.
2. Call `tools/list` to discover callable Operations.
3. Use `create`, then `describe`, `get_domain`, or `get_metadata` for a new
   project.
4. Use domain Operations with explicit `database_path`.
5. Add operation nodes with their parameters to the persisted workflow.
6. Connect typed output ports to downstream input ports.
7. Validate the graph, run it, and inspect the resulting artifacts.

Operations are the units of execution. A workflow is a persisted graph of
operation nodes and typed connections. See the [C++ MCP quickstart](quickstart/cpp-mcp.md)
for request examples.

## Shared contract

The semantic catalogue defines operation names, parameters, nested input
schemas, results, units, constraints, and agent-facing guidance. The C++
backend implements the contract and exposes the public concepts through its API
and MCP server.

The [architecture](architecture.md) page explains the relationship between
the catalogue, native APIs, and MCP. The current React application uses the C++
backend through its public service boundary; it does not access project DuckDB
files directly. See [Web app](web-app.md) for the packaged Windows launch path
and development setup.
