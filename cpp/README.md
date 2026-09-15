# streamfind-core

Standalone C++20 core for project persistence and generic workflow execution.
The core does not depend on R, Python, FastAPI, or React.

## Native architecture

The C++ implementation is split into three layers:

- `core/` owns project files, DuckDB connections, transactions, generic workflow
  execution, schema lifecycle, and framework operations.
- `sdk/` defines the public plugin contract, semantic catalogue projection,
  manifest validation, and the versioned C ABI used by runtime-loaded libraries.
- `plugins/` owns domain catalogues, readers, algorithms, and domain capability
  bindings. The shipped domains are `mass_spec`, `raman`, and `sensors`.

Plugins do not receive `Project`, DuckDB handles, STL objects, JSON objects, or
C++ exceptions across the dynamic boundary. Dynamic execution receives an opaque
transaction context and uses the generic host table callbacks declared in
`sdk/include/streamfind/plugin_abi.h`. The current ABI is major `1`, minor `1`.

Each dynamic plugin package contains a `plugin.json`, a package-local
`catalogue.duckdb`, and the platform-specific shared library declared by the
manifest. Runtime loading is allowlisted: presence under a plugin root does not
enable a plugin. Loaded libraries remain resident for the process lifetime.

The current package manifests are:

| Domain | Windows library | POSIX library | Domain state |
| --- | --- | --- | --- |
| `mass_spec` | `streamfind_mass_spec.dll` | `libstreamfind_mass_spec.so` | Native readers and NTA methods |
| `raman` | `streamfind_raman.dll` | `libstreamfind_raman.so` | Dynamic operation boundary; processing is not implemented yet |
| `sensors` | `streamfind_sensors.dll` | `libstreamfind_sensors.so` | Dynamic empty domain; no executable capabilities yet |

For a development-tree runtime, place `streamfind.json` beside
`streamfind_mcp.exe` and configure explicit plugin roots and IDs:

```json
{
  "plugin_roots": ["C:/path/to/plugins"],
  "enabled_plugins": ["mass_spec", "raman"]
}
```

The host loads the core catalogue first, validates each selected package and
ABI descriptor, imports the package catalogue, and exposes only capabilities
present in both the catalogue and the loaded plugin.

## Vendor compatibility and trademarks

streamfind is independent of the instrument vendors named in its compatibility
documentation. Vendor and product names identify file-format compatibility only;
they do not indicate endorsement or affiliation. streamfind does not redistribute
vendor software, SDKs, DLLs, or proprietary runtime components.

See the root [`NOTICE.md`](../NOTICE.md) for distribution and support
boundaries.

The native reader process uses independently implemented parsers based on
lawfully obtained data files, public information, and observable program
output. The C++ core does not distribute or require vendor SDKs, vendor DLLs,
ProteoWizard, or vendor software at runtime. It does not decrypt or circumvent
encrypted SCIEX WIFF2 metadata; classic WIFF/WIFF.SCAN support is not WIFF2
support.

Native C++ preview packages are available from the
[release documentation](../docs/releases.md).

## Project API

`streamfind::Project` owns one DuckDB-backed project selected by
`ProjectOptions`:

```cpp
streamfind::ProjectOptions options{
    "project.duckdb", "demo", std::nullopt, false, false, "mass_spec"
};
auto project = streamfind::Project::create(options);
```

Canonical methods use these prefixes:

- `get_*`: read project state, metadata, workflow, cache, audit, identity, or
  database path.
- `set_*`: mutate metadata, workflow, or cache entries.
- `run_*`: execute one method or the persisted workflow.
- `delete_*`: remove project-owned cache data.

