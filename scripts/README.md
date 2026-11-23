# Developing streamfind with `sfdev`

`sfdev` is the supported command-line entry point for developing, building,
testing, packaging, and preparing releases of streamfind. It gives people and
automation the same commands, tool checks, build locations, and logs across
Windows and Linux. Start here instead of memorizing individual build scripts.

Run commands from the repository root unless a command says otherwise. The
canonical version is in [`VERSION`](../VERSION), and the development rules and
temporary-file policy are in [`AGENTS.md`](../AGENTS.md).

`sfdev.ps1` resolves its checkout from the script's own location, not the
terminal's current directory. You can invoke a worktree's script by a relative
path while standing in the main checkout, for example
`pwsh -NoProfile -File .worktrees/mass-spec-expansion/scripts/sfdev.ps1 env show`;
its build trees, projects, packages, and logs remain under that worktree.

`sfdev` is the checkout development framework: it builds and tests the source
checkout, creates the unpacked Windows verification package, and prepares
releases. The embedded development framework is the `streamfind-dev` command
and `dev/` payload shipped inside a packaged build or extracted release. It is
for developing external plugins and viewers against that package's SDK and is
not a replacement for checkout `sfdev`.

## Quick start for AI agents

Use this sequence at the beginning of a coding task:

```powershell
sfdev env show
sfdev doctor --workflow scripts
sfdev help <command>
```

Then run only the setup and validation needed for the task. For example,
`sfdev doctor --workflow native` checks native build tools, while
`sfdev doctor --workflow frontend` checks the frontend toolchain. If the
`sfdev` shell function is unavailable, call the same framework directly:
`pwsh -NoProfile -File scripts/sfdev.ps1 <command> ...`.

Before a command that removes files, starts a service, replaces a package, or
creates a release, inspect its `sfdev help` entry and use `--dry-run` when that
command supports it. Depending on the command, dry-run prints planned process
steps and environment or cleanup targets. It does not execute the planned
steps or deletions, and it does not guarantee that a build, test, or service
operation will succeed. Do not infer that `--dry-run` is accepted globally.

## 1. Install and start `sfdev`

`sfdev` requires **PowerShell 7 or later** (`pwsh`) on both Windows and Linux.
Windows PowerShell 5.1 cannot run it. Install PowerShell 7 through the
platform's normal package manager; it is a system prerequisite, not a pinned
StreamFind tool, and is not installed under `.streamfind/tools`.

On Windows, install it with WinGet:

```powershell
winget install --id Microsoft.PowerShell --source winget
```

