# Plugin development

streamfind supports two extension surfaces:

- **Backend plugins** are C++20 dynamic libraries built against the versioned StreamFind plugin ABI.
- **Frontend plugins** are TypeScript packages that consume the frontend plugin and result contracts.

The normal `streamfind-core-cpp` runtime archive is not a development SDK. It contains the built-in plugins and runtime vendor files, but no SDK headers, libraries, CMake package, Jena tools, or plugin validators.

## Backend plugin development

Use the repository source tree to build backend plugins. The main areas are:

```text
cpp/core/       service lifecycle, DuckDB metadata, transactions, workflow runtime
cpp/sdk/        ABI, host callbacks, manifests, catalogue projection, loader
cpp/plugins/    domain readers, algorithms, catalogues, and plugin packages
```

Plugins use the generic host boundary. Do not pass `Project`, DuckDB handles,
STL objects, JSON objects, or C++ exceptions across the dynamic boundary.

The developer kit is enabled with:

```text
-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON
```

The kit contains public headers, the CMake package, libraries, catalogue tools,
the plugin validator, and semantic tooling. It is separate from the runtime
package.

A developer-kit consumer uses the installed package:

```cmake
find_package(streamfind CONFIG REQUIRED)
streamfind_add_plugin(my_plugin example)
```

The repository example is `cpp/sdk/examples/minimal_plugin`. Validate a plugin
by configuring, compiling, linking, and executing an out-of-tree consumer;
`find_package` alone is not enough.

## Frontend extension development

Frontend plugins do not link to the C++ SDK. They consume versioned TypeScript contracts for:

- operation and capability metadata;
- result and table schemas;
- viewer/editor registration;
- service communication and result retrieval;
- plugin manifests.

Develop frontend plugins in the `frontend/` workspace with the Vite server and
local C++ service. Keep frontend schemas derived from the shared catalogue
contracts rather than duplicating backend table definitions.

## Vendor dependencies

Runtime vendor binaries are centralized beside the executables:

```text
bin/<vendor-library>
```

Built-in plugins reuse the CMake targets created by the central vendor layer. Do not search for arbitrary DLL, SO, LIB, or archive files in a plugin's CMake file. Build the vendor once for the selected platform and compiler, then link every consumer to the same target and package the runtime closure once.

On Windows, use one MSYS2 UCRT64 installation for CMake, Ninja, GCC, and G++.
The UCRT64 build also requires the Apache Arrow development package
`mingw-w64-ucrt-x86_64-arrow`; the build helper installs it when it is missing.
Arrow IPC is used for bounded worker/service row-batch transport and result
delivery, and the release package stages its required runtime libraries under
`bin/`. On Linux, install the distribution's Apache Arrow C++
development package before configuring CMake. Keep compiler and runtime
dependencies aligned with the target package.
