# Project containers with the `.sf` format

**Status:** Complete (reviewed 2026-11-18).  
**Branch:** `dev_refactoring`  
**Scope:** Replace the split project database and sibling result-store directory with one user-visible `.sf` project folder.

> Implemented and verified on Windows and Linux. Detailed commands and outcomes are recorded under each phase.

## Goal

Make a StreamFind project a single recognizable, movable, and backup-ready folder. The service creates and owns the container; the app and MCP request creation and opening through the service using a project-folder path.

The target on-disk layout is:

```text
river-screening.sf/
├── project.duckdb
└── data/
    ├── store.json
    ├── results/
    └── runs/
```

The folder is the project boundary. `project.duckdb` stores workflow, project metadata, execution history, and the result inventory. `data/` stores result payloads and staging/run data. The result store must be derived from the container path and must not be kept as a sibling named `*.data`.

External source files (for example, mzML files) remain external inputs referenced by workflow parameters. Moving a project container does not move those source files or promise that their recorded paths remain accessible.

## Decisions and constraints

- The public project identifier is `project_path`, referring to the `.sf` directory. Public create/open/copy requests, responses, events, app state, and MCP interfaces use this value consistently.
- The database filename is fixed as `project.duckdb`. The service resolves it internally; callers do not select or address it directly.
- A project folder name must end in `.sf`. Names may contain spaces, Unicode, and additional dots before the extension. Path comparison must use platform-appropriate canonicalization and must handle equivalent paths consistently.
- Creation is atomic from the caller's perspective: a ready project is returned only after its directory, database, store manifest, and initial metadata are valid. Partial output from a failed attempt is cleaned only if that attempt created it.
- One service instance owns an open project container at a time. Same-service repeated opens may reuse the existing session. A different service instance must get a clear busy/ownership error until the owner closes it.
- Each container and its result store have matching stable identities. The service checks identity before writing recovery state or publishing results.
- Copying a project creates a distinct container identity and a fresh, empty result store while retaining the workflow and project metadata. It resets execution history, audit history, and cached/published results. It must not alias or mutate the source store.
- No automatic migration, compatibility shim, dual storage path, or legacy project rewrite is in scope. Opening an old `.duckdb` path gives a clear diagnostic and instructions for rebuilding it with a matching release, exporting its workflow JSON, and rerunning that workflow in a new `.sf` project. The original files remain untouched.
- A closed project is backed up or moved as a whole `.sf` directory. Do not claim that copying a live project is a consistent backup.
- The app's folder picker selects one exact `.sf` directory; opening a parent directory and choosing among its projects is not part of this change.
- The ownership lock is guaranteed for local filesystems with the platform's normal exclusive file-lock semantics. Do not claim safe multi-service ownership on network filesystems with unreliable locking.
- Use `sfdev` for supported setup, build, test, package, and release verification. Keep generated projects, logs, and build outputs under this checkout's `tmp/`.

## Out of scope

- Automatically upgrading or migrating existing project databases or result stores.
- Relocating or copying external scientific input files into the container.
- Changing workflow JSON schema or Arrow payload representation unless a specific incompatibility blocks the container design and is separately approved.
- Changing result semantics, execution caching, plugin ABI, or domain operations.
- Defining a cloud-sync or concurrent multi-writer project format.

## Phase overview

| Phase | Result | Status |
| --- | --- | --- |
| 1 | Stable container layout, identity, and schema contract | Complete |
| 2 | Atomic service create/open/close/copy lifecycle | Complete |
| 3 | Consistent service, app, MCP, and C++ public path contract | Complete |
| 4 | Development tooling, fixtures, and user documentation aligned | Complete |
| 5 | Windows and Linux packaged behavior verified; plan closed | Complete |

Phases are ordered. A phase is complete only when its deliverables are implemented, its listed checks pass, and the evidence is recorded below. If an acceptance decision changes, revise this plan before continuing into dependent phases.

---

## Phase 1 — Container layout, identity, and schema contract

### Work

