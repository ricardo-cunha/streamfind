# streamfind build & test scripts

Machine-independent helpers for building and testing the C++ backend
(`cpp/`). All transient artifacts land under the repository-local `tmp/` folder
(see AGENTS.md "Repository Scratch, Build, and Log Locations"): build trees in
`tmp/build/`, release packages in `tmp/release-output/`, and logs in `tmp/logs/`.

## Quick start

| Task | Command |
| --- | --- |
| Build the C++ backend | `powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1` |
| C++ backend + run CTest | `powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1 -Tests` |
| Run C++ CTest only | `scripts\build\cpp\test-cpp.cmd` |
| Run C++ built-MCP data test | `scripts\dev\cpp\test-data.ps1` |
| Run C++ built-MCP NTA test | `scripts\dev\cpp\test-nta.ps1` |
| Run C++ vendor reader parser test | `scripts\dev\cpp\test-vendor-readers.ps1 -Vendor Shimadzu` |
| Run data-backed NTA pipeline | Add `-RunPipeline` to `scripts\dev\cpp\test-nta.ps1` |
| Build C++ release archive | `scripts/release/cpp/release-cpp.cmd -Version <version>` |
| Run extracted packaged MCP smoke | `scripts/release/cpp/test-packaged-mcp.ps1 -PackageRoot <extracted-package>` |
| Publish prepared release assets | `scripts/release/publish-release.ps1 -Version <version>` |
| Clean build/test artifacts | `scripts\build\clean-build-temp.cmd` |

Every `.cmd` is a thin wrapper over its `.ps1`; use either form.

## Native toolchain contract

The native stack is deliberately fixed to **CMake + Ninja + GCC-family C/C++**.
Do not mix compilers or reuse a build directory after changing toolchains.

| Host | Required tools | Resolution policy |
| --- | --- | --- |
| Windows | MinGW-w64 UCRT64 GCC/G++, CMake, Ninja | Standard MSYS2 root `C:\msys64`; override with `STREAMFIND_MINGW_ROOT` |
| Linux | GCC/G++, CMake, Ninja, tar, sha256sum | Standard `/usr/bin` installation; override through `PATH`, `CC`, `CXX`, and `NINJA` |

The Windows scripts prepend `ucrt64\bin` and `usr\bin` to the process `PATH`,
set `MSYSTEM=UCRT64`, validate the compiler paths, and isolate temporary files
under `tmp\scratch`. This is required even when `g++.exe` itself is discoverable:
its `cc1plus.exe` subprocess needs the UCRT64 runtime directories as well.

Use the host presets for direct CMake work:

```powershell
# Windows PowerShell
cmake --preset mingw-ucrt64 -S cpp
cmake --build --preset mingw-ucrt64

# Select a subset of plugins when developing:
cmake --preset mingw-ucrt64 -S cpp -DSTREAMFIND_ENABLED_PLUGINS="mass_spec;raman"
```

```bash
# Linux
cmake --preset linux-gcc -S cpp
cmake --build tmp/build/linux-gcc --preset linux-gcc
```

The canonical scripts perform the same setup and are preferred for releases:

```powershell
scripts/build/cpp/build-cpp.ps1 -Tests -Plugins ALL
scripts/release/cpp/release-cpp.ps1 -Version <version> -Plugins ALL
```

```bash
STREAMFIND_PLUGINS=ALL scripts/release/cpp/release-cpp-linux.sh <version>
```

`STREAMFIND_ENABLED_PLUGINS` is a semicolon-separated CMake list. The wrapper
scripts accept comma-separated values on Windows (`-Plugins mass_spec,raman`)
and the Linux release lane accepts the same CMake list through
`STREAMFIND_PLUGINS`. `ALL` is the release default. Plugin discovery remains
directory-based, but every enabled plugin must define
`streamfind_<domain>_plugin`; its manifest, semantic catalogue, ABI validation,
and runtime dependencies are staged by that plugin's CMake target.

Never share `tmp/build/mingw-ucrt64` with another compiler configuration or a release
build directory. Delete the tree or use a new named preset when changing the compiler.

### Semantic catalogue Java

Semantic catalogue generation validates Turtle contracts with the vendored
Apache Jena runtime. It always resolves Java from
`%USERPROFILE%\.streamfind\tools\java\jdk-*\bin\java.exe` on Windows or
`~/.streamfind/tools/java/jdk-*/bin/java` on POSIX. System Java, `JAVA_HOME`,
and `PATH` are deliberately ignored. If the managed JDK is absent, the
catalogue tool downloads Temurin JDK 21 from Adoptium, extracts it into the
same `.streamfind` tool directory, and then uses that executable.