Then open a new terminal and check `pwsh --version`. WinGet installs the
PowerShell package for the Windows user; StreamFind does not need an
administrator-managed copy. In VS Code, choose **Terminal: Select Default
Profile**, select the PowerShell 7 profile, and open a new integrated terminal.
The terminal must actually be running PowerShell 7; installing it does not
change already-open Windows PowerShell 5.1 sessions.

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
or change the machine's PATH. On Linux or WSL, install PowerShell 7 using the
package manager for that distribution, following Microsoft's
[PowerShell on Linux installation guide](https://learn.microsoft.com/en-us/powershell/scripting/install/linux-overview).
For Ubuntu, Microsoft's instructions configure the package source before
installing the `powershell` package. Then use the same script from a PowerShell
7 session, with the repository checked out in the Linux filesystem for native
Linux builds. The direct `pwsh -File` form works when you do not want to enter
the development shell.

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
sfdev setup --components data
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
override the managed tools and shared example-data root. Browser setup is only
needed for browser smoke checks; Playwright browsers are stored in `tools/playwright` under the
same managed tools root. On Ubuntu/Debian, native setup uses the system
package manager and needs an APT source that provides `libarrow-dev`; package
installation uses `sudo`.

### Release-distributed extension setup

Extracted runtime archives contain `dev/streamfind-dev-setup.ps1`,
`dev/StreamFindProvisioning.psm1`, `dev/toolchains.lock.json`, and
`dev/setup-manifest.json`. These files are self-contained and do not require a
repository checkout. They are the provisioning boundary used by the native
`streamfind-dev` dispatcher shipped in the same archive:

```powershell
pwsh -NoProfile -File dev/streamfind-dev-setup.ps1 setup -Target all `
  -Workspace "$HOME\Documents\streamfind-dev"
pwsh -NoProfile -File dev/streamfind-dev-setup.ps1 doctor -Json
```

The helper stores its cache under `$STREAMFIND_HOME/downloads`, uses exact
version and SHA-256 entries from `toolchains.lock.json`, retries downloads,
uses an exclusive setup lock, stages installs before replacing a destination,
and supports `-Offline` and `-DryRun`. A missing offline artifact reports the
expected cache path and digest. Node/npm setup runs `npm ci` only when a viewer
path with `package-lock.json` is supplied. Windows provisions MSYS2 UCRT64;
Ubuntu and Debian install native dependencies through `sudo apt-get`, so those
system packages are intentionally not represented as contents of
`$STREAMFIND_HOME`.

Python scripts always run with the repository `.venv`. `sfdev setup
--components python` installs and uses the pinned Python 3.11 runtime under
`%USERPROFILE%\.streamfind\tools\python` on Windows or
`~/.streamfind/tools/python` on Linux, then creates `.venv` in the checkout.
System Python installations and the Windows `py.exe` launcher are never used
for repository development. Re-running setup reuses a healthy managed `.venv`,
replaces a healthy `.venv` created from another Python installation, and gives
an actionable recovery command when the environment is incomplete.
The pin and platform archive checksums are maintained in
`scripts/build/python-version.txt` and `scripts/build/setup-python.ps1`.
`STREAMFIND_HOME` also relocates this managed Python. Project Python packages
remain in `.venv` and are never shared from the user tools directory.

`sfdev` also uses the pinned Node.js/npm installation under the same
`.streamfind` tools root for frontend commands. This keeps Node selection
aligned with the complete locked dependency tree rather than a broad major
version check against the system Node installation.

The release-distributed `streamfind-dev` setup has a separate `python` target
for packaged-build development. It keeps the pinned interpreter in
`$STREAMFIND_HOME/tools/python` and creates the virtual environment in the
selected workspace, for example:

```powershell
streamfind-dev setup --target python --workspace D:\streamfind-dev
```

This creates `D:\streamfind-dev\.venv`; it never places a virtual environment
inside the extracted package. `streamfind-dev setup --target all` provisions
the Python runtime and workspace environment together. The workspace `.venv`
is independent for each development workspace, while the managed interpreter
and verified download cache remain shared.

Package installation is intentionally opt-in and the environment remains
extensible. To install an initial requirements file, pass it explicitly:

```powershell
streamfind-dev setup --target python --workspace D:\streamfind-dev `
  --requirements D:\streamfind-dev\requirements.txt
```

The setup does not enforce or replace a lockfile, and developers may install
additional packages into the workspace environment as needed.

### Supported native toolchains

The Windows C++ build uses **MSYS2 MinGW-w64 UCRT64 GCC/G++, CMake, Ninja, and
Arrow development packages**. `sfdev` first uses an existing UCRT64 MSYS2
installation found through `PATH` or at `C:\msys64`; if unavailable, it uses
`%USERPROFILE%\.streamfind\tools\msys2`.
`STREAMFIND_MSYS2_ROOT` selects an explicit root, and `STREAMFIND_HOME` changes
the managed tools root. When no system installation is available, the
user-local `.streamfind/tools/msys2` installation does not require
administrator rights. Run `sfdev setup --components native` to install
MSYS2 and the required UCRT64 packages when missing. The installer version is
pinned in `scripts/build/msys2-version.txt`; package selection is defined in
`scripts/build/setup-msys2.ps1`. Setup persists the selected root for the
current Windows user so VS Code can use it; restart VS Code after initial
setup. Do not use MSVC or Visual Studio build tools for the streamfind C++ core
or bundled plugins.

Linux builds use GCC/G++, CMake, and Ninja. WSL Ubuntu is supported for Linux
release creation and validation. From Windows, use the checkout framework's
Linux release bridge instead of building on `/mnt/c`:

```powershell
sfdev release prepare --linux --distribution Ubuntu
sfdev release verify --linux --distribution Ubuntu
```

The bridge mirrors the current worktree into the selected distribution's
native home as `/home/<linux-user>/<worktree-name>`, runs the complete Linux
build, CTest, package, dependency, and extracted MCP checks there, and exports
only the verified archive and checksums back through `/mnt/c`. It resolves the
Linux user from that distribution's `$HOME`; no username or machine path is
hard-coded. Each platform has a separate build with its own deterministic
build tree; do not share one build directory across core and development
configurations,
compilers, or operating systems. The Linux scripts remove Windows-mounted executable paths
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

### Verify changes in the packaged app

The package in `tmp/packaged_build/` is the single app verification target.
Do not run a separate Vite app or build-tree service for UI or app checks.
For a frontend-only change, rebuild the frontend and update the app assets in
the existing package:

```powershell
sfdev package --frontend-only
```

For backend, worker, packaging, or full app changes, build and refresh the
whole package:

```powershell
sfdev package
```

Then open `tmp/packaged_build/bin/streamfind-app.exe` or use the package's
`streamfind.exe service start` command. The app and service run from that
package so the checked UI, backend, plugins, and runtime files all belong to
one build. `sfdev test browser` runs its smoke check against the packaged app
at `http://127.0.0.1:8787/`; pass `--url` when the package uses another port.
On Linux, use a release package for manual app verification; `tmp/packaged_build`
is reserved for the native Windows development machine.

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
For the browser smoke check, refresh and start the packaged app first:

```powershell
sfdev test browser
```

Large-data, mass-spectrometry, Raman, parser, and workflow checks are
development checks, not CTest tests. Keep them under `scripts/dev/` and use
the development-script runner described below. Example data is shared across
checkouts at `<user>/.streamfind/streamfind.data`; run
`sfdev setup --components data` to clone it on demand. Set
`STREAMFIND_HOME` to change the managed directory, or set
`STREAMFIND_EXAMPLE_DATA_ROOT` to use a specific data directory.

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
development scripts resolve the shared managed `streamfind.data` checkout and
download it when a data-backed check requires it; they may also accept
explicit paths. Use `script info` and the script's help to confirm them.

Use the JSON option where supported for automation. A dry run prints the
planned working directory, executable, arguments, and relevant environment
without running the command:

```powershell
sfdev script run validate_native_dependencies.py --dry-run -- --help
sfdev test backend --filter '^streamfind_statistics_unit$' --dry-run
```

Build, test, and script-run logs are written under `tmp/logs/`.

For reusable editable test projects, use the purpose-built workflow builders:

```powershell
sfdev workflows nta
sfdev workflows impurities
sfdev workflows quantification
```

They call the native MCP interface and require an already-running StreamFind
service. The commands validate and save a workflow in a new project under
`tmp/projects/`; they do not execute operations. Use
`sfdev help workflows` for `--project-name`, `--url`, and `--force` behavior,
and see [`scripts/dev/README.md`](dev/README.md) for the data and template
sources. These commands do not start or stop the service.

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

Do not run `sfdev package` from WSL. WSL is reserved for Linux release builds
and their extracted-package checks; those write to the native Linux mirror and
export only release artifacts, and never create or replace
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
streamfind external plugin workflow:

```powershell
sfdev release prepare
sfdev plugin list
sfdev plugin new ./tmp/scratch/example-plugin --plugin example_ping
sfdev plugin build ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
sfdev plugin test ./tmp/scratch/example-plugin
sfdev plugin stage ./tmp/scratch/example-plugin
sfdev plugin validate ./tmp/scratch/example-plugin --sdk <extracted-kit-path>
```

Replace `<extracted-kit-path>` with the matching runtime archive's extracted
root. The stage command installs the plugin under
`tmp/build/external-plugins/<source-key>/stage/plugins/custom/custom.plugin.<name>/`;
the validator uses that same canonical package layout. Add the stage's
`plugins/custom/` directory to the service's configured custom plugin roots and
enable the full `custom.plugin.<name>` ID before loading it. For external
plugin development from an installed package, use the embedded
`streamfind-dev` commands instead; they install versioned packages under
`<STREAMFIND_HOME>/extensions/plugins/`. `release prepare` writes its
versioned combined runtime and development package to `tmp/release-output/`.

## 7. Prepare a release

Release preparation requires a clean, committed checkout and uses the
canonical version in `VERSION`. The checkout framework can build the current
host release directly, or orchestrate a Linux release from Windows through a
native WSL worktree mirror. The release output folder is
`tmp/release-output/`.

```powershell
sfdev version show
sfdev release prepare
sfdev release verify
sfdev release prepare --linux --distribution Ubuntu
sfdev release verify --linux --distribution Ubuntu
```

The `--linux` commands, when run from Windows, synchronize the checkout into
`/home/<linux-user>/<worktree-name>` in the selected WSL distribution, execute
the Linux release script and tests there, and export the verified `.tgz` back
to `tmp/release-output/`. `release verify` checks local checksums and
smoke-tests the extracted archive. After the version tag has been pushed and
the release is ready to publish, use `sfdev release publish`; publishing requires
authenticated GitHub CLI (`gh`) and an existing remote version tag. See
[`AGENTS.md`](../AGENTS.md) for the guarded release and tag procedure.

The release archives are the distributable outputs. `tmp/packaged_build/` is
only the unpacked Windows manual-test package and is never a release asset.

## 8. A repeatable workflow for AI agents and automation

`sfdev` is the public orchestration layer over the repository's build, release,
and development scripts. Use it instead of inventing tool paths or composing
ad-hoc build commands. When a needed action is not exposed by `sfdev`, use the
owning documented script, and keep new development scripts under
`scripts/dev/`.

1. Confirm the checkout and its tool resolution with `sfdev env show`. Check
   only the relevant workflows with `sfdev doctor --workflow native`,
   `frontend`, or `scripts`. Do not install dependencies preemptively.
2. Read `sfdev help <command>` for prerequisites, exact option names, and
   destructive effects. For script work, discover the script ID with
   `sfdev script list`, inspect it with `sfdev script info PATH`, and check
   forwarded arguments with the script's own help.
3. If supported, preview with `--dry-run`. Confirm the resolved checkout,
   build tree, package/project destination, service endpoint, and port before
   running. A dry run only previews the plan; it is not a build, test, or
   service health check.
4. Run the narrowest useful action: a native target, CTest filter or label,
   frontend check, one development script, or one workflow builder. A workflow
   builder creates a validated project; an explicit workflow-run command is
   still needed to execute its operations.
5. Keep generated builds, packages, projects, data-backed checks, and logs in
   the current checkout's `tmp/`. Use only that checkout's outputs when
   working in a worktree. Follow the port allocation and process-ownership
   checks in `AGENTS.md` before browser or packaged-app validation.
6. Report the exact command, completion status, and relevant `tmp/logs/` path.
   Do not claim success if the command returned a nonzero exit code or only
   completed a dry run.

For orchestration, `--json` is available on supported inspection and
execution commands. It provides structured status and log-path information;
child command output is sent to stderr in JSON mode so stdout remains a JSON
status object. `--json` and `--dry-run` are command-specific; check
`sfdev help <command>` for the exact options. Script arguments are passed only
after `--`, while `sfdev` options such as `--dry-run` go before that separator.

When extending `sfdev`, update the parser, its built-in help, and this guide in
the same change. Keep build and release implementations in their owning
`scripts/build/` or `scripts/release/` directories; `sfdev` should compose or
invoke those implementations rather than duplicate them. Add a focused
example to `scripts/dev/README.md` when a new development helper is introduced.

## 9. Build, package, release, and cleanup locations

| Location | Purpose |
| --- | --- |
| `tmp/build/windows/core/` | Windows core build tree |
| `tmp/build/windows/devkit/` | Windows development configuration build tree used for SDK/package validation |
| `tmp/build/linux/core/` | Linux core build tree and Linux release build |
| `tmp/build/linux/devkit/` | Linux development configuration build tree used for SDK/package validation |
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
| `sfdev script list\|info\|run` | Discover and run development-stage scripts |
| `sfdev workflows nta\|impurities\|quantification` | Create validated editable workflow projects through MCP |
| `sfdev package` | Build or refresh the unpacked Windows development-verification package |
| `sfdev release prepare` / `sfdev plugin ...` | Build the combined runtime/development release or work on an external plugin |
| `sfdev version show\|bump` | Inspect or advance the canonical version |
| `sfdev release prepare\|verify\|publish` | Prepare, verify, or publish versioned release archives |
| `sfdev clean` | Remove the platform build tree or all temporary output |

Run `sfdev help` for the current command list and `sfdev help <command>` for
full syntax. The implementation in `scripts/sfdev.ps1` is authoritative when
this guide and command help differ.
