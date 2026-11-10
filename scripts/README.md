# Developing streamfind with `sfdev`

`sfdev` is the supported command-line entry point for developing, building,
testing, packaging, and preparing releases of streamfind. It gives people and
automation the same commands, tool checks, build locations, and logs across
Windows and Linux. Start here instead of memorizing individual build scripts.

Run commands from the repository root unless a command says otherwise. The
canonical version is in [`VERSION`](../VERSION), and the development rules and
temporary-file policy are in [`AGENTS.md`](../AGENTS.md).

## 1. Install and start `sfdev`

`sfdev` requires **PowerShell 7 or later** (`pwsh`) on both Windows and Linux.
Windows PowerShell 5.1 cannot run it. On Windows, install PowerShell 7 with:

```powershell
winget install --id Microsoft.PowerShell --exact --source winget
```

Then open a new terminal and check `pwsh --version`. In VS Code, choose
**Terminal: Select Default Profile**, select the PowerShell 7 profile, and
open a new integrated terminal. The terminal must actually be running PowerShell
7; installing it does not change already-open Windows PowerShell 5.1 sessions.

From a PowerShell 7 terminal at the repository root, the direct invocation is:

```powershell
pwsh -NoProfile -File scripts/sfdev.ps1 help
```

To open a development shell where the short `sfdev` command is available for
this checkout:

```powershell
pwsh -NoProfile -File scripts/sfdev.ps1 env shell
sfdev help
```

Type `exit` to leave that shell. `env shell` does not install a global command
or change the machine's PATH. On Linux or WSL, use the same script from a
PowerShell 7 session, with the repository checked out in the Linux filesystem
for native Linux builds. The direct `pwsh -File` form works when you do not
want to enter the development shell.

## 2. Understand the environment before working

Use `doctor` to see whether the tools for a task are available, and `env show`
to inspect the checkout, selected build directory, and resolved tool paths:

```powershell
sfdev env show
sfdev doctor
sfdev doctor --workflow native
sfdev doctor --workflow frontend
sfdev doctor --workflow scripts
```

Add `--json` to supported inspection commands when another tool needs
machine-readable output. `doctor` exits unsuccessfully when a required tool is
missing. `sfdev help <command>` shows the options and prerequisites for a
specific command, for example `sfdev help test backend`.

Install missing prerequisites through the setup command rather than guessing
which machine-specific path or package is needed:

```powershell
sfdev setup --components native,java,frontend,python
sfdev setup --components python
sfdev setup --components browser
```

`native` includes the managed Java needed for semantic-catalogue generation.
Frontend setup uses a compatible Node.js and npm from `PATH` when both are
available. If either is missing or incompatible, it installs the pinned
Node.js/npm bundle under
`%USERPROFILE%\.streamfind\tools\node` on Windows or
`~/.streamfind/tools/node` on Linux, then installs project packages under
`frontend/node_modules`. The managed Node.js version is pinned in
`scripts/build/node-version.txt`; update its official archive hashes in
`scripts/build/setup-node.ps1` when changing that pin. `STREAMFIND_HOME` can
override the managed tools root. Browser setup is only needed for browser
smoke checks; Playwright browsers are stored in `tools/playwright` under the
same managed tools root. On Ubuntu/Debian, native setup uses the system
package manager and needs an APT source that provides `libarrow-dev`; package
installation uses `sudo`.

Python scripts always run with the repository `.venv`. `sfdev setup
--components python` uses an installed Python 3.11–3.14 that supports `venv`
when available. Otherwise it installs the pinned Python 3.11 runtime under
`%USERPROFILE%\.streamfind\tools\python` on Windows or
`~/.streamfind/tools/python` on Linux, then creates `.venv` in the checkout.
The pin and platform archive checksums are maintained in
`scripts/build/python-version.txt` and `scripts/build/setup-python.ps1`.
`STREAMFIND_HOME` also relocates this managed Python. Project Python packages
remain in `.venv` and are never shared from the user tools directory.

### Supported native toolchains

The Windows C++ build uses **MSYS2 MinGW-w64 UCRT64 GCC/G++, CMake, Ninja, and
Arrow development packages**. `sfdev` first uses an existing UCRT64 MSYS2
installation found through `PATH` or at `C:\msys64`; if unavailable, it uses
`%USERPROFILE%\.streamfind\tools\msys2`.
`STREAMFIND_MSYS2_ROOT` selects an explicit root, and `STREAMFIND_HOME` changes
the managed tools root. Run `sfdev setup --components native` to install
MSYS2 and the required UCRT64 packages when missing. The installer version is
pinned in `scripts/build/msys2-version.txt`; package selection is defined in
`scripts/build/setup-msys2.ps1`. Setup persists the selected root for the
current Windows user so VS Code can use it; restart VS Code after initial
setup. Do not use MSVC or Visual Studio build tools for the streamfind C++ core
or bundled plugins.

