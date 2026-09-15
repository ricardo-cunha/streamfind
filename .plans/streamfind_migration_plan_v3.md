# streamfind Living Implementation Roadmap

**Branch:** `dev_refactoring`  
**Purpose:** authoritative forward implementation plan for the StreamFind refactor. It records the accepted C++ architecture, verified implementation stage, remaining delivery work, and deferred Rust plan.  
**Last reviewed:** 2026-09-15  
**Current milestone:** Four-front development split: frontend, MassSpec reader verification, MassSpec processing methods, and Sensors framework; Rust remains deferred.

## Current development stage — 2026-09-15

**Stage 4 — C++ dynamic plugin boundary and package automation: substantially complete.**

Verified in the current working tree:

- C++ is the active native backend; Rust is preserved but not a C++ acceptance gate.
- `cpp/plugins/` is the single plugin root. Plugin directories are discovered automatically when they contain `CMakeLists.txt`.
- The deterministic plugin convention is enforced by the SDK build validator: each plugin owns `plugin.json`, `semantic/`, `src/plugin_entrypoint.cpp`, and `src/methods/`, `src/operations/`, and `src/utils/`.
- MassSpec, Raman, and Sensors build as dynamic libraries with plugin-local manifests and semantic catalogues staged beside their build-tree libraries.
- `cpp/core/src/streamfind-mcp.cpp` is the executable composition/stdio entry point; `cpp/core/src/mcp.cpp` remains the reusable MCP session implementation.
- The MCP host loads configured plugins dynamically and is not statically linked to domain plugin libraries.
- Plugin discovery, aggregate catalogue inputs, MCP `enabled_plugins`, default builds, and Release packaging are derived from `cpp/plugins/` rather than hard-coded domain lists.
- Windows Release packaging was verified for version `0.2.0`; the archive contains the MCP/CLI binaries and all discovered plugin DLLs, manifests, and catalogues under `share/streamfind/plugins/<domain>/`.
- The SDK plugin validator runs after every plugin build and passed for MassSpec, Raman, and Sensors.
- The reduced C++ official suite passes **11/11** tests. C++/Rust conformance was removed from the C++ gate; future conformance will be Rust-owned and will invoke a released C++ MCP executable.
- Legacy root `tests/` content was removed; active C++ fixtures now live under `cpp/tests/fixtures/`.

The repository contains uncommitted structural and packaging changes related to this stage. Do not treat the working tree as release-ready until the changes are reviewed, staged, and committed deliberately.

## Next steps

1. **Finish and review the dynamic package acceptance gate.** Extract a Release archive, launch the packaged C++ MCP with its installed catalogue, run `initialize` and `tools/list`, and execute representative MassSpec, Raman, and Sensors discovery/invocation checks against disposable projects. Verify plugin loading, package confinement, catalogue selection, and project isolation.
2. **Complete SDK data-plane hardening.** Replace remaining transitional string-row/raw-SQL paths with typed batch reads/writes, and add focused rollback, authorization, nullable-value, timestamp, decimal, and binary round-trip coverage through the real dynamic host.
3. **Complete MassSpec public-interface gaps.** Continue native SCIEX/LCD inspection coverage, malformed-input and lazy-decode checks, chromatogram method coverage, aggregate raw-spectrum/chromatogram reopen checks, and the outstanding transformation-product contract decision.
4. **Keep plugin structure automated.** When adding a plugin, add only its plugin directory, manifest, semantic sources, implementation, and local `CMakeLists.txt`; require the SDK validator and Release archive checks to pass without top-level domain-list edits.
5. **Harden installed-package and cross-platform validation.** Run the Linux Release package workflow and verify the discovered plugin libraries/manifests/catalogues in the TGZ, then run the packaged host rather than relying only on build-tree tests.
6. **Rebuild Rust conformance as a separate future lane.** From Rust, invoke a versioned/released C++ MCP executable and compare the public MCP contract. Do not restore C++/Rust conformance to the active C++ CTest suite or make Rust a C++ build dependency.
7. **Only after the native package boundary is stable, revisit frontend and deferred bindings.** React should use the public C++ MCP/service boundary; R and Cogniflow remain preserved deferred integrations.


---

## 1. Read this first — decisions that are already made

Do not reopen these architecture choices unless a concrete implementation blocker is demonstrated with a failing test or benchmark.

**Current planning scope — C++ plugins first:** the active implementation roadmap is
now the C++ core/SDK/plugin refactor. Rust remains in the repository as a preserved,
stale implementation and reference surface, but Rust parity work, Rust plugin work,
Rust MCP work, and Rust-specific gates are paused. Do not describe Rust as an active
parallel workstream or require Rust validation for the C++ plugin milestones. Revisit
Rust only after the C++ SDK, static plugin contract, standalone plugin build, and
runtime/package boundary have stabilized.

1. **C++ is the active StreamFind implementation.**
   - `cpp/` owns the C++ implementation.
   - `rust/` is retained but stale for this plan; do not extend it during the C++ plugin refactor.
   - Rust must never link to, wrap, or call the C++ backend if/when it is resumed.
   - Future parity work is subordinate to the completed C++ plugin contract, not a current gate.

2. **Co-located C++ semantic resources are the authoritative public capability contract.**
   - Turtle is authored under `cpp/core/semantic` and `cpp/plugins/*/semantic`; generated JSON/DuckDB projections are build artifacts.
   - Domain IDs, Methods, Operations, parameters, results, errors, table/column metadata, and public documentation originate there.
   - MCP metadata must not become a second source of truth.

3. **The active native backend exposes the generic MCP contract.**
   - `streamfind-cpp` must expose capabilities through core registries and discovered/static plugins without domain-specific MCP logic.
   - Rust MCP equivalence is a stale future concern and is not a current acceptance criterion.
   - MCP is the canonical orchestration/control boundary for the new frontend.
   - Existing stdio MCP remains useful; add a browser-capable transport for the React application.

4. **There is one shared React/TypeScript web frontend.**
   - Do not build separate main C++ and Rust GUIs.
   - The React app talks to the selected native backend through MCP.
   - Scientific visualisation uses the web ecosystem: D3 for scales/interactions where useful and Canvas/WebGL/Plotly-style rendering for dense 2D/3D data.
   - The frontend never reads DuckDB directly and never imports native implementation internals.

5. **A StreamFind DuckDB file contains exactly one project.**
   - The file path is the sole external locator for that project.
   - Optional caller identifiers belong inside project metadata and are not part of project selection or persistence scope.
   - `domain_id` is immutable and is resolved from the sole `PROJECT` row.
   - Creating a second project in an existing file is rejected; opening a file validates that exactly one project row exists.
   - The frontend displays multiple projects by opening multiple DuckDB files, each with its own scheduler/worker and writer ownership.
   - Combining projects is an explicit future merge into a new destination file and requires compatible domains and table contracts.

6. **Each project has one immutable domain.**
   - `domain_id` is set at project creation and cannot change.
   - The domain determines valid input types, domain Operations, workflow Methods, workflow validation, result tables, and frontend capabilities.
   - The backend always enforces domain constraints; the frontend only reflects them.

7. **Long-running workflow runs are durable backend executions.**
   - They are not owned by the browser/MCP session that launched them.
   - Each project has one current workflow execution and its own lifecycle.
   - Different project files may have independent active executions at the same time.
   - C++ implements the active `WorkflowExecutionManager` and scheduler/worker boundary; Rust is not part of the current execution milestone.

8. **Keep the current local C++ validation approach.**
   - `scripts/build/build-core.cmd -Tests` is the active plugin-refactor gate.
   - `scripts/build/build-rust.cmd -Tests` may be used only as a stale-baseline diagnostic and must not block C++ plugin progress.
   - `scripts/release/release.ps1` is not updated for runtime plugin distribution until the SDK/package boundary is complete.
   - Do not add a hosted GitHub Actions test workflow unless this decision is changed separately.

9. **Domain expansion must not require changes to the generic framework after the extension boundary lands.**
   - Runtime binding is explicit and static for now: domain modules provide `MethodBinding`, `OperationBinding`, and domain table/schema bindings.
   - Semantics remain authoritative for public metadata; bindings only connect canonical IDs to executable/native implementations.
   - New MassSpec processing families such as `mass_spec.nta`, `mass_spec.qc`, or `mass_spec.targeted` belong in domain modules, not in generic `core/` or generic MCP dispatch.
   - Do not introduce a dynamic plugin ABI until there is a concrete distribution requirement; static module composition is the first target.

---

## 2. Status legend

| Status | Meaning |
| --- | --- |
| **Complete** | Present in the branch and covered by its component tests/build workflow. |
| **Partial** | Foundation exists but the public/runtime contract still needs hardening. |
| **Next** | Required for the primary milestone and unblocked after its listed dependencies. |
| **Parallel** | Can proceed in a separate worktree without changing the primary milestone contract. |
| **Future** | Intentionally deferred until preceding gates pass. |

---

## 3. Accepted implementation baseline

The following baseline is accepted from the current `dev_refactoring` roadmap and should not be reimplemented as greenfield work.

| Area | Status | Accepted baseline | Implication |
| --- | --- | --- | --- |
| C++ backend | **Complete foundation / active** | Standalone C++20 framework under `cpp/core` with Project/JSON APIs, DuckDB persistence, workflow/cache/audit, cooperative progress/cancellation primitives, generic MCP services, plus statically composed domain targets under the intermediate `cpp/domains` tree. | Keep the framework generic; move domain implementations toward the SDK/plugin boundary without building a second runtime. |
| Rust backend | **Stale / paused** | The Rust workspace and prior parity implementation remain in the repository, but it is not an active target of this roadmap. | Do not extend, refactor, or require Rust validation during the C++ plugin track. Re-audit it after the C++ SDK/runtime boundary stabilizes. |
| Semantic catalogue | **Complete C++ runtime foundation** | Turtle + SHACL + deterministic `catalogue.json` and `catalogue.duckdb`; the active C++ runtime consumes the generated projection. Rust consumption is retained only as stale reference state. | Extend semantics first when a new C++ public contract is required. |
| C++ MCP | **Partial / active** | Generic catalogue/registry-based discovery and dispatch exist; durable execution work is locally advanced. | Keep MCP thin and C++ registry/plugin driven; do not add domain-specific dispatch. |
| Rust MCP | **Stale / paused** | Existing Rust MCP code is retained but is not being evolved or used as a gate. | Do not add browser transport or parity work until the C++ plugin/runtime contract is accepted. |
| WorkflowExecutionManager | **Partial / C++ active** | Durable execution work is substantially implemented locally in C++; the Rust equivalent is stale for this roadmap. | Audit and harden the existing C++ manager; do not create another job/runtime layer. |
| NTA methods | **Complete C++ capability surface / plugin refactor target** | C++ MassSpec workflow Methods are registered through static module descriptors; the Rust registration is historical/stale. | Preserve C++ behaviour while moving ownership into the SDK/plugin package; do not rewrite algorithms merely to achieve the split. |
| R binding | **Relocated / deferred** | Existing functional package under `bindings/r/`. | Keep functional; no new domain work there. |
| Cogniflow | **Relocated / deferred** | Integration boundary under `integrations/cf-streamfind/`. | Align after the public native/MCP path is stable. |
| Frontend | **Not implemented** | Architecture selected: React/TypeScript + common MCP + scientific web visualisation. | Frontend can begin from the frozen execution/MCP contract; it does not wait for Python. |
| Reader hardening | **Parallel** | Active MassSpec reader hardening remains unfinished. | Continue in a dedicated worktree without blocking execution architecture. |

### NTA C++ registry baseline

The accepted C++ inventory is 19 MassSpec workflow Methods:

```text
find_features, load_features_ms1, load_features_ms2,
create_components, annotate_components, group_features, fill_features,
subtract_blank, correct_matrix_suppression, filter_features,
filter_features_ms2, suspect_screening, find_internal_standards,
filter_suspects, filter_internal_standards, metfrag_screening,
assign_transformation_products, load_chromatograms,
filter_chromatograms_retention_time
```

Do not manually maintain a frontend/MCP copy of this list. C++ capability discovery
must remain semantic catalogue + executable registry driven. Rust parity is stale and
must not be used to block this C++ inventory or plugin refactor.

### Remaining migration/public-interface work that must not be lost

The primary milestone changes the execution/frontend architecture, but it does **not** retire the remaining migration work from the prior roadmap. Keep these tracked:

