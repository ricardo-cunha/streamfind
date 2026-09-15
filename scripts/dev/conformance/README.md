# Cross-backend conformance

This directory contains opt-in comparisons between the authoritative C++ backend
and the later Rust backend. These scripts are not registered with CTest or Cargo.

Run after building both backends:

```powershell
powershell -File scripts/dev/conformance/run-conformance.ps1 `
  -CppExecutable tmp/build/core-default/bin/streamfind_mcp.exe `
  -RustExecutable tmp/build/rust-target/debug/streamfind-rust-mcp.exe `
  -Thermo
```

Use `-Sciex` for the SCIEX development corpus. The runner requires both executable
paths explicitly, sends the same public MCP requests to both processes, and compares
normalized responses. C++ development and testing remain independent when Rust is
not built.
