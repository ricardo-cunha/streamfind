# Streamfind Rust Backend

Independent Rust implementation of the generic Streamfind project backend.
It uses the same DuckDB project schema and JSON contract as `streamfind-core`
(C++), but does not link to or wrap the C++ implementation.

## Vendor compatibility and trademarks

streamfind is independent of the instrument vendors named in its compatibility
documentation. Vendor and product names identify file-format compatibility only;
they do not indicate endorsement or affiliation. streamfind does not redistribute
vendor software, SDKs, DLLs, or proprietary runtime components.

See the root [`NOTICE.md`](../NOTICE.md) and the Rust
[`LICENSES.md`](LICENSES.md) inventory for distribution and support boundaries.

Native Rust preview packages are available from the
[release documentation](../docs/releases.md).

## Crates

- `streamfind-rust-core`: Project, workflow, typed parameters, methods, cache,
  audit, and execution APIs.
- `streamfind-rust-cli`: Minimal project `create` and `describe` CLI.
- `streamfind-rust-external`: Resolves user-installed tools from `PATH`.
- `streamfind-rust-mcp`: Line-delimited JSON-RPC MCP stdio adapter.

## Project API

Create or open a project with `ProjectOptions`:

```rust
let project = Project::create(ProjectOptions {
    database_path: "project.duckdb".into(),

    domain: "mass_spec".into(),
    create_if_missing: false,
    read_only: false,
})?;
```

Canonical `Project` methods use these prefixes:

- `get_*`: read state, metadata, workflow, cache, audit, or path.
- `set_*`: mutate metadata, workflow, or cache entries.
- `run_*`: execute a method or workflow.
- `delete_*`: remove project-owned cache data.

Canonical methods:

```text
get_metadata() / set_metadata(Json)
get_database_path()
get_domain()
validate()
get_workflow() / set_workflow(Workflow)
copy(ProjectOptions)
list_tables()
get_cache() / get_cache_size() / get_cache_entry(hash)
set_cache(name, description, hash, Json)
delete_cache()
get_audit_trail()
run_method(method_id, parameters)
run_workflow(workflow, registry)
close()
```

`Project` owns its DuckDB connection. Rust `close(self)` consumes the handle;
normal scope destruction also releases the connection. Project domains are
assigned in `ProjectOptions` at creation and are immutable afterward.

## JSON API

The `streamfind_rust_core::api` module exposes the same generic operations as
Requests identify a project with `database_path`; one DuckDB file contains one
project.

Canonical operations include:

```text
create, describe, validate
get_metadata, set_metadata
get_domain
get_workflow, set_workflow, validate_workflow, run_workflow
get_methods, run_method
copy
get_cache, get_cache_size, delete_cache
get_audit_trail
close
```

The API returns JSON objects or arrays matching the C++ facade. Mutating calls
require a writable project; read and validation calls open read-only projects.

## Execution Contracts

`ExecutionResult` is the stable workflow result envelope:

```json
{"results": [], "cancelled": false}
```

`CancellationToken` provides cooperative cancellation. `ProgressEvent` reports
the operation, completed count, and total count to a progress callback.

Errors use `ErrorCode`, aligned with C++: invalid arguments, project lookup and
creation failures, schema/database failures, workflow validation, method
execution, closed projects, and cancellation.

## DuckDB Interoperability

The Rust and C++ implementations share the `PROJECT`, `CACHE`, and
`AUDIT_TRAIL` tables, workflow JSON, metadata JSON, cache representation, and
audit representation. Shared fixtures and interoperability tests live in:

```text
tests/fixtures/project/project_conformance.json
core/tests/unit/conformance.cpp
rust/crates/core/tests/conformance.rs
```

## CLI

From the repository root:

```powershell
cargo run --manifest-path rust/Cargo.toml -p streamfind-rust-cli -- create `
  --database-path "$HOME\.streamfind\projects\demo.duckdb" `
  --project-id demo `
  --domain mass_spec

cargo run --manifest-path rust/Cargo.toml -p streamfind-rust-cli -- describe `
  --database-path "$HOME\.streamfind\projects\demo.duckdb" `
  --project-id demo
```

## External Tools

External tools are prerequisites. The Rust backend does not download or install
them. OpenBabel must be installed so that `obabel` is available on the process
`PATH`; the resolver reports an actionable error when it is missing.
