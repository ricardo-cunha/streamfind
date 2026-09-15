# streamfind build & test scripts

Machine-independent helpers for building and testing the primary C++ backend
(`cpp/`) and the alternative Rust backend (`rust/`). All transient artifacts land under
the repository-local `tmp/` folder (see AGENTS.md "Repository Scratch, Build,
and Log Locations"): build trees in `tmp/build/`, release packages in
`tmp/release-output/`, and logs in `tmp/logs/`.

## Quick start

| Task | Command |
| --- | --- |
| Build the C++ backend | `powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1` |
| C++ backend + run CTest | `powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1 -Tests` |
| Run C++ CTest only | `scripts\build\cpp\test-cpp.cmd` |
| Build the alternative Rust backend | `scripts/build/rust/build-rust.cmd` |
| Build Rust and run its tests | `scripts/build/rust/build-rust.cmd -Tests` |
| Run Rust tests only | `scripts/build/rust/test-rust.cmd` |
| Run C++ built-MCP data test | `scripts\dev\cpp\test-data.ps1` |
| Run C++ built-MCP NTA test | `scripts\dev\cpp\test-nta.ps1` |
| Run C++ vendor reader parser test | `scripts\dev\cpp\test-vendor-readers.ps1 -Vendor Shimadzu` |
| Run data-backed NTA pipeline | Add `-RunPipeline` to `scripts\dev\cpp\test-nta.ps1` |
| Run explicit cross-backend conformance | `scripts/dev/conformance/run-conformance.ps1 -CppExecutable <path> -RustExecutable <path> -Thermo` |
| Build C++ release archive | `scripts/release/cpp/release-cpp.cmd -Version <version>` |
| Build Rust release archive | `scripts/release/rust/release-rust.cmd -Version <version> -CppCatalogue <path>` |
| Publish prepared release assets | `scripts/release/publish-release.ps1 -Version <version> -Backend Cpp|Rust|All` |
| Clean build/test artifacts | `scripts\build\clean-build-temp.cmd` |

Every `.cmd` is a thin wrapper over its `.ps1`; use either form.

The official C++ suite is the authoritative framework, plugin, mass-spectrometry
interface, and lightweight NTA coverage registered by CMake. Rust is an
alternative backend: its wrapper requires the C++ build/release catalogue through
`STREAMFIND_CATALOGUE` and does not define the C++ acceptance gate. Raw reader,
parity, and data-backed NTA tests are development-stage checks under
`scripts/dev/cpp/`; they launch the C++ built MCP executable and are not C++ test
source targets. Cross-backend comparisons are isolated under `scripts/dev/conformance/`
and require both executable paths explicitly. They are not part of the C++ gate.

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

Only small backend-neutral fixtures remain under `tests/fixtures/`; large
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
- **cargo** — `$env:CARGO` override, else `PATH`.

Failures are explicit with a helpful message pointing at the missing tool or
install method.

## What each script does

- `scripts/build/cpp/build-cpp.ps1` — required Windows C++ entry point; initializes
  the MSVC environment, configures with Ninja into `tmp/build/core-default`
  (`STREAMFIND_BUILD_TESTS=ON`, `STREAMFIND_BUILD_SHARED=OFF`), builds, and
  optionally runs CTest. Flags: `-Clean`, `-Tests`, `-Target <name>`,
  `-Config <Debug|Release>`.
- `scripts/build/cpp/test-cpp.ps1` — runs `ctest --test-dir tmp/build/core-default
  --output-on-failure` for the official framework, mass-spec interface, and
  lightweight NTA interface suite. Data-backed parsing and NTA checks use the
  dedicated scripts in `scripts/dev/`.
- `scripts/build/rust/build-rust.ps1` — sets `CARGO_TARGET_DIR=tmp/build/rust-target`, requires the C++ catalogue, and builds
  the workspace (or one `-Package`). Flags: `-Clean`, `-Tests`,
  `-Package <name>`, `-Release`.
- `scripts/build/rust/test-rust.ps1` — `build-rust.ps1 -Tests` shorthand against the C++ catalogue.
- `scripts/build/cpp/build-cpp-linux.sh` — configures and builds the authoritative C++ backend with Ninja on Linux; set `STREAMFIND_RUN_TESTS=1` to run CTest.
- `scripts/build/rust/build-rust-linux.sh` — builds the alternative Rust workspace on Linux using `STREAMFIND_CATALOGUE` from the C++ backend; set `STREAMFIND_RUN_TESTS=1` to run Rust tests.
- `scripts/release/cpp/release-cpp.ps1` — builds, tests, packages, and hashes the
  authoritative C++ backend archive. It does not run development-stage data or
  NTA scripts.
- `scripts/release/rust/release-rust.ps1` — builds and optionally tests the
  alternative Rust backend against an explicitly supplied C++ catalogue, then
  packages and hashes only the Rust archive. It does not define the C++ release
  gate.
- `scripts/release/cpp/release-cpp-linux.sh <version>` — builds, tests, and
  packages only the authoritative C++ Linux backend.
- `scripts/release/rust/release-rust-linux.sh <version> <cpp-catalogue>` —
  builds, tests, and packages only the Rust Linux backend against the supplied
  C++ catalogue.
- `scripts/release/publish-release.ps1` — validates the versioned archives and checksums in
  `tmp/release-output/`, then creates a GitHub Release. Pass `-Replace` only
  when intentionally replacing assets in an existing release.

## Notes

- On a plain terminal, `TMP`/`TEMP` must be valid Windows paths for MSVC's
  `link.exe` and cargo doctests (the scripts assume a normal user
  environment).
- Build artifacts are gitignored; `scripts/build/clean-build-temp.cmd` removes
  them while preserving tracked scripts and `tmp/logs/`.