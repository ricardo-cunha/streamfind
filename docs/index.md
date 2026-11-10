
<p align="center">
  <img src="assets/streamfind.png" width="70%" />
</p>

streamfind is an open-source platform for reproducible analytical data
processing. It brings domain-specific algorithms, workflow design, and result
exploration together in a shared environment, helping researchers and
developers build, extend, and communicate analytical work. Workflows can be
used interactively in the browser app or automated through MCP clients.

# Start here

- [Download the current native packages](releases.md).
- [Use the browser application](web-app.md).
- [Use the C++ MCP server](quickstart/cpp-mcp.md).
- [Develop backend and frontend plugins](plugin-development.md).

For an overview of the system boundaries and data flow, see the
[architecture](architecture.md) page.

# Typical workflow

1. Install the native package and start the browser app via `bin/streamfind-app.exe` or connect an MCP
   client to streamfind service via `bin/streamfind-mcp.exe`.
2. Create or open a project. Its `*.duckdb` file stores the workflow and
   metadata; its `*.data` folder stores published results.
3. Discover operations and configure their parameters.
4. Connect compatible typed ports to form a workflow graph.
5. Validate and run the graph through the service.
6. Inspect result tables, visualizations, and run history in the app or MCP.

# Interfaces

| Interface | Provides |
| --- | --- |
| C++ backend service | Project and workflow runtime, semantic catalogue, and domain processing |
| MCP | JSON-RPC over stdio for applications and AI agents |
| React web app | Browser application served by the packaged C++ service; also available from the Vite development server |

The service is the single owner of project database access and result
publication. Operation workers receive typed inputs and return typed outputs
through the runtime; they do not open the project database. Table results are
stored in Arrow format and can be queried by the backend for app and MCP views.
Other result types can use JSON, images, or binary formats as appropriate.

The plugin framework has two extension surfaces: C++ plugins add backend
operations and domain capabilities; TypeScript frontend plugins add UI
capabilities such as result viewers and editors. See [Plugin development](plugin-development.md).

# R package (legacy)

The R package is maintained in its own repository as
[`streamfind.r`](https://git.uni-due.de/odea-project/streamfind/streamfind.r),
and its reference documentation is available at
<https://streamfind-r-836f9a.gitpages.uni-due.de>. This is the legacy
StreamFind R package, separate from the current native runtime and plugin
framework. It may provide a starting point for a future R binding to the
current streamfind platform. For now, use its repository and reference page
for package status and usage.

# Supported analytical work

streamfind brings several analytical domains into a common workflow platform:

- **Mass spectrometry:** import analysis files and work with spectra and
  chromatograms; detect and group features; annotate isotopes, adducts, and
  molecular relationships; screen suspects; and filter or visualize results.
  Reader and operation support depends on the source format and parameters.
- **Raman and infrared spectroscopy:** import supported ASC, SIF, WIP, SMF, and
  WiTec text files, then summarize, normalize, smooth, baseline-correct, or
  filter spectra. Time-ordered acquisitions can retain their spectrum and
  acquisition-time metadata.
- **Statistics:** explore tabular data with PCA and PLS regression or
  classification, and compare two groups with fold-change analysis.
- **Sensors:** the domain is being developed for sensor-data workflows. A
  planned direction is connecting to OPC UA servers to bring process data into
  streamfind; sensor operations are not yet available in the current package.

These domains share the workflow and result-viewing framework, while each
provides its own data structures and analytical methods.

# MCP at a glance

The C++ MCP server exposes the catalogue-backed interface.

1. Call `initialize` to receive the server capabilities and usage guidance.
2. Call `tools/list` to discover callable Operations.
3. Use `create` to create a project. Then use `describe` or `get_metadata` to
   inspect it, and `get_project_domains` to list domains represented in that
   project.
4. Use `get_domains` to discover domains with available Operations, then call
   domain Operations with an explicit `database_path`.
5. Add operation nodes with their parameters to the persisted workflow.
6. Connect typed output ports to downstream input ports.
7. Validate the graph, run it, and inspect the resulting published results.

Operations are the units of execution. A workflow is a persisted graph of
operation nodes and typed connections. See the [C++ MCP quickstart](quickstart/cpp-mcp.md)
for request examples.

# Shared contract

The semantic catalogue defines operation names, parameters, nested input
schemas, results, units, constraints, and agent-facing guidance. The C++
backend implements the contract and exposes the public concepts through its API, MCP server and React App.
The [architecture](architecture.md) page explains the relationship between
the catalogue, native API, MCP and React App.