- remaining SCIEX/LCD public reader/inspection Operations, including the outstanding LCD inspection/listing boundary where still unimplemented;
- broader malformed-input, calibration, truncation/overflow, lazy indexed-decode, and vendor-corpus validation;
- explicit test coverage for both chromatogram workflow Methods;
- aggregate raw-spectrum/chromatogram interface and persistence/reopen coverage;
- the remaining decision on a pure transformation-product assignment `sf:Operation`: define the shared record contract and implement it independently in C++/Rust, or record an explicit retirement decision.

These items proceed in the reader/public-interface lane and must not be silently dropped because frontend/execution work has higher priority.

### Semantic source layout

Keep the existing backend-neutral semantic layout and generator model:

```text
semantic/ontology/
├── vocabulary.ttl
├── shapes.ttl
├── core/
│   ├── scheme.ttl
│   ├── parameters.ttl
│   ├── operations.ttl
│   ├── results.ttl
│   ├── errors.ttl
│   ├── tables.ttl
│   ├── fields.ttl
│   └── columns.ttl
└── domains/
    ├── mass_spec/
    ├── raman/
    └── sensors/

semantic/generated/
├── catalogue.json
└── catalogue.duckdb
```

Turtle remains authoritative; generation is deterministic; both native backends consume the same projection shape; runtime native code must not require its own RDF interpretation stack.

---

## 4. Target runtime architecture

The active target is a C++ runtime whose MCP layer is thin and whose capabilities come
from the core registry plus statically composed or, later, runtime-discovered plugins:

```text
                              React + TypeScript
                         scientific web application
                                     │
                                     │ MCP
                                     ▼
                         StreamFind C++ MCP host
                                     │
                                     ▼
                         StreamFind C++ Core
        Project / DuckDB / workflows / provenance / registries
                                     │
                               StreamFind SDK
                                     │
                 ┌─────────────────┼─────────────────┐
                 ▼                 ▼                 ▼
          MassSpec plugin    Raman plugin      Sensors plugin
          (static first,     (static first,    (static first,
           runtime later)     runtime later)    runtime later)
```

The Rust tree is retained as a stale independent implementation and is not part of
this active runtime path. It must not be removed or silently rewritten, but its MCP,
workflow, plugin, and parity claims are deferred until the C++ SDK and plugin package
contract has been accepted.

```text
DuckDB project file
├── PROJECT [one row, immutable domain_id, current workflow]
├── WORKFLOW_EXECUTION [latest run of the one workflow]
├── WORKFLOW_EXECUTION_STEP [durable steps for that run]
├── CACHE [project-owned cache/materialized outputs]
├── AUDIT_TRAIL [project and workflow history]
└── plugin-defined tables [declared by plugin, lifecycle managed by core]
```

The React application connects to the C++ MCP endpoint. Backend selection, Rust
reconnection, and dual-backend frontend parity are future work, not current C++ plugin
acceptance criteria.

---

## 5. Ownership boundaries

### `semantic/` — public metadata and contract

Owns:

- canonical IDs;
- domains and domain labels/definitions;
- Methods vs Operations;
- parameters and validation metadata;
- results and shared errors;
- table/field/column declarations;
- capability documentation;
- MCP exposure metadata where already represented;
- fixture references and cross-backend contract metadata.

Does **not** own executable analytical logic, scheduling, database connections, or frontend state.

### `cpp/core/` — independent C++ framework

Owns:

- C++ Project/runtime;
- C++ persistence implementation;
- C++ MethodRegistry/OperationRegistry;
- generic binding/runtime support used by domain plugins;
- C++ `WorkflowExecutionManager`;
- core-owned MCP host/transport services;
- framework tests.

Domain implementations, vendor readers, domain semantics, and domain-owned tables
must move toward `cpp/plugins/<domain>/`; `cpp/core` must not include domain headers
or contain MassSpec/Raman/Sensors dispatch. The current `cpp/domains/` layout is an
accepted intermediate location while the SDK and plugin package boundary is extracted.

### `rust/` — preserved stale backend

Owns its existing Rust implementation only as a retained reference surface:

- Rust Project/runtime;
- Rust persistence implementation;
- Rust registries and domain crates;
- Rust workflow/MCP code;
- historical Rust tests.

No Rust file is part of the active C++ plugin refactor unless a later decision reopens
that lane. Do not delete the tree, but do not add new Rust capabilities, plugin
reorganization, browser transport, or parity gates now.

### `frontend/` — backend-neutral web client

Owns:

- React/TypeScript application;
- MCP connection/client abstraction;
- generated or shared TypeScript contract types;
- project/workflow UI;
- execution store and execution monitor;
- scientific data visualisation;
- frontend tests/end-to-end tests.

Must not own backend workflow state, workflow scheduling, domain validation, or direct DuckDB access.

### `bindings/python/` and `bindings/r/`

Python remains a useful future C++ distribution/scripting surface but is **not** a dependency of the React frontend. R remains the migration reference and existing binding until later alignment.

---

## 6. Agent operating contract

Every coding agent/worktree must follow these rules.

### Before editing

1. Read this roadmap and the nearest component-specific plan/README/`AGENTS.md`; treat the current local working tree/attached roadmap as authoritative for completion status when GitHub has not yet been updated.
2. Identify the single workstream owned by the branch.
3. Run or record the relevant baseline tests before changing behaviour.
4. Inspect existing abstractions before adding new classes/tables/protocol layers.
5. If a public capability/schema changes, update `semantic/` first or document why it is runtime-only execution metadata rather than a domain capability.

### During implementation

- Prefer the smallest extension of existing Project/workflow/MCP abstractions.
- Do not introduce backend-specific frontend contracts.
- Do not copy C++ implementation logic into Rust or vice versa; implement from the shared contract/fixtures.
- Do not add per-method MCP dispatch branches.
- Use `database_path` as the sole external project locator. Optional identifiers belong inside project metadata.
- Do not let UI state become execution truth.
- Prefer the project-scoped table abstraction over hand-written row-scope SQL. Raw SQL is an escape hatch, not the default domain API.
- Preserve project/domain isolation in every SQL statement and cache/audit key.
- Keep migrations deterministic and restart-safe.
- Add tests with the implementation, not in a later cleanup branch.

### Before merging

The agent must provide:

```text
Scope completed
Files/areas changed
Contract/schema changes
Migration impact
Tests run + result
Cross-backend parity impact
Known limitations/follow-up
```

No branch is complete merely because it compiles.

---

## 7. Canonical semantic, binding, and registry model

The ontology distinguishes workflow Methods from direct Operations. Runtime code must also distinguish **metadata** from **executable bindings**.

```text
semantic catalogue                         native implementation
------------------                         ---------------------
sf:Method / sf:Operation                   MethodBinding / OperationBinding
parameters/results/docs                    executor + optional native validator
reads/writes/domain/module                 domain-native implementation
             \                                 /
              \                               /
               ------ generic registrar ------
                           │
                           ▼
                 MethodRegistry / OperationRegistry
```

Rules:

- only Methods may be workflow steps;
- Operations never become workflow steps;
- every public capability has one canonical semantic ID;
- semantics own labels, descriptions, parameters, result schemas, domain, `reads`/`writes`, and documentation;
- bindings own executable/native callbacks only;
- registries are execution registries, not metadata catalogues;
- generic MCP discovery is the intersection of semantic executable IDs and registered bindings;
- adding a Method or Operation to an existing domain must not require generic MCP source changes;
- a binding ID missing from semantics is an error; a semantic executable ID expected for a shipped module but missing a binding is a conformance error.

### 7.1 `MethodBinding` (C++ active; Rust shape retained as reference)

The active C++ implementation uses this concept. A future Rust reopening may converge
on the same contract independently:

```text
MethodBinding
  canonical_id
  executor
  optional native/cross-field validator
```

Suggested C++ shape:

```cpp
struct MethodBinding {
    std::string_view id;
    MethodExecutor executor;
    MethodValidator validator{};
};
```

Historical Rust shape (not an active implementation target):

```rust
pub struct MethodBinding {
    pub id: &'static str,
    pub executor: MethodExecutor,
    pub validator: Option<MethodValidator>,
}
```

These are binding records, not copies of `MethodDefinition`.

The generic registrar loads the `MethodDefinition` from the semantic catalogue, verifies domain/module compatibility, attaches the executor/validator, and registers the resulting Method.

Do not duplicate semantic parameter metadata in the binding. Simple scalar/range/enum validation belongs in semantic constraints. Native validators are reserved for constraints that cannot be represented cleanly in the shared schema, such as cross-field relationships or data-dependent preconditions.

### 7.2 `OperationBinding`

Operations use the same pattern:

```text
OperationBinding
  canonical_id
  executor
  optional native/cross-field validator
```

If Operations do not yet support a validator callback symmetrically with Methods, add that capability once as part of the extension-boundary refactor. After Gate E, a new Operation must not require another generic framework change.

This removes large domain-specific `if`/`match` dispatch blocks from registration code. The generic registrar constructs `OperationDefinition` from semantics and attaches the native executor.

### 7.3 `DomainModuleBinding`

The unit of native composition is a **domain module**, not a growing monolithic domain registration file.

```text
DomainModuleBinding
  module_id          e.g. mass_spec.base | mass_spec.nta | mass_spec.qc
  domain_id          e.g. mass_spec
  module_version
  required_modules[]
  method_bindings[]
  operation_bindings[]
  schema_binding / migrations
```

Initial MassSpec decomposition:

```text
mass_spec.base
  readers
  analyses/project Operations
  spectra/chromatogram query Operations
  base tables

mass_spec.chromatograms
  load_chromatograms
  filter_chromatograms_retention_time
  chromatogram processing/query helpers

mass_spec.nta
  feature detection/grouping/filling/filtering
  component creation/annotation
  suspect/internal-standard screening
  matrix correction
  transformation products
  MetFrag
  NTA result/query Operations and tables

future modules
  mass_spec.qc
  mass_spec.targeted
  mass_spec.statistics
  ...
```

Logical C++ module separation is required first. Physical plugin libraries are optional
until build/dependency boundaries justify them; do not create package fragmentation
merely for aesthetics. Rust crate reorganization is deferred.

Application composition may have one explicit line per shipped module, but generic `core/`, generic MCP, Project, workflow execution, and frontend code must not change when a new Method/Operation is added inside an already shipped module. The binding/context/schema infrastructure is a deliberate **one-time framework extension boundary**; after Gate E it should be treated as frozen unless a demonstrated generic requirement cannot be expressed through it.

### 7.4 Static domain-plugin composition

The first plugin architecture is compile-time/static composition, not runtime DLL
loading. A domain module is plugin-like because it owns an isolated descriptor and
contributes executable bindings and schema installation to the main framework, but it
does not introduce a second ABI, loader, dependency resolver, or metadata authority.
This is the required implementation boundary for the current C++ plugin migration.
The Rust descriptor implementation is retained as historical reference and is not an
active acceptance target.

```text
PluginDescriptor
  module_id
  domain_id
  module_version
  required_modules[]
  method_bindings[]
  operation_bindings[]
  schema_binding
  owned_table_ids[]
```

The active C++ backend exposes one descriptor per shipped logical module, for example
`mass_spec_base_plugin`, `mass_spec_chromatograms_plugin`, and
`mass_spec_nta_plugin`. The application performs one explicit composition step per descriptor. Adding a Method
or Operation inside an existing module changes only its semantic declaration, C++
implementation, binding collection, and C++ proof test; generic Project, workflow,
registry, and MCP code must not change.

Registration must consume one immutable `CatalogueSnapshot` (or equivalent backend
value) created by the application at startup. The snapshot is passed explicitly to
every plugin registrar and test. No module registrar may call a hidden cached/default
catalogue lookup, because that allows a caller to validate one projection while
registering against another. The compatibility overload that resolves the default
catalogue may remain only for existing public callers until all composition points
accept a snapshot; new module code must use the explicit form.

The generic plugin registrar performs these checks before registry exposure:

```text
binding ID exists in snapshot
binding kind matches Method/Operation
entry domain matches descriptor domain
entry module matches descriptor module
owned table exists and has the generated manifest contract
dependencies are registered and schema-compatible
no executable ID is registered twice
```

Missing or mismatched bindings are startup/conformance errors, not silently omitted
capabilities. Native validators run before executors. Semantic labels, parameters,
result schemas, effects, cache settings, table columns, and documentation continue
to come only from the generated catalogue.

Methods are the module's processing boundary: they validate parameters, require their
semantic input tables, compute natively, and create/update only declared output tables.
Operations are the module's query/mutation boundary: they read module-owned tables
through `ProjectTableStore`, return the semantic result schema, and do not become
workflow steps. A table is owned by the module whose Methods create or maintain it;
an Operation reading that table does not transfer ownership.