Canonical `Project` methods:

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
run_workflow()
close()
```

`Project` is move-only and owns its native DuckDB state. `close()` marks the
handle closed; later operations fail. Destruction closes the handle as well.
Project domains are assigned in `ProjectOptions` during creation and cannot be
changed afterward. Opening a project does not modify its domain.

## JSON API

`streamfind::api::run()` exposes the same operations through
`streamfind::api::ProjectCommand`. The database path selects the project with:

```json
{"database_path":"project.duckdb"}
```

Canonical commands are:

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

`set_metadata`, `set_workflow`, `run_method`, `run_workflow`, `delete_cache`,
and `copy` require a writable project. `get_*`, `describe`, and validation
commands are read-only.

## Execution Contracts

Workflow execution returns `ExecutionResult`:

```json
{"results": [], "cancelled": false}
```

Long-running callers may provide a `CancellationToken` and `ProgressCallback`.
Cancellation is cooperative. Progress events contain `operation`, `completed`,
and `total`.

Errors use the stable `ErrorCode` enum, including invalid arguments, missing or
existing projects, schema/database failures, workflow validation, method
execution, closed projects, and cancellation.

## Persistence Contract

The C++ core uses the shared `PROJECT`, `CACHE`, and `AUDIT_TRAIL` DuckDB tables.
Workflow, metadata, cache, audit, and result JSON are backend-neutral and are
covered by the lightweight project and execution fixtures in
`cpp/tests/fixtures/`. Large example datasets are
provided separately through the streamfind.data repository/data directory and
are exercised by the development PowerShell scripts under `scripts/dev/`.

## Build and test

From the repository root, the supported Windows wrappers are:

```text
scripts\build\cpp\build-cpp.cmd -Clean -Tests -Config Release
scripts\build\cpp\test-cpp.cmd -Config Release
```

The official CTest suite is intentionally lightweight and fixture-independent.
It contains 11 focused contracts covering:

- project lifecycle, execution lifecycle, project isolation, and persistence
  manifests;
- SDK ABI, plugin manifests, configuration, loader failures, dynamic package
  invocation, and dynamic schema handling;
- MCP protocol/discoverability and plugin registration;
- MassSpec and NTA public interfaces; and
- DuckDB/OpenBabel dependency smoke coverage.

The corresponding source files are under `cpp/tests/unit/` and use behavior-based
names such as `project_lifecycle_contract.cpp`,
`dynamic_plugin_package_contract.cpp`, and `plugin_registration_contract.cpp`.
Raw vendor corpora, full NTA pipelines, and external-data checks are not part of
the default CTest run. Invoke those explicitly through the development scripts,
for example:

```powershell
scripts\dev\cpp\test-nta.ps1 -RunPipeline
```

### Plugin discovery and Release packaging

Every directory under `cpp/plugins/` containing a `CMakeLists.txt` is discovered
automatically. A plugin directory uses its directory name as its domain and must
define the target `streamfind_<domain>_plugin`; its semantic sources live under
`semantic/`. The plugin CMake file owns its source list, manifest, semantic
catalogue, build-tree staging, and install rules. The top-level build automatically
includes discovered plugins in the aggregate catalogue, MCP configuration, default
build, and Release package. Adding a plugin therefore does not require editing
`cpp/CMakeLists.txt` or the Release scripts.

### Full native C++ NTA workflow

The NTA development test imports the 18 wastewater analyses from the sibling
`streamfind.data` repository, executes all 12 workflow methods, and verifies the
persisted DuckDB results with
`scripts/dev/cpp/verify-nta-project.py`. Verification includes:

- imported analysis count and nonempty feature results;
- features containing encoded MS2 payloads;
- suspect rows and shared-fragment counts; and
- cosine-similarity results against the configured `0.7` threshold.

On Windows, build and run it with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  scripts/build/cpp/build-cpp.ps1 -Config Release -Tests
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  scripts/dev/cpp/test-nta.ps1 -RunPipeline
```

On Linux, use the native Linux build and runner:

```bash
bash scripts/build/cpp/build-cpp-linux.sh
bash scripts/dev/cpp/test-nta-linux.sh -SkipBuild
```

The Linux runner requires PowerShell 7 (`pwsh`) and the repository-local
`.venv` with DuckDB installed. The expensive workflow is intentionally separate
from CTest. A verified 18-analysis run produced approximately 20.5k feature
rows, 4.4k features with MS2 payloads, 123 suspect rows, and 25 suspects with
cosine similarity at or above `0.7`; exact counts can vary slightly between
native platforms.
