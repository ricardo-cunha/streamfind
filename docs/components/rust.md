# Rust API

The Rust backend provides a native project API, command-line interface, and MCP
server. The current Windows x64 and Linux x86_64 packages are listed on
[Releases](../releases.md).

!!! warning "Development status"
    This is a preserved preview backend from an earlier development phase.
    Rust development is currently paused; new capabilities are implemented in
    the C++ core and dynamic plugin framework first.

!!! note "Compatibility"
    The native Rust package is a versioned preview release. It is suitable
    for applications and integration testing, but does not yet promise a stable
    cross-version Rust API.

## Package contents

The Rust package includes:

- `streamfind-rust-cli` / `streamfind-rust-cli.exe`;
- `streamfind-rust-mcp` / `streamfind-rust-mcp.exe`;
- `share/streamfind/catalogue.duckdb`;
- `share/streamfind/catalogue.json`.

The catalogue files are required runtime data for the MCP server.

## Project API

Create or open a project with `ProjectOptions`:

```rust
let project = Project::create(ProjectOptions {
    database_path: "project.duckdb".into(),
    project_id: "demo".into(),
    domain: "mass_spec".into(),
    create_if_missing: false,
    read_only: false,
})?;
```

Common project operations include:

```text
create, describe, validate
get_metadata, set_metadata
get_domain
get_workflow, set_workflow, validate_workflow, run_workflow
get_methods, run_method
get_cache, get_cache_size, delete_cache
get_audit_trail
copy, close
```

## CLI

The Rust package includes commands for creating and inspecting projects:

```text
streamfind-rust-cli create
streamfind-rust-cli describe
```

The exact command options are shown by `streamfind-rust-cli --help`.

## MCP and external tools

The [Rust MCP quickstart](../quickstart/rust-mcp.md) documents the stdio server.
It uses the same catalogue-backed Operation and workflow Method model as the
C++ server.

Optional tools such as Open Babel, Java, and MetFrag are separate components.
They are not downloaded automatically by the native package.
