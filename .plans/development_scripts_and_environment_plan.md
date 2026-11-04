# Development scripts, environment, and plugin expansion plan

## Purpose and status

The developer CLI, bundled version generation, and external plugin generation
are implemented. Windows validation covers browser startup and shutdown,
the extracted runtime, the installed developer-kit consumer, and the
generated native plugin's build, CTest, staging, and structural validation.
Ubuntu under WSL covers native configure/build/test and extracted runtime
verification. Remaining acceptance checks include a generated full plugin
running and rendering in the live host, Linux app lifecycle and SDK packaging,
and fresh cross-platform release preparation with matching provenance.

This is an implementation plan. Commands marked as proposed below are the intended interface, not commands already available in the checkout.

Give developers and agents a clear understanding of how streamfind is configured, compiled, tested, run, packaged, and released. Every automated step must have an inspectable manual equivalent. Use the current `dev_refactoring` branch as the implementation baseline.

Preserve the current ownership boundaries: CMake owns native targets, npm owns frontend tasks, the SDK owns plugin contracts, and domain plugins own their implementations and semantics. Refactor existing scripts into one owning implementation per operation. Do not introduce alternate implementations or compatibility wrappers for retired workflows. Preserve `bindings/r` and vendored third-party code.

## 1. Explain the development lifecycle

The documentation must teach the following stages separately, including their inputs, outputs, prerequisite tools, working directory, and commands:

1. **Setup:** install or select tools, install repository dependencies, and record resolved paths.
2. **Configure:** resolve the platform compiler and choose Debug or Release; create the CMake build tree.
3. **Build:** compile selected native targets or produce frontend assets. Building does not imply running tests.
4. **Test:** run selected existing native/frontend tests, browser smoke checks, or explicitly requested data-backed development checks.
5. **Run:** start the selected service/worker pair and frontend, or connect the frontend to an explicitly selected existing service.
6. **Package:** produce the disposable application package for manual testing.
7. **Release:** build and validate versioned archives, review them, then explicitly tag/push/publish.
8. **Clean:** remove a selected class of generated artifacts, with the deletion list visible beforehand.

Include recipes for UI-only work, a native core change, a plugin algorithm change, a semantic catalogue change, a frontend viewer change, debugging one failing test, and preparing a release. Each recipe states what must be rebuilt and which checks provide useful coverage.

## 2. Software inventory and setup transparency

Maintain a tracked dependency manifest describing purpose, supported platforms, required version or constraints, install source, checksum where available, install destination, and workflows using each dependency. Record concrete installed versions and paths in machine state. Do not silently substitute a different tool because it happens to be first on PATH.

| Area | Required software or assets | Ownership and setup |
| --- | --- | --- |
| CLI and source control | PowerShell 7, Git | Explain bootstrap prerequisites before the CLI is available; report executable versions and paths. |
| Windows native toolchain | MSYS2 UCRT64 GCC/G++, binutils/resource compiler, CMake with CTest/CPack, Ninja | Use one UCRT64 installation, including matching runtime libraries. Never select MSVC. |
| Linux native toolchain | GCC/G++, binutils, CMake with CTest/CPack, Ninja, platform C/C++ headers and runtime | Record the host toolchain and required system packages. Explain privileges and distribution support for installation. |
| Native external libraries | Apache Arrow; Abseil where required by the Linux configuration; their package dependency closure | Select compiler-compatible libraries through explicit CMake paths. Record package versions and source. |
| Vendored native dependencies | DuckDB platform headers/libraries and repository-owned build definitions for vendored libraries such as Open Babel, zlib, zstd, and IsoSpec | Inventory the actual CMake dependency graph. Keep existing vendored artifacts in place and validate their platform compatibility and packaging. |
| Semantic catalogue tooling | Native SDK catalogue generator and plugin validator; vendored Apache Jena and managed Java 21 used by the current SDK validation path | Setup provisions the Java required by catalogue generation. The native SDK command remains authoritative; do not add a parallel Python/Jena projection pipeline. |
| Python development tools | A supported Python interpreter, venv/pip, and declared packages required by selected scripts | Bootstrap Python may create the environment; all repository Python execution and package installation use the repository .venv. Verify the interpreter path. |
| Frontend build | Node.js, npm, React, React DOM, TypeScript, Vite, React Vite plugin | Pin a compatible Node/npm pair and install the complete package-lock.json dependency graph with npm ci. |
| Frontend libraries | D3, Plotly, TanStack React Virtual, and associated typings | Installed by the frontend lockfile, not by global npm installs. |
| Frontend unit tests | Vitest, jsdom, Testing Library React and jest-dom | Run the repository npm test scripts; distinguish watch mode from a single run. |
| Frontend quality checks | ESLint, TypeScript ESLint, React lint plugins, Prettier | Separate lint/format checking from commands that rewrite files. |
| Browser checks | Playwright, its matching Chromium build, and Linux browser system libraries | Provision the browser explicitly; report the browser path and server prerequisites for smoke checks. |
| Archive and dependency checks | ZIP/TGZ support, tar, SHA-256 tooling, and platform binary-inspection tools used by the current dependency validator | Report tools required by the selected package/release lane. |
| Publishing | GitHub CLI and authenticated access to the release repository | Needed for explicit publication, not local builds or archive preparation. |
| Optional domain workflows | MetFrag/fragmenter tools and workflow-specific dependencies; external example/vendor datasets | Install only when the corresponding workflow is selected. Detect data paths and report missing assets. |

