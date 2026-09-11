# streamfind Agent Instructions

## General Guidance

- Read and follow the applicable skill below before making changes in its area.
- Keep changes focused and preserve the existing architecture and package boundaries.
- Do not modify third-party or vendored source under `cpp/vendor/` unless explicitly requested.
- Run the narrowest relevant validation after each change and report concrete results.

## C++ Project Rules

- Do not create anonymous namespaces (`namespace {}`) in project C++ code.
- Use an explicit named internal namespace such as `streamfind::detail` for non-public helpers, or use a file-local `static` function where appropriate.
- This rule applies to `cpp/` and project-owned C++ code. Do not rewrite third-party or vendored source under `cpp/vendor/`.

## Rust Project Rules

- Put Rust tests in the owning crate's `tests/` directory. Do not add inline `#[cfg(test)] mod tests` modules to implementation files.
- Keep integration tests under the relevant crate, such as `rust/crates/core/tests/` or `rust/crates/cli/tests/`.

## C++/Rust Test Parity

- The official C++ and Rust suites must cover the same controlled test matrix:
  project lifecycle, shared conformance, MCP behavior, dependency smoke,
  mass-spec interface, and NTA interface.
- Each corresponding C++/Rust test must exercise the same behavior and use the
  same filename stem, with only the language extension differing. For example:
  `project_smoke.cpp` / `project_smoke.rs`, `mcp_smoke.cpp` /
  `mcp_smoke.rs`, and `mass_spec_interface_smoke.cpp` /
  `mass_spec_interface_smoke.rs`.
- When adding, removing, or materially changing an official test, apply the
  same change to its counterpart backend or document the deliberate exception
  in the test plan before proceeding.
- Before finalizing test changes, verify redundancy, necessity, and coverage
  across both backends; do not add a backend-specific duplicate merely to
  increase test counts.

## Legacy-Free Development

During the active streamfind refactor, the codebase must move toward the target architecture without accumulating legacy scaffolding.

### Required Rules

- Do not create a legacy fallback, compatibility shim, forwarding module/package, duplicate source tree, dual execution path, transitional adapter, or migration helper.
- Do not preserve an old API or build path because the replacement is incomplete. Implement the replacement at its intended boundary.
- Treat relocations as atomic: after a move, there must be one owning implementation path.
- Do not add feature flags that select old versus new behaviour.
- Repairing an existing package or build break is allowed only when it preserves current behaviour without adding a new compatibility layer.

### Deferred Boundaries

- Treat `bindings/r` as a preserved, functional package during the current C++/Python and Rust implementation work. Do not refactor it, redirect it, or add R migration helpers until those implementations are complete.
- Treat `integrations/cf-streamfind` the same way: defer public-Python integration work until the end-state C++/Python and Rust domain implementations are complete.

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
- Create `.venv` in the repository root if it does not exist.
- Install dependencies only into that environment.
- Prefer `python -m pip` through the selected interpreter.
- Verify the selected interpreter before running a script:
  - Windows: `.venv\Scripts\python.exe -c "import sys; print(sys.executable)"`
  - POSIX: `.venv/bin/python -c 'import sys; print(sys.executable)'`
- Do not use `python`, `python3`, or a system `pip` directly for repository work.

## Repository Scratch, Build, and Log Locations (`tmp/`)

All transient artifacts — scratch files, custom build trees, test outputs, temp
projects created during tests, logs, and any other disposable asset — belong in
the repository-local `tmp/` folder at the
checkout root (`<repo-root>/tmp/`). `tmp/` is gitignored
(`.gitignore` → `/tmp/`), removed by `scripts\build\clean-build-temp.cmd`, and
disposable by design: treat it as managed scratch, never as a place for
anything that must be committed.

- **Never** write repository-work temp files outside the repository (system
  `%TEMP%` / `TMP`, `AppData`, user home, `<tmp>`, `/tmp`, etc.).
- **Never** drop temp files, scratch, or ad-hoc build outputs in the
  repository root or in source directories (`cpp/`, `rust/`, `semantic/`,
  `docs/`, `bindings/`, `tests/`, ...). Root-level `log/` and `cache/` are
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
  `scripts\build\clean-build-temp.cmd` removes build/test artifacts and disposable
  scratch (`tmp/build/`, `tmp/projects/`, `tmp/scratch/`, plus the legacy
  `cpp/build/`, `rust/target/`, `log/`, `cache/` dirs). It **preserves**
  `tmp/logs/` by default so diagnostics survive a routine clean. Run
  `scripts\build\clean-build-temp.cmd --all` only to wipe legacy temporary
  scripts/logs; tracked scripts under `scripts/` are never removed.

## GitHub Releases

Release archives are not committed under a repository `releases/` directory.
The release builder writes temporary packages and checksums to
`tmp/release-output/`; the GitHub Release is the authoritative distribution
location.

When a version is ready:

1. Update and commit the C++/Rust version metadata and any release notes.
2. Run `scripts/release/release.ps1 -Version <version>` (add `-Linux` when Linux
   packages are required). This builds, tests, packages, and hashes the
   archives under `tmp/release-output/`.
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
cmake --build tmp\build\core-default --target streamfind_aggregate_catalogue
```

The native SDK catalogue generator is the authoritative projection path. Python/Jena
validation is no longer part of the tracked runtime build layout.

## R Package Native Build

Use this workflow for native C++ and Rcpp work in the StreamFind R package.

### Package Root

The R package root is `<repository-root>/bindings/r`. Run all R package commands with `bindings/r` as the working directory; do not treat the repository root as the R package root.

### Required Validation

After native or R changes, load the package with `devtools::load_all()`:

```powershell
Rscript -e "devtools::load_all()"
```

Run that command from `bindings/r`. This is the required package compilation and load check for this workflow. Do not substitute `R CMD check` for this routine unless the user explicitly requests a full CRAN-style check.

### Generated Files

Never manually edit generated or compiled package outputs, including:

- `src/RcppExports.cpp`
- `R/RcppExports.R`
- `src/**/*.o`
- `src/**/*.dll`
- generated `man/*.Rd` files

Use the project's R tooling to regenerate files when generation is explicitly required. `devtools::load_all()` must be used to compile and load the package; do not patch generated output to make a build pass.

### Native Change Workflow

1. Identify the package-relative native source under `bindings/r/src/`.
2. Make the smallest source change in the hand-authored C++ or R file.
3. Run `devtools::load_all()` from `bindings/r`.
4. Inspect the first compiler or loader error and fix the owning source.
5. Re-run `devtools::load_all()` until the package loads successfully.
6. Run the narrowest relevant R tests or development fixture after loading.

Keep the existing R package build authority in `bindings/r/src/Makevars` and `bindings/r/src/Makevars.win` until a later migration phase replaces it with the standalone core build. Do not move native code to `core/` as part of a routine compilation check.

### Failure Rules

- Do not delete or rewrite generated exports to bypass an error.
- Do not compile from the repository root.
- Do not introduce a second package root or compatibility copy.
- Do not claim validation passed unless `devtools::load_all()` completed.
