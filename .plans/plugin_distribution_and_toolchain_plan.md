# Plugin distribution and MSYS2 toolchain plan

## Goal

Keep `cpp/sdk` as the internal plugin framework while producing lean, relocatable Windows and Linux C++ runtime releases. Support backend and frontend plugin developers through the source tree and an explicit optional developer kit. Build the Windows stack consistently with MSYS2 UCRT64 CMake, Ninja, and MinGW-w64 GCC.

## Architecture

- Runtime release: host executables, frontend assets, built-in plugin packages, catalogues, licenses, and runtime vendor files under `core/vendors/<vendor>`.
- Internal SDK: always built when required by the host/plugins, but not installed into the runtime release.
- Optional developer kit: enabled explicitly with `STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON`; installs SDK headers, libraries, CMake package files, plugin tools, schemas, templates, and documentation.
- Backend plugins: CMake/C++ plugins using the versioned ABI and shared vendor targets.
- Frontend plugins: versioned TypeScript contracts and templates; no dependency on C++ SDK headers.

## Implementation phases

1. Add an explicit developer-kit install option, defaulting OFF, and make SDK headers/libraries/tools/Jena/CMake exports conditional on it.
2. Move all runtime DuckDB, MinGW, and Open Babel binaries to `core/vendors/`; remove runtime copies from `bin/` and `sdk/bin/`.
3. Centralize vendor targets and runtime layout so built-in plugins reuse one platform-built dependency set.
4. Make the Windows build and release scripts resolve CMake and Ninja from the selected MSYS2 installation. If required tools are absent, install the UCRT64 packages with that installation's `pacman` and then verify the resolved paths.
5. Produce only the lean `streamfind-core-cpp-<version>-<platform>` runtime archive by default. Keep developer-kit packaging explicit and separate.
6. Add backend/frontend plugin templates, schemas, documentation, and an out-of-tree consumer smoke test.
7. Validate clean Windows and Linux extracted archives with restricted environment paths, MCP startup, plugin discovery, and one representative operation.

## Acceptance criteria

- Runtime archives contain no `sdk/` directory.
- Runtime vendor binaries exist only under `core/vendors/`.
- Windows release uses MSYS2-provided CMake, Ninja, GCC, and G++ from one MSYS2 root.
- Missing MSYS2 build tools are installed through that root's package manager or fail with an actionable diagnostic.
- `cpp/sdk` still builds and built-in plugins still load dynamically.
- Developer-kit installation remains available and does not alter the runtime archive.
- Windows and Linux extracted packages start without developer PATH/toolchain variables.
- Existing project and scratch directories under `tmp/` are preserved.

## Implemented in this slice

- Added `STREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT`, default OFF.
- Removed SDK headers, libraries, tools, Jena, CMake exports, and plugin import archives from the runtime install.
- Moved Windows DuckDB and MinGW runtime installation to `core/vendors/` and stopped duplicating them beside plugins or under `bin/`.
- Added the MSYS2 UCRT64 developer-kit preset.
- Made the Windows build/release helper select MSYS2 CMake and Ninja and install missing packages through MSYS2 `pacman`.
- Added plugin-development documentation and linked it from the documentation index.
- Verified the runtime CPack archive contains no `sdk/` entries, no vendor DLLs under `bin/` or plugin directories, and the extracted MCP smoke test passes.
- Verified the developer-kit configuration, build, and install still produce the SDK package.

## Remaining work

- Add a first-class backend/frontend plugin project generator rather than only templates and documentation. **Implemented** by `scripts/dev/plugin_generator.py`.
- Add a dedicated developer-kit archive command and out-of-tree consumer test to the release pipeline. **Implemented** by the Windows/Linux dev-kit packaging commands and `scripts/dev/test_plugin_consumer.py`.
- Add Linux extracted-archive startup testing and recursive ELF/PE dependency-closure validation. **Implemented** by the Linux release smoke test and `scripts/dev/validate_native_dependencies.py`; Linux execution remains to be run on a Linux host/CI runner.

## Developer commands

```text
.venv/Scripts/python.exe scripts/dev/plugin_generator.py init my-plugin --kind full --domain example --name my-plugin
scripts/release/cpp/package-plugin-devkit.ps1 -Version <version>
scripts/release/cpp/package-plugin-devkit-linux.sh <version>
```

The release scripts validate the extracted runtime archive's native dependency closure before the packaged MCP startup smoke test. The dev-kit command also configures, builds, installs, packages, and compiles the out-of-tree C++ consumer against the installed CMake package.