Documentation groups dependencies by purpose. Derive actual version constraints from CMake and npm requirements, including the CMake preset schema and Node engine requirements in the lockfile. Do not claim a version is supported without checking these constraints.

Correct existing misleading documentation: DuckDB currently consumes vendored platform libraries, rather than being built from source by this workflow; Java is used by the current SDK catalogue validation path. Keep DuckDB's existing vendored layout.

Track direct dependencies and the authority for each transitive closure, such as the npm lockfile or platform package manager. Capture the resolved transitive inventory in machine-specific reports where the tooling supports it. Avoid a second hand-maintained list of transitive versions. Separate bootstrap requirements, build requirements, optional development checks, and runtime dependencies.

### Managed home and repository layout

| Location | Purpose | Cleanup policy |
| --- | --- | --- |
| STREAMFIND_HOME, default ~/.streamfind | Shared managed tools and durable development assets | Never removed by ordinary repository clean commands. |
| ~/.streamfind/tools/ | Managed tool installations, including existing Java and domain-tool locations | Explicit setup/update operations; select exact versions. |
| ~/.streamfind/development/ | Machine setup inventory, dependency resolution records, and versioned installed developer kits | Preserve across repository cleanup. Identify SDK version, platform, architecture, and compiler provenance. |
| Repository .venv and frontend/node_modules | Checkout-specific Python and frontend dependencies | Preserve during ordinary cleanup; reinstall explicitly. |
| Repository cpp/vendor/ | Existing vendored source, headers, and platform libraries | Preserve; do not duplicate or relocate as part of this plan. |
| Repository tmp/build/ | Platform build trees, package staging, external-plugin build trees | Disposable; clean the platform build tree explicitly. |
| Repository tmp/packaged_build/ | Disposable Windows application package for manual validation | Regenerate through packaging scripts. |
| Repository tmp/release-output/ | Versioned archives and checksums | Remove only through an explicit release-output or complete-temp clean. |
| Repository tmp/projects/, tmp/logs/, tmp/scratch/ | Test projects, run logs, temporary downloads/extraction, scratch | Disposable with explicit cleanup scope. |
| Sibling streamfind.data | Large example data | Detect or accept an override; never download/delete implicitly. |

New managed tools and durable assets default to the managed home. Existing system tools may be selected explicitly and recorded rather than copied. Linux system packages that cannot be installed as portable tools remain machine-provided dependencies; expose this exception in the inventory. Temporary setup downloads and logs remain under repository tmp/.

Setup must be repeatable, verify downloads, and avoid implicit upgrades. Record platform package versions even when package managers cannot guarantee historical package availability; report any inability to reproduce a requested version rather than claiming identical environments. A failed install must not mark a partial tool directory as ready.

Provide `env show`, `env shell`, and `doctor`:
- `env show` prints the home, checkout, build directory, tools, versions, paths, resolution source, compiler/library match, data roots, and relevant environment variables. Offer JSON output for agents.
- `env shell` opens a development shell with the same resolved environment used by the CLI, enabling direct CMake, CTest, npm, and Python commands.
- `doctor` checks prerequisites for a selected workflow, reports missing/mismatched components, and prints the exact setup command to fix them. It does not install tools.
- Resolve explicit command overrides first, recorded setup configuration second, and documented defaults last. Display the chosen source. Keep environment changes process-local by default.
- Explain bootstrap installation of PowerShell, Git, and Python, then setup phases for native, frontend, browser, and optional workflow dependencies. Do not make build/test commands silently install missing software.

## 3. Proposed developer CLI and manual controls

