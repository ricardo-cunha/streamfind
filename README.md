# streamfind

<p align="center">
  <img src="docs\assets\streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed framework for analytical data processing. Its
active native backend is C++, with mass-spectrometry data access, a shared
semantic catalogue, a dynamic plugin framework, and MCP servers for applications
and AI agents. A Rust backend is preserved from an earlier development phase.

## Start here

- [Read the user documentation](https://streamfind.odea-project.org/).
- [Download the native packages](docs/releases.md).
- [Use the C++ MCP server](docs/quickstart/cpp-mcp.md).
- [See the preserved Rust backend](docs/components/rust.md).
- [See the legacy R package](docs/components/bindings-r.md).
- [Check availability and compatibility](docs/status.md).

## Current availability

| Interface | Availability | Recommended use |
| --- | --- | --- |
| C++ backend | Preview packages for Windows x64 and Linux x86_64 | Native C++ applications and C++ MCP clients |
| Rust backend | Stale development backend | Existing Rust experiments only |
| MCP | Included with the native C++ package | Applications and AI agents using JSON-RPC over stdio |
| R package | Legacy interface | Existing R and Shiny workflows |
| Python package | Not released | No public installation path yet |
| Cogniflow integration | Separate future path | Not included in native packages |

The native C++ and Rust project version is maintained in the Rust workspace
manifest. See [Releases](docs/releases.md) for the latest downloadable assets.

## Native implementation

The C++ implementation is the native implementation. Its core owns project
files, DuckDB transactions, operation-graph execution, schema lifecycle, and
the generic host ABI. Domain plugins own their semantic catalogues, native
readers, processing algorithms, and operations. Plugins access project data
through the generic SDK host boundary.

The Rust backend is a stale, preserved development backend. It is not the
recommended runtime or extension point. The R package is a separate legacy
interface and is not the native C++ API.

A React frontend is a future interface, not a released component. It is planned
to consume the C++ backend through its public API and MCP/HTTP integration
boundary rather than embedding domain logic or accessing DuckDB directly. The
frontend will be added after the C++ backend contract and plugin framework are
stable.

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
The C++ and Rust backends implement that public contract independently.

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
