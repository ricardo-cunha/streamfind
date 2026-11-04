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
| Clean generated output, `tmp/`, and Python caches | `scripts\clean\clean-repo-temp.cmd` |

Build `.cmd` files are thin wrappers over their `.ps1` counterparts; the
standalone cleanup command is `scripts/clean/clean-repo-temp.cmd`.

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
and `PATH` are deliberately ignored. Run `sfdev setup --components java`
to provision a SHA-256-verified Temurin JDK 21 before building. Catalogue
generation uses the installed JDK without downloading tools.

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

Only small backend-neutral fixtures remain under `cpp/core/tests/fixtures/`; large
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
- The Windows C++ archive is self-contained at launch: run `bin\\streamfind-app.exe`
  from the package root. It asks `bin\\streamfind.exe` to start or reuse
  `bin\\streamfind-server.exe` with the same
  package-relative runtime paths. Advanced MCP clients can use
  `bin\\streamfind-mcp.exe`. Do not
  prepend any vendor subdirectory to `PATH`; all native runtime libraries are
  kept once in the package's `bin/` directory and the launchers supply that
  path to their child processes.
- The public terminal command is `bin\\streamfind.exe` in the package. Use
  `bin\\streamfind.exe service start`, `service list`, `service status`, and
  `service stop` (or `service stop --instance <id>` when multiple services are
  running) to manage the independent background service.
- Validate an extracted Windows archive with
  `scripts/release/cpp/test-packaged-mcp.ps1 -PackageRoot <package-root>`.
- `scripts/release/publish-release.ps1` — validates the versioned archives and checksums in
  `tmp/release-output/`, then creates a GitHub Release. Pass `-Replace` only
  when intentionally replacing assets in an existing release.

## Notes

- On a plain terminal, the scripts set repository-local Windows `TMP`/`TEMP`
  paths before invoking MinGW, CMake, or Ninja (the scripts assume a normal user
  environment).
- Build artifacts are gitignored; `scripts/clean/clean-repo-temp.cmd` removes
  generated output, all disposable `tmp/` contents, and repository-owned
  Python caches while preserving tracked source, scripts, `.venv`,
  `node_modules`, and vendored source.
## Developer CLI

`sfdev` requires PowerShell 7 or later (`pwsh`). On Windows, install it with:

```powershell
winget install --id Microsoft.PowerShell --exact --source winget
```

Open a new terminal and run `pwsh --version` to confirm the installation.
In VS Code, run **Terminal: Select Default Profile**, choose **PowerShell**,
and open a new integrated terminal.

Run the following commands from the repository root.
CMake discovers all bundled plugins.

```powershell
pwsh -NoProfile -File scripts/sfdev.ps1 env show --json
pwsh -NoProfile -File scripts/sfdev.ps1 doctor
pwsh -NoProfile -File scripts/sfdev.ps1 configure --dry-run
pwsh -NoProfile -File scripts/sfdev.ps1 configure
pwsh -NoProfile -File scripts/sfdev.ps1 build backend --target streamfind_statistics_unit
pwsh -NoProfile -File scripts/sfdev.ps1 test backend --filter '^streamfind_statistics_unit$'
```

The dry run prints the working directory, executable, arguments, and process
environment used for each command. Use `sfdev.ps1 help` for the implemented
commands. The existing setup, application launch, packaging, and release
commands below continue to own their respective workflows.

To use the short command from any directory in a development shell:

```powershell
.\scripts\sfdev.ps1 env shell
sfdev script list
sfdev script info validate_native_dependencies.py
sfdev script run validate_native_dependencies.py -- --help
exit
```

Script IDs are paths relative to `scripts/dev/`. Arguments after `--`
go to the selected script. Runs start from the checkout root, use the
checkout's Python environment for Python scripts, and log to `tmp/logs/`.
Use `script run PATH --dry-run` to preview a run.

On Windows, use `sfdev package --dry-run` to preview the unpacked test
package workflow. Run `sfdev package` to build and test the full package
under `tmp/packaged_build/`. If you already have a complete test package,
`sfdev package --frontend-only` refreshes its frontend assets.

Use `sfdev run app` to start the development service and Vite. Use
`sfdev run backend` for the service alone, or `sfdev run frontend --url URL`
to connect Vite to a ready service. The default endpoint is
`http://127.0.0.1:8790`. Build `streamfind_service`
and `streamfind_worker` before starting the app.
The launcher stops the processes it starts when you exit with Ctrl+C.

Additional workflows:

```powershell
sfdev setup --components frontend,browser --dry-run
sfdev setup --components native --dry-run
sfdev doctor --workflow native
sfdev test browser --url http://127.0.0.1:5173/
sfdev clean --scope build --dry-run
sfdev sdk build --dry-run
sfdev release prepare --dry-run
sfdev release verify
sfdev release publish --dry-run
sfdev version bump patch --dry-run
```

The browser check needs a running frontend. Release preparation requires a
clean committed checkout and builds the archive for the current host.
Bundled plugin manifests and native descriptors use the canonical
`VERSION`. Release verification checks existing local archives against
their recorded checksums. Publishing requires an existing remote tag.
Use `--json` on build, test, package, setup, SDK, release, or script run
commands to get an exit status and log path on stdout. Command output goes
to stderr in JSON mode.

To try an external plugin, first build and extract the developer kit. Then
run the following from the repository development shell:

```powershell
sfdev plugin list
sfdev plugin new ./tmp/scratch/example-plugin --domain example_ping
sfdev plugin build ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
sfdev plugin test ./tmp/scratch/example-plugin
sfdev plugin stage ./tmp/scratch/example-plugin
sfdev plugin validate ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
```

The staged native plugin must be added to the service's configured plugin
roots and enabled plugin IDs before use. The generated frontend module
includes a manifest and a table summary viewer. Serve both at an allowed
origin and add the manifest entry to the frontend's plugin list.

Native setup requires an initialized MSYS2 UCRT64 installation on Windows.
On Ubuntu/Debian, configure an APT source that provides `libarrow-dev` and
refresh the package index before running setup. APT setup uses `sudo`.