- [x] Define one shared project-layout API that accepts a `.sf` folder path and derives the canonical folder identity, fixed `project.duckdb` path, and `data/` store path. Keep this in the core boundary used by service and project code rather than duplicating path rules.
- [x] Validate path shape and existence according to operation: creation accepts a not-yet-existing `.sf` destination; open requires a valid container and required members. Reject a `.duckdb` file path with a diagnostic that identifies the expected `.sf` folder.
- [x] Handle Windows and Linux path rules, Unicode, spaces, dots in the basename, trailing separators, relative paths, and equivalent canonical paths. Define what happens for symlinks/junctions and ensure identity/ownership behavior is deterministic.
- [x] Move result-store root derivation to `<project>.sf/data`. Keep persisted payload references relative to the store root so the whole container can move without rewriting each result reference.
- [x] Introduce a project/store identity contract. Persist a generated identity in the project database and store manifest, and validate that they match before recovery, cleanup, publication, or query operations that could mutate state.
- [x] Select and document the project schema-version change needed for this structural change. Preserve workflow JSON schema and Arrow payload format unless implementation reveals a blocking need; do not add a migration path.
- [x] Define clear errors for missing database, missing store manifest, invalid manifest, wrong container version, and mismatched project/store identities. Errors should identify the relevant container-relative path and next troubleshooting action.

### Verification and exit criteria

- [x] Unit/contract coverage for layout derivation on Windows and Linux, including Unicode and dotted names.
- [x] Verify equivalent canonical paths yield the same project identity; distinct containers do not.
- [x] Verify an independently substituted store manifest is detected before any database/store mutation.
- [x] Verify malformed or incomplete containers fail with actionable messages and remain unchanged.
- [x] Build the affected core/service targets through `sfdev` and run the narrow contracts.

**Evidence:** `sfdev build backend --jobs 4` succeeded on Windows. `sfdev test backend --jobs 4` passed all 44/44 CTest contracts on both Windows and Linux. `project_runtime_store_recovery_contract` exercises a Unicode and dotted `.sf` name, fixed `project.duckdb`, nested `data/store.json`, duplicate-create refusal, missing-parent refusal, legacy `.duckdb` diagnostics, distinct store identity for copies, cross-service ownership refusal, same-service session reuse, identity mismatch refusal without mutation, and reopening after owner close. `ProjectLayout` uses weak canonicalization with absolute normalized fallback and UTF-8 path conversion. Schema version 5 is used for the breaking layout.

---

## Phase 2 — Service lifecycle and store ownership

### Work

- [x] Make service creation accept a destination `.sf` folder path. Create the directory, database, store manifest, and initial project state as one operation. Do not report success or emit the project-created event until all components pass validation.
- [x] Reject an occupied destination, including an existing empty directory. On initialization failure, remove only the destination created by that attempt; never recursively remove a pre-existing path.
- [x] Make open resolve the fixed internal database and store paths. Validate required members, schema/layout version, and matching identities before recovery writes or session registration.
- [x] Define exclusive ownership for a project container. Use a reliable owner lock/lease that includes enough service identity for diagnostics, is released after worker/database shutdown, and does not leave a stale lock that prevents a later valid open. Handle process termination and lock cleanup safely.
- [x] Preserve same-service repeated-open behavior by returning/reusing the existing session, without opening duplicate database/store handles.
- [x] Route result reads, writes, inventory, cache clearing, run staging, recovery, and cleanup through `<project>.sf/data`. Confirm no operation derives or recreates a sibling `.data` path.
- [x] Implement copy semantics: source workflow and approved metadata are copied; destination gets a new project/store identity and empty result payload store; source remains byte-for-byte/state-equivalent. Define which execution-history fields are copied, reset, or omitted and document that choice.
- [x] Ensure close waits for project workers/handles as required before releasing ownership. Report close errors without falsely advertising the container as available.

### Verification and exit criteria

- [x] Service contract: create → close → reopen succeeds and keeps the same results and project identity.
- [x] Reject duplicate create and confirm no source/pre-existing directory is removed.
- [x] Inject or simulate initialization failures at directory/database/store stages and verify bounded cleanup of only the new destination.
- [x] Open incomplete, wrong-version, and identity-mismatched containers; verify clear errors and no recovery mutation.
- [x] Start two independent service instances: first opens, second receives a clear ownership error; after first closes, second opens successfully.
- [x] Open twice through one service and verify one session/one ownership lock.
- [x] Publish and query an Arrow result, clear results, and inspect that all data stays below the container `data/` directory.
- [x] Copy a project and verify new identity, empty results, expected metadata/workflow, and unchanged source.
- [x] Run the focused C++ contracts through `sfdev`.

**Evidence:** The 44/44 backend CTest suite passed on Windows and Linux. The service contract above covers create/open/close/reopen/copy, owner locking, idempotent same-service open, competing service rejection, identity mismatch, and fresh empty copy store. On both Windows and Linux packaged services, a one-row Arrow result was published, the project was closed and the whole `.sf` folder renamed, then reopened and queried with the same row intact. Copy checks observed a distinct store id and zero published results while preserving metadata. `sfdev workflows nta --force` refused replacement while the project was open; after close it recreated the target container successfully.

