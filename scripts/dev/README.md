# Development-stage scripts

These scripts are outside the official test suites. The official C++ gate is
independent and does not require Rust, external vendor data, Java, or MetFrag.

## C++ development checks

Run these against the active C++ backend:

```powershell
scripts\dev\cpp\test-data.ps1
scripts\dev\cpp\test-nta.ps1 -MaxAnalyses 3
scripts\dev\cpp\test-vendor-readers.ps1 -Vendor Shimadzu
```

Add `-RunPipeline` to `test-nta.ps1` for the expensive NTA workflow. External data
is resolved from the sibling `streamfind.data` repository and can be overridden with
`STREAMFIND_EXAMPLE_DATA_ROOT`. Vendor fixtures use `STREAMFIND_VENDOR_DATA_ROOT`.

The C++ scripts use the native catalogue generated at:

```text
tmp\build\core-default\semantic_catalogue\catalogue.duckdb
```

## Rust development checks

Rust is an independent backend. Its build and tests are kept under the Rust build
wrapper; C++ development scripts do not accept a Rust backend:

```powershell
scripts\build\rust\build-rust.cmd -Tests
scripts\build\rust\test-rust.cmd
```

## Cross-backend conformance

Conformance is a separate, opt-in lane. It requires both executable paths and never
runs as part of C++ CTest:

```powershell
powershell -File scripts\dev\conformance\run-conformance.ps1 `
  -CppExecutable <path-to-cpp-mcp.exe> `
  -RustExecutable <path-to-rust-mcp.exe> `
  -Thermo
```

Use `-Sciex` for the SCIEX corpus. The conformance scripts send the same public MCP
requests to both backends and compare normalized responses. They are intended to
become a required Rust gate when Rust development is reopened; until then, the C++
gate remains authoritative and independently runnable.
