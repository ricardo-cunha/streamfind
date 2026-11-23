# Plugin development

streamfind supports two extension surfaces:

- **Backend plugins** are C++20 dynamic libraries built against the versioned streamfind plugin ABI.
- **Frontend plugins** are TypeScript packages that consume the frontend plugin and result contracts.

## Choose a development environment

The streamfind repository is the normal development environment for plugins
that will ship with streamfind. It contains the backend host and SDK sources,
all bundled C++ plugins, the frontend application and plugin framework, the
semantic catalogues, examples, and build tools. You do not need a separate
runtime archive to work on these in-repository plugins.

Use the matching native SDK from the platform runtime archive when you are
building a C++ plugin in another repository or distributing a plugin
independently of the streamfind source tree. The archive supplies the public
headers, CMake package, linkable libraries, catalogue tools, validator and
viewer SDK. It is version- and platform-specific.

| Work | Recommended environment | Matching runtime archive needed? |
| --- | --- | --- |
| Modify a bundled C++ plugin | This repository, under `cpp/plugins/<plugin>/` | No |
| Modify the frontend or a bundled UI plugin | This repository, under `frontend/` | No |
| Build an independent C++ plugin project | External project plus matching runtime archive SDK | Yes |
| Build an independent frontend plugin | Frontend plugin contracts and a compatible frontend host | No C++ kit |

The combined runtime archive makes the backend plugin contract consumable
without cloning or building the whole repository. It does not replace the
repository as the development environment for streamfind itself.

## Develop bundled plugins in this repository

Backend plugin directories are discovered from `cpp/plugins/` when they
contain a `CMakeLists.txt`. The default configuration builds all bundled
plugins. Work in the relevant plugin directory, then configure, build, and run
its focused CTest selection from the repository root:

```powershell
sfdev configure
sfdev build backend
sfdev test backend --list
```

Frontend plugin code lives under `frontend/src/plugins/`; the shared plugin
framework and contracts live under `frontend/src/framework/plugins/`. Build
and validate the frontend from the same checkout:

```powershell
sfdev build frontend
sfdev test frontend
```