---

## Phase 3 — Public service, app, MCP, and C++ interfaces

### Work

- [x] Replace public `database_path` with `project_path` for project create/open/list/session-related APIs, events, app state, MCP tools, and the C++ command/API surface. Use `destination_project_path` for copy. Audit schemas, examples, generated docs, and error payloads for consistency.
- [x] Keep database-file paths only in internal persistence, worker, and database-opening boundaries. Do not expose the internal fixed filename as a caller requirement.
- [x] Update the app's create flow: ask for parent folder and project name, validate/preview the resulting `<name>.sf` path, and submit it to service creation. Define behavior for invalid names, duplicate paths, cancellation, and unavailable parent folders.
- [x] Update demo workflow creation to use the same project-container creation path and naming rules.
- [x] Update open flow to accept a selected `.sf` folder and a typed `.sf` path. Explain invalid/legacy `.duckdb` selection clearly. Do not silently choose a database file inside a selected folder.
- [x] Update project cards, location display, copy-path action, copy-project action, and empty-state text to refer to the container path and folder size accounting. Ensure card refresh responds to service project events and not a stale `.duckdb`/`.data` pair.
- [x] Update MCP `create`/`open` and subsequent project-scoped tools to use `project_path`. Resolve relative paths according to one documented rule before sending to service. Copy accepts `destination_project_path`.
- [x] Update service protocol version/contracts if required. Remove the old public path contract atomically; do not maintain both fields as a transitional fallback.
- [x] Preserve service/app/MCP session identity: creating/opening through one client must be discoverable by the other clients attached to that same service.

### Verification and exit criteria

- [x] MCP schema contract checks required names/types and rejects removed public fields.
- [x] Service API contract covers create/open/copy response and event payloads using `.sf` paths.
- [x] Frontend unit/component tests cover create preview, typed/selected open, cancellation, invalid legacy path messaging, card path, and copy behavior.
- [x] Packaged app scenario: create from the app, then attach with MCP and query the same project/session.
- [x] MCP scenario: create a project, then confirm the packaged app receives the event and shows the project card without a full reload.
- [x] Verify the same project is not opened twice by repeated calls from one service session.
- [x] Run relevant frontend checks and native contracts via `sfdev`.

**Evidence:** `sfdev test backend --jobs 4` passed 44/44 CTest contracts on Windows; the Linux backend suite also passed 44/44 in the isolated Linux verification checkout. The frontend suite passed 30 files / 120 tests. Public command, service, and MCP project operations use `project_path`; copy uses `destination_project_path`. The packaged app and MCP were both exercised against the current checkout package and service at `http://127.0.0.1:8794`: the app created `tmp/projects/sf-container-app-mcp-review-1795013392213.sf`, displayed its card, and a separately launched packaged MCP client called `describe` on that exact `project_path` and received the same path. The complementary MCP-created project → live app card refresh was verified by the browser smoke at port 8793. Package and executable paths were confirmed under this checkout. C++ MCP schema contract passed as part of CTest.

---

## Phase 4 — Tooling, fixtures, examples, and transition documentation

### Work

- [x] Update `sfdev workflows nta|impurities|quantification` to create `.sf` projects under the dynamically resolved current checkout's `tmp/projects/`. Ensure output and follow-up instructions report the folder path.
- [x] Define `--force` safety precisely: resolve the target; require it to remain inside this checkout's `tmp/projects/`; refuse if open/owned; remove only that exact project container and its documented workflow sidecar; never treat a dry run as a completed deletion or project creation.
- [x] Update development scripts, reusable PowerShell helpers, Python workflow checks, fixtures, samples, shell snippets, and API examples to create/read `project.duckdb` inside the `.sf` folder and use `data/` for results.
- [x] Search tracked sources and docs for public `database_path`, sibling `.data`, direct DuckDB-picker, and old pair-backup language. Classify internal-only uses explicitly and remove stale public examples.
- [x] Document the `.sf` layout, service ownership, create/open flows, moving and backing up a **closed** project, external input path limitations, and how to identify the project's result store.
- [x] Document the no-migration legacy rebuild procedure: open with a matching release; export workflow JSON; preserve original database and sibling store; create a new `.sf`; import/validate workflow; rerun; compare key outputs. State that this is a user-directed rebuild and does not rewrite old project files.
- [x] Document troubleshooting for missing container members, store identity mismatch, schema version mismatch, and a project already owned by another service.

### Verification and exit criteria