Use `scripts/sfdev.ps1` as the canonical PowerShell 7 entry point. From the checkout root, invoke it as `pwsh -NoProfile -File scripts/sfdev.ps1 help` on Windows or Linux. Inside the development shell, expose `sfdev` as a session-local command bound to that checkout's absolute script path. It must work after changing directories and require no profile or PATH edits. Use the full invocation in CI examples.

The installed `streamfind` runtime CLI remains the entry point for service control. The repository developer CLI orchestrates build and development tasks. Keep Windows and Linux execution native to their respective toolchains. For WSL, resolve Linux tools and package paths inside WSL and exclude inherited Windows tool directories from native discovery.

| Proposed command | Developer control |
| --- | --- |
| sfdev help / sfdev help <command> | Purpose, defaults, options, prerequisites, outputs, and manual equivalents. |
| sfdev env show --json / sfdev env shell | Inspect the resolved environment or enter a shell with checkout-local commands. |
| sfdev doctor | Check prerequisites and print remediation for the selected workflow. |
| sfdev setup --components native,frontend,browser | Select dependency groups; show the install plan and resolved destinations. |
| sfdev configure --config Debug | Configure only; discover all bundled plugins. |
| sfdev build backend --target <target> --jobs 4 | Incremental build of selected targets; no implicit test or clean. |
| sfdev build frontend | Run the owning npm production-build task. |
| sfdev build all | Explicitly build backend and frontend. |
| sfdev test backend --list | Show CTest tests without running them. |
| sfdev test backend --filter <regex> --label <label> --jobs 2 | Select tests; expose timeout, verbose output, and rerun-failed controls. |
| sfdev test frontend -- <test arguments> | Single Vitest run by default; explicit watch option; retain argument boundaries. |
| sfdev test browser --url <url> | Run the existing browser smoke workflow against the stated URL. |
| sfdev check frontend | Type/build validation, lint, and format checks; report individual outcomes. |
| sfdev run backend / sfdev run frontend / sfdev run app | Choose processes, and service URL; show startup paths and ownership. |
| sfdev package / sfdev package --frontend-only | Produce or refresh the disposable package using owning packaging workflows. |
| sfdev clean --scope build --dry-run | List targets before deleting the platform build tree. |
| sfdev clean --scope all-temp --dry-run | Show complete scratch/output cleanup, including projects and release archives. |

Every mutating command supports `--dry-run`. Construct a shared execution plan and render it or execute it, so both paths agree on executable paths, argument arrays, working directories, environment changes, outputs, and deletions. Dry-run must avoid creating logs or directories, invoking mutating subprocesses, and making network requests. Redact secrets in both modes. In JSON mode, reserve stdout for one structured response and stream subprocess output to stderr and the run log. Preserve child exit codes.

Keep configure, build, and test independently invokable. A command with unmet prerequisites explains the required preceding step instead of secretly rebuilding or discarding a build tree. Preserve explicit convenience workflows such as package/release, but show their full stage list.

Use one native development build directory per platform and discover all bundled plugins. Keep SDK and release staging separate. Reject incompatible cache reuse with an actionable cleanup instruction.

Document exact manual equivalents with working directories and environment. For example, `cmake --preset ... -S cpp`, `cmake --build <resolved-tree> --target ... --parallel ...`, `ctest --test-dir <resolved-tree> --output-on-failure -R ...`, and `npm run test:run -- ...` in frontend/. Copyable examples must use paths printed by the environment commands.

The run workflow shows which service/worker binaries are paired, whether an existing service is reused, and which processes it will stop on exit. Backend changes require rebuild and restart; frontend changes use Vite HMR. Extend the owning frontend launcher for Linux rather than creating another launcher; current combined startup is Windows-specific.

Cleanup uses explicit allowlisted scopes, validates resolved repository paths, handles symlinks/junctions without following them outside the target tree, and fails clearly on locked artifacts. Default cleanup must not delete managed tools, .venv, node_modules, or user datasets. Full-temp cleanup explicitly includes temporary projects and release archives.

## 4. Development scripts without registration

Use `sfdev script list`, `sfdev script info <relative-path>`, and `sfdev script run <relative-path> -- <arguments>`.

- Discover .ps1, .py, .sh, and Windows .cmd/.bat entry points recursively under scripts/dev/. Include extensions in identifiers to avoid collisions. Sort deterministically and indicate host/interpreter availability.
- Keep helper modules in a private directory such as scripts/dev/_lib/ and exclude private directories/files beginning with an underscore. Move existing shared helpers and update imports atomically. Do not require a central registration list or metadata file.
- Resolve paths canonically, reject traversal and links escaping scripts/dev/, and invoke executables with argument arrays rather than shell-generated command strings.
- Run PowerShell through the resolved PowerShell, Python through the verified repository .venv, shell scripts through the selected Bash, and Windows wrappers only on Windows.
- Set the working directory to the repository root, provide the selected environment and tmp paths, stream output, log each run, and preserve its exit status.
- `dev info` displays path, interpreter, source help/comment text where available, and the command that would execute. Do not run a script merely to discover its help.
- Audit existing development entry points for hardcoded paths, external writes, and argument conventions; document dataset modifications explicitly. Running a discovered script remains an explicit action, not part of list, setup, or ordinary testing.

