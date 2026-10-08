# streamfind

<p align="center">
  <img src="docs\assets\streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed framework for analytical data processing. Its
active native backend is C++, with mass-spectrometry data access, a shared
semantic catalogue, a dynamic plugin framework, a browser-facing development
application, and MCP servers for applications and AI agents.

## Start here

- [Read the user documentation](https://streamfind.odea-project.org/).
- [Download the native packages](docs/releases.md).
- [Use the C++ MCP server](docs/quickstart/cpp-mcp.md).
- [Build and develop the C++ backend](cpp/README.md).
- [Run the development frontend](frontend/README.md).
- [Use the R interface](docs/components/bindings-r.md).
- [Check the supported platform and interfaces](docs/status.md).

## Current availability

| Interface | Availability | Recommended use |
| --- | --- | --- |
| C++ backend | 0.5.0 test packages for Windows x64 and Linux x86_64 | Native C++ applications and C++ MCP clients |
| MCP | Included with the native C++ package | Applications and AI agents using JSON-RPC over stdio |
| React web app | Windows and Linux archives include built assets and a browser launcher | Local browser UI over the C++ service |
| R package | Separate R interface | Existing R and Shiny workflows |
| Python package | No public package | Use the C++ CLI or MCP interface |
| Cogniflow integration | Not included in the native distribution | Use the native C++ package or MCP interface |

The native C++ project version is currently `0.5.0` and is maintained in the
repository `VERSION` file so CMake, documentation, and release tooling share one
version source. See [Releases](docs/releases.md) for downloadable assets.

## Native implementation

The C++ implementation is the native implementation. Its core owns project
files, DuckDB transactions, operation-graph execution, schema lifecycle, and the
generic host ABI. Domain plugins own their semantic catalogues, native readers,
processing algorithms, and operations. Plugins access project data through the
generic SDK host boundary.

The C++ development guide documents the current MSYS2 UCRT64/GCC and Linux GCC
toolchains, build and test commands, runtime package layout, plugin generator,
optional developer kit, out-of-tree consumer check, and extracted-package release
validation routines: [`cpp/README.md`](cpp/README.md).

The React web app is a development and preview application. The native 0.5.0
archives include its built static assets and a root-level `streamfind` launcher;
running that launcher starts the local C++ service and opens the app in the
default browser. The frontend consumes the typed service boundary and does not
access DuckDB files or plugin internals directly. See
[`docs/web-app.md`](docs/web-app.md) for launch and development instructions.

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

## MCP usage model

The C++ MCP server exposes the catalogue-backed public contract.
A typical client:

1. calls `initialize`;
2. discovers callable Operations with `tools/list`;
3. calls `create`, then `describe`, `get_domain`, or `get_metadata`;
4. invokes domain Operations with `database_path`;
5. builds a persisted operation graph with operation nodes and typed connections;
6. validates and runs the graph, then inspects its artifacts.

Operations are discovered through `tools/list` and operation metadata. A
multi-step workflow is a persisted graph, not an ordered list of methods.
See the C++ MCP quickstart for request examples.

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
