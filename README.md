# streamfind

<p align="center">
  <img src="docs/assets/streamfind.png" width="70%" />
</p>

streamfind is an open-source platform for reproducible analytical data
processing. A project combines a DuckDB database for its workflow and metadata
with a sibling `.data` folder for operation results. You build processing as a
typed, validated workflow and run it through the local C++ service. The service
supervises isolated operation workers and makes results available to both the
browser app and MCP clients. Domain plugins provide the scientific operations;
frontend plugins provide result viewers and other UI capabilities.

## Start here

- [Read the user documentation](https://streamfind.odea-project.org/).
- [Download the native packages](docs/releases.md).
- [Launch the browser application](docs/web-app.md).
- [Use the C++ MCP server](docs/quickstart/cpp-mcp.md).
- [Build and develop the C++ backend](cpp/README.md).
- [Run the development frontend](frontend/README.md).
- [Check the supported platform and interfaces](docs/status.md).

## What you can do

- Create project databases with metadata and workflows, alongside their
  `.data` result stores and execution history.
- Discover operations and their typed inputs, outputs, parameters, constraints,
  and results from the shared semantic catalogue.
- Build mass-spectrometry workflows for analyses, spectra, chromatograms,
  feature processing, isotope/adduct/loss annotation, and suspect screening.
- Run workflows interactively in the browser or programmatically through MCP.
- Extend the backend with C++ domain plugins and the browser app with frontend
  plugins for result viewers, editors, and other UI capabilities.

## Available interfaces

| Interface | Availability | Recommended use |
| --- | --- | --- |
| Native C++ runtime | Windows x64 and Linux x86_64 packages | Standalone use and client applications |
| MCP | Included with the native C++ package | Applications and AI agents using JSON-RPC over stdio |
| Browser application | Included in native packages | Local project and workflow editing |
| C++ developer kit | Separate Windows and Linux archives | Out-of-tree C++ plugins |

The current native package version is defined in [`VERSION`](VERSION). See
[Releases](docs/releases.md) for archives, checksums, package contents, and
developer kits.

## How it is used

The service is the shared control plane for the browser app, CLI, and MCP
server. It owns project sessions, workflow state, DuckDB metadata updates,
worker supervision, result publication, and audit information. Workers execute
one operation at a time and exchange typed data through the service runtime;
they do not open the project DuckDB. Tabular results use Arrow files in the
project's `.data` store, while other result types use formats suited to their
payload. Both web and MCP clients inspect the same published results and run
state.

## R package (legacy)

The R package is maintained separately as [`streamfind.r`](https://git.uni-due.de/odea-project/streamfind/streamfind.r),
with its reference documentation at <https://streamfind-r-836f9a.gitpages.uni-due.de>.
It is the legacy StreamFind R package and is not part of this repository's
native runtime or plugin framework. It may provide a starting point for a
future R binding to the current streamfind platform; for now, consult its own
repository and documentation for package status and usage.

The C++ development guide documents the current MSYS2 UCRT64/GCC and Linux GCC
toolchains, build and test commands, runtime package layout, plugin generator,
optional developer kit, out-of-tree consumer check, and extracted-package release
validation routines: [`cpp/README.md`](cpp/README.md). The plugin framework has
two extension surfaces: native C++ backend plugins and TypeScript frontend
plugins. See [`docs/plugin-development.md`](docs/plugin-development.md).

The native archives include the built web app and executables under `bin/`.
Run `streamfind-app` to open the browser app; use `streamfind` to manage the
shared local service. See [`docs/web-app.md`](docs/web-app.md) for installation
and local development instructions.

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
3. calls `create` to create a project, then `describe` or `get_metadata` to
   inspect it;
4. calls `get_domains` to discover available operation domains and
   `get_operations` / `get_operation` to inspect operations;
5. invokes operations with `database_path`, or builds a persisted operation
   graph with nodes and typed connections;
6. validates and runs the graph, then inspects its published results.

Operations are discovered through `tools/list` and operation metadata. A
multi-step workflow is a persisted graph with typed connections, validation,
execution, and inspectable results. See the [C++ MCP quickstart](docs/quickstart/cpp-mcp.md)
for request examples.

## Shared semantic catalogue

The semantic catalogue defines operation names, operation parameters,
nested input schemas, results, units, constraints, and agent-facing guidance.
The C++ backend implements that public contract.

## Licensing

streamfind is distributed under the GNU General Public License, version 3;
see [`LICENSE.md`](LICENSE.md). Native distributions include third-party
components with their own licence terms. See [`NOTICE.md`](NOTICE.md) and the
[`cpp/vendor/`](cpp/vendor/) vendor-specific licence files before redistributing a source or binary
package.
