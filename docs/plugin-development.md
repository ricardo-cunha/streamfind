# Plugin development

StreamFind has one internal native framework and two external extension surfaces:

- **Backend plugins** are C++20 dynamic libraries built against the versioned StreamFind plugin ABI.
- **Frontend plugins** are TypeScript packages that consume the frontend plugin and artifact contracts.

The normal `streamfind-core-cpp` runtime archive is not a development SDK. It contains the built-in plugins and runtime vendor files, but no SDK headers, libraries, CMake package, Jena tools, or plugin validators.

## Backend plugin development

For StreamFind contributors, use the repository source tree. The native framework is layered as:

```text
cpp/core/       project lifecycle, DuckDB, transactions, workflow runtime
cpp/sdk/        ABI, host callbacks, manifests, catalogue projection, loader
cpp/plugins/    domain readers, algorithms, catalogues, and plugin packages
```

The SDK is built internally and is not a second backend. A plugin must use the generic host boundary; it must not receive `Project`, DuckDB handles, STL objects, JSON objects, or C++ exceptions across the dynamic boundary.

The optional developer-kit install is enabled with:

```text
-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON
```

That install contains the public headers, CMake package, import/static libraries, catalogue tools, plugin validator, and Jena build tooling. It is intentionally separate from the runtime package.

A developer-kit consumer uses the installed package:

```cmake
find_package(streamfind CONFIG REQUIRED)
streamfind_add_plugin(my_plugin example)
```

The repository example is `cpp/sdk/examples/minimal_plugin`. A complete acceptance test must configure, compile, link, and execute an out-of-tree consumer; `find_package` alone is not enough.

## Frontend plugin development

Frontend plugins do not link to the C++ SDK. They consume versioned TypeScript contracts for:

- operation and capability metadata;
- artifact and table schemas;
- viewer/editor registration;
- backend service communication;
- plugin manifests.

Develop frontend plugins in the `frontend/` workspace with the Vite development server and the local C++ service. Keep frontend schemas derived from the shared semantic/catalogue contracts rather than duplicating backend table definitions.

## Vendor dependencies

Runtime vendor binaries are always installed below:

```text
core/vendors/<vendor>/
```

Built-in plugins reuse the CMake targets created by the central vendor layer. Do not search for arbitrary DLL, SO, LIB, or archive files in a plugin's CMake file. Build the vendor once for the selected platform and compiler, then link every consumer to the same target and package the runtime closure once.

The Windows build uses one MSYS2 UCRT64 installation for CMake, Ninja, GCC, and G++. The build wrapper installs missing UCRT64 packages through that installation's `pacman` and refuses to mix the MSYS2 compiler with a separate CMake installation.