Module schema installation remains separate from executable registration. The plugin
descriptor supplies the module schema binding and owned table IDs; generic core owns
transaction/lifecycle coordination and `MODULE_SCHEMA` tracking, while the module owns
its DDL and migrations. Listing capabilities must not silently mutate a project.
Required module schemas are installed at project creation or explicitly before the
first mutating Method, in dependency order.

MCP discovery remains generic:

```text
advertised Methods/Operations = semantic executable IDs ∩ registered binding IDs
```

The catalogue provides MCP descriptions and schemas; the registry proves that native
executors exist. A semantic declaration without a bound executor is not advertised,
and an executor without a semantic declaration rejects plugin registration.

Do not implement dynamic shared-library plugins in this phase. A future dynamic ABI
would require a separately approved contract for C ABI boundaries, allocator/error
ownership, version negotiation, discovery, and Windows packaging. It is not required
for simple capability expansion and would create compatibility scaffolding during the
active refactor.

### 7.5 Plugin implementation gates

Implement the C++ plugin framework in these independently testable slices:

1. Keep explicit catalogue-snapshot input in the C++ module registrar and remove new
   hidden cached/default catalogue reads.
2. Maintain the C++ registration-contract tests for complete valid registration and
   unknown ID, wrong kind, wrong domain, wrong module, duplicate ID, missing table,
   and missing dependency failures.
3. Keep `mass_spec.base`, `mass_spec.chromatograms`, and `mass_spec.nta` as explicit
   static descriptors while extracting the C++ SDK boundary.
4. Move module-owned schema installation and manifest checks behind the C++ descriptor;
   prove that creating/updating a Method table does not require generic Project edits.
5. Add one small C++ proof Method plus one query Operation and a new module-owned
   table; verify MCP discovery and project-file persistence without editing generic MCP.
6. Remove the resolver/exclusion loops only after the C++ complete registered-ID proof
   remains green.
7. Reopen the Rust parity/plugin lane only after the C++ SDK and runtime package
   contracts are accepted; do not make Rust a prerequisite for these slices.

### 7.6 C++ registry-first plugin refactoring plan

The long-term C++ architecture is a lean, domain-independent framework plus an SDK
and one runtime-loadable shared library per domain plugin. The DLL is not the first
change. The safe migration is to establish the ownership and registry boundary while
plugins remain statically composed and fully testable.

Target source layout:

```text
cpp/
├── core/
│   ├── include/streamfind/       # stable framework-facing implementation headers
│   ├── src/                      # Project, DuckDB, workflows, registries, services
│   ├── semantics/                # core ontology only, when extracted from semantic/
│   ├── tests/
│   └── CMakeLists.txt
├── sdk/
│   ├── include/streamfind/sdk/   # public extension contract
│   ├── cmake/                    # find/package/plugin helpers
│   ├── templates/
│   ├── examples/
│   └── CMakeLists.txt
├── plugins/
│   ├── mass_spec/
│   ├── raman/
│   ├── sensors/
│   └── generic/                  # only when generic operations justify a plugin
├── mcp/
│   ├── src/                      # thin composition/transport executable
│   └── CMakeLists.txt
└── CMakeLists.txt
```

The current `cpp/domains/` tree is an intermediate implementation location. Relocate
into `cpp/plugins/` only after the core/plugin dependency audit and the first SDK
contract are in place; do not perform a directory move that leaves duplicate source
or registration paths.

Ownership and dependency direction:

```text
cpp/plugins/<domain>  →  cpp/sdk  →  cpp/core
cpp/mcp              →  cpp/core + registered plugin descriptors
cpp/core             ✕→  MassSpec/Raman/Sensors implementation
```

The SDK is a public extension contract, not a second processing engine. Its first
stable surface should contain only the abstractions required by a plugin:
`PluginDescriptor`, `PluginContext`, `DomainDescriptor`, `ModuleDescriptor`,
`MethodBinding`, `OperationBinding`, `TableContract`, `ParameterDescriptor`,
`ExecutionContext`, project-scoped database/table access, logging/resource services,
registry interfaces, SDK/runtime version information, and explicit error ownership.
Do not expose internal core classes, STL ownership across a future DLL boundary, or
analytical datasets through the SDK.

Use one shared library per domain plugin at the delivery boundary, not one library
per algorithm. A plugin source tree may contain readers, processing methods, table
contracts, semantics, migrations, workflows, resources, tests, and `plugin.json`.
The runtime package contains the manifest, plugin library, semantic resources,
migrations/workflow resources, and declared dependencies; compile-time headers remain
in the SDK package and are not required at runtime.

Execute the C++ refactor in this order:

1. **Audit and freeze ownership.** Inventory every C++ include, target, source,
   semantic file, table contract, vendor dependency, and MCP composition point.
   Classify it as core framework, SDK contract, plugin implementation, MCP transport,
   test, or temporary artifact. Core must compile without MassSpec/Raman/Sensors
   headers and without domain-specific dispatch.
2. **Extract generic registries and services.** Keep the existing static descriptor
   registrar as the implementation seam. Ensure `RegistryManager`, Method/Operation
   registries, table-contract registration, project-scoped `ProjectTableStore`,
   execution context, logging, and schema/migration coordination are generic. A
   plugin declares tables; core creates, validates, migrates, and owns their lifecycle.
3. **Create the C++ SDK target.** Move only the minimal public extension headers into
   `cpp/sdk/include/streamfind/sdk/`, add a CMake package/configuration target, and
   provide a small plugin template/example. The SDK must link against installed
   StreamFind runtime interfaces rather than require the core source tree.
4. **Make MassSpec the reference static plugin.** Move/organize MassSpec sources under
   `cpp/plugins/mass_spec/` without algorithm rewrites. Keep `base`, `chromatograms`,
   and `nta` as logical modules in one domain plugin. Move domain semantics, table
   ownership, migrations, workflows, and resources beside the plugin when their
   generator/package support is ready.
5. **Route all composition through the SDK/registries.** The C++ MCP executable must
   initialize core, create one explicit catalogue snapshot, and compose plugin
   descriptors. It must not contain MassSpec-specific dispatch or capability lists.
   MCP discovery remains the semantic/native intersection.
6. **Validate standalone plugin development.** Build and install the C++ core/runtime
   and SDK, then build a MassSpec plugin against the installed package in a separate
   CMake build. The in-repository aggregate build remains useful, but it is not proof
   that the SDK boundary is consumable.
7. **Add the runtime plugin boundary last.** Only after MassSpec is fully SDK-based,
   independently packaged, and statically/runtime-contract tested, design the dynamic
   ABI and `PluginManager`: manifest discovery, ABI/SDK version checks, dependency
   resolution, symbol entry point, allocator/error ownership, security policy,
   unload/reload policy, search paths, and Windows packaging. Do not pass C++ STL,
   `std::function`, JSON objects, or exceptions across this boundary.

Required C++ acceptance evidence:

- core builds without domain implementation targets;
- SDK headers and CMake package install successfully;
- MassSpec remains behaviorally unchanged through the static registry boundary;
- table declarations are validated and installed by core, while DDL/migrations remain
  plugin-provided;
- a new plugin Method/Operation does not require generic core or MCP source changes;
- a separately built plugin can register against an installed SDK/runtime;
- dynamic loading is tested only after an approved ABI exists.

This track is C++-focused. Rust remains a preserved stale implementation and is not
updated when C++ plugin contracts change. If reopened later, it must consume the
stabilized semantic/SDK contract independently and must not become a C++ SDK dependency
or runtime bridge.

### 7.7 Traceable module metadata

Add a backend-neutral semantic/module field such as `sf:providedByModule` (name may be finalized during implementation) and project it into generated catalogue artifacts.

Generate or test a capability matrix containing at least:

```text
canonical_id
kind                 method | operation
domain_id
module_id
module_version
reads_tables
writes_tables
semantic_executable
cpp_bound
rust_bound
```

This matrix is a development/conformance artifact, not a new metadata authority. Its
active purpose is to make C++/semantics drift auditable. Rust columns, if retained in
the generated artifact, are historical and not a current gate.


---

## 8. Single-project DuckDB contract

Each DuckDB file contains exactly one StreamFind project. This simplifies the public identity model and should be used to remove redundant `database_path + project metadata` call signatures.

### 8.1 External locator and optional metadata

Use these concepts distinctly:

```text
database_path
  sole external locator / container identity used to open a project

project metadata
  optional user-defined metadata stored inside the project
  may contain an application identifier when a caller needs one
  is not a registry key, selector, or row-scope column

domain_id
  immutable project domain resolved from PROJECT
```

Therefore, the normal public/native/MCP contract is:

```text
create(database_path, domain_id, metadata?) -> project descriptor
open/describe/validate/get_*/set_*/run_*(database_path, ...) -> project resolved from file
connect(database_path) -> optional session convenience
```

Do not require or accept a separate project identity alongside `database_path`. One DuckDB file contains exactly one project, and `database_path` is the only project locator.

### 8.2 Persisted-schema rule

The project identity is not persisted as a dedicated column. Project-owned tables do not carry a project row-scope key, and domain SQL must not inject project-identity predicates, joins, or placeholders. File ownership provides isolation; analysis, feature, chromatogram, workflow, cache, and audit filters remain explicit where required.

### 8.3 Domain-defined tables and schema validation

Audit every table and classify it explicitly as either:

1. **runtime/catalogue data** — intentionally outside the project file; or
2. **project-owned/domain data** — belongs to the file's sole project and is owned by the base project schema or a registered domain module.

The expected project-file categories include at least:

```text
PROJECT
current workflow
workflow executions and durable step records
CACHE
AUDIT_TRAIL
analyses
chromatograms/spectra/features
domain/module result tables
future report/result references
```

### 8.4 Required invariants

- `PROJECT` contains exactly one row.
- a second project creation request for the same file is rejected;
- opening a file requires only `database_path`;
- opening resolves and validates the sole `PROJECT` row and immutable `domain_id`;
- the domain plus registered modules determine the expected domain tables/schema;
- cache, audit, execution, and domain rows cannot escape the file's project scope;
- the project has one current workflow; workflow replacement is audited;
- execution history semantics follow the implemented `WorkflowExecutionManager` contract rather than frontend/session state.

### 8.5 Migration strategy

1. Inventory public APIs/MCP Operations and remove separate project-identity arguments.
2. Remove dedicated project-identity resources from the semantic input and persisted schemas.
3. The C++ Project open/create APIs read the sole project row; the Rust implementation is retained as stale reference.
4. Store optional caller identifiers only inside project metadata.
5. Update domain Operations, MCP fixtures, React types, and documentation from the same semantic contract.
6. Reject files with zero or multiple `PROJECT` rows or an invalid domain/module schema.
7. Test multiple projects using **multiple DuckDB files**, never multiple project rows in one file.

### 8.6 Multiple-file conformance fixture

The shared fixture must contain at least:

```text
project_mass_spec.duckdb  (one stored project, domain_id = mass_spec)
project_raman.duckdb      (one stored project, domain_id = raman)
```

The active C++ backend proves:

- each file opens from `database_path` alone;
- project descriptors expose the file locator and optional project metadata;
- each file validates its predefined domain and installed module/table expectations;
- operations cannot cross file/project scope accidentally;
- cache/audit/execution rows remain scoped to their file's sole project;
- both files can be processed concurrently by independent workers.


---

## 9. Domain-constrained project contract

`domain_id` is immutable after project creation.

For every project request, backend code resolves:

```text
database file -> sole PROJECT row -> immutable domain_id -> allowed capabilities and expected table schema
```

The effective capability set is:

```text
semantic declarations
        ∩
registered domain-module bindings
        ∩
selected project's immutable domain
        ∩
required module schema compatibility
```

The domain determines:

- accepted analytical input families/file types;
- domain Operations;
- workflow Methods;
- required Method dependencies/order;
- domain tables/results;
- frontend creation/import/workflow controls.

If accepted input families are not yet represented sufficiently in the semantic catalogue for frontend discovery, add a small backend-neutral semantic declaration rather than hard-coding file/domain rules in React.

The backend revalidates all domain constraints at execution time. Frontend filtering is convenience, not authority.

---

## 10. Extensible domain modules and project-scoped data access

This is the primary framework-expansion refactor. The goal is that a future processing Method, direct Operation, processing family, or result table can be added without edits to generic Project/MCP/workflow code.

### 10.1 `ProjectTableStore` / project-scoped table access

Introduce the active C++ abstraction first. Rust retains its existing implementation
but is stale and must not be extended during this phase:

```text
Project
  │
  └── tables() / ProjectTableStore
         ├── query(table_id, projection/filter/order/page)
         ├── append(table_id, rows)
         ├── replace/upsert(...)
         ├── update/delete(...)
         ├── transaction(...)
         ├── ensure_module_schema(module_id)
         └── raw_sql(...)          # explicit escape hatch only
```

