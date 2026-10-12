# streamfind

<p align="center">
  <img src="assets/streamfind.png" width="70%" />
</p>

streamfind is an open-source platform for reproducible analytical data
processing. Projects are stored in DuckDB and processing is expressed as
validated workflows. The platform includes a browser application, native C++
runtime, MCP interface, and extensible domain plugins.

## Start here

- [Download the current native packages](releases.md).
- [Develop backend and frontend plugins](plugin-development.md).
- [Use the C++ MCP server](quickstart/cpp-mcp.md).
- [Check supported interfaces and limitations](status.md).
- [Use the R interface](components/bindings-r.md).

## Typical workflow

1. Install a native package and launch the browser application, or configure an
   MCP client.
2. Create or open a DuckDB project.
3. Discover operations and configure their parameters.
4. Connect compatible typed ports into a workflow.
5. Validate and run the workflow.
6. Inspect tables, visualizations, artifacts, and execution history.

!!! note "Version {{ streamfind_version }}"
    The native C++ package version is **{{ streamfind_version }}**. Packages
    target Windows x64 and Linux x86_64. Cross-version C++ ABI stability is not
    guaranteed; use matching runtime and developer-kit versions.

## Interfaces

| Interface | Provides | Availability |
| --- | --- | --- |
| C++ core | Native project API, mass-spectrometry operations, and MCP server | Available as a native package |
| MCP | JSON-RPC over stdio for applications and AI agents | Available through the native C++ package |
| R package | Existing R workflows, non-target screening, and Shiny application | Separate R interface |
| Python package | — | No public package |
| React web app | Browser application served by the packaged C++ service; also available from the Vite development server | Included in the Windows and Linux native archives |

## Supported analytical work

The mass-spec plugin supports workflows that combine analysis metadata, raw
file readers, spectra, chromatograms, feature detection and grouping, isotope
and molecular-link annotation, suspect screening, and downstream filtering or
visualization. Exact reader and operation support depends on the file format
and operation parameters.

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
