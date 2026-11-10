# streamfind Agent Instructions

## General Guidance

- Read and follow the applicable skill below before making changes in its area.
- Keep changes focused and preserve the existing architecture and package boundaries.
- Do not modify third-party or vendored source under `cpp/vendor/` unless explicitly requested.
- Run the narrowest relevant validation after each change and report concrete results.

## C++ Project Rules

- On Windows, build and package the C++ core and native plugins with the MSYS2 MinGW-w64 UCRT64 GCC stack: GCC/G++, CMake, Ninja, and Arrow development packages. Prefer an existing system MSYS2 installation; if unavailable, provision MSYS2 and its packages under `%USERPROFILE%\.streamfind\tools\msys2` (or `%STREAMFIND_HOME%\tools\msys2`). Resolve the root through `STREAMFIND_MSYS2_ROOT`; do not hard-code a machine-specific MSYS2 path. Do not use MSVC, Visual Studio environment initialization, `cl.exe`, or the MSVC linker for streamfind core/plugin builds or release distributions. Package matching MinGW runtime DLLs under the runtime vendor tree when required.
- Do not create anonymous namespaces (`namespace {}`) in project C++ code.
- Use an explicit named internal namespace such as `streamfind::detail` for non-public helpers, or use a file-local `static` function where appropriate.
- This rule applies to `cpp/` and project-owned C++ code. Do not rewrite third-party or vendored source under `cpp/vendor/`.

## Test Ownership and Separation

- `cpp/core/tests/` and `cpp/plugins/*/tests/` contain the official native test
  suite only. Tests in these directories must be C++ unit or contract tests,
  must be registered with CTest, and must run through the supported CMake/Ninja
  build. Do not add Python, notebook, workflow, or ad-hoc DuckDB tests there.
- There is no repository-root `tests/` test suite. Do not recreate one for
  plugin or mass-spectrometry testing. Plugin-specific C++ tests belong under
  the owning plugin's `cpp/plugins/<domain>/tests/` directory.
- Python tests, data-backed workflow checks, NTA/MassSpec integration runs, and
  disposable project validation are development checks. Keep them under
  `scripts/dev/` (normally `scripts/dev/cpp/`), invoke them with the repository
  `.venv`, and do not register them as official CTest tests.
- Official native tests may use only small backend-neutral fixtures kept next to
  their owning C++ test area. Mass-spectrometry and other large example data
  must come from the shared `streamfind.data` checkout at
  `<user>/.streamfind/streamfind.data` (or `$STREAMFIND_HOME/streamfind.data`).
  Use `sfdev setup --components data` to provision it on demand. Development
  scripts resolve the same checkout so worktrees and external plugin projects
  use one data location. `STREAMFIND_EXAMPLE_DATA_ROOT` may explicitly point
  to a different data directory.
- Build and release validation may invoke CTest from `scripts/build/`, while
  development-stage Python and workflow validation remains in `scripts/dev/`.

## Local-only Rust development

The `rust/` source tree and `rust_scripts/` helper tree are intentionally
ignored and are not part of the tracked C++ project or any release artifact.
They may be used locally for experiments, but do not add Rust source, tests,
Cargo metadata, or Rust scripts to commits. The tracked build, test, release,
and documentation workflows are C++-only.
## Legacy-Free Development

During the active streamfind refactor, the codebase must move toward the target architecture without accumulating legacy scaffolding.

### Required Rules

- Do not create a legacy fallback, compatibility shim, forwarding module/package, duplicate source tree, dual execution path, transitional adapter, or migration helper.
- Do not preserve an old API or build path because the replacement is incomplete. Implement the replacement at its intended boundary.
- Treat relocations as atomic: after a move, there must be one owning implementation path.
- Do not add feature flags that select old versus new behaviour.
- Repairing an existing package or build break is allowed only when it preserves current behaviour without adding a new compatibility layer.

### When Compatibility Is Truly Required

For a released, user-facing transition that genuinely requires compatibility or data migration:

1. Stop the routine refactor.
2. Request an explicit, separately scoped decision.
3. Document the supported versions, removal date, and tests.
4. Isolate the transition code from the target implementation.
5. Remove it when the approved transition window ends.

Never introduce such code speculatively.

## Repository Python Environment

Use the repository-local `.venv` for every Python script, test, formatter, or package command in this repository. Use the same environment for dependency installation.