- [x] Inspect `sfdev help workflows` and the development guide to ensure command behavior, flags, and safety language match the implementation.
- [x] Create at least one disposable workflow through the real `sfdev workflows` + MCP/service path and verify its `.sf` layout.
- [x] Exercise `--force` against a disposable in-checkout project and verify a closed project is replaced; verify an open project and an out-of-root target are refused without deletion.
- [x] Validate the documented legacy rebuild instructions against the actual supported UI/MCP flow without modifying the legacy source project.
- [x] Run script syntax checks and targeted project fixture checks through supported tooling.
- [x] Review repository-wide search results and confirm any remaining `database_path` references are internal implementation details, not public project APIs.

**Evidence:** `sfdev workflows nta --project-name sf-container-plan-smoke-20261118` created a service-backed `.sf` under this checkout's `tmp/projects`. Force replacement while open failed safely and retained the project; after close, force replacement recreated it. The updated script set passed PowerShell parser validation (13 changed scripts) and `scripts/dev/cpp/test_annotate_components.py` passed `.venv` `py_compile`. `git diff --check` passed. README, C++ docs, architecture, quickstart, release, status, and development-script guidance now describe `.sf` folders and the explicit no-migration rebuild path.

---

## Phase 5 — Packaged portability and release-quality verification

### Work

- [x] Run the relevant native and frontend suites through `sfdev`; resolve all regressions attributable to the container work.
- [x] Build the Windows package from the current checkout using the supported `sfdev package` flow. Build/extract the Linux package using the supported release/package workflow without replacing Windows `tmp/packaged_build/`.
- [x] Verify package-relative service, app, and MCP resource resolution for each package. Record the actual executable/package path and service endpoint so a different checkout or stale service cannot accidentally satisfy the check.
- [x] On Windows and Linux, create/open a disposable `.sf` project and run a small workflow that publishes an Arrow table. Close the project, move and rename the entire folder, reopen it, query the same result, and confirm the workflow, execution history, result inventory, and Arrow data remain available.
- [x] Verify a closed whole-folder backup can be opened independently and that the project-copy operation has an empty result store with a distinct identity.
- [x] Verify a `.duckdb` legacy path produces the documented guidance and does not modify the legacy project or its sibling store.
- [x] Stop only services started for these tests and confirm test ports are released. Keep all test projects/logs under this checkout's `tmp/`.

### Exit criteria

- [x] All required checks pass on both target operating systems, or the remaining platform limitation is explicitly recorded and approved before declaring completion.
- [x] The `.sf` layout is the only runtime project/store layout; there is no fallback that reads or writes sibling `.data` stores.
- [x] Public app, service, and MCP project APIs use the same folder-path contract.
- [x] All phase evidence is recorded in this plan. Once implementation is complete, move the plan to `.plans/completed/` according to repository plan-management rules.

**Evidence:** Windows `sfdev package` completed from current sources; its release CTest run passed 44/44. After the app copy workflow was added, `sfdev package --frontend-only` rebuilt the frontend and refreshed only `tmp/packaged_build/app`; `sfdev test browser --url http://127.0.0.1:8794` passed against that package. The app → MCP live round trip and app card visibility passed at port 8794. The Windows backend suite was rerun during final review and passed 44/44; the frontend suite passed 30 files / 120 tests. The test-owned service was stopped and port 8794 released. Linux `sfdev release prepare` completed from an isolated Linux verification checkout: 44/44 CTests passed, package ELF dependency closure passed for 11 binaries, Linux packaged MCP smoke passed (37 tools), and the release archive plus `sha256sums.txt` were produced. `sfdev release verify` validated the v0.8.0 archive/checksum, repeated ELF dependency validation, and repeated MCP smoke successfully. Live Linux packaged-service validation at port 8792 created and ran a workflow publishing one Arrow row, copied it with a new store identity and zero results, then renamed and reopened the original `.sf` and queried the preserved row. Windows equivalent live packaged checks passed at port 8793. Legacy `.duckdb` open returned a clear 400 diagnostic without creating or modifying the legacy database/store. Services started for verification were stopped; test ports were checked clear. Linux verification used `/home/cunha/streamfind-sf-container-verify` to avoid overwriting the Windows `tmp/packaged_build`; this was an isolated temporary verification checkout, not a commit to `dev_refactoring`.

## Resolved design choices

- Use the fixed database filename `project.duckdb`.
- Copy workflow and project metadata only. Reset execution, audit, cache, and result history.
- The picker selects one `.sf` folder; it does not add a parent-directory project chooser.
- Use an exclusive local filesystem lock; network filesystem locking behavior is outside the guarantee.
- Use project schema version 5 for this breaking layout change, as selected in the implementation. Reconfirm the version against `VERSION` and current schema before release packaging.
