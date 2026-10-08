# Availability and compatibility

streamfind is a native C++ analytical platform with a browser application,
catalogue-backed MCP server, and domain plugins. The current preview platform
targets Windows x64 and Linux x86_64.

## Available interfaces

| Interface | Current availability | Recommended use |
| --- | --- | --- |
| C++ backend | Version {{ streamfind_version }} project version; test packages target Windows x64 and Linux x86_64 | Native C++ applications and MCP clients |
| C++ MCP server | Included in the C++ packages | Applications or agents using the C++ implementation |
| R package | Separate R interface | Existing R and Shiny workflows |
| Python package | No public package | Use the C++ CLI or MCP interface |
| Cogniflow integration | Not included in the native distribution | Use the native C++ package or MCP interface |
| React web app | Browser interface using the C++ service | Included in the Windows and Linux native archives |

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
flow.

## Compatibility scope

The native packages are preview releases. They do not currently promise:

- a stable cross-version C++ ABI;
- identical support for every vendor format on every platform;
- automatic installation of Java, MetFrag, or other optional scientific tools;
- the public Python package or a production Cogniflow adapter.

The R package is a separate interface and installation path. It should not be
installed as a replacement for the native C++ package.

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

The React web app is a development/preview application backed by the C++ service.
Both native archives include the built static assets and a root-level `streamfind`
launcher, which starts the local service and opens the app in the default browser.
For development, the app can be run from `frontend/` with the Vite launcher. It
uses the C++ backend's public service boundary and does not contain a second
persistence or domain-processing implementation. See [Web app](web-app.md).