Official native tests stay C++/CTest under their owning core/plugin test directories. Data-backed, Python, and scripts-framework validation remains development work under scripts/dev/. No repository-root tests suite is introduced.

## 5. Backend and frontend plugin expansion

### Existing framework and changes needed

Backend domains under cpp/plugins/ are discovered by CMake when they have CMakeLists.txt and define `streamfind_<domain>_plugin`. Each plugin owns its manifest, native implementation, semantic catalogue, tests, staging, and install rules. Runtime loading also requires explicit plugin roots and enabled IDs; file presence alone does not activate a native plugin.

Frontend plugins use the existing `FrontendPlugin` contract: a manifest and `setup(api)`, with viewer/visualization registration and the host API client. Bundled entries are discovered from frontend/src/plugins/, and build tooling emits plugins.json from manifests and Vite output. Preserve the existing API version checks, origin restrictions, and duplicate handling.

The current generator is incomplete: its native entry point is a placeholder, its SDK compatibility range is hardcoded, and its frontend interface does not implement the host contract. Replace these placeholders in the owning generator with usable examples before describing generated plugins as supported.

### Proposed plugin commands and workflow

| Proposed command | Result |
| --- | --- |
| sfdev plugin list | List discovered backend/frontend plugins, domain/ID mappings, selected build state, and runtime activation requirements. |
| sfdev plugin new <destination> --kind backend\|frontend\|full --domain <id> | Generate a minimal working example using the real SDK/frontend contracts; refuse to overwrite a nonempty destination. |
| sfdev sdk build | Build/install the developer kit through its existing owning workflow. |
| sfdev plugin build <path> --sdk <installed-prefix> | Build an external backend plugin against a selected compatible kit, or frontend plugin against the exported frontend contract. |
| sfdev plugin test <path> | Run the plugin's native CTest or frontend tests, plus explicit contract validation. |
| sfdev plugin validate <path> | Validate manifest, API/ABI compatibility, catalogue, library/assets, and declared runtime requirements. |
| sfdev plugin stage <path> | Stage a disposable development package under tmp/build/ and print explicit activation instructions. |

Document two complete tutorials: adding a bundled domain to this repository, and creating an external plugin project against an installed developer kit. A temporary tutorial project goes under tmp/scratch/; durable external source goes to a user-selected project directory. Generated native tests belong to their owning plugin project, never a root StreamFind tests directory.

The backend example must export the actual versioned ABI entry point, register one small operation, provide explicit Turtle input/output contracts, build its catalogue through the native SDK generator, and load in the host. Derive compatibility from authoritative SDK definitions; the current ABI is 1.1, but tooling must not duplicate that constant.

The frontend example must implement the real manifest/setup contract and register one viewer or visualization for a declared artifact contract. Consume shared host types and API services without copying schemas or private application modules. Add a supported export/build boundary for external frontend authors where the current workspace-only types are insufficient; keep the actual interface owned by the existing frontend framework. Ship a browser-loadable entry and CSS where used, with its runtime manifest.

For a full plugin, demonstrate one backend operation publishing a typed result through the service and a frontend viewer rendering it through the host API. Validate domain IDs, result contracts, frontend API version, native ABI, SDK compatibility, and package dependency closure. Do not assume backend folder names, frontend folder names, and public IDs always have identical spelling.

Keep discovery and activation distinct: development-script discovery requires no registration; native plugin activation still follows the runtime allowlist and frontend loading follows existing manifest/origin rules. Stage only into disposable development outputs and rebuild owning sources to refresh the application package.

Document debugging: missing target, missing catalogue, disabled plugin, incompatible ABI/API, unresolved DLL/SO, frontend module import failure, unmatched artifact contract, and duplicate registration. Each diagnosis should identify the responsible source or manifest and the next manual command.

## 6. Version, packaging, and release controls