Responsibilities:

- resolve canonical semantic table ID to physical DuckDB table name;
- know the open file's sole project;
- never inject a dedicated project identity on inserts/appends;
- never add project-identity predicates to project-owned reads/updates/deletes;
- expose bound/prepared query paths so domain code stops concatenating identity values into SQL;
- centralize appender/bulk-write behaviour;
- centralize transaction ownership and schema checks;
- reject a domain/module accessing a table that is not declared/installed for that project domain unless explicitly allowed;
- when invoked through a Method/Operation context, optionally enforce semantic `reads`/`writes`: undeclared writes should fail; undeclared reads should fail in strict/conformance mode.

Do not attempt to build a general ORM. The abstraction should remain thin and DuckDB-oriented.

### 10.2 Domain-owned schema bindings and migrations

Generic core provides migration/table infrastructure; **domain modules own their schemas**.

Prefer a module-level schema binding over one callback per table:

```text
ModuleSchemaBinding
  module_id
  domain_id
  schema_version
  migrations[]
  owned_table_ids[]
```

Add one small generic project table such as:

```text
MODULE_SCHEMA
  module_id          PRIMARY KEY
  domain_id
  schema_version
  module_version
  applied_at
```

This table is generic runtime infrastructure. Domain table DDL is not.

Rules:

- `PROJECT.schema_version` tracks generic project-container schema only;
- `MODULE_SCHEMA` tracks independently evolving domain-module storage;
- module migrations are ordered, deterministic, transactional where DuckDB permits, and idempotence-tested;
- opening/listing capabilities must not silently mutate a project merely because a new module exists in the executable;
- install/upgrade module schema at project creation for required base modules, or explicitly/lazily before the first mutating capability that requires it;
- module dependency order is validated (`mass_spec.nta` may require `mass_spec.base`, etc.);
- a project cannot execute a binding whose required module schema is absent/incompatible.

The semantic ontology remains authoritative for public table/column meaning and Method/Operation `reads`/`writes`. Native module migrations remain authoritative for executable DuckDB DDL until/unless a later generator proves capable of expressing all required constraints/indexes/migrations safely.

Adding a new domain table in the active C++ track must require:

```text
semantic table/columns
+ C++ owning-module migration
+ C++ schema/conformance fixture
```

and must **not** require edits to generic Project schema code.

### 10.3 Method/Operation execution context

Move domain executors gradually away from unrestricted `Project & + raw SQL` toward a context that exposes approved services.

Conceptually:

```text
DomainContext
  project descriptor
  ProjectTableStore
  domain_id
  module_id
  catalogue access

MethodExecutionContext extends/contains DomainContext
  project-owned workflow execution context
  cancellation token
  progress reporter
  current workflow step
  declared read/write table set
```

At invocation, attach traceability metadata that can be persisted in execution/audit details without changing the public algorithm API:

```text
canonical capability id
module_id + module_version
catalogue/projection version or hash
backend implementation/version
workflow revision when applicable
```

This makes it possible to determine which module/version produced a stored result in
the active C++ runtime. A future Rust reopening must preserve the same traceability
fields without becoming a C++ dependency.

Operations use `DomainContext`; workflow Methods use `MethodExecutionContext` when lifecycle services are required.

Do not rewrite all existing signatures in one breaking change. Add adapters so old executors continue to run, then migrate module-by-module.

### 10.4 MassSpec module split

Refactor registration/ownership without rewriting working algorithms.

Target source ownership:

```text
mass_spec/
  base/
    readers/
    analyses operations
    raw spectra/chromatogram operations
    base tables
    bindings

  chromatograms/
    processing methods
    related operations
    table bindings/migrations
    bindings

  nta/
    feature processing algorithms
    screening/annotation/transformation products/MetFrag
    NTA query operations
    NTA table bindings/migrations
    bindings

  future/
    qc/
    targeted/
    statistics/
```

Each module exports one descriptor/binding collection. The MassSpec domain composition imports those modules; generic framework code does not enumerate their methods or operations.

Preferred logical C++ source pattern (adapt to existing conventions rather than forcing exact names):

```text
cpp/plugins/mass_spec/
  base/{bindings,operations,schema,...}
  chromatograms/{bindings,methods,operations,schema,...}
  nta/{bindings,methods,operations,schema,algorithms,...}

rust/crates/mass-spec/src/        # preserved stale reference; do not reorganize now
```

The existing independent algorithm files may remain where they are initially and be referenced by module bindings. File moves are secondary to ownership/registration clarity.

### 10.5 Expansion workflow for a new Method

After this refactor, adding `mass_spec.qc.calculate_metrics` should require only:

1. semantic Method/parameters/results/reads/writes/module declaration;
2. implementation inside the C++ `mass_spec.qc` module;
3. one C++ `MethodBinding` entry;
4. C++ fixtures and conformance tests.

No generic MCP, Project, workflow engine, React business logic, or base registry switch/match is edited.

### 10.6 Expansion workflow for a new Operation

Likewise a new direct Operation requires:

1. semantic Operation declaration;
2. C++ module implementation + `OperationBinding`;
3. C++ result/fixture tests.

Generic MCP discovers and dispatches it from semantics + registry automatically.

### 10.7 Expansion workflow for a new table

A new result table requires:

1. semantic `sf:Table` + columns;
2. module table binding/migration in C++;
3. update the owning Method/Operation `sf:reads`/`sf:writes`;
4. C++ schema fixture;
5. ProjectTableStore-based access in the implementation.

No `cpp/core/src/project.cpp`-style generic schema block should be edited for a domain table.

### 10.8 Proof-of-extensibility gate

Do not declare this refactor complete based only on moving files. Prove it by adding one small representative capability—preferably a simple MassSpec QC Method/Operation that writes a new table—using only semantic + module + tests.

The proof passes only if:

- generic core framework files do not change after the binding/table infrastructure commit;
- generic MCP dispatch does not change;
- generic Project schema code does not change;
- C++ registers the canonical capability through bindings;
- the new table is created through module schema ownership;
- ProjectTableStore scopes access correctly;
- the frontend can discover the new capability from the existing catalogue path without a hard-coded capability list.


---

## 11. WorkflowExecutionManager contract

`WorkflowExecutionManager` is an active generic C++ runtime service. Substantial work
exists locally and may not yet be visible on GitHub; treat the local implementation
as the baseline. This section is therefore a **C++ gap-audit and hardening contract**,
not permission to rebuild the manager from scratch. Rust execution-manager parity is
stale and deferred.

```text
MCP / CLI / tests
       │
       ▼
WorkflowExecutionManager
       │
       ├── execution registry
       ├── scheduler
       ├── persistence
       ├── progress publication
       ├── cancellation
       └── recovery
       │
       ▼
workflow engine / MethodRegistry
       │
       ▼
project-scoped data
```

### 11.1 Project-owned workflow execution identity

Each project database contains one current workflow and one current workflow execution.
The execution is identified by the project database plus its current workflow revision
and persisted lifecycle state. A public or generated `execution_id` is not required and
must not be used as a second workflow identity. An internal storage key is allowed only
as an implementation detail and must not be required by MCP callers.

Workflow edits are rejected while the project worker owns the database for an active
execution. Once the execution is terminal, changing the workflow creates a new workflow
revision. The normal run evaluates the ordered workflow from step zero, using valid cache
entries and materialized table snapshots to avoid rerunning unchanged prefix Methods.
`run_method(method, parameters)` may instead begin at the appended tail when the
persisted execution registry shows that the immediately preceding step is the matching
completed step for the current workflow/cache chain; otherwise it must fall back to the
step-zero evaluation.

### 11.2 Public lifecycle

Use these lifecycle semantics in the active C++ backend. A future Rust reopening must
reuse the same contract rather than changing it:

```text
queued
  │
  ▼
running ───────────────► completed
  │
  ├────────────────────► failed
  │
  └──► cancelling ─────► cancelled

backend termination while active ─► interrupted
queued cancellation ──────────────► cancelled
```

`interrupted` marks work that was active when the worker/backend stopped. The next worker may resume the latest execution from its completed step records and valid cache/materialized outputs; it must not rerun completed steps.

Allowed transitions must be centrally validated. Invalid transitions are programming errors and must be tested.

### 11.3 Execution launch snapshot

An active execution runs the stable definition captured when the worker acquires the
project. Since the project stores one mutable current workflow and does not need
concurrent executions, workflow mutation is rejected while the worker owns the database.
At the next terminal-to-queued run boundary, capture the resolved current workflow
steps/configuration in the execution parent or child step records before processing begins.

### 11.4 Externally triggered worker execution and console output

Workers are one-shot, externally triggered actors rather than a required long-lived
background scheduler loop. A caller creates the queued execution and invokes
`run_workflow` with a `worker_id`; the operation claims the project, executes the
workflow, persists step-wise progress, and releases the execution with an
owner-conditional terminal transition. The optional `worker_id` preserves the existing
direct synchronous run path when omitted.

Durable execution and step state are authoritative for the frontend progress bar.
Detailed processing diagnostics, including `find_features` output, remain the worker
console stream: structured JSON-RPC stays on stdout and native diagnostics are captured
from stderr, scoped to the worker's project by the caller.

At launch persist enough information to reproduce/identify the exact run:

- `domain_id` captured from the project;
- the current workflow identity/configuration hash;
- resolved method IDs and parameters for every planned step;
- launch parameters/overrides or a durable reference to them;
- backend implementation/version metadata where already available;
- resolved `module_id`/module version for each step where the module contract is available;
- semantic catalogue/projection version or hash sufficient to identify the contract used for the run.

Do not create a second workflow identity for execution. Audit history records workflow replacement; execution step records preserve the run snapshot.

### 11.4 Manager responsibilities

The manager owns:

- create/enqueue execution;
- validate project/domain/workflow at launch;
- schedule queued work;
- start worker execution;
- persist state transitions;
- expose list/get execution state;
- propagate cooperative cancellation;
- receive progress/current-step callbacks;
- finalize result/error state;
- reconcile active executions after process restart;
- enforce configurable concurrency policy.

The manager does **not** implement analytical Methods or domain rules.

### 11.5 Suggested backend-neutral manager API

The active C++ API should expose this behaviour. A future Rust reopening may implement
the same contract independently:

```text
start_workflow(database_path, parameters?) -> current execution state
get_workflow_execution(database_path) -> current execution detail
cancel_workflow(database_path) -> accepted/current state
run_scheduler_tick() / internal worker scheduling
reconcile_on_startup(database)
```

Internal workflow execution receives an execution context:

```text
database_path / project-owned execution context
report_progress(completed, total, message)
report_step(step_index, step_id, completed, total, message)
is_cancel_requested()
throw_if_cancelled()
```

### 11.6 Concurrency policy — implement the simple safe version first

Initial policy:

- configurable global maximum concurrent workflow executions across different files;
- **one writer-owned scheduler/worker per DuckDB file**;
- **exactly one current workflow execution per project/file**;
- separate DuckDB files may run concurrently in separate worker processes;
- additional triggers for the same project are coalesced into the current execution or rejected while active;
- direct read-only Operations remain outside this workflow queue unless they require a resource lock for correctness.

This policy matches DuckDB file-level write ownership: the worker that owns a file's read/write connection serializes execution-state and domain-data writes. Full parallelism is provided by workers owning different project files, not by multiple writer processes sharing one file.

Before claiming concurrent execution is complete, test multiple separate project files under real write workloads and verify that no process opens the same file read/write outside its owning worker.

Future optimization may derive finer conflict rules from semantic read/write metadata, but that is not part of the first manager milestone.

### 11.7 Cancellation

Cancellation is cooperative.

- queued execution: transition directly to `cancelled`;
- running execution: record request, transition to `cancelling`, signal cancellation token;
- worker checks cancellation at safe boundaries;
- complete/failed/cancelled/interrupted executions reject cancellation as already terminal;
- never kill an OS process merely to simulate normal cancellation;
- never leave a project transaction half-applied.

### 11.8 Progress

Progress originates in the workflow engine/Methods.

Persist/publish:

```text
overall completed / total / fraction when known
current step index
current step canonical method ID
current step completed / total / fraction when known
short progress message
updated_at
```

Requirements:

- progress must not go backwards except when a new step begins and only step-local progress resets;
- `total` may be unknown/null;
- progress writes should be coalesced/throttled so tight loops do not write DuckDB or emit MCP notifications for every data point;
- final lifecycle state is always persisted immediately.

### 11.9 Worker/process identity

