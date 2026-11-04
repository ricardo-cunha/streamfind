# Plugin development

streamfind supports two extension surfaces:

- **Backend plugins** are C++20 dynamic libraries built against the versioned StreamFind plugin ABI.
- **Frontend plugins** are TypeScript packages that consume the frontend plugin and result contracts.

## Choose a development environment

The streamfind repository is the normal development environment for plugins
that will ship with streamfind. It contains the backend host and SDK sources,
all bundled C++ plugins, the frontend application and plugin framework, the
semantic catalogues, examples, and build tools. You do not need to install a
separate developer kit to work on these in-repository plugins.

Use the separate backend developer kit when you are building a C++ plugin in
another repository or distributing a plugin independently of the streamfind
source tree. The kit supplies the matching public headers, CMake package,
linkable libraries, catalogue tools, and validator. It is version- and
platform-specific. It is not needed for frontend plugin development.

| Work | Recommended environment | Separate kit needed? |
| --- | --- | --- |
| Modify a bundled C++ plugin | This repository, under `cpp/plugins/<domain>/` | No |
| Modify the frontend or a bundled UI plugin | This repository, under `frontend/` | No |
| Build an independent C++ plugin project | External project plus matching backend developer kit | Yes |
| Build an independent frontend plugin | Frontend plugin contracts and a compatible frontend host | No C++ kit |

The kit is optional: it makes the backend plugin contract consumable without
cloning or building the whole repository. It does not replace the repository
as the development environment for streamfind itself.

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

Use `sfdev run app` to work against the local service while developing the UI.
See the [C++ guide](../cpp/README.md) and [frontend guide](../frontend/README.md)
for toolchain and application details.

The normal `streamfind-core-cpp` runtime archive is not a development SDK. It contains the built-in plugins and runtime vendor files, but no SDK headers, libraries, CMake package, Jena tools, or plugin validators.

## Build an out-of-tree backend plugin

The main backend areas in the repository are:

```text
cpp/core/       service lifecycle, DuckDB metadata, transactions, workflow runtime
cpp/sdk/        ABI, host callbacks, manifests, catalogue projection, loader
cpp/plugins/    domain readers, algorithms, catalogues, and plugin packages
```

Plugins use the generic host boundary. Do not pass `Project`, DuckDB handles,
STL objects, JSON objects, or C++ exceptions across the dynamic boundary.

Run `sfdev sdk build` from the repository development shell. On Windows it
always builds, tests, installs, packages, and checks the Windows kit. If an
Ubuntu WSL distribution is installed, it also builds and validates the Linux
kit there. On Linux, it builds the Linux kit for that host. Archives and
checksums are written to `tmp/release-output/`. The normal runtime release
does not include the SDK.

The kit can also be enabled directly in a CMake developer build with:

```text
-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON
```

The installed kit contains public headers, the CMake package, libraries,
catalogue tools, the plugin validator, and semantic tooling. It is separate
from the runtime package.

A developer-kit consumer uses the installed package:

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
