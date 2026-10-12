# Compatibility and support

The native C++ package targets Windows x64 and Linux x86_64. It provides the
runtime, MCP server, browser application, semantic
catalogues, and built-in domain plugins.

## What is included

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

The native packages do not promise a stable cross-version C++ ABI. Match an
application or plugin developer kit to the runtime version it targets.

## Reporting a compatibility issue

When a file or workflow does not behave as expected, record the streamfind
version, operating system, operation name, relevant parameters, and a minimal
reproducible project or data description. Keep vendor-format support claims
specific to the tested file family and acquisition mode.