- Never run repository Python code with the system interpreter.
- Use `.venv\Scripts\python.exe` on Windows and `.venv/bin/python` on POSIX.
- Create `.venv` in the repository root if it does not exist, using a
  compatible installed Python or the managed fallback from `.streamfind/tools/python`.
- Provision the fallback and create `.venv` with `sfdev setup --components python`.
- Install dependencies only into that environment.
- Prefer `python -m pip` through the selected interpreter.
- Verify the selected interpreter before running a script:
  - Windows: `.venv\Scripts\python.exe -c "import sys; print(sys.executable)"`
  - POSIX: `.venv/bin/python -c 'import sys; print(sys.executable)'`
- Do not use `python`, `python3`, or a system `pip` directly for repository work.

## User-Managed Development Tools

Install development tools that are missing from the machine under the current
user's `.streamfind/tools` directory. Python is the only additional standalone
development tool to provision there: prefer a compatible system Python, and
otherwise install the pinned Python runtime under `.streamfind/tools/python`.
Use that interpreter only to create the checkout's `.venv`; all repository
Python scripts and packages stay in `.venv`. On Windows, default to
`%USERPROFILE%\.streamfind\tools`; on Linux/WSL, default to the Linux user's
`~/.streamfind/tools`. Respect `STREAMFIND_HOME` when it is set. Prefer a
compatible system tool already on `PATH`; use the managed copy when the system
tool is unavailable. Keep repository package dependencies, such as
`frontend/node_modules`, in their project directory rather than copying them
into the user tool cache. Report the resolved tool path through `sfdev doctor`.

## Repository Scratch, Build, and Log Locations (`tmp/`)

All transient artifacts — scratch files, custom build trees, test outputs, temp
projects created during tests, logs, and any other disposable asset — belong in
the repository-local `tmp/` folder at the
checkout root (`<repo-root>/tmp/`). `tmp/` is gitignored
(`.gitignore` → `/tmp/`), removed by `scripts\clean\clean-repo-temp.cmd`, and
disposable by design: treat it as managed scratch, never as a place for
anything that must be committed.

- **Never** write repository-work temp files outside the repository (system
  `%TEMP%` / `TMP`, `AppData`, user home, `<tmp>`, `/tmp`, etc.).
- **Never** drop temp files, scratch, or ad-hoc build outputs in the
  repository root or in source directories (`cpp/`, `rust/`, `semantic/`,
  `docs/`, `tests/`, ...). Root-level `log/` and `cache/` are
  legacy locations that are being folded into `tmp/`; do not create new
  content there.

### Layout

- `scripts/build/` — tracked build and official-test scripts
- `scripts/release/` — tracked release and publishing scripts
- `scripts/dev/` — tracked development-stage data/parser/NTA scripts
- `tmp/build/` — custom/experimental build trees, ad-hoc binaries, staging areas
- `tmp/projects/` — DuckDB project files and fixtures created by tests or ad-hoc runs
- `tmp/logs/` — build, test, server, and session logs
- `tmp/scratch/` — anything else transient

### Rules

- Development-stage scripts and wrapper `.bat`/`.sh` files belong under the
  tracked `scripts/dev/` directory; build and release wrappers belong under
  `scripts/build/` and `scripts/release/`. Do not create them in generated
  `tmp/` directories or the system temp directory.
- Test code and ad-hoc runs that create temporary DuckDB projects or fixtures
  must place them under `tmp/projects/` (or another `tmp/`-anchored
  directory), never in `std::filesystem::temp_directory_path()` / system temp.
- Logs of builds, tests, servers, interactive sessions, and agent runs go to
  `tmp/logs/` with one file per run (or a run-specific subdirectory).
- Custom build trees created outside the canonical build presets go to
  `tmp/build/`.
- Housekeeping before committing on `dev_refactoring`:
  `scripts\clean\clean-repo-temp.cmd` removes generated build/test output,
  all contents of `tmp/`, legacy `build/`, `log/`, and `cache/` directories,
  and repository-owned Python caches and bytecode. It recreates only the empty
  `tmp/` anchor. It never removes tracked source or scripts, `.venv`,
  `node_modules`, or vendored source. Run it from any working directory before
  regenerating builds and packages.

### Native build, packaged-build, and release separation

Keep the three native distribution stages separate:

- `tmp/build/` contains four deterministic CMake/Ninja trees:
  `windows/core`, `windows/devkit`, `linux/core`, and `linux/devkit`. Core and
  developer-kit configurations must never share a build tree. Release scripts
  reuse the matching platform/core or platform/devkit tree and write archives
  only to `tmp/release-output/`.
- `tmp/packaged_build/` is the unpacked package for manual testing on the
  development machine's native OS. On this Windows checkout, create or refresh
  the Windows package from native Windows PowerShell with:

  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\build\cpp\package-cpp.ps1
  ```

  Do not run this package workflow through WSL. WSL is reserved for Linux
  runtime and developer-kit release builds and their extracted-package tests;
  those workflows must not create or replace `tmp/packaged_build/`.

  For frontend-only changes, refresh the existing package without rebuilding
  native code with:

  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\build\cpp\update-packaged-frontend.ps1
  ```


  The script rebuilds `frontend/dist` and replaces only the package `app/`
  directory. If the package does not exist yet, create it with
  `package-cpp.ps1` first. The full package script also rebuilds the frontend
  automatically, so a clean package always contains the current UI.

  Use `-SkipTests` only when a test run has already been completed for the
  same build, and use `-CleanBuild` when changing toolchains or diagnosing a
  stale package. This directory is not archived, committed, tagged, or used
  as a GitHub release asset.
- `tmp/release-output/` contains versioned ZIP/TGZ archives and checksums
  produced only by the release scripts. A release build must use the
  canonical `VERSION`, run its required tests and package validation, and be
  published through the guarded release workflow described below.

The packaged build must be generated from the current frontend and native
sources in one run. Do not patch files inside `tmp/packaged_build/`; fix the
owning source or packaging rule and regenerate it. When validating the app,
run the executable from `tmp/packaged_build/bin/` so package-relative service,
plugin, catalogue, frontend, and runtime resolution is exercised. Keep
project databases and result-store sidecars used for testing under
`tmp/projects/` unless the test explicitly targets a user-provided project.

### Worktree sandbox testing

Treat every Git worktree as an isolated development and test sandbox. Use only
that worktree's source, build trees, package, release output, test projects,
result stores, and logs. Never point a worktree test at another checkout's
`tmp/` outputs, and never overwrite the main checkout's `tmp/packaged_build/`
or release files from a worktree.

- Assign each simultaneously tested worktree a unique service/Vite port pair.
  Keep the primary checkout defaults (`8790` for the development service,
  `5173` for Vite) and packaged service default (`8787`) reserved. Allocate
  worktree slots from service ports `8791`–`8890` and Vite ports `5174`–`5273`
  using the same slot number: slot 1 is service `8791` + Vite `5174`, slot 2
  is `8792` + `5175`, and so on. Check that both ports are free and not assigned
  to another active worktree before starting; never silently reuse another
  worktree's pair.
- For a development app, start it from the target worktree and pass both
  assigned ports, for example:

  ```powershell
  sfdev run app --url http://127.0.0.1:8791 --vite-port 5174
  ```

  `sfdev run frontend` accepts the same options when connecting to an already
  running service. The service URL selects the service port, and `--vite-port`
  selects the Vite listener. Direct use of `frontend/tools/dev/start-dev.mjs`
  can set `STREAMFIND_SERVICE_URL` and `STREAMFIND_VITE_PORT` instead.
- Before browser testing, open the matching Vite URL (for example,
  `http://127.0.0.1:5174/`) and verify both the Vite and service processes
  belong to the intended worktree. Do not mistake another checkout's UI or
  service for the worktree under test.
- For a packaged build or extracted release package, run that package's
  `bin/streamfind-server[.exe]` with the worktree's assigned service port as
  its port argument, from that package's `bin/` directory, then test that
  endpoint (for example, `http://127.0.0.1:8791/`). The server accepts a port
  override, so package-relative resource resolution is tested without changing
  packaged configuration. Use the package built or extracted inside the
  current worktree's `tmp/`.
  To test the packaged app launcher on that same assigned port, first run
  `bin/streamfind[.exe] service start --port <assigned-service-port>`, then
  launch `bin/streamfind-app[.exe]`; the app attaches to the active service's
  discovered endpoint. Stop that service when the test is finished.
- Keep any temporary project database and its sibling `.data` result store in
  the current worktree's `tmp/projects/`. Do not open a user's live project for
  routine worktree validation.
- Record test server output under the worktree's `tmp/logs/`. Stop only the
  server process started for that test (Ctrl+C for a foreground server), then
  verify that the assigned ports are released. Never stop a service merely
  because it is using one of the worktree test ports unless its executable path
  confirms it belongs to the current worktree and test.
- Before reporting a browser or release test as successful, verify the service
  endpoint and running executable path belong to the intended worktree/package.
  This prevents false results from a main-checkout service, stale package, or
  another worktree.

## Plan Management

- Keep at most one active Markdown plan directly under `.plans/`.
- Move completed plans to `.plans/completed/` and superseded, paused, or
  otherwise inactive plans to `.plans/stale/` before starting another plan.
- Keep completion notes and provenance in the relevant plan document. Do not
  create a separate `.plans/provenance/` folder.

## GitHub Releases

Release archives are not committed under a repository `releases/` directory.
The release builder writes temporary packages and checksums to
`tmp/release-output/`; the GitHub Release is the authoritative distribution
location.

When a version is ready:

1. Update the C++ `VERSION` file and any release notes.
2. Run `scripts/release/cpp/release-cpp.ps1 -Version <version>` (add `-Linux` when Linux
   packages are required). This builds, tests, packages, and hashes the
   archives under `tmp/release-output`.
3. Create and push the annotated tag to the official repository:

   ```powershell
   git tag -a v<version> -m "streamfind <version>"
   git push upstream dev_refactoring
   git push upstream v<version>
   ```

4. Publish the generated assets with the guarded helper:

   ```powershell
   scripts/release/publish-release.ps1 -Version <version>
   ```

   The helper requires authenticated `gh`, verifies every archive against
   `sha256sums.txt`, and refuses to overwrite an existing release by default.

To replace assets in an existing release intentionally, rebuild the same
version and pass the explicit replacement switch:

```powershell
scripts/release/publish-release.ps1 -Version <version> -Replace
```

This uses `gh release upload --clobber`. Do not move an already-published tag
for a materially different code revision; create a new version instead.

The current official repository is `ricardo-cunha/streamfind`. Use
`-Repository <owner>/<repo>` only when publishing to a deliberately different
repository. GitHub Release assets are separate from commits and do not update
automatically when `scripts/release/release.ps1` is run.

## Turtle Ontology Formatting

Use this style for every edit to `cpp/core/semantic/**/*.ttl` and
`cpp/plugins/*/semantic/**/*.ttl`.

### Syntax

- Keep the file as Turtle, not TriG. Do not add graph-wrapper braces around the document.
- Keep the existing `@prefix` declarations at the top of the file.
- Use the Turtle `a` shorthand for `rdf:type`; do not expand it to the full RDF type IRI.
- Use prefixed names such as `sf:Operation`, `sfcore:mass_spec`, and `skos:prefLabel` instead of full IRIs when a prefix exists.
- End declarations with `.` and use `;` for continued predicates.
- Use commas only for compact lists of objects with the same predicate.

### Layout

- Put one named resource per block.
- Indent predicates four spaces from the subject and continued objects eight spaces.
- Put long predicate lists on separate lines.
- Keep labels and definitions adjacent to the resource type.
- Keep `skos:inScheme sfcore:scheme` on catalogue concepts that belong to the shared scheme.
- Keep domain declarations in `cpp/plugins/<domain>/semantic/` and generic declarations in `cpp/core/semantic/`.
- Keep table columns and result properties explicit; do not hide schemas in comments or implementation code.

Example:

```turtle
sfcore:example
    a sf:Operation ;
    skos:inScheme sfcore:scheme ;
    sf:operationId "example" ;
    skos:prefLabel "Example operation" ;
    skos:definition "A concise description of the operation." ;
    sf:returns sfcore:exampleResult ;
    sf:mutatesProject false .
```

### Ontology Validation

Before validation, use the repository formatter when ontology formatting is
needed:

```powershell
& ".venv\Scripts\python.exe" scripts\python-utils\format_ttl.py
```

Run it from the repository root. The script formats every `*.ttl` file under
`cpp/core/semantic/` and `cpp/plugins/*/semantic/`; inspect the resulting diff and avoid using it when an
unrelated formatting-only rewrite would obscure the intended change.

After ontology edits, run from the repository root:

```powershell
cmake --build tmp\build\windows\core --target streamfind_aggregate_catalogue
```

The native SDK catalogue generator is the authoritative projection path. Python/Jena
validation is no longer part of the tracked runtime build layout.
