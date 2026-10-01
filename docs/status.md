# Availability and compatibility

streamfind currently offers a native C++ preview release. The Rust backend is
stale development code and the R package is a legacy interface.

## Available interfaces

| Interface | Current availability | Recommended use |
| --- | --- | --- |
| C++ backend | Version {{ streamfind_version }} project version; 0.3.0 targets Windows x64 and Linux x86_64 preview packages | Native C++ applications and MCP clients |
| Rust backend | Stale development backend; no 0.3.0 release | Existing Rust applications and compatibility work |
| C++ MCP server | Included in the C++ packages | Applications or agents using the C++ implementation |
| Rust MCP server | Preserved with the stale Rust backend | Existing Rust experiments only |
| R package | Legacy interface | Existing R and Shiny workflows |
| Python package | Not released | No public installation path currently |
| Cogniflow integration | Separate future path | Not part of the native packages |
| React frontend | Development/preview interface using the C++ service | Not included in native archives |

See [Releases](releases.md) for package downloads and checksums.

## Native capabilities

The current native packages include:

- DuckDB-backed project creation, inspection, metadata, workflow, cache, and
  audit operations;
- catalogue-backed MCP Operations and operation-graph schemas;
- mass-spectrometry analysis management and metadata/query operations;
- raw and persisted spectrum and chromatogram access;
- native mzML and vendor-container reader implementations;
- feature-processing and non-target-analysis operations at different stages of
  validation across domains and file formats.

Vendor-specific behavior depends on the file format and the available fixture
or data source. A package should not be interpreted as support for every vendor
format merely because a reader exists in the catalogue.

## MCP usage model

The native C++ server uses JSON-RPC over standard input/output.

- `initialize` provides usage instructions.
- `tools/list` exposes callable Operations, including domain Operations, without
  requiring a connected project.
- Domain Operations require `database_path` in each request; additional
  parameters are operation-specific.
- `get_operations` and `get_operation` expose operation schemas and typed ports.
- `add_operation` persists operation nodes and their JSON parameters.
- `connect_operations` persists typed output-to-input connections.
- `validate_workflow` checks the complete graph before execution.
- `run_workflow` executes the graph and publishes artifacts.

The [C++ MCP quickstart](quickstart/cpp-mcp.md) provides the current request
flow. The Rust quickstart is retained only for stale backend development.

## Compatibility scope

The native packages are preview releases. They do not currently promise:

- a stable cross-version C++ ABI;
- a stable cross-version Rust API;
- identical support for every vendor format on every platform;
- automatic installation of Java, MetFrag, or other optional scientific tools;
- the public Python package or a production Cogniflow adapter.

The R package is a separate interface and installation path. Native C++/Rust
packages should not be installed as replacements for the R package.

## Vendor compatibility and trademarks

streamfind is an independent open-source project and is not affiliated
with, sponsored by, or endorsed by Agilent, SCIEX, Bruker, Shimadzu,
Waters, or any other vendor referenced in the compatibility documentation.

Vendor names, product names, trademarks, and file-format names identify
compatibility only. streamfind does not redistribute vendor software, vendor
SDKs, vendor DLLs, or vendor proprietary runtime components.

Compatibility is based on native file structures and datasets validated by the
project. Support for a vendor format, instrument family, acquisition mode, or
calibration variant is not implied merely because a reader exists.

SCIEX WIFF2 decryption is not implemented.

See the root [`NOTICE.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/NOTICE.md)
for the distribution notice and compatibility boundaries.

## Future interfaces

The [Python package](components/bindings-python.md) and
[Cogniflow integration](components/cf-streamfind.md) pages describe the current
availability of those separate future-facing assets without implying that they
are part of the native C++ release.

The React frontend is currently a development/preview application. It uses the
C++ backend's public service boundary and does not contain a second persistence
or domain-processing implementation. It is not included as a separately
supported desktop application in the native archives. New backend capabilities
should therefore target the C++ core/plugin framework first.