`STREAMFIND_HOME` can override the `.streamfind` root for CI and disposable
test environments. The normal packaged runtime does not need Java; this
provisioning applies only to development and release-time catalogue generation.

The official C++ suite is the authoritative framework, plugin, mass-spectrometry
interface, and lightweight NTA coverage registered by CMake. Raw reader, parity,
and data-backed NTA checks are development-stage checks under `scripts/dev/cpp/`;
they launch the C++ built MCP executable and are not C++ test source targets.
They are not part of the C++ gate.

## External example data

Large mass-spectrometry and Raman example datasets are maintained in the
auxiliary `streamfind.data` repository next to this checkout:

```text
<parent-directory>/streamfind.data
```

For example, when the repositories are under `C:/Users/cunha/Documents/GitHub`:

```text
C:/Users/cunha/Documents/GitHub/streamfind
C:/Users/cunha/Documents/GitHub/streamfind.data
```

The development PowerShell scripts detect the sibling repository's `data/`
directory automatically. Clone the auxiliary repository from:

```text
https://git.uni-due.de/odea-project/streamfind/streamfind.data
```

To use a different data directory, set:

```text
STREAMFIND_EXAMPLE_DATA_ROOT=<path-to-streamfind.data/data>
```

Only small backend-neutral fixtures remain under `cpp/tests/fixtures/`; large
example datasets are not release contents.

## Toolchain detection (recommended standards)

`scripts/build/build-common.ps1` resolves the toolchain with no hardcoded machine
paths, using the standard mechanisms:

- **Visual Studio / MSVC** — located via `vswhere.exe` (the official Visual
  Studio Installer query tool), selecting the latest installation with the
  VC++ x64 tools component; `vcvarsall.bat` is then derived from that install.
  Override with `$env:VSINSTALLDIR`.
- **cmake** — `$env:CMAKE` override, else `PATH` (`Get-Command`).
- **ctest** — resolved as the sibling of the resolved `cmake` (same
  installation), else `PATH`.
- **ninja** — `$env:NINJA` override, else `PATH`, else Visual Studio's bundled
  Ninja (`Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`).

Failures are explicit with a helpful message pointing at the missing tool or
install method.

## What each script does

- `scripts/build/cpp/build-cpp.ps1` — required Windows C++ entry point; initializes
  the MinGW-w64 UCRT64 environment, configures with Ninja into `tmp/build/mingw-ucrt64`
  (`STREAMFIND_BUILD_TESTS=ON`, `STREAMFIND_BUILD_SHARED=OFF`), builds, and
  optionally runs CTest. Flags: `-Clean`, `-Tests`, `-Target <name>`,
  `-Config <Debug|Release>`.
- `scripts/build/cpp/test-cpp.ps1` — runs `ctest --test-dir tmp/build/mingw-ucrt64
  --output-on-failure` for the official framework, mass-spec interface, and
  lightweight NTA interface suite. Data-backed parsing and NTA checks use the
  dedicated scripts in `scripts/dev/`.
- `scripts/build/cpp/build-cpp-linux.sh` — configures and builds the C++ backend with Ninja on Linux; set `STREAMFIND_RUN_TESTS=1` to run CTest.
- `scripts/release/cpp/release-cpp.ps1` — builds, tests, packages, and hashes the
  C++ backend archive. It does not run development-stage data or NTA scripts.
- `scripts/release/cpp/release-cpp-linux.sh <version>` — builds, tests, and
  packages the C++ Linux backend.
- The Windows C++ archive is self-contained at launch: run `streamfind.exe`
  from the package root. It starts `bin\\streamfind_service.exe` with the same
  package-relative runtime paths. Advanced MCP clients can use
  `bin\\streamfind_mcp_launcher.exe` or `bin\\streamfind_mcp.exe`. Do not
  prepend the package's `core\\vendors\\mingw` or `core\\vendors\\duckdb`
  directories to `PATH`; the launchers supply those paths to their child
  processes.
- Validate an extracted Windows archive with
  `scripts/release/cpp/test-packaged-mcp.ps1 -PackageRoot <package-root>`.
- `scripts/release/publish-release.ps1` — validates the versioned archives and checksums in
  `tmp/release-output/`, then creates a GitHub Release. Pass `-Replace` only
  when intentionally replacing assets in an existing release.

## Notes

- On a plain terminal, the scripts set repository-local Windows `TMP`/`TEMP`
  paths before invoking MinGW, CMake, or Ninja (the scripts assume a normal user
  environment).
- Build artifacts are gitignored; `scripts/build/clean-build-temp.cmd` removes
  them while preserving tracked scripts and `tmp/logs/`.