Persist diagnostic runtime identity:

```text
process_id
server_instance_id
worker_id (optional)
heartbeat_at (recommended for active executions)
```

Rules:

- `process_id` is the OS process currently responsible for the run;
- a backend may run several executions in the same process, so duplicate PIDs are valid;
- PID is **not** durable identity because operating systems reuse PIDs;
- generate a new `server_instance_id` for each backend process start;
- recovery uses `server_instance_id` plus persisted active state (and heartbeat if implemented), not PID alone.

### 11.10 Restart reconciliation

At backend startup:

1. create a new `server_instance_id`;
2. inspect the singleton current execution row;
3. a queued current execution remains eligible for scheduling if its launch snapshot is valid;
4. a current execution recorded as `running`/`cancelling` under a previous server instance becomes `interrupted` and eligible for step-aware recovery;
5. persist an explanatory error/recovery message;
6. never silently mark an orphaned execution `completed`;
7. resume from the appended tail only when the immediately preceding execution record matches the current workflow/cache chain; otherwise re-evaluate from step zero and skip only valid cache/materialized outputs. Never trust a completion row without validating its cache/materialized output.

---

## 12. Workflow execution persistence

Refactor the existing workflow execution table into one latest-run parent plus durable child step records; do not create a competing job subsystem.

### 12.1 `WORKFLOW_EXECUTION` — latest workflow-run parent

Use the existing singular table name. There is one current execution for the project's one workflow; do not model a competing queue of user-visible executions. The logical contract should contain at least:

| Column | Purpose |
| --- | --- |
| `domain_id` | Project domain captured at launch. |
| `workflow_id` | Stable identity for the project's current workflow, not a version. |
| `workflow_hash` | Resolved workflow/configuration hash captured for this run. |
| `status` | `queued`, `running`, `cancelling`, `cancelled`, `completed`, `failed`, `interrupted`. |
| `created_at` | Execution creation timestamp. |
| `queued_at` | Queue timestamp. |
| `started_at` | Worker start timestamp. |
| `finished_at` | Terminal timestamp. |
| `progress_completed` | Overall completed units, nullable. |
| `progress_total` | Overall total units, nullable. |
| `current_step_index` | Active workflow step, nullable. |
| `current_step_id` | Canonical Method ID, nullable. |
| `step_completed` | Current step completed units, nullable. |
| `step_total` | Current step total units, nullable. |
| `progress_message` | Latest short status message. |
| `cancel_requested_at` | Cooperative cancellation request timestamp. |
| `process_id` | Current OS process PID, nullable before running/after reconciliation. |
| `server_instance_id` | Runtime instance owning active work. |
| `worker_id` | Optional thread/task/worker diagnostic identity. |
| `heartbeat_at` | Optional/recommended active execution heartbeat. |
| `backend` | `cpp` or `rust`, useful for diagnostics/history. |
| `result_reference` | Durable result/summary reference when needed. |
| `error_code` | Structured terminal error code, nullable. |
| `error_message` | User-readable terminal error, nullable. |
| `updated_at` | Last materialized state update. |

Do not persist redundant frontend-only presentation fields.

### 12.2 `WORKFLOW_EXECUTION_STEP` — durable child step records

Create one child row for every resolved workflow step in the latest execution:

| Column | Purpose |
| --- | --- |
| `workflow_revision` | Parent workflow revision captured for this run. |
| `step_index` | Stable order within the captured workflow. |
| `method` | Canonical registered Method ID. |
| `module_id` | Owning domain module captured at launch. |
| `module_version` | Owning module implementation/version where available. |
| `parameters` | Resolved parameter snapshot. |
| `parameter_hash` | Hash of resolved parameters. |
| `cache_key` | Deterministic cache/materialization key. |
| `status` | Queued/running/completed/failed/interrupted state. |
| `progress` | Step-local progress JSON. |
| `result_reference` | Durable result or materialized-output reference. |
| `error_code` / `error_message` | Structured failure details. |
| `started_at` / `completed_at` | Step lifecycle timestamps. |
| `updated_at` | Last step-state update and lease/heartbeat marker. |

The parent is completed only after every child step is completed. A worker restart marks active parent/child rows interrupted, preserves completed rows, restores valid cached outputs, and resumes from the first safely proven step; if no immediately preceding step match exists, it re-evaluates from step zero and skips only steps whose cache key and materialized output remain valid. Parent and child updates are scoped by the owning database file and current workflow revision; compare-and-transition updates must reject stale workers.

### 12.3 Optional `WORKFLOW_EXECUTION_EVENTS`

Add an append-only event table only when required for execution history/logging or reliable replay diagnostics. It is **not required for the first frontend milestone** because `WORKFLOW_EXECUTION` plus its step records are the authoritative current state.

If added:

```text
workflow_revision
sequence
created_at
event_type
step_id
completed
total
message
payload_json (optional)
```

The frontend must never reconstruct authoritative current state solely by replaying events.

### 12.4 Persistence transaction rules

- lifecycle transition + terminal result/error metadata must be atomic enough that a crash cannot produce impossible combinations;
- progress persistence may be throttled;
- execution state must be committed before returning the current state to MCP;
- cancellation request must be persisted before/with signaling the worker;
- project deletion must define behaviour for active executions: reject deletion while active in v1 unless a separately tested cancellation-and-delete workflow exists.

---

## 13. MCP contract for applications and agents

MCP remains generic and catalogue driven. `WorkflowExecutionManager` is surfaced through MCP but is not implemented inside the MCP adapter.

### 13.1 Required execution capabilities

Both MCP servers expose equivalent behaviour for:

```text
start workflow execution
list executions
get one execution
cancel execution
receive progress/state notifications
retrieve final result/error metadata
list project/domain capabilities
```

Use existing tool names where possible. If new tool names/schemas are required, define
them once in shared fixtures/semantic metadata and apply them to the C++ server. Do
not introduce `cpp_*` or `rust_*` execution tools.

### 13.2 Project identity in MCP

Because one DuckDB file contains one project, the normal MCP locator is `database_path` (or a server-issued project handle derived from it).

Rules:

- `create` stores optional caller identifiers only inside project metadata;
- project Operations that open a file resolve the sole stored project automatically;
- a connected project may remain a client convenience, but it must not become server-global authority;
- the MCP server may manage multiple open project files concurrently;
- execution commands identify the project by its database locator/handle; the project owns one current workflow execution.

Legacy tools must not accept a separate project identity. Migrate callers to `database_path` and place optional identifiers in project metadata.
### 13.3 C++ browser transport

Keep stdio for existing MCP/AI use. Add the supported browser-capable MCP transport
to the C++ MCP host. Do not add or maintain a Rust transport during the stale period.

Requirements:

- one React client implementation works with both;
- refresh/disconnect does not cancel runs;
- reconnect can list active executions and resume observation;
- server endpoint/version/capability handshake is explicit;
- transport code does not contain domain-specific dispatch.

### 13.4 MCP task/lifecycle mapping

Where the MCP protocol provides standard long-running task primitives, map StreamFind executions onto them instead of inventing conflicting transport-level states. `WorkflowExecutionManager` remains the source of truth and StreamFind-specific fields (`workflow_id`, step information, result references) remain available.

### 13.5 Large analytical data

Do not prematurely duplicate the whole API as REST solely for the frontend.

Implementation order:

1. use existing MCP Operations/results for metadata and modest data;
2. benchmark representative chromatograms, spectra, feature tables, 2D maps, and 3D data;
3. add paging/downsampling/viewport Operations where this solves the problem cleanly;
4. only if MCP payload overhead is demonstrated to be a bottleneck, add a versioned high-throughput data representation/handle behind the same backend-neutral contract.

MCP remains the control/semantic plane either way. React must not receive backend-specific file paths or implementation objects.

---

## 14. React frontend architecture

The first frontend is one client for the C++ backend. A later Rust reopening may reuse
the same frontend contract after the C++ MCP schemas are stable.

```text
frontend/
├── src/
│   ├── app/
│   ├── mcp/                 # transport + typed client + reconnect
│   ├── generated/           # generated/shared contract types if used
│   ├── projects/
│   ├── domains/
│   ├── workflows/
│   ├── executions/
│   ├── data/
│   └── visualization/
└── tests/
```

### 14.1 Frontend state ownership

Split state into:

**server authoritative:**

- projects;
- domains/capabilities;
- workflows/revisions;
- execution states/progress;
- persisted analytical results.

**frontend local:**

- selected project;
- active panel/tab;
- plot viewport/zoom;
- temporary form edits before submission;
- presentation preferences.

Never infer execution completion from a progress bar or browser timer.

### 14.2 Backend connection

The same production build must be able to connect to either backend endpoint.

Connection state should expose diagnostic information such as:

```text
backend implementation: cpp | rust
server version
semantic catalogue version
MCP/protocol version/capabilities
connection status
```

Do not branch domain/business behaviour on `backend === cpp` or `backend === rust`.

### 14.3 Domain-driven UI

For the selected project:

1. fetch project + immutable domain;
2. fetch effective project capabilities;
3. show only valid inputs/Operations/Methods/workflows;
4. validate forms from generated/shared parameter metadata where practical;
5. still expect backend validation errors and present them cleanly.

### 14.4 Workflow execution UI

Required views:

- launch workflow for a selected project;
- global execution monitor across opened project files;
- per-project-file execution history;
- execution detail with current step/progress/timestamps;
- cancel control for cancellable states;
- completed result/error summary;
- reconnect/refresh reconstruction from backend state.

The execution monitor should display at least:

```text
workflow revision
project
workflow/revision
backend
status
current step
progress
created/started/finished timestamps
process/server instance diagnostics when useful
result/error
```

### 14.5 Scientific visualisation strategy

Use the browser graphics ecosystem rather than building a scientific renderer from scratch.

- D3: scales, axes, selections, brushes, zoom/pan, data transforms where useful.
- Canvas/WebGL: dense chromatograms/spectra/scatter maps where SVG/DOM would be too large.
- Plotly or equivalent: high-value ready-made 2D/3D scientific views where it reduces implementation time.
- Avoid one DOM/SVG node per raw point for large datasets.
- Backend APIs should support viewport-aware reduction/paging when raw data substantially exceeds display resolution.

Initial visualisation priority:

1. analysis list + metadata;
2. TIC/BPC/EIC;
3. mass spectrum;
4. virtualized feature/result tables;
5. 2D RT × m/z/intensity views;
6. 3D views only after 2D interaction/data transport is stable.

### 14.6 Frontend development can start before backend completion

Once the MCP execution schemas and multiple-project-file fixture are frozen, the frontend agent may develop against:

- schema-generated TypeScript types;
- fixture/mock MCP responses;
- one available backend first.

Before merge to the active C++ milestone branch, the C++ end-to-end scenarios must pass;
Rust is excluded while stale.

---

## 15. Efficient implementation sequence — C++ plugin framework

This is the active implementation sequence. It starts from the current local state rather
than repeating completed migration work.

### Current starting state — accepted C++ baseline

The following work is already present and must be preserved:

- the maintained C++ tree is rooted at `cpp/`, with framework code under `cpp/core/`,
  domains under the intermediate `cpp/domains/`, tests under `cpp/tests/`, tools under
  `cpp/tools/`, and vendored dependencies under `cpp/vendor/`;
- C++ static and shared targets build through `cpp/CMakeLists.txt`, including the core,
  MassSpec, Raman, and Sensors targets, with Windows runtime output and install/export
  layout already verified;
- project-scoped DuckDB ownership, `PROJECT`, workflow persistence, cache/materialized
  output reconciliation, worker claims, cancellation, progress, and failure persistence
  are implemented in the current C++ baseline;
- semantic module ownership is projected as `module_id`, table manifests expose columns
  and types, and C++ table/schema validation uses generated catalogue metadata;
- C++ `MethodBinding`, `OperationBinding`, `DomainModuleBinding`, explicit catalogue
  snapshot registration, module ownership validation, schema hooks, and lifecycle
  metadata propagation exist;
- MassSpec uses explicit static descriptors for `mass_spec.base`,
  `mass_spec.chromatograms`, and `mass_spec.nta`; resolver/exclusion registration is no
  longer the intended ownership model;
- paired registration-contract coverage verifies the complete C++ registered set against
  the semantic catalogue and MCP intersection, including invalid binding rejection;
- the Rust tree and its prior tests remain in the repository but are stale. Rust changes,
  Rust parity, Rust MCP, and Rust plugin work are paused and must not block this sequence.

Do not restart the relocation, project-locator, table-manifest, workflow-parent/step,
worker-claim, or explicit-descriptor work as new phases. Audit the current files and
preserve their behaviour while extracting the next boundary.

### Stage 1 — Freeze the C++ ownership and dependency inventory

**Goal:** establish one authoritative map before moving domain code or public headers.

Inventory:

- `cpp/core/include`, `cpp/core/src`, and `cpp/core/CMakeLists.txt`;
- `cpp/domains/<domain>` headers, sources, targets, and public exports;
- `cpp/tools/streamfind-mcp.cpp` and all domain composition calls;
- `cpp/tests`, CMake test registration, generated catalogue copying, and runtime DLL
  placement;
- `semantic/ontology`, generated projections, domain tables, module ownership,
  migrations, and resource files;
- vendor and external dependencies used by each domain target;
- every core include or symbol that mentions MassSpec, Raman, Sensors, or a domain ID.

Classify each item as framework, SDK candidate, plugin implementation, MCP host,
semantic resource, test, vendor dependency, or temporary artifact. Record the result in
`.plans/cpp_layout_inventory.md` and update it rather than creating a second inventory.

**Gate C++-OWNERSHIP:** core/domain ownership, target ownership, include edges, semantic
ownership, and runtime/package paths are listed; no source move begins while duplicate
owners or hidden domain-to-core dependencies remain unexplained.

### Stage 2 — Make `cpp/core` domain-independent

**Goal:** compile and test the framework without MassSpec, Raman, or Sensors
implementation targets.

#### Stage 2 semantic catalogue artifact slice

Keep semantic authoring resources with their owning library while retaining the
Python validation/projection toolkit as the current development implementation:

- define a plugin catalogue metadata table and versioned artifact contract;
- make each plugin build produce its own validated `catalogue.duckdb`;
- implement the catalogue projector as a native SDK C++ component using the
  repository's RDF/Turtle parser and DuckDB API; Python must not be required by
  plugin builds once this component is accepted;
- make the generator accept core semantic input, one plugin semantic directory, and
  explicit JSON/matrix/DuckDB output paths;
- generate and validate separate catalogues for core, MassSpec, Raman, and Sensors;
- add a core-owned catalogue importer that merges validated plugin catalogues;
- import plugin catalogues during static composition before registering native bindings;
- test valid import, duplicate canonical IDs, wrong domain, wrong module, incompatible
  catalogue schema, missing plugin catalogue, and native/semantic intersection.

The released plugin artifact is the runtime-ready semantic contract. Apache Jena may
remain a user-scoped build-time validator under `.streamfind/tools/jena`; it is not a
runtime dependency. Python remains a development/differential oracle while the native
SDK projector is being proven, then is removed from the plugin build path. Native
projection must preserve the existing DuckDB schema, deterministic ordering, JSON
schemas, table manifests, module IDs, and catalogue metadata.

1. Move or extract only generic interfaces needed by plugins: registries, catalogue
   snapshot types, binding records, `ProjectTableStore`, execution/plugin context,
   logging/resource services, schema/migration coordination, and export/version metadata.
2. Remove domain-specific headers, source files, dispatch branches, target links, and
   test assumptions from `cpp/core`.
3. Keep explicit application composition outside core. Core may accept descriptors and
   registries, but must not enumerate MassSpec/Raman/Sensors IDs.
4. Keep the current `cpp/domains/` targets as an intermediate static composition while
   this isolation is proven; do not move directories and change ownership in the same
   patch unless the dependency audit permits it.

Likely files:

```text
cpp/core/include/streamfind/*.hpp
cpp/core/src/*.cpp
cpp/core/CMakeLists.txt
cpp/CMakeLists.txt
cpp/tools/streamfind-mcp.cpp
cpp/tests/CMakeLists.txt
```

**Validation:**

```text
scripts\build\build-core.cmd -Clean -Tests
scripts\build\build-core.cmd -Clean -Tests -CMakeArgs '-DSTREAMFIND_BUILD_SHARED=ON'
```

The core target must build without linking domain implementation targets. The aggregate
C++ test suite must remain green, including the registration-contract test and the full
shared runtime suite.

### Stage 3 — Define and package the C++ SDK

**Goal:** create the smallest public contract required by an independently built plugin.

Create the SDK only after Stage 2 identifies the actual public boundary:

```text
cpp/sdk/include/streamfind/sdk/
cpp/sdk/cmake/
cpp/sdk/templates/
cpp/sdk/examples/
cpp/sdk/CMakeLists.txt
```

The initial SDK surface should provide:

- `PluginDescriptor`, `DomainDescriptor`, and `ModuleDescriptor`;
- `MethodBinding` and `OperationBinding` registration interfaces;
- `TableContract`/owned-table declarations and schema/migration hooks;
- `PluginContext` and `MethodExecutionContext`;
- project-scoped table/database access;
- progress, cancellation, logging, resource, and error services;
- SDK/runtime version information and explicit ownership rules;
- CMake package/configuration support such as `find_package(StreamFindSDK)` and a
  `streamfind_add_plugin(...)` helper.

Do not expose internal `Project` implementation classes, analytical datasets, C++ STL
objects, `std::function`, JSON objects, or exceptions across a future binary boundary.
The first SDK may be statically consumed in-tree, but its public headers must be usable
without including private `cpp/core/src` headers.

**Gate C++-SDK:** a clean CMake install contains core/runtime libraries, SDK headers,
CMake package files, generated catalogue resources, and no domain implementation header
is required merely to compile an SDK consumer.

### Stage 4 — Make MassSpec the reference C++ static plugin

**Goal:** organize one domain plugin without rewriting algorithms or adding a second
execution path.

Target location:

```text
cpp/plugins/mass_spec/
├── CMakeLists.txt
├── include/
├── src/
│   ├── plugin.cpp
│   ├── base_*.cpp / base_*.hpp
│   ├── chromatograms_*.cpp / chromatograms_*.hpp
│   └── nta_*.cpp / nta_*.hpp
├── semantics/
├── migrations/
├── workflows/
├── resources/
├── tests/
└── plugin.json
```

During extraction, preserve the current flat module-prefixed source convention and
logical modules `base`, `chromatograms`, and `nta`. A single MassSpec plugin library is
preferred; do not create one library per algorithm or one subdirectory per logical
module without a measured dependency/build reason.

Move or associate with the plugin:

- native readers and processing implementations;
- explicit module descriptor collections;
- MassSpec table contracts and schema bindings;
- MassSpec ontology/resources when the generator can consume package-local semantics;
- migrations, workflow templates, and runtime resources;
- MassSpec-specific tests and external dependencies.

The plugin must consume the SDK and generic core services. Core must not include
MassSpec headers or call MassSpec registration by capability ID.

**Gate MASS-SPEC-STATIC:** the MassSpec plugin registers all current C++ capabilities
through descriptors, preserves table ownership and workflow behaviour, and requires no
new generic Project/MCP dispatch for a new Method or Operation.

### Stage 5 — Move schema lifecycle behind the plugin contract

**Goal:** plugins declare data contracts while core owns project/database lifecycle.

1. Keep semantic `Table`/Column declarations authoritative for public meaning.
2. Let the plugin provide owned table IDs, DDL/migration definitions, schema version,
   and dependencies through its descriptor.
3. Let core validate module dependencies, install/upgrade schemas transactionally,
   persist `MODULE_SCHEMA`, and enforce project/file ownership.
4. Do not let a plugin open arbitrary DuckDB files or bypass `ProjectTableStore`.
5. Do not let capability discovery silently mutate a project.
6. Require a valid module schema before a mutating Method executes.

Add a small C++ proof plugin capability consisting of one module-owned table, one Method
that creates/updates it, and one Operation that reads it. Prove valid schema, missing
schema, missing column, incompatible type, and project-file isolation paths through
production code without editing generic MCP or Project business logic.

**Gate TABLE-LIFECYCLE:** table declarations, schema installation, migrations, ownership,
Method effects, Operation reads, and MCP discovery agree for the proof plugin.

### Stage 6 — Make C++ MCP a thin plugin host

**Goal:** MCP exposes registries and core services, not domain implementations.

1. Move the MCP executable toward `cpp/mcp/src/`; keep `cpp/tools/` only for generic
   command-line utilities during the transition.
2. Startup loads one immutable generated catalogue snapshot.
3. Startup creates core services and registries, then composes shipped static plugin
   descriptors explicitly.
4. MCP derives capabilities from the semantic/native intersection.
5. No MassSpec-specific tool list, switch, resolver loop, or dispatch branch may be
   added to MCP.
6. Keep one explicit composition entry per shipped plugin; adding a capability inside a
   plugin must not modify generic MCP code.

**Validation:** run C++ `tools/list`, method discovery, registration-contract tests,
MassSpec interface tests, MCP smoke tests, and the complete CTest suite.

### Stage 7 — Prove standalone SDK/plugin development

**Goal:** demonstrate that the SDK is a real boundary rather than an in-repository
include convention.

1. Install the C++ core/runtime and SDK to `tmp/scratch/install-cpp-sdk`.
2. Build a minimal proof plugin in a separate CMake build against the installed SDK.
3. Register its descriptor with a generated catalogue snapshot.
4. Run it through the C++ MCP host and a disposable project under `tmp/projects/`.
5. Verify the install/package contains the expected core runtime, SDK headers, CMake
   package files, plugin artifact, generated catalogue, and Windows `duckdb.dll`.

**Gate SDK-CONSUMABLE:** an out-of-tree C++ plugin builds and registers without
including the StreamFind source tree or modifying core/MCP source.

### Stage 8 — Package static plugins for development and release

Define the runtime package shape before implementing discovery:

```text
mass_spec/
├── plugin.json
├── streamfind_mass_spec.dll
├── semantics/
├── migrations/
├── workflows/
├── resources/
└── dependencies/
```

The manifest may declare plugin ID, name, version, SDK/runtime compatibility, library,
domain, semantic resources, migrations, dependencies, and platform compatibility. At
this stage the manifest is packaging/inspection metadata; static composition remains the
execution mechanism.

Verify CMake install and release staging with repository-local temporary paths. Do not
claim runtime plugin discovery merely because shared domain DLLs can be built.

### Stage 9 — Design dynamic loading only after the static C++ gates

Dynamic loading is a separate future phase, unblocked only after `C++-OWNERSHIP`,
`C++-SDK`, `MASS-SPEC-STATIC`, `TABLE-LIFECYCLE`, and `SDK-CONSUMABLE` pass.

Design and approve separately:

- C ABI entry point and ABI version negotiation;
- allocator and string/buffer ownership;
- structured error transport without C++ exceptions;
- descriptor serialization/registration boundary;
- manifest discovery and dependency resolution;
- plugin search paths, trust/security policy, and unload/reload rules;
- Windows DLL dependency/package layout;
- compatibility and deprecation policy.

Do not pass STL, `std::function`, JSON, or exceptions across the DLL boundary. Do not
implement `PluginManager` or remove static linkage before this ABI is approved and
covered by a standalone loading test.

### Deferred Rust lane

Rust is not part of the active sequence. Preserve its current source, tests, and
artifacts, but do not:

- port the C++ SDK;
- reorganize Rust crates or module ownership;
- add Rust MCP/browser transport;
- require Rust parity in C++ gates;
- change Rust generated/semantic consumers for C++ plugin work.

When the C++ SDK/runtime/plugin/package contracts are accepted, create a new Rust
reopening plan with its own inventory and acceptance gates. That plan must consume the
stabilized public semantic contract independently; it must not turn Rust into a C++
runtime dependency.

### C++ parallel work that is allowed now

- Continue MassSpec reader/public-interface hardening in its dedicated worktree.
- Continue C++ WorkflowExecutionManager recovery/cancellation hardening against the
  existing project-scoped contract.
- Add frontend mock work against the C++ MCP schema, without direct DuckDB access.
- Update semantic declarations when required by a C++ plugin capability, then regenerate
  and validate the projection.

These parallel slices must not create a second C++ runtime, bypass the SDK/plugin
boundary, or modify the stale Rust lane.

### Active C++ validation order

Run serially from the repository root:

```text
scripts\build\clean-build-temp.cmd
scripts\build\build-core.cmd -Clean -Tests
scripts\build\build-core.cmd -Clean -Tests -CMakeArgs '-DSTREAMFIND_BUILD_SHARED=ON'
cmake --install tmp/build/core-default --config Debug --prefix <repo>\tmp\scratch\install-cpp
```

For every C++ plugin slice, also run semantic validation/projection freshness, the
focused affected CTest target, the complete CTest suite, `git diff --check`, and any
standalone SDK/plugin build. Rust validation is optional diagnostic evidence only and
must not be reported as an active acceptance gate.

**Stage 4 progress (2026-09-10): Complete.** MassSpec `base`, `chromatograms`, and `nta`
descriptors declare their owned table sets through the SDK contract; the final expanded
catalogue validates those ownership declarations; all ten C++ tests pass; the installed
SDK consumer configures, compiles, links, and runs; and the installed MCP host serves
`tools/list` from the installed aggregate catalogue. Runtime DLL loading remains deferred.

**Current next slice:** begin Stage 7 by proving SDK consumability as a real development
boundary. Install the C++ core/runtime and SDK, build an out-of-tree plugin against only
the installed SDK package, register it with a generated catalogue snapshot, execute it
through the installed static host on a disposable project, and verify the complete
installed package. Rust catalogue validation, projection, and plugin work remain deferred
until the C++ plugin and package gates are accepted.

**Static plugin boundary note (2026-09-10):** Raman and Sensors now use the same SDK
`DomainModuleBinding`/`register_module` path as MassSpec, with explicit `raman.base` and
`sensors.base` module identities. Registration-contract coverage verifies Raman's two
semantic operations and Sensors' currently empty executable intersection against the
aggregate catalogue. Sensors still has no semantic executable capabilities; adding one
requires a corresponding implementation and catalogue declaration rather than a
placeholder dispatch path.

**Stage 5 progress (2026-09-10): Partial.** `DomainModuleBinding::schema_binding` now
receives a project-scoped `ProjectTableStore`; registration validates and binds module
capabilities without executing schema side effects; and core exposes
`install_module_schema(...)` for explicit installation against the caller's project.
Installation now uses a single transaction-scoped project/store connection, persists
versioned `MODULE_SCHEMA` state, runs registered upgrades, and verifies every declared
table was created before commit. MassSpec base, chromatogram, and NTA operation paths
now use manifest-backed module installers; analysis and NTA/chromatogram writes use the
transaction/batch path; the registration contract test covers installation and version
1-to-2 migration into a disposable project file; the focused and complete C++ suites
pass. NTA load/persist paths now require an installed module manifest before operating.
Transaction-scoped query and execute paths now use the transaction connection and reject
SQL that does not reference an owned module table. All NTA table creation has been removed
from mutating implementation paths; direct NTA paths require the installed manifest.
Stage 5 is complete for the active C++ backend. Direct base, chromatogram, and NTA
Methods require installed manifests; schema creation is confined to module installers;
transaction-scoped query/execute uses one connection with ownership checks; writes are
batch/set-based; and the production contract tests cover missing tables, missing columns,
incompatible types, migration rollback, foreign-table SQL rejection, and project-scoped
schema installation/isolation. The official C++ build and CTest gate passes 10/10.

**Stage 6 progress (2026-09-10): Complete for the active C++ static host.** The MCP host and
static composition sources now live under `cpp/mcp/src/`; MCP startup delegates core/plugin
catalogue loading and static composition to that plugin-host boundary instead of owning the
domain catalogue import loop. MCP smoke coverage verifies the semantic/native operation
intersection across MassSpec and Raman, confirms Sensors contributes no tools while its
native intersection is empty, and verifies workflow-method discovery separately from
operation discovery. Registration-contract coverage rejects missing, wrong-kind,
wrong-domain, and wrong-module bindings plus duplicate, malformed, incompatible, and
wrong-domain/plugin catalogue imports. The C++ build and all ten CTest tests pass. The
installed MCP host was executed with an absolute installed aggregate catalogue and served
MassSpec and Raman operations while excluding Sensors, matching the installed semantic and
native intersection. The installed SDK plugin helper was also verified with an out-of-tree
consumer that configures, compiles, and links using installed SDK headers, the exported
`streamfind::sdk` target, and `streamfind_add_plugin(...)`. Standalone external-plugin
execution is Stage 7; dynamic DLL loading remains deferred.

**Stage 7 progress (2026-09-11): Complete for the active C++ backend.** Windows and Linux
installed SDK/package boundaries pass. An out-of-tree proof plugin configures, compiles,
links, and executes against installed headers and CMake exports on both platforms; the
installed MCP host serves `tools/list` from the installed aggregate catalogue. Windows
and Linux package payloads include the SDK exports, catalogues, semantic resources,
licences, Jena resources, and platform-appropriate DuckDB assets. Arbitrary dynamic
plugin loading remains deferred to Stage 9.

**Stage 8 progress (2026-09-11): Complete for the active C++ backend.** MassSpec, Raman,
and Sensors have versioned static-plugin manifests installed beside their plugin
catalogues. Release validation parses and checks manifest identity, domain, version,
library, catalogue, and static-composition metadata. Windows ZIP validation passed with
`duckdb.dll`/`duckdb.lib`; Linux TGZ validation passed with the vendored static DuckDB
archives. Both release lanes passed the complete C++ CTest suite (10/10), and package
assertions cover the installed SDK, catalogues, semantic resources, licences, and CMake
exports. Static composition remains the execution mechanism; Stage 9 is now unblocked
for dynamic-loading design only.

**Current next slice:** begin Stage 9 by documenting and reviewing the dynamic-loading
boundary—C ABI/version negotiation, ownership, descriptor serialization, discovery,
dependency/security policy, unload rules, and platform packaging—without implementing
`PluginManager` or removing static linkage before the ABI and standalone loading test
are approved.

**Batch-write invariant (2026-09-10):** DuckDB creation and updates must use transaction
scopes and batch operations. MassSpec analysis insertion now collects rows and calls one
typed `append_rows()` batch; analysis removal uses one set-based delete. NTA and
chromatogram paths already use batch append APIs. New schema/data paths must not add
per-row SQL writes.

---

## 16. Recommended worktree/agent split

Use one branch + worktree + agent session per owned slice. Avoid two agents editing the same shared contract files concurrently.

```text
main/dev_refactoring integration branch

wt-contract
  branch: refactor/execution-contract
  owns: shared fixtures, semantic/runtime contract docs, lifecycle names

wt-multiproject-cpp  # historical branch name: separate-file project persistence
  branch: refactor/cpp-multiproject
  owns: C++ project metadata persistence migration

wt-domain-contract
  branch: refactor/domain-module-contract
  owns: shared binding/module semantic contract + fixtures

wt-domain-cpp
  branch: refactor/cpp-domain-modules
  owns: C++ MethodBinding/OperationBinding/ProjectTableStore + MassSpec module split

wt-execution-cpp
  branch: feature/cpp-workflow-execution-manager
  owns: C++ manager + execution persistence + tests

wt-mcp-cpp
  branch: feature/cpp-mcp-execution
  owns: C++ MCP exposure/HTTP transport

wt-frontend
  branch: feature/react-frontend
  owns: frontend/ only + generated contract consumption

wt-readers
  branch: hardening/mass-spec-readers
  owns: reader plan slice
```

### Merge order

```text
contract
  ↓
single-project-file C++ + locator amendment A1 [complete]
  ↓
┌──────────────────────────────┬──────────────────────────────┐
│ C++ SDK/plugin boundary      │ C++ execution hardening       │
│ ownership + proof plugin     │ WorkflowExecutionManager     │
└──────────────────────────────┴──────────────────────────────┘
  ↓
C++ MCP execution/transport + conformance
  ↓
frontend foundation
  ↓
scientific visualisation + release hardening
```

Frontend scaffolding/mock work may start after the contract merge, but production integration waits for MCP conformance.

### Conflict avoidance

- `wt-contract` is the only worktree that changes shared lifecycle/fixture names until merged.
- C++ plugin branches should not modify generated semantic artifacts independently; regenerate after the shared contract merge. Rust is stale and must not modify the active projection.
- Frontend must consume generated/shared schemas and should not edit backend code.
- Reader branch must not opportunistically refactor workflow execution.
- Domain-module branches own registration/table-access refactors; execution-manager branches should consume the new context only after the shared binding contract merges.
- Only the shared domain-contract worktree changes `sf:providedByModule`/module projection semantics until that contract is merged.

---

## 17. C++ conformance matrix

The following scenarios must be executable against the active C++ backend with the
semantic catalogue as the expected public contract. A future Rust reopening may reuse
this matrix; it is not a current gate.

### Project/database

| Scenario | Expected behaviour |
| --- | --- |
| Create mass-spec + Raman project files | Each file contains one project with its immutable domain and expected domain schema. |
| Reopen project file | Its sole project and domain are preserved and validated from the file without an external project selector. |
| Create a second project in one file | Rejected before a second PROJECT row can be written. |
| Domain-invalid Method or table schema | Rejected before execution/use. |
| Cache same workflow in two files | Cache identity remains project/file-scoped. |

### Workflow execution

| Scenario | Expected behaviour |
| --- | --- |
| Start workflow | Returns committed current execution state; state becomes queued/running. |
| Two workflow triggers same project file | At most one current execution; later work is coalesced or rejected while active. |
| Workflows in different project files | Independent project-owned lifecycles; may run concurrently. |
| Progress | Backend reports current execution/step progress with equivalent semantics. |
| Cancel queued | Becomes cancelled without running. |
| Cancel running | running -> cancelling -> cancelled at safe boundary. |
| Method failure | Execution becomes failed with structured error. |
| Backend process restart | The singleton active workflow becomes interrupted; it resumes from a proven preceding step when possible, otherwise re-evaluates from step zero and skips only valid cache/materialized outputs. |
| Browser/MCP disconnect | Execution continues. |
| Reconnect | Client lists/fetches current state and resumes observation. |

### MCP/frontend

| Scenario | Expected behaviour |
| --- | --- |
| tools/capabilities | Same canonical capability IDs for same backend coverage. |
| Execution schemas | Same field names/types/state semantics. |
| Backend switch | Deferred until the Rust implementation is explicitly reopened. |
| Multiple-file monitor | Shows executions across project files without backend-specific business logic. |
| Cancel from UI | Cancels the selected project's current workflow execution. |

---

## 18. Test strategy and gates

### Lightweight native suites

Keep small, deterministic fixtures under repository tests for:

- lifecycle transition validation;
- project-file SQL scoping and expected domain tables;
- execution table persistence/reopen;
- cancellation/progress primitives;
- registry/catalogue parity;
- MCP protocol schemas;
- domain enforcement.

### C++ conformance tests

Prefer one scenario definition executed against the C++ implementation and semantic
fixtures. Do not add parallel Rust expectations while that lane is stale.

Normalize expected backend-specific diagnostics before diffing, but never normalize public state/schema differences that should be identical.

### Development/data-backed tests

Keep large/vendor/NTA performance datasets in `streamfind.data` and invoke through `scripts/dev/`.

Add explicit dev scenarios for:

```text
two project files + concurrent workflows
large TIC/EIC viewport query
large feature table paging
backend restart/interrupted execution recovery
C++ MCP end-to-end workflow and plugin discovery
```

### Local aggregate command

If not already present, add one thin documented local aggregate command that runs:

```text
semantic validation/freshness
C++ native suite
shared C++ MCP conformance
frontend unit tests
frontend E2E against C++
```

Do not duplicate test logic inside the aggregate wrapper.

---

## 19. Performance guidance

### Backend execution

- Avoid opening/closing a new database for every progress callback.
- Use batched persistence for analytical results as already established by the NTA work.
- Coalesce progress updates.
- Keep workflow scheduling metadata small.
- Do not copy large result tables into `WORKFLOW_EXECUTION` or its child step records; store a result/materialized-output reference.

### Project-file SQL and writer ownership

- Do not require domain callers to pass a project identity; the open database file already selects the sole project.
- Prefer prepared/bound values and ProjectTableStore filters rather than string-built project predicates.
- Never open the same DuckDB file read/write from more than one worker process.
- Serialize execution-state and domain-data writes through the file's owning worker.
- Add indexes/keys only after examining actual query plans/use; do not add speculative indexes to every table.

### Frontend large data

- Never render millions of DOM/SVG nodes.
- Virtualize long tables/lists.
- Request only visible/needed analytical ranges where possible.
- Downsample/aggregate server-side when screen resolution makes raw resolution invisible.
- Keep plot interaction state local; keep analytical data/result truth on the backend.
- Benchmark before introducing a second transport.

---

## 20. Error and recovery model

Both backends should converge on equivalent public error categories for the new runtime layer:

```text
project_not_found
project_domain_mismatch
workflow_not_found
invalid workflow snapshot or step configuration
workflow_invalid
execution_not_found
execution_not_cancellable
execution_state_conflict
execution_interrupted
backend_busy / queue capacity if implemented
database_error
method_error
```

Exact internal exception/error types may differ. MCP must expose equivalent structured meaning.

Do not leak raw SQL, C++ exception class names, Rust panic details, or native stack traces as the public error contract. Preserve detailed diagnostics in logs where appropriate.

---

## 21. Target repository shape

The active target shape is C++-first. C++ semantic resources are co-located with their
owners under `cpp/core/semantic` and `cpp/plugins/*/semantic`; generated catalogues are
build artifacts. Rust remains a deliberately broken stale implementation until its
native catalogue pipeline is implemented independently.

```text
streamfind/
├── cpp/
│   ├── core/
│   │   ├── include/streamfind/       # framework implementation/public runtime API
│   │   ├── src/                      # Project, DuckDB, workflows, registries, services
│   │   ├── semantics/                # core semantics after controlled extraction
│   │   ├── tests/
│   │   └── CMakeLists.txt
│   ├── sdk/
│   │   ├── include/streamfind/sdk/   # stable extension contract
│   │   ├── cmake/
│   │   ├── templates/
│   │   ├── examples/
│   │   └── CMakeLists.txt
│   ├── plugins/
│   │   ├── mass_spec/                # one domain plugin; base/chromatograms/nta modules
│   │   ├── raman/
│   │   ├── sensors/
│   │   └── generic/                  # only if justified by actual ownership/build needs
│   ├── mcp/
│   │   ├── src/                      # thin C++ composition/transport host
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt
├── frontend/                          # later React client; never direct DuckDB access
├── bindings/
│   ├── python/
│   └── r/
├── integrations/
│   └── cf-streamfind/
├── tests/fixtures/
├── scripts/
├── docs/
├── .plans/
└── rust/                               # preserved stale implementation; not active now
```

During the transition, `cpp/domains/` is an accepted intermediate location. Move a
whole domain into `cpp/plugins/<domain>/` only after its ownership inventory, SDK
surface, CMake target, semantic resources, table contracts, migrations, and tests are
ready. A relocation is atomic: there must be one source owner and one registration
path after the move.

The runtime package for one domain plugin should eventually contain:

```text
mass_spec/
├── plugin.json
├── streamfind_mass_spec.dll
├── semantics/
├── migrations/
├── workflows/
├── resources/
└── dependencies/       # only declared runtime dependencies
```

SDK headers and CMake package files belong to the development SDK, not the runtime
plugin package.

Exact filenames/classes should follow existing component conventions rather than
creating new top-level abstractions unnecessarily.

---

## 22. Definition of done

### Single-project-file persistence and identity are done when

- every DuckDB file contains exactly one `PROJECT` row;
- normal public/MCP calls open the project from `database_path` alone;
- optional caller identifiers remain inside project metadata rather than becoming selectors;
- C++ passes the separate-file isolation fixtures; Rust results are retained as historical reference only;
- workflow/cache/audit/domain data remain scoped to the file's sole project;
- files with zero/multiple project rows or invalid immutable domains are rejected;
- reopen/migration/legacy-compatibility tests pass.

### Domain extensibility is done when

- C++ has generic `MethodBinding` and `OperationBinding` registration paths;
- C++ MassSpec Methods/Operations are composed from domain modules rather than one monolithic registration switch/match;
- new domain tables are owned by module schema migrations rather than generic Project schema code;
- module schema versions are persisted independently of generic Project schema version;
- project-scoped table access uses the owning database file and does not inject a dedicated project identity;
- semantics carry `module_id`/module ownership and generated C++ conformance can show semantic/native binding parity;
- a proof Method/Operation with a new table is added without generic core, generic MCP, or frontend business-logic changes.

### WorkflowExecutionManager is done when

- C++ independently implements the lifecycle;
- execution creation is durable before ID return;
- queue/global concurrency/per-project concurrency work;
- progress/current step are persisted and observable;
- queued and running cancellation work;
- different project files have independent execution lifecycles;
- restart reconciliation marks orphaned active runs interrupted;
- process/server identity is stored correctly;
- C++ lifecycle conformance tests pass; Rust lifecycle parity is deferred.

### MCP execution surface is done when

- the execution tools/schemas work against the C++ server;
- browser-capable transport is available in both;
- disconnect/reconnect does not lose execution state;
- no per-domain/method execution dispatch is added;
- the C++ shared conformance harness passes.

### React frontend foundation is done when

- the initial build connects to the C++ backend;
- user can manage/select projects and see domain-specific capabilities;
- user can launch workflows in multiple project files;
- global execution monitor shows lifecycle/progress;
- cancellation targets the correct execution;
- refresh/reconnect reconstructs state from backend;
- E2E scenarios pass against the C++ backend.

### Scientific frontend is done for first release when

- required TIC/BPC/EIC/spectrum/table views handle representative real datasets interactively;
- large tables are virtualized/paged;
- large plots do not create raw-point DOM explosions;
- backend viewport/downsampling exists where measurements require it;
- C++ provides the frontend-visible data contracts; Rust parity is deferred.

---

## 23. Definition of done for a domain capability

A domain Method or Operation is complete only when:

- one canonical qualified semantic ID exists;
- semantic type is explicit (`sf:Method` or `sf:Operation`);
- label, definition, domain, parameters, results, shared errors, and mappings are in Turtle;
- SHACL/semantic validation passes;
- representative fixture coverage exists;
- the active C++ implementation registers and tests an independent executor under the same ID through its owning domain module binding;
- semantic projection, module ownership, and registry agree;
- cross-backend behaviour is not a current gate; a later Rust reopening must add parity tests before claiming equivalence;
- Methods participate in workflow validation/execution;
- Operations remain direct Operations and never become workflow steps;
- MCP exposure/dispatch remains generic;
- frontend mappings are generated/derived where practical rather than duplicated;
- any new domain-owned table is declared semantically and installed/migrated by the owning module rather than generic core schema code;
- packaging/documentation are updated when public release requires them.

---

## 24. Maintenance acceptance criteria

The architecture remains healthy only if these stay true:

- **Add Method to existing domain/module:** semantics + module implementation/binding + tests; no generic core/MCP/frontend business-logic changes.
- **Add Operation to existing domain/module:** semantics + module implementation/binding + tests; no generic MCP dispatch changes.
- **Add domain table:** semantics + owning module migration/binding + tests; no generic Project schema change.
- **Add processing module:** semantic module metadata + C++ module descriptor + one C++ application composition entry; no generic framework changes. Rust is deferred.
- **Add domain:** semantic declaration + C++ domain plugin + one C++ application composition entry.
- **Change docs/parameter contract:** edit semantics once; generated/public metadata follows.
- **Add backend:** a future backend may consume the same semantic projection only after the C++ plugin/runtime contracts are stable.
- **Backend-neutral frontend:** the first React build operates the C++ MCP endpoint; multi-backend support is deferred.
- **Execution durability:** browser disconnect/reconnect does not terminate or lose runs.
- **Single-project identity:** one DuckDB contains exactly one project; `database_path` locates it and optional identifiers remain metadata-only.
- **Domain enforcement:** the C++ domain/plugin controls valid inputs, Operations, Methods, and workflows.
- **Execution contract:** the C++ public lifecycle/progress/cancellation schemas are stable before Rust parity resumes.
- **No hidden drift:** tests reject semantic executables without registered implementations and registered public capabilities without semantics.
- **Traceability:** generated C++ conformance identifies domain/module ownership and native binding coverage for every public capability.

---

## 25. Deliberately avoided in this milestone

Do not introduce:

- Rust calling/wrapping C++;
- separate main C++ and Rust web frontends;
- Qt/Slint/egui as the primary frontend path for this milestone;
- Python/FastAPI as a required layer between React and the native backends;
- a second metadata registry competing with `semantic/`;
- method-specific MCP switch/match dispatch;
- runtime RDF parsing in every backend;
- a new ad hoc job database separate from existing StreamFind DuckDB execution persistence;
- automatic workflow resume after process crash;
- process killing as normal cancellation;
- one workflow worker per OS process as a requirement;
- fine-grained table-level workflow scheduling before the simple per-project policy is proven insufficient;
- a second high-throughput transport before profiling shows it is needed;
- frontend direct DuckDB access;
- backend-specific React business logic;
- dynamic domain plugin ABI work without a concrete distribution requirement;
- a heavyweight ORM or generic query language in place of a thin DuckDB-oriented `ProjectTableStore`;
- forcing every MassSpec logical module into its own library/crate before dependency/build measurements justify it.

---

## 26. Immediate next actions

Execute these in dependency order, allowing the two independent backend workstreams to run in parallel where indicated:

1. **C++ ownership audit:** classify current `cpp/core`, `cpp/domains`, `cpp/tools`, tests, semantics, and CMake targets against the proposed core/SDK/plugins/MCP structure; do not move files until duplicate ownership and dependency edges are recorded.
2. **C++ registry/SDK seam:** extract the smallest public SDK contract and prove that core services, registries, project-scoped table access, execution context, logging, and schema coordination do not depend on domain headers.
3. **Proof-of-extensibility capability:** add one small module-owned table, one Method that creates/updates it, and one Operation that reads it without generic core/MCP edits; prove valid and missing-schema paths in C++.
4. **Static MassSpec plugin package:** organize MassSpec as one domain plugin with logical `base`, `chromatograms`, and `nta` modules, keeping the existing explicit descriptors and flat module-prefixed source convention until package/build measurements justify further splitting.
5. **Standalone SDK/plugin build:** install the C++ core/runtime and SDK, then build a minimal plugin against the installed package in a separate CMake build.
6. **WorkflowExecutionManager gap audit:** compare the substantial local C++ implementation against Sections 11–12; implement only missing lifecycle/persistence/concurrency/recovery pieces.
7. **MCP execution/browser transport:** expose the hardened manager from the C++ host and run the C++ conformance suite.
8. **React scaffolding/integration:** consume the semantic/MCP contract, not backend-specific lists; project identity is database-path based.
9. **Scientific visualization:** add paged/viewport-aware analytical data access and D3/Canvas/WebGL/Plotly views after transport measurements.
10. **Continue MassSpec reader hardening independently** in its dedicated worktree.

The critical path is now:

```text
A1 project-locator simplification [complete]
   ├──> static descriptor + ProjectTableStore [complete] ──> C++ ownership/SDK seam
   │
   └──> WorkflowExecutionManager gap audit/hardening
                    │
                    └──────────────┬─────────────────
                                   ▼
                       common MCP execution/browser transport
                                   ▼
                              React frontend
                                   ▼
                      scientific visualization/release
```

The extensibility lane and execution-manager hardening lane should meet at the execution context boundary: Methods obtain progress/cancellation/execution identity from `MethodExecutionContext` and project data access from `ProjectTableStore` without coupling either concern to MCP.

Historical bootstrap/superseded architecture belongs in commit history or `.plans/completed/`, not in this active implementation roadmap.

---

## 27. Current development fronts

The next development stage is intentionally split into four independent fronts.
Detailed plans are separate handoff documents so each front can be developed by an
independent agent without turning this migration roadmap into an implementation log.

### Front 1 — Frontend

Detailed plan: [`.plans/frontend_plan.md`](frontend_plan.md)

Create the shared React/TypeScript frontend against the public C++ MCP/service
boundary. The frontend must not read DuckDB directly, import native/plugin
internals, or maintain a duplicated capability list.

### Front 2 — MassSpec reader verification

Detailed plan: [`.plans/expansion_mass_spec_reader_plan.md`](expansion_mass_spec_reader_plan.md)

Continue native reader verification, corpus coverage, malformed-input handling,
calibration and indexed/lazy-read checks, and public C++ interface validation.
This is the reader front that remains assigned to the current development lane.

### Front 3 — MassSpec processing methods

Detailed plan: [`.plans/mass_spec_processing_methods_plan.md`](mass_spec_processing_methods_plan.md)

Continue the C++ MassSpec processing-method implementation while preserving the
existing method IDs, parameters, defaults, result schemas, persistence behavior,
and dynamic plugin boundary. The detailed plan is intentionally empty and will
be authored by the collaborating agent.

### Front 4 — Sensors framework

Detailed plan: [`.plans/sensors_framework_plan.md`](sensors_framework_plan.md)

Develop the Sensors domain framework within the deterministic plugin layout,
using the SDK project-access and semantic-catalogue contracts without adding
Sensors-specific logic to generic core or MCP routing. The detailed plan is
intentionally empty and will be authored by the collaborating agent.

### Coordination rules

- These four fronts may proceed in parallel within their ownership boundaries.
- New public capabilities must originate in the owning semantic source and pass
  the SDK/plugin build and packaged-MCP validation gates.
- Frontend, processing-method, and Sensors planning belongs in their new topic
  files; do not recreate those plans in this roadmap.
- Reader work continues in `expansion_mass_spec_reader_plan.md`.
- Rust remains stale/deferred and is not a dependency of these fronts.
