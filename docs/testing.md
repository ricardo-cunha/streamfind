# Compatibility and support

The native C++ package is the active preview backend for Windows x64 and Linux
x86_64. The Rust package is a preserved, stale development backend whose active
development is paused. The existing R package is a separate preserved interface.

## Native package scope

The native packages provide:

- project creation and inspection;
- metadata, workflow, cache, and audit operations;
- catalogue-backed MCP Operations and workflow Method schemas;
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
- Use the Rust package only for existing Rust compatibility work while Rust
  development remains paused.
- Use the R package for existing R and Shiny workflows.
- The Python package and Cogniflow integration are not currently released.

The native packages do not yet provide stable cross-version C++ ABI or Rust API
compatibility guarantees.

## Development priority

New capabilities, native readers, persistence behavior, and plugin interfaces
should be implemented and verified in the C++ core/plugin framework. The Rust
backend remains useful for preserved behavior and future parity checks, but is
not the active implementation path. A future React frontend will be tested
against the C++ public boundary rather than directly against plugin internals.