Use the packaged app as the verification target while developing the UI. See
the [C++ guide](https://github.com/ricardo-cunha/streamfind/blob/main/cpp/README.md)
and [frontend guide](https://github.com/ricardo-cunha/streamfind/blob/main/frontend/README.md)
for toolchain and application details.

The `streamfind-core-cpp` platform archive is one combined runtime and
development distribution. It contains the built-in plugins, runtime vendor
files, `streamfind-dev`, the matching native SDK, viewer SDK and provisioning
tools.

## Build an out-of-tree backend plugin

The main backend areas in the repository are:

```text
cpp/core/       service lifecycle, DuckDB metadata, transactions, workflow runtime
cpp/sdk/        ABI, host callbacks, manifests, catalogue projection, loader
cpp/plugins/    plugin readers, algorithms, catalogues, and plugin packages
```

Plugins use the generic host boundary. Do not pass `Project`, DuckDB handles,
STL objects, JSON objects, or C++ exceptions across the dynamic boundary.

Run `sfdev release prepare` from the repository development shell. The Windows
and Linux release workflows each produce one archive containing the runtime,
`streamfind-dev`, the matching native SDK, viewer SDK and setup manifests.
Archives and checksums are written to `tmp/release-output/`.

The development payload can also be enabled directly in a CMake build with
`-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON`. This option controls which
development files are installed into the combined archive; it does not create
a separate release or standalone kit.

```text
-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON
```

The development payload contains public headers, the CMake package, libraries,
catalogue tools, the plugin validator, and semantic tooling. It is separate
catalogue tools, the plugin validator, and semantic tooling. It is shipped
with the matching runtime archive.

An out-of-tree plugin consumes the SDK from that archive:

```cmake
find_package(streamfind CONFIG REQUIRED)
streamfind_add_plugin(my_plugin example)
```

The repository example is `cpp/sdk/examples/minimal_plugin`. Validate a plugin
by configuring, compiling, linking, and executing an out-of-tree consumer;
`find_package` alone is not enough.

## Develop frontend plugins

Frontend plugins do not link to the C++ SDK. Develop them in the
`frontend/src/plugins/` workspace and use the shared contracts in
`frontend/src/framework/plugins/`. A plugin supplies a manifest and an entry
module that registers viewers or visualization renderers through the
versioned frontend plugin API. The frontend build discovers optional plugin
manifests, bundles their code, and emits a same-origin `plugins.json` manifest;
the app loads those bundles during startup. Plugins use shared TypeScript
contracts for operation and capability metadata, results and table schemas,
viewer registration, service communication, and manifests. Run `sfdev build
frontend` and `sfdev test frontend` while developing. Keep frontend schemas
derived from the shared catalogue contracts rather than duplicating backend
table definitions.

### Verify app changes in the package

The unpacked package is the single app verification target. After changing
frontend or bundled UI plugin code, refresh its assets with:

```text
sfdev package --frontend-only
```

After changing native code, worker code, package contents, or when a complete
verification is needed, run:

```text
sfdev package
```

The package command builds the frontend and native targets, runs the required
native checks, and stages `tmp/packaged_build/`. Launch
`tmp/packaged_build/bin/streamfind-app.exe` or the packaged CLI on Windows to
inspect the result. Use the platform's release package for manual Linux app
verification. Keep `sfdev build frontend`, `sfdev test frontend`, and
`sfdev check frontend` for compilation, unit tests, lint, and formatting; they
do not start an alternate app server.

See the [frontend development guide](https://github.com/ricardo-cunha/streamfind/blob/main/frontend/README.md)
and [sfdev guide](https://github.com/ricardo-cunha/streamfind/blob/main/scripts/README.md)
for setup and command details.

## Vendor dependencies

Runtime vendor binaries are centralized beside the executables:

```text
bin/<vendor-library>
```

Built-in plugins reuse the CMake targets created by the central vendor layer. Do not search for arbitrary DLL, SO, LIB, or archive files in a plugin's CMake file. Build the vendor once for the selected platform and compiler, then link every consumer to the same target and package the runtime closure once.

On Windows, use one MSYS2 UCRT64 installation for CMake, Ninja, GCC, and G++.
The UCRT64 build also requires the Apache Arrow development package
`mingw-w64-ucrt-x86_64-arrow`; install it with `sfdev setup --components native`.
Arrow IPC is used for bounded worker/service row-batch transport and result
delivery, and the release package stages its required runtime libraries under
`bin/`. On Linux, install the distribution's Apache Arrow C++
development package before configuring CMake. Keep compiler and runtime
dependencies aligned with the target package.

## Testing, compatibility, and release validation

Run focused checks while developing a plugin. For backend work, list the
registered native tests and filter to the relevant CTest cases:

```powershell
sfdev test backend --list
sfdev test backend --filter <test-name-or-pattern>
```

For frontend plugins and viewers, run the frontend test suite and type/lint
checks:

```powershell
sfdev test frontend
sfdev check frontend
```

An independently developed viewer can be scaffolded and packaged without
copying it into the frontend checkout. Generate it with the repository `.venv`,
build it with the managed Node toolchain, and package the resulting ESM/CSS
assets:

```powershell
& .venv\Scripts\python.exe scripts\dev\plugin_generator.py init `
  tmp\scratch\my-viewer --kind viewer --name my-viewer `
  --contract https://streamfind.dev/catalogue/plugins/example#result
& "$env:USERPROFILE\.streamfind\tools\node\v22.23.4\win-x64\npm.cmd" ci `
  --prefix tmp\scratch\my-viewer --ignore-scripts
& "$env:USERPROFILE\.streamfind\tools\node\v22.23.4\win-x64\npm.cmd" run build `
  --prefix tmp\scratch\my-viewer
& .venv\Scripts\python.exe scripts\dev\package_viewer.py `
  tmp\scratch\my-viewer --output tmp\scratch\my-viewer-package
```

Install and activate the package through the native development command. The
package is checksum-verified and remains inactive until explicitly selected:

```powershell
streamfind-dev viewer install --package tmp\scratch\my-viewer-package
streamfind-dev viewer activate @streamfind/my-viewer-viewer --version 0.1.0
```

Runtime extension locations are configured per StreamFind package or service;
they are not taken from a global user cache. Keep source checkouts separate
from deployable packages and configure the four roots in `streamfind.json`:

```json
{
  "plugin_roots": {
    "development": ["D:/streamfind/dev/plugins"],
    "deployment": ["D:/streamfind/deployed/plugins"]
  },
  "viewer_roots": {
    "development": ["D:/streamfind/dev/viewers"],
    "deployment": ["D:/streamfind/deployed/viewers"]
  },
  "enabled_plugins": ["custom.plugin.example"],
  "enabled_viewers": ["custom.viewer.example"]
}
```

Development roots describe authoring workspaces for development tooling. The
runtime loads native libraries only from plugin deployment roots and loads
viewer packages only from viewer deployment roots. `enabled_plugins` is the
native-plugin allow-list: roots locate possible packages, while this list
selects the plugin IDs that the service may load. `enabled_viewers` applies the
same explicit selection to activated viewer packages. The global
`.streamfind/tools` directory remains the managed cache for external tools
such as MinGW, Python and Node; it is not an extension source or deployment
root.

For packaged-build development, use `streamfind-dev setup --target python
--workspace <workspace>`. The pinned Python runtime is retained under
`$STREAMFIND_HOME/tools/python`, while the workspace receives its own
`<workspace>/.venv`. The extracted package itself is not modified with a
virtual environment.

If a workspace has initial Python dependencies, install them explicitly with
`streamfind-dev setup --target python --workspace <workspace> --requirements
<requirements-file>`. This is not lockfile enforcement: the environment stays
open for additional development packages.

The packaged service then exposes the activated viewer through its combined
`/plugins.json` manifest and same-origin `/viewers/...` asset paths.
Use the worktree's assigned packaged-service port when validating this flow.

Bundled viewers follow the same packaged boundary as native plugins. The
frontend build stages optional built-in viewers under
`viewers/native/native.viewer.<name>/` in the package and the generated runtime
config uses `viewer_roots.deployment: ["../viewers/native"]`. Custom viewer packages
belong under `viewers/custom/` (or another explicitly configured deployment
root). Their manifests use IDs such as `native.viewer.mass_spec` or
`custom.viewer.example`; `enabled_viewers` selects which staged viewers the
service publishes. Native C++ plugin packages are staged under
`plugins/native/`, while generated or user-built plugin packages are staged
under `plugins/custom/`. The core table viewer remains bootstrap code in the
app shell, while feature explorers and other optional viewers are independently
visible in the packaged `viewers/native/` tree.

Official native tests are C++ tests registered with CTest under
`cpp/core/tests/` or the owning `cpp/plugins/<plugin>/tests/` directory.
Development-stage Python checks, data-backed workflow runs, and integration
checks belong under `scripts/dev/` and are not part of CTest. Use the
repository `.venv` for Python development checks. Large mass-spectrometry
fixtures are maintained in the shared `<user>/.streamfind/streamfind.data`
checkout rather than in the native unit-test directories. Run
`sfdev setup --components data` to provision the checkout; worktrees and
external plugin repositories resolve the same managed location.

The native C++ package targets Windows x64 and Linux x86_64. It includes the
runtime, MCP server, browser application, semantic catalogues, and built-in
plugins. Support depends on the backend plugin, file format, and
platform; confirm vendor-reader availability against the specific format and
dataset. Keep catalogue files with the executable. Optional scientific tools
such as Open Babel, Java, and MetFrag are separate dependencies and are not
automatically installed by the native packages.

The packaged interfaces cover project creation and inspection, metadata and
operation-graph access, cache and audit operations, catalogue-backed MCP
schemas, and plugin operations supplied by the enabled plugins. The mass-spec
plugin includes analysis, spectrum, chromatogram, and supported native-file
reader workflows. For the current platform and format matrix, see the
[availability page](status.md).

The native packages do not promise a stable cross-version C++ ABI. Match an
independently developed plugin and the SDK from its runtime archive to the
runtime version it targets. When reporting a compatibility issue, include the streamfind
version, operating system, operation name, relevant parameters, and a minimal
reproducible project or data description. Keep vendor-format support claims
specific to the tested file family and acquisition mode.

Installed native plugins retain their versioned package archives and source
provenance. After a runtime upgrade, inspect compatibility before enabling a
plugin:

```powershell
streamfind-dev doctor --json
streamfind-dev rebuild --plugin <retained-source> --sdk <matching-runtime-sdk>
```

An incompatible plugin is quarantined as inactive with diagnostics; streamfind
does not load it speculatively. Rebuilding proves compatibility only for the
tested runtime, SDK, platform, and compiler combination. Moving a source
workspace requires updating the retained source path before rebuilding.

Before publishing a native release, validate both supported package platforms
with the release scripts. They build the frontend and native runtime, run the
CTest suite, check package contents and dependency closure, and smoke-test the
packaged MCP interface:

```powershell
scripts\release\cpp\release-cpp.ps1
```

On Linux, run:

```sh
scripts/release/cpp/release-cpp-linux.sh
```

Confirm that the complete CTest suite passes and packaged MCP smoke validation
succeeds. Review the generated archives and `sha256sums.txt` in
`tmp/release-output/` before publishing. See the [release guide](releases.md)
for the release process.