Linux builds use GCC/G++, CMake, and Ninja. WSL Ubuntu is supported for Linux
release and developer-kit creation and validation. Run
PowerShell 7 inside the WSL environment and keep the checkout/build on the
Linux filesystem for the Linux toolchain. Each platform has a separate build
with its own deterministic build tree; do not share one build directory across
core/developer-kit modes, compilers, or operating systems. The Linux scripts remove Windows-mounted executable paths
from `PATH` and discard imported Windows CMake/pkg-config hints. The Linux
CMake preset clears inherited package-search hints. If an existing Linux CMake
cache contains Windows/MSYS2 package or compiler paths, the managed tree is
discarded and configured again with native Linux tools.

## 3. Common development workflows

### Build the native backend and frontend

Configure once after checkout or after changing toolchains. CMake discovers
and configures all bundled plugins; there are no plugin build profiles to
select.

```powershell
sfdev configure
sfdev build all
```

`build all` builds the configured native targets and then the frontend. If you
only changed one area, use the narrower command:

```powershell
sfdev build backend
sfdev build backend --target streamfind_statistics_unit
sfdev build frontend
```

Use `--config Release` with `configure` when you specifically need a Release
build. The default is Debug. `--jobs N` controls native parallel build jobs.

### Run the development app

```powershell
sfdev run app
```

This starts Vite and starts or reuses a development service at
`http://127.0.0.1:8790`. Press Ctrl+C to stop processes started by this
command. To run only one side:

```powershell
sfdev run backend
sfdev run frontend --url http://127.0.0.1:8790
```

Build the service and worker before starting the backend. The frontend-only
form connects to an already-ready service; it does not start one.

### Run tests and checks

The native C++ tests registered with CTest are the official native test suite.
List them before choosing a narrow test, then run the relevant selection:

```powershell
sfdev test backend --list
sfdev test backend --filter '^streamfind_statistics_unit$'
sfdev test backend --label 'mass_spec|core'
```

`--filter` and `--label` accept CTest regular expressions. Build the selected
target first when needed. Frontend checks are separate:

```powershell
sfdev test frontend
sfdev test frontend --watch
sfdev check frontend
```

`check frontend` runs the frontend build, lint, and formatting check in order.
For a browser smoke check, start the frontend first and provide its URL:

```powershell
sfdev test browser --url http://127.0.0.1:5173/
```

Large-data, mass-spectrometry, Raman, parser, and workflow checks are
development checks, not CTest tests. Keep them under `scripts/dev/` and use
the development-script runner described below. Large example data comes from
the sibling `streamfind.data` repository, unless `STREAMFIND_EXAMPLE_DATA_ROOT`
points to another data directory.

## 4. Development scripts and data checks

List available development scripts before choosing one. Inspect its required
arguments and resolved interpreter, then run it through `sfdev`:

```powershell
sfdev script list
sfdev script info validate_native_dependencies.py
sfdev script run validate_native_dependencies.py -- --help
```

Script paths are relative to `scripts/dev/`. Put script arguments after the
separator `--`; `sfdev` arguments such as `--dry-run` go before it. Python
scripts always use the repository `.venv`, never the system Python. Data-backed
development scripts may require the sibling `streamfind.data` repository or
explicit paths; use `script info` and the script's help to confirm them.

Use the JSON option where supported for automation. A dry run prints the
planned working directory, executable, arguments, and relevant environment
without running the command:

```powershell
sfdev script run validate_native_dependencies.py --dry-run -- --help
sfdev test backend --filter '^streamfind_statistics_unit$' --dry-run
```

Build, test, and script-run logs are written under `tmp/logs/`.

## 5. Package the app for manual testing

The unpacked Windows test distribution is separate from the native build trees
and release archives. Create or refresh it from native Windows PowerShell with:

```powershell
sfdev package
```

The full Windows package workflow builds the frontend and native code, runs
CTest, and stages the package under `tmp/packaged_build/`. Run the app from
`tmp/packaged_build/bin/` to exercise the packaged service, plugins, catalogue,
frontend, and runtime paths together.

Do not run `sfdev package` from WSL. WSL is reserved for Linux release and
developer-kit builds and their extracted-package checks; those write to the
Linux build trees and `tmp/release-output/`, and never create or replace
`tmp/packaged_build/`.

For a frontend-only update to an existing complete package:

```powershell
sfdev package --frontend-only
```

This rebuilds the frontend and replaces the package's `app/` assets without
rebuilding native code. Do not patch files inside `tmp/packaged_build/`; update
the source or packaging rule and regenerate the package.

## 6. Create and validate an external plugin

The plugin commands scaffold and validate an external plugin against the
streamfind plugin developer kit:

```powershell
sfdev sdk build
sfdev plugin list
sfdev plugin new ./tmp/scratch/example-plugin --domain example_ping
sfdev plugin build ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
sfdev plugin test ./tmp/scratch/example-plugin
sfdev plugin stage ./tmp/scratch/example-plugin
sfdev plugin validate ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
```

