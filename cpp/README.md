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
get_domains()
validate()
get_workflow() / set_workflow(Workflow)
copy(ProjectOptions)
list_tables()
get_audit_trail()
run_operation(operation_id, parameters, registry)
run_operation_graph(registry)
run_worker(worker_id, registry)
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
get_project_domains
get_workflow, set_workflow, validate_workflow, run_workflow
add_operation, connect_operations, get_artifact_inventory
copy
get_audit_trail
close
```

`set_metadata`, `set_workflow`, and `run_workflow`
and `copy` require a writable project. `get_*`, `describe`, and validation
commands are read-only.

## Execution Contracts

Operation-graph execution returns a JSON graph result:

```json
{"status": "completed", "operations": []}
```

Each persisted operation node carries its parameters. Typed connections determine
the execution order and input artifacts; validate the graph before running it.

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
`streamfind.data` repository, executes the connected operation graph, and verifies the
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

## Current state

The C++ backend is the authoritative native implementation. The current workspace version is `0.5.0`.

The normal C++ runtime package is intentionally lean. It contains the native host, MCP/service executables, frontend assets, built-in plugin packages, catalogues, and runtime vendors. It does **not** contain `sdk/`.

The `cpp/sdk/` tree remains an internal framework layer used to define the plugin ABI, host access services, catalogue projection, package validation, and built-in plugin integration. An optional developer kit can install that framework for external plugin development; it is separate from the runtime release.

Runtime vendor files are centralized under:

```text
core/vendors/
├── duckdb/
├── mingw/
└── openbabel/
```

Do not copy vendor DLLs beside individual plugins or into `bin/`. The packaged launcher and host configure package-relative native library search paths.

The shipped plugin domains are:

| Domain | Current state |
| --- | --- |
| `mass_spec` | Native readers and NTA methods; primary production domain |
| `raman` | Dynamic plugin boundary; processing remains incomplete |
| `sensors` | Dynamic empty domain; no executable capabilities yet |



## Requirements

### Windows

- Windows 10/11 x64.
- Git.
- MSYS2 installed at `C:\msys64`, or another root selected with `STREAMFIND_MSYS2_ROOT`.
- MSYS2 UCRT64 packages for CMake, Ninja, GCC, and G++.
- Node.js and npm for building frontend assets.
- A repository-local Python environment at `.venv` for validation scripts.

The build scripts use one MSYS2 installation for the complete native toolchain. They prefer `STREAMFIND_MSYS2_ROOT`, then `STREAMFIND_MINGW_ROOT`, and finally `C:\msys64`. Missing UCRT64 CMake/Ninja/compiler packages are installed through that installation's `pacman` when the release/build helper is used.

### Linux

- GCC and G++.
- CMake and Ninja.
- `tar` and `sha256sum` for release validation.
- Node.js and npm for frontend packaging.
- Python in `.venv` for dependency and MCP validation.
- PowerShell 7 (`pwsh`) only for the full NTA development workflow.

Do not use the system Python for repository scripts. Use `.venv/bin/python` on Linux or `.venv\\Scripts\\python.exe` on Windows.

## Build and test routines

### Windows MSYS2 UCRT64 build

Run from the repository root in PowerShell:

```powershell
$env:STREAMFIND_MSYS2_ROOT = 'C:\msys64'
cmake --preset mingw-ucrt64
cmake --build tmp\\build\\mingw-ucrt64 --parallel 8
ctest --test-dir tmp\\build\\mingw-ucrt64 --output-on-failure
```

The preset uses the UCRT64 GCC/G++ compiler and Ninja from the selected MSYS2 root. For a developer-kit configuration:

```powershell
cmake --preset mingw-ucrt64-devkit
cmake --build tmp\\build\\mingw-ucrt64-devkit --parallel 8
cmake --install tmp\\build\\mingw-ucrt64-devkit --prefix tmp\\build\\mingw-ucrt64-devkit\\install
```

### Linux GCC build

```bash
cmake --preset linux-gcc
cmake --build tmp/build/linux-gcc --parallel
ctest --test-dir tmp/build/linux-gcc --output-on-failure
```

### Frontend assets

The C++ package expects a built frontend when creating a complete application package:

```bash
cd frontend
npm ci
npm run build
cd ..
```

### Native release validation

The Windows runtime release builds the frontend and native package, runs CTest, extracts the archive, checks the runtime payload, validates the recursive PE dependency closure, and starts the packaged MCP server:

```powershell
scripts\\release\\cpp\\release-cpp.ps1 -Version 0.5.0
```

The Linux runtime release performs the equivalent extracted-package checks, including recursive ELF dependency validation and packaged MCP startup:

```bash
bash scripts/release/cpp/release-cpp-linux.sh 0.5.0
```

Archives and checksums are written under `tmp/release-output/`. The runtime archive must not contain an `sdk/` directory or plugin-local vendor copies.

## Plugin development routine

Generate a backend/frontend project scaffold:

```powershell
.venv\\Scripts\\python.exe scripts\\dev\\plugin_generator.py init tmp\\scratch\\my-plugin --kind full --domain example --name my-plugin
```

Build the generated backend against an installed developer kit:

```powershell
cmake -G Ninja -S tmp\\scratch\\my-plugin\\backend -B tmp\\build\\my-plugin `
  -DCMAKE_PREFIX_PATH="$PWD\\tmp\\build\\mingw-ucrt64-devkit\\install\\sdk\\cmake" `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build tmp\\build\\my-plugin
```

The first-party out-of-tree consumer check is:

```powershell
.venv\\Scripts\\python.exe scripts\\dev\\test_plugin_consumer.py --source cpp\\sdk\\examples\\minimal_plugin --prefix tmp\\build\\mingw-ucrt64-devkit\\install --build tmp\\build\\plugin-consumer --cmake C:/msys64/ucrt64/bin/cmake.exe --ninja C:/msys64/ucrt64/bin/ninja.exe
```

For a distributable developer kit, use:

```powershell
scripts\\release\\cpp\\package-plugin-devkit.ps1 -Version 0.5.0
```

The developer-kit command configures, builds, installs, packages, and validates an out-of-tree C++ consumer. Frontend plugins use the generated TypeScript contract and do not require the C++ SDK headers.

## Using a packaged C++ runtime

After extracting a runtime archive, keep its directory layout intact. The important paths are:

```text
streamfind-core-cpp-<version>-<platform>/
├── streamfind(.exe)          # browser-facing launcher
├── bin/                      # MCP, service, CLI, and launcher binaries
├── core/
│   ├── catalogue.duckdb
│   ├── ontology/
│   └── vendors/
└── plugins/                  # plugin.json, catalogue.duckdb, shared libraries
```

For the local browser application, run the root launcher:

```powershell
.\\streamfind.exe
```

For MCP or service integration, use the executable under `bin/` and communicate through newline-delimited JSON-RPC over standard input/output. Start with `initialize`, then discover operations with `tools/list`. Runtime plugin loading is allowlisted through the package configuration; merely placing a plugin under `plugins/` does not enable it.

For a development-tree host, place `streamfind.json` beside `streamfind_mcp.exe`:

```json
{
  "plugin_roots": ["C:/path/to/plugins"],
  "enabled_plugins": ["mass_spec", "raman"]
}
```

## Routine checks before submitting C++ changes

1. Keep changes within the owning layer: core owns projects, transactions, workflow execution, and lifecycle; SDK owns generic plugin contracts; plugins own domain readers, algorithms, semantics, and operations.
2. If semantic Turtle or catalogue inputs change, format/validate them and rebuild the native catalogue before testing MCP discovery.
3. Run the narrowest affected C++ build and CTest target first.
4. Run the complete CTest suite for framework, plugin, MCP, dependency, mass-spec, and NTA interface changes.
5. For package or SDK changes, run an installed out-of-tree consumer and the extracted package MCP smoke test; build-tree success alone is insufficient.
6. Run `git diff --check` and inspect package contents before reporting completion.
7. Keep disposable builds, projects, and scratch data under `tmp/`; do not add generated artifacts to source directories and do not remove existing user fixtures from `tmp/`.

Do not add vendor SDKs, proprietary DLLs, mzML conversion fallbacks, or a second legacy execution path to make a native reader or plugin pass. Preserve the native processing level and keep public mass-spectrometry retention times in seconds.
