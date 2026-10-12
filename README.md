# streamfind

<p align="center">
  <img src="docs/assets/streamfind.png" width="70%" />
</p>

streamfind is an open-source platform for reproducible analytical data
processing. Projects are stored in DuckDB and processing is expressed as
validated workflows. The platform includes a browser application, a native C++
runtime, an MCP interface for applications and AI agents, and extensible
domain plugins. Mass-spectrometry processing is provided by the built-in
mass-spec plugin.

## Start here

- [Read the user documentation](https://streamfind.odea-project.org/).
- [Download the native packages](docs/releases.md).
- [Use the C++ MCP server](docs/quickstart/cpp-mcp.md).
- [Build and develop the C++ backend](cpp/README.md).
- [Run the development frontend](frontend/README.md).
- [Use the R interface](docs/components/bindings-r.md).
- [Check the supported platform and interfaces](docs/status.md).

## What you can do

- Create portable projects with metadata, workflows, artifacts, and execution
  history.
- Discover operations and their typed inputs, outputs, parameters, constraints,
  and results from the shared semantic catalogue.
- Build mass-spectrometry workflows for analyses, spectra, chromatograms,
  feature processing, isotope/adduct/loss annotation, and suspect screening.
- Run workflows interactively in the browser or programmatically through MCP.
- Extend the runtime with C++ domain plugins and build compatible frontend
  viewers or editors.

## Available interfaces

| Interface | Availability | Recommended use |
| --- | --- | --- |
| Native C++ runtime | Windows x64 and Linux x86_64 packages | Native applications and integrations |
| MCP | Included with the native C++ package | Applications and AI agents using JSON-RPC over stdio |
| Browser application | Included in native packages | Local project and workflow editing |
| C++ developer kit | Separate Windows and Linux archives | Out-of-tree C++ plugins |
| R package | Separate R interface | Existing R and Shiny workflows |
| Python package | No public package | Use the C++ CLI or MCP interface |
| Cogniflow integration | Not included in the native distribution | Use the native C++ package or MCP interface |

The current native package version is `0.5.0`. See [Releases](docs/releases.md)
for archives, checksums, package contents, and developer kits.

## How it is used

The native runtime owns project files, DuckDB transactions, workflow
validation and execution, schemas, artifacts, and audit information. Domain
plugins provide readers, algorithms, schemas, and catalogue entries.
Applications can use the browser UI, the C++ API, the CLI, or MCP depending on
the integration.

The C++ development guide documents the current MSYS2 UCRT64/GCC and Linux GCC
toolchains, build and test commands, runtime package layout, plugin generator,
optional developer kit, out-of-tree consumer check, and extracted-package release
validation routines: [`cpp/README.md`](cpp/README.md).

The native archives include the built web app and a root-level `streamfind`
launcher. Running it starts the local service and opens the app in the default
browser. See [`docs/web-app.md`](docs/web-app.md) for installation and local
development instructions.

## Vendor compatibility and trademarks

streamfind is an independent open-source project and is not affiliated
with, sponsored by, or endorsed by Agilent, SCIEX, Bruker, Shimadzu,
Waters, or any other vendor referenced in the compatibility documentation.

Vendor names, product names, trademarks, and file-format names are used
solely to identify compatibility with files produced by those systems.
streamfind does not redistribute vendor software, vendor SDKs, vendor DLLs,
or vendor proprietary runtime components.

Compatibility is based on the native file structures and datasets validated
by the project. Support for a particular vendor format, instrument family,
acquisition mode, or calibration variant is not implied merely because a
reader exists.

This notice is an engineering and trademark clarification, not a legal
certification of reverse-engineering rights or compatibility with every
vendor format. Review applicable agreements, laws, and licence obligations
before redistributing vendor-format data or software. See
[`NOTICE.md`](NOTICE.md).

## MCP automation

The C++ MCP server exposes the same catalogue-backed contract used by the web
application.
A typical client:

1. calls `initialize`;
2. discovers callable Operations with `tools/list`;
3. calls `create`, then `describe`, `get_domain`, or `get_metadata`;
4. invokes domain Operations with `database_path`;
5. builds a persisted operation graph with operation nodes and typed connections;
6. validates and runs the graph, then inspects its artifacts.

Operations are discovered through `tools/list` and operation metadata. A
multi-step workflow is a persisted graph with typed connections, validation,
execution, and inspectable artifacts. See the [C++ MCP quickstart](docs/quickstart/cpp-mcp.md)
for request examples.

## Shared semantic catalogue

The semantic catalogue defines operation names, operation parameters,
nested input schemas, results, units, constraints, and agent-facing guidance.
The C++ backend implements that public contract.

## R package

The R package provides the existing R and Shiny workflows for mass-spectrometry
and non-target screening. Install it from GitHub:

```r
options(timeout = 600)
if (!requireNamespace("remotes", quietly = TRUE)) {
  install.packages("remotes")
}
remotes::install_github("ricardo-cunha/streamfind", subdir = "bindings/r")
```

See [`docs/components/bindings-r.md`](docs/components/bindings-r.md) for the R
workflow and Shiny application.

## Licensing

streamfind is distributed under the GNU General Public License, version 3;
see [`LICENSE.md`](LICENSE.md). Native distributions include third-party
components with their own licence terms. See [`NOTICE.md`](NOTICE.md) and the
[`cpp/vendor/`](cpp/vendor/) vendor-specific licence files before redistributing a source or binary
package.
