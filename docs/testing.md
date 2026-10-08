# Compatibility and support

The native C++ package is the preview platform for Windows x64 and Linux
x86_64. It provides the runtime, MCP server, browser application, semantic
catalogues, and built-in domain plugins.

## Native package scope

The native packages provide:

- project creation and inspection;
- metadata, operation-graph, cache, and audit operations;
- catalogue-backed MCP Operations and operation-graph schemas;
- mass-spectrometry analysis, spectrum, and chromatogram access;
- native readers for supported mzML and vendor-container formats.

Support varies by backend, domain, file format, and platform. Vendor-reader
availability should be confirmed for the specific format and dataset.

## Runtime requirements

Keep the package's catalogue files with the executable. Optional scientific
tools such as Open Babel, Java, and MetFrag are separate dependencies and are
not automatically installed by the native packages.

## Interface selection

- Use the C++ package for native C++ applications or the C++ MCP server.
- Use the R package for R and Shiny workflows.
- The native distribution does not include a Python package or Cogniflow integration.

The native packages do not yet provide a stable cross-version C++ ABI.

## Development priority

Native readers, persistence behavior, and plugin interfaces are provided by the
C++ core/plugin framework. The current React development frontend uses the C++
public service boundary rather than accessing plugin internals directly.
