# streamfind

<p align="center">
  <img src="docs\assets\streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed framework for analytical data processing. It
provides native C++ and Rust backends, mass-spectrometry data access, a shared
semantic catalogue, and MCP servers for applications and AI agents.

## Start here

- [Read the user documentation](https://streamfind.odea-project.org/).
- [Download the native packages](docs/releases.md).
- [Use the C++ MCP server](docs/quickstart/cpp-mcp.md).
- [Use the Rust MCP server](docs/quickstart/rust-mcp.md).
- [Use the R package](docs/components/bindings-r.md).
- [Check availability and compatibility](docs/status.md).

## Current availability

| Interface | Availability | Recommended use |
| --- | --- | --- |
| C++ backend | Preview packages for Windows x64 and Linux x86_64 | Native C++ applications and C++ MCP clients |
| Rust backend | Preview packages for Windows x64 and Linux x86_64 | Native Rust applications, CLI use, and Rust MCP clients |
| MCP | Included with both native backends | Applications and AI agents using JSON-RPC over stdio |
| R package | Preserved and functional | Existing R and Shiny workflows |
| Python package | Not released | No public installation path yet |
| Cogniflow integration | Separate future path | Not included in native packages |

The native C++ and Rust project version is maintained in the Rust workspace
manifest. See [Releases](docs/releases.md) for the latest downloadable assets.

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

The C++ and Rust MCP servers expose the same catalogue-backed public contract.
A typical client:

1. calls `initialize`;
2. discovers callable Operations with `tools/list`;
3. calls `create`, then `describe`, `get_domain`, or `get_metadata`;
4. invokes stateless domain Operations with `database_path` and `project_id`;
5. uses `connect` and `get_available_methods` when a workflow is required;
6. validates and runs the workflow, then calls `close`.

Operations and workflow Methods are distinct: Methods are discovered through
`get_available_methods`, not `tools/list`. See the MCP quickstarts for request
examples.

## Shared semantic catalogue

The semantic catalogue defines operation names, workflow Methods, parameters,
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