Replace `<extracted-kit-path>` with the actual extracted kit location. The
staged native plugin must be added to the service's configured plugin roots
and enabled plugin IDs before it can be loaded. `sdk build` writes its
versioned developer-kit package to `tmp/release-output/`.

## 7. Prepare a release

Release preparation requires a clean, committed checkout and uses the
canonical version in `VERSION`. Build each platform's archive on its native
host: Windows on Windows, Linux on Linux or WSL. The release output folder is
`tmp/release-output/`.

```powershell
sfdev version show
sfdev release prepare
sfdev release verify
```

Run the same preparation and verification from Linux/WSL to produce and check
the Linux archive. `release verify` checks local checksums and smoke-tests the
archive for the current host. After the version tag has been pushed and the
release is ready to publish, use `sfdev release publish`; publishing requires
authenticated GitHub CLI (`gh`) and an existing remote version tag. See
[`AGENTS.md`](../AGENTS.md) for the guarded release and tag procedure.

The release archives are the distributable outputs. `tmp/packaged_build/` is
only the unpacked Windows manual-test package and is never a release asset.

## 8. A repeatable workflow for AI agents and automation

Agents should use `sfdev` as the shared interface rather than inventing
toolchain paths or invoking an unrelated legacy script. This makes actions
reproducible and lets a human inspect the same plan and logs.

1. Start at the repository root and inspect `sfdev env show` and the relevant
   `sfdev doctor --workflow ...` result.
2. Read `sfdev help <command>` before using an unfamiliar command. For a
   development data script, use `sfdev script list` and `sfdev script info`
   first to learn its arguments and interpreter.
3. Preview consequential or destructive commands with `--dry-run`. Review the
   resolved directory, executable, arguments, and environment before running.
4. Choose the narrowest command that validates the change: a CMake target,
   CTest filter/label, frontend check, or one development script.
5. Keep generated builds, package outputs, test projects, and logs under
   `tmp/`. Keep large-data checks out of CTest and official native test folders.
6. Report the command run, whether it completed, and the relevant log path
   under `tmp/logs/`. Do not report a test or release as successful unless its
   command completed successfully.

For orchestration, `--json` is available on supported inspection and
execution commands. It provides structured status and log-path information;
command output is sent to stderr in JSON mode. Check `sfdev help <command>` for
the exact options of that command.

## 9. Build, package, release, and cleanup locations

| Location | Purpose |
| --- | --- |
| `tmp/build/windows/core/` | Windows core build tree |
| `tmp/build/windows/devkit/` | Windows plugin developer-kit build tree |
| `tmp/build/linux/core/` | Linux core build tree and Linux release build |
| `tmp/build/linux/devkit/` | Linux plugin developer-kit build tree |
| `tmp/packaged_build/` | Native host-machine unpacked package for manual app/service/MCP testing; this Windows workflow creates it on Windows |
| `tmp/release-output/` | Versioned release archives and checksums |
| `tmp/projects/` | Disposable DuckDB projects and fixtures created for checks |
| `tmp/logs/` | Build, test, script, and service logs |
| `tmp/scratch/` | Other disposable development files |

To preview cleanup, then remove only the active platform build tree:

```powershell
sfdev clean --scope build --dry-run
sfdev clean --scope build
```

`sfdev clean --scope all-temp` removes the contents of the whole repository
`tmp/`, including packages, release archives, test projects, and logs. Preview
it first and preserve anything needed before running it:

```powershell
sfdev clean --scope all-temp --dry-run
```

## 10. Command map

| Command | Use |
| --- | --- |
| `sfdev env show` / `env shell` | Inspect the checkout or enter its development shell |
| `sfdev doctor` / `setup` | Check or install development prerequisites |
| `sfdev configure` | Configure the native CMake build tree |
| `sfdev build backend\|frontend\|all` | Build native code, UI, or both |
| `sfdev test backend\|frontend\|browser` | Run CTest, frontend tests, or a browser smoke check |
| `sfdev check frontend` | Build, lint, and format-check frontend code |
| `sfdev run backend\|frontend\|app` | Start development service and/or Vite |
| `sfdev script list\|info\|run` | Discover and run development-stage scripts |
| `sfdev package` | Build or refresh the unpacked Windows test package |
| `sfdev sdk build` / `sfdev plugin ...` | Build a plugin SDK or work on an external plugin |
| `sfdev version show\|bump` | Inspect or advance the canonical version |
| `sfdev release prepare\|verify\|publish` | Prepare, verify, or publish versioned release archives |
| `sfdev clean` | Remove the platform build tree or all temporary output |

Run `sfdev help` for the current command list and `sfdev help <command>` for
full syntax. The implementation in `scripts/sfdev.ps1` is authoritative when
this guide and command help differ.