- `sfdev version show` reads VERSION; `sfdev version bump patch|minor|major --dry-run` previews deterministic arithmetic, and the corresponding execution updates VERSION. Reject unsupported prerelease forms with an explanation; do not infer intent from commit history.
- Audit version consumers and identify which values represent application, plugin, ABI, or frontend API versions. Do not mechanically overwrite independently versioned contracts or the preserved R package.
- `sfdev package` builds current frontend and native sources and validates the disposable package. `--frontend-only` uses the existing refresh workflow and requires an existing complete package.
- `sfdev release prepare` uses the owning Windows and Linux release scripts with separate Release build trees. Run required native and packaged checks, record frontend check results, and emit archives/checksums plus version, commit, toolchain, and plugin-selection provenance. Require a clean committed source state for publishable artifacts. Use a version-scoped staging area under `tmp/release-output/` for verification, and generate the final checksum list only from the selected version's validated archives.
- `sfdev release verify` checks archives, checksums, extracted runtime, plugin payloads, dependency closure, and packaged MCP startup. Failures identify the failed stage and its log.
- Keep tagging, pushing, and `sfdev release publish` explicit and separate from preparation. Target the official repository and documented dev branch. Reuse the guarded publishing authority; do not overwrite releases or move tags by default.
- Do not expose an unchecked release test-skip path. Any permitted package test reuse must refer to evidence for the same build and inputs.
- Explain which commands delete/recreate staging directories and which artifacts a developer should inspect before publication.

## 7. Implementation sequence and acceptance

1. **Baseline and inventory.** Map `scripts/build/cpp/`, `scripts/release/cpp/`, `scripts/clean/clean-repo-temp.cmd`, `cpp/CMakePresets.json`, and `frontend/package.json` to their owning stages. Document the manual commands and capture one working Windows and Linux build. Record which checks exist and which need implementation.
2. **Read-only CLI first.** Deliver help, build directory resolution, `env show`, and `doctor` before adding installers. Verify tool paths, resolution precedence, and machine-readable output on Windows and Linux. Then add component setup and `env shell`. Move any automatic tool provisioning out of the build path, including managed Java installation currently reachable through SDK semantic validation. Prove that catalogue generation succeeds offline after setup.
3. **Lifecycle execution.** Add configure, build, test, run, package, and scoped clean using the existing CMake/npm and script owners. Keep the existing clean command's complete-temp behavior documented. Exercise configure → incremental target build → filtered test on both platforms. Verify run ownership by starting and stopping a test-owned service and confirming that a separately started service stays available.
4. **Development scripts.** Add discovery, info, and explicit execution. Move helpers and update all references in the same change. Verify argument forwarding with a harmless nested script, interpreter failures, traversal rejection, and private helper exclusion.
5. **Plugin expansion.** Update `scripts/dev/plugin_generator.py` and validate a generated backend plugin against the installed kit. Then establish the external frontend contract and exercise a generated viewer through the real loader. Validate the full example only after both independent examples work.
6. **Version and release.** Resolve bundled manifest and descriptor version duplication before exposing the version bump command. Prepare and verify both archives locally using `release-cpp.ps1` for Windows and `release-cpp-linux.sh` for Linux. Test mismatch and corruption handling with disposable fixtures. Verify the guarded publish command through dry-run and keep publication a separate explicit action.

Each phase is independently reviewable. Update the lifecycle guide alongside the owning implementation and mark commands as available only after their normal entry points pass validation. Keep cross-platform CLI fixtures under `scripts/dev/` and all disposable outputs under `tmp/`.

Acceptance checks must demonstrate:
- A developer can inspect the machine configuration, configure all bundled plugins, build one target, list/select one test, and reproduce the same steps manually from the printed commands.
- Setup is repeatable; alternate STREAMFIND_HOME works; missing/mismatched tools fail clearly; downloads are verified; builds do not install dependencies.
- Dry-run performs no work, argument quoting survives spaces/metacharacters, logs capture failures, and agent-facing JSON corresponds to actual results.
- Dev discovery handles nested paths and unavailable interpreters, excludes helpers, and rejects traversal/escaping links.
- Cleanup respects its selected scope and preserves shared tools, dependency environments, vendored content, and external data.
- CTest remains the official native suite; frontend unit/lint/build checks and browser smoke execute through their owning tasks; data checks remain opt-in.
- Generated backend and frontend plugins build and load; a full plugin renders its backend artifact; incompatible manifests and dependencies fail with actionable diagnostics.
- Bundled plugins still discover/package correctly, external plugins consume the installed kit, and runtime releases remain free of developer-kit contents.
- Version bump previews and changes agree; release validation rejects mismatches, stale evidence, or corrupt assets. Validation never publishes a release.

Keep implementation validation proportional to each phase. This document update itself requires only content/diff review; no build, dependency installation, service startup, or publication is part of saving the plan.
