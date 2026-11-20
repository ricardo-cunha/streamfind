# streamfind — Integrated Extension Development Platform
## Stepwise refactoring plan for `dev_refactoring`
**Planning baseline:** 20 November 2026  
**Target branch:** `dev_refactoring`  
**Status:** implementation proposal, not a claim of completed functionality  
**Repository:** https://github.com/ricardo-cunha/streamfind/tree/dev_refactoring

## 0. Executive decision and non-negotiable boundaries

Every native streamfind release shall include a first-class, optional development entry point. A user can run `streamfind dev setup`, generate a plugin containing one or more operations, build it with the SDK matching the installed runtime, test and install it, and make its operations discoverable to the existing service, CLI, MCP clients, and browser app **without rebuilding streamfind-core**. A parallel development path supports independent, custom frontend **viewers** for typed scientific results.

**Separate extension kinds:**
1. **Native processing plugin:** one deployable C++ dynamic library/package containing one or more operations. Operations declare typed inputs, parameters, outputs, result contracts, and any permitted table/schema registration.
2. **Frontend viewer extension:** a separately deployable, compiled TypeScript/React package. A viewer declares supported domain, module/capability, artifact/result contract, representation, and compatible frontend API. It renders data but does not implement scientific processing.
3. Both may be distributed together as an optional convenience collection, but **retain independent registration, versioning, activation and SDKs**. Neither requires the other.

**Ownership:** The core service continues to own project sessions, workflows, DuckDB metadata, operation registry, worker supervision, and result publication. The new developer executable owns toolchain provisioning, scaffolding, compilation, testing, validation, packaging, and installation. CLI and MCP delegate to a shared development command/service implementation. Avoid a second scientific execution path.

**Critical correction from earlier discussions:** The current `dev_refactoring` README describes `.sf` project folders containing `project.duckdb` metadata and published tabular results in Arrow files under `data/`. Workers do **not** open the project DuckDB directly. Preserve this service-owned result architecture. Do not implement a new “plugin writes arbitrary result tables directly to DuckDB” path.

## 1. Verified repository baseline and reuse inventory

The following are observed in the `dev_refactoring` branch as of this review:

| Existing component | Evidence / path | Proposed treatment |
|---|---|---|
| C++ service, worker, CLI and MCP runtime | `cpp/core/`, `README.md`, `docs/releases.md` | Reuse; add minimal registry activation and dev dispatch |
| Native plugin loader | `cpp/core/include/streamfind/plugin_loader.hpp` | Extend discovery and diagnostics; do not replace blindly |
| C++ SDK and plugin manager | `cpp/sdk/`, `cpp/sdk/src/dynamic_plugin_manager.cpp` | Reuse versioned ABI and SDK; audit stability before promises |
| SDK CMake integration | `cpp/sdk/cmake/streamfindAddPlugin.cmake` | Extend installed-package consumer workflow |
| SDK minimal plugin | `cpp/sdk/examples/minimal_plugin/` | Basis of generated plugin template |
| Plugin generator | `scripts/dev/plugin_generator.py` | Extract/reuse generator logic; eliminate checkout-relative assumptions |
| SDK validator and catalogue tools | `cpp/sdk/tools/streamfind-sdk-plugin-validator.cpp`, `streamfind-sdk-catalogue.cpp` | Package and call from dev executable |
| Separate SDK release packaging | `scripts/release/cpp/package-plugin-devkit.ps1`, `package-plugin-devkit-linux.sh` | Integrate matching kit into release or installable release payload |
| Repo developer command | `scripts/sfdev.ps1`, `scripts/README.md` | Reuse provisioning logic, not repo-only CLI assumptions |
| Managed dependencies | `scripts/build/setup-msys2.ps1`, `setup-node.ps1`, `setup-python.ps1`, version pin files | Extract reusable provisioning modules |
| Native domain plugins | `cpp/plugins/mass_spec`, `raman`, `sensors`, `statistics` | Continue bundled and built using same plugin contracts |
| Frontend plugin loader and registry | `frontend/src/framework/plugins/FrontendPluginLoader.ts`, `FrontendPluginRegistry.ts` | Extend to runtime-installed viewer packages |
| Existing frontend manifest | `frontend/tools/build/*`, `/plugins.json` loader | Preserve bundled loading; add generated runtime manifest |
| Frontend domain viewers | `frontend/src/plugins/` and `frontend/src/framework/viewers/` | Reuse result contracts and viewer slots |
| Package release checks | `scripts/release/cpp/`, `docs/releases.md` | Add extracted-package dev-kit and extension end-to-end checks |

**Important gaps to resolve, not assume already implemented:**
- Current release notes say the runtime archive **does not include the SDK**; developer kit is separate.
- The documented native ABI is version/platform matched, **not guaranteed stable across streamfind versions**.
- The frontend loader currently reads `/plugins.json` and imports same-origin JS. That does not itself establish runtime discovery of user-installed viewers.
- Existing `sfdev` assumes repository-oriented workflows. It is not equivalent to a release-installed `streamfind dev`.
- The existing SDK may expose library interfaces requiring matching runtime/linkage; inspect ABI before defining cross-version policy.
- A successful install does not imply a running worker can safely load a newly installed DLL without lifecycle coordination.

## 2. Target architecture

```text
                             User / AI agent
                                  |
                    +-------------+-------------+
                    |                           |
               streamfind CLI              MCP developer tools
                    |                           |
                    +-------------+-------------+
                                  |
                         Development API
                                  |
                         streamfind-dev
                   (separate executable/process)
                       /                 \
            Native Plugin SDK         Frontend Viewer SDK
                + CMake                   + Node/npm
                       \                 /
                       build / test / validate
                                  |
                      immutable versioned install
                                  |
                       ~/.streamfind/extensions
                         /                  \
               native plugin registry    viewer registry
                         |                  |
                  core worker manager   app same-origin loader
                         |                  |
                   typed result contract / published artifact
```

`streamfind-dev` may initially be a process invoked by the CLI, with JSON stdin/stdout or a versioned request/response protocol. A permanently running daemon is unnecessary for MVP. MCP uses the same dev dispatcher, not duplicated scripts. Never allow developer commands to run arbitrary external programs via unrestricted user-provided shell strings.

## 3. Target directories

### 3.1 Repository additions (adapt to actual current tree after audit)

```text
cpp/
  dev/
    CMakeLists.txt
    include/streamfind/dev/
      command_protocol.hpp
      environment_manager.hpp
      extension_project.hpp
      build_manager.hpp
      package_manager.hpp
    src/
      main.cpp
      command_dispatch.cpp
      environment_manager.cpp
      scaffold.cpp
      build.cpp
      validate.cpp
      install.cpp
      doctor.cpp
    tests/
  sdk/                        # existing, expanded
    templates/
      native-plugin/
      native-operation/
  core/                       # existing
    ...                       # registry refresh / worker lifecycle changes
frontend/
  src/framework/plugins/      # existing, expanded
  src/framework/viewers/      # existing, expanded
  sdk/                        # NEW exportable viewer SDK package
    package.json
    src/
  templates/
    viewer/
scripts/
  build/                      # existing setup logic reused
  release/cpp/                # existing release scripts extended
docs/
  extension-development.md
  extension-compatibility.md
  extension-security.md
tests/
  extension-e2e/              # optional, if tests are consolidated
```

Do not duplicate `sfdev` into a second codebase. Factor reusable provisioning functions into a library/commands that work without the repository checkout, then expose them through `streamfind-dev`. Maintain `sfdev` for contributors building streamfind itself.

### 3.2 Installed runtime (read-only)

```text
streamfind-<version>/
  bin/
    streamfind[.exe]
    streamfind-dev[.exe]
    streamfind-mcp[.exe]
    streamfind-server[.exe]
    streamfind-worker[.exe]
  app/
  core/
    catalogue.duckdb
  plugins/                   # bundled native plugins
  sdk/
    native/
      include/
      lib/
      cmake/
      templates/
      tools/
      sdk-manifest.json
    viewer/
      package/
      templates/
      viewer-sdk-manifest.json
  dev/
    toolchains.lock.json
    setup-manifest.json
  licenses/
```

The release must contain all **SDK assets and setup metadata**, not necessarily the entire GCC/MSYS2/Node toolchain. `streamfind dev setup` provisions missing tools on demand.

### 3.3 Persistent user home

```text
~/.streamfind/
  config.toml
  tools/
    msys2/                   # Windows managed UCRT64
    node/
    cmake/                   # only if needed separately
    downloads/
  dev/
    projects/
      plugins/
        my_peak_tools/
          extension.toml
          CMakeLists.txt
          src/
          include/
          semantic/
          tests/
      viewers/
        my_peak_viewer/
          viewer.json
          package.json
          src/
          tests/
    build/
      <extension-id>/<version>/<platform>/<sdk-version>/
    logs/
  extensions/
    plugins/<extension-id>/<version>/<platform>/
    viewers/<extension-id>/<version>/
    registry.json
  cache/
  trust/
```

Use `STREAMFIND_HOME` override consistently. Source projects remain editable; installed versions remain immutable. Keep compiled outputs and cached downloads out of the source tree. Upgrades must not delete `~/.streamfind`.

### 3.4 Project-local reproducibility

Within a `.sf` project, store an **extension dependency lock/manifest** as project metadata or a sidecar owned by the service, without changing the `.sf` format arbitrarily. Record extension ID, version, package hash, ABI/API version, operation ID, and result contract version for every execution. Never silently substitute another plugin version when replaying a workflow.

## 4. Extension contracts

### 4.1 Native plugin manifest (proposed, subject to compatibility mapping)

```toml
schema_version = 1
id = "org.example.peak_tools"
name = "Peak Tools"
version = "0.1.0"
kind = "native-plugin"
domain = "mass_spec"

[compatibility]
streamfind_min = "0.0.0" # generated from current release
streamfind_max_exclusive = "" # exact runtime pin initially
plugin_abi = 1
platform = "windows-x86_64-ucrt64"

[native]
library = "native/peak_tools.dll"
entrypoint = "streamfind_plugin_init"

[[operations]]
id = "org.example.peak_tools.peak_quality"
result_contract = "mass_spec.peak_quality.v1"
```

Do **not** introduce this manifest as a competing format until existing `plugin.json.in`, SDK manifest validation and semantic catalogue formats have been inventoried. Prefer an additive schema or explicit migration adapter. Use stable operation IDs and namespace them to prevent collisions.

### 4.2 Operation contracts

An operation must specify:
- immutable ID, display name, semantic domain and module;
- typed inputs, optional/required status, shape and cardinality;
- parameter schema, units, defaults, bounds and descriptions;
- typed outputs, artifact contract ID/version and representation;
- declared table/schema capabilities, if relevant, through the existing SDK;
- resource limits, cancellation and progress capabilities;
- deterministic/cache behavior and provenance;
- validation of data shape and semantic definitions.

Native plugins must communicate through the existing host callbacks and service-owned result publication. Do not expose `Project`, DuckDB connections, STL types or C++ exceptions across the plugin ABI. Validate opaque handles, buffer ownership, pointer lifetime and error propagation.

### 4.3 Viewer contract

```json
{
  "id": "org.example.peak_quality_viewer",
  "name": "Peak Quality Viewer",
  "version": "0.1.0",
  "apiVersion": 1,
  "domains": ["mass_spec"],
  "modules": ["peak_processing"],
  "artifactContracts": ["mass_spec.peak_quality.v1"],
  "artifactRepresentations": ["arrow-table"],
  "entry": "/extensions/viewers/org.example.peak_quality_viewer/0.1.0/index.js",
  "css": []
}
```

Map this onto the existing `FrontendPluginManifest` and registry rather than replacing them. Viewer compatibility is decided by **result contract and representation first**, with domain/module as additional filtering. Viewer selection should not depend on the producing algorithm's ID. Add explicit preference/fallback behavior if multiple viewers support the same result.

### 4.4 Compatibility rules

MVP: **exact streamfind runtime + SDK version and platform/toolchain match** for native plugins. A C ABI version alone is not sufficient to promise binary compatibility. Later, introduce tested compatibility ranges after ABI auditing and compatibility CI.

Viewer API compatibility is separately versioned. Persist source so users can rebuild after upgrade. A failed compatibility check disables an extension with diagnostics; it must not crash the service.

## 4.5 Semantic domain ownership and cross-domain vocabulary reuse (mandatory)

**Architectural invariant:** Every plugin owns exactly one unique semantic domain namespace. A plugin may **reference existing ontology vocabulary from another registered domain without altering it**, but may introduce or modify definitions **only in its own domain**. This rule applies equally to bundled and custom plugins. A custom plugin must never expand the bundled `mass_spec` ontology.

**Identity and namespace rules:**
- A plugin manifest declares a globally unique, immutable `plugin_id`, its unique `domain_id`, and its owned ontology namespace IRI. One plugin owns one domain; no two installed plugins may claim the same domain ID or namespace.
- Reserve all installed/bundled domain IDs, aliases, and namespace IRIs, including `mass_spec`; a user cannot install another `mass_spec` plugin or use a confusing alias to bypass collision checks.
- Operation identifiers belong to the defining plugin/domain; parameter identifiers belong to the defining operation. Display labels are **not** unique identifiers.
- New RDF subjects that define classes, properties, operations, parameters, result contracts, or shapes must belong to the plugin's owned namespace. Reuse of an external concept must be a reference to its existing IRI, not a copied or redefined term.
- A plugin can refer to external terms as RDF objects (e.g., `rdf:type`, parameter type, input/output contract) and, where permitted by existing contracts, reference them as inputs or outputs. It cannot assert new descriptive or schema-changing triples *about* externally owned subjects, nor override external SHACL shapes, ranges, defaults, units, labels, or constraints.
- Cross-domain relationships must be expressed using predicates/relationship types allowed by the core ontology. If a new relationship predicate is necessary, define it in the plugin's own namespace and validate it; never create it in another domain's namespace.
- When a referenced external term is unavailable, incompatible, or removed, installation/activation fails with a diagnostic or the plugin is marked unresolved; do not silently create or reinterpret the term.
- Installation uses **separate named graphs per plugin** with provenance (plugin ID, version, hash, graph ID). The runtime catalogue may present a merged query view, but source graphs remain immutable and ownership is checked before merge.
- Namespaces are immutable for a published plugin identity. Changing a domain/namespace requires a new identity and explicit migration.

**Example:**
```text
Bundled domain: mass_spec
  mass_spec:PeakTable                  [existing result contract]
  mass_spec:minimum_intensity          [existing semantic term]

Custom domain: custom_peak_analysis
  custom_peak_analysis:peak_quality    [new operation: allowed]
  custom_peak_analysis:minimum_intensity [new local parameter: allowed]

custom_peak_analysis:peak_quality
    usesInputContract mass_spec:PeakTable
    referencesConcept mass_spec:minimum_intensity

REJECT: defining mass_spec:new_parameter
REJECT: changing mass_spec:minimum_intensity's definition or constraints
REJECT: registering a second plugin/domain called mass_spec
```

A parameter label such as **“minimum intensity”** may occur in several plugins. Resolve it by stable semantic IRI and the operation/parameter ownership path, not by text label. Only reuse the exact existing term if its meaning, units, and constraints fit; otherwise define a distinct term within the custom domain and optionally relate it to the existing term through a permitted semantic relation.

**Validation algorithm (before installation and on catalogue rebuild):**
1. Parse manifest, normalize IDs/IRIs, and reject duplicate/reserved domain IDs, aliases and namespace IRIs.
2. Parse the plugin's RDF/TTL into an isolated named graph. Enumerate newly defined subjects, including RDF/OWL declarations and SHACL shapes.
3. Reject any definition or schema-changing assertion whose subject belongs to a different namespace, even if the statement is stored in the custom plugin's graph. Check for semantic overrides, not merely string-prefix ownership.
4. Resolve every external IRI against the installed immutable catalogue; check its kind and the allowed reference relationship.
5. Validate core ontology structure, required operation/input/output/parameter properties, datatype/units, SHACL constraints, and graph consistency.
6. Detect collisions in operation IDs, result-contract IDs, domain IDs and protected terms; distinguish shared labels from true identity collisions.
7. Produce structured diagnostics (offending triple, owning domain, violated rule, suggested local namespace).
8. Publish the validated named graph transactionally; do not mutate bundled ontology files. On failure, leave the previous catalogue/registry generation active.

**Agent and generator rules:** `streamfind dev plugin create` allocates a unique domain namespace; `add-operation` generates semantic definitions in that namespace only. Agent prompts and MCP tool schemas must state “reuse external vocabulary by reference only; never extend another domain.” Provide catalogue lookup commands so generators can discover existing terms rather than duplicate them.

**Required tests:**
- Accept custom operation using `mass_spec:PeakTable` and referencing an existing mass-spec parameter without redefining either.
- Accept same display label `minimum intensity` in two separate domains with distinct IRIs.
- Reject duplicate `mass_spec` domain identity and reserved namespace aliases.
- Reject custom triples redefining `mass_spec:minimum_intensity`, adding `mass_spec:new_parameter`, or replacing a bundled SHACL shape.
- Reject missing external term, wrong semantic type, conflicting result contract, and unauthorized cross-domain relationship.
- Verify failed installation leaves bundled graph and active registry unchanged; verify upgrade preserves graph ownership and provenance.

## 5. Developer CLI contract

Commands below are **target design**, not current implementation.

| Command | Required behavior |
|---|---|
| `streamfind dev help` | Show commands, examples, JSON support |
| `streamfind dev setup --target native|ui|all` | Install and verify pinned tools |
| `streamfind dev doctor --json` | Report runtime/SDK/toolchain and compatibility |
| `streamfind dev plugin create NAME --domain DOMAIN` | Generate plugin project from SDK template |
| `streamfind dev plugin add-operation NAME --plugin PATH` | Generate operation skeleton and metadata |
| `streamfind dev viewer create NAME --contract ID` | Generate React viewer project |
| `streamfind dev build PATH` | Build native or UI package |
| `streamfind dev test PATH` | Run extension tests |
| `streamfind dev validate PATH` | Check manifests, ABI/API, semantic/result contracts, package closure |
| `streamfind dev package PATH` | Produce immutable versioned artifact with hashes |
| `streamfind dev install PATH` | Install atomically into user registry |
| `streamfind dev list --json` | List source projects and installed versions |
| `streamfind dev activate ID --version V` | Activate with safe worker/manifest refresh |
| `streamfind dev deactivate ID` | Stop new jobs from using an extension |
| `streamfind dev rebuild ID` | Rebuild from retained source against current SDK |
| `streamfind dev logs ID` | Inspect reproducible build/validation logs |

Also expose user-facing `streamfind extension list|doctor|remove` aliases if useful; ensure one underlying implementation. Use meaningful nonzero exit codes and machine-readable JSON for AI agents.

## 6. Implementation phases and exact acceptance gates

### Phase 0 — Freeze baseline and map existing contracts
**Goal:** avoid replacing working SDK, worker, or frontend plugin architecture.

Tasks:
1. Record `dev_refactoring` commit SHA and release version.
2. Read `cpp/sdk/include`, `dynamic_plugin_manager.cpp`, manifest code, plugin loader, worker registration and `plugin.json.in`.
3. Map `sfdev setup`, plugin generator, SDK packaging and runtime package layout.
4. Trace frontend bundled plugin discovery and `/plugins.json` loading.
5. Identify actual artifact/result contracts, Arrow access and viewer registration.
6. Write an architecture decision record (ADR) confirming native plugins vs UI viewers and service-owned data.
7. Add a baseline CI job proving the existing runtime, MCP and frontend tests remain green.

Deliverables: `docs/adr/extension-platform.md`, contract inventory, command inventory, compatibility matrix.  
**Gate:** reviewers can identify the exact existing function that each new feature extends.

### Phase 1 — Define manifest, registry and compatibility
**Goal:** one extension identity and version model.

Tasks:
1. Define canonical package identity, semantic version, kind, platform, content hash, origin and trust state.
2. Extend existing native manifest schema without breaking bundled plugins.
3. Define separate viewer manifest schema using existing frontend plugin types.
4. Define registry JSON schema, lockfile rules, atomic writes, and collision precedence.
5. Specify bundled vs user vs project-local precedence; require explicit override rather than silent shadowing.
6. Add schema validation and negative fixtures (duplicate IDs, bad paths, missing dependencies, incompatible ABI).
7. Add migration/versioning policy for future registry changes.
8. Implement and test **exclusive domain/namespace ownership**, cross-domain reuse by reference, immutable bundled vocabularies, and per-plugin named-graph provenance (see §4.5).

Deliverables: manifest schemas, compatibility validator, registry library.  
**Gate:** bundled plugins and test user plugins validate; invalid manifests fail safely.

### Phase 2 — Release SDK as an installed package
**Goal:** out-of-tree native plugins compile from an extracted release.

Tasks:
1. Reuse existing SDK CMake exports, headers, libraries and tools.
2. Integrate current separate developer-kit payload into runtime archive under `sdk/native/`.
3. Ensure installed `streamfindConfig.cmake`, exported targets, dependencies and `streamfind_add_plugin` resolve without source checkout.
4. Add `sdk-manifest.json` with runtime version, ABI, architecture, compiler/runtime profile, package hashes.
5. Build `cpp/sdk/examples/minimal_plugin` from a fresh directory against extracted release.
6. Check dynamic dependency closure and actual plugin load in packaged worker, not only successful linking.
7. Keep normal runtime use free of toolchain requirements.

Deliverables: self-contained SDK payload and out-of-tree consumer test.  
**Gate:** `find_package(streamfind CONFIG REQUIRED)` and plugin load succeed on clean Windows and Linux runners.

### Phase 3 — Extract provisioning from `sfdev`
**Goal:** reproducible setup without a repository checkout.

Tasks:
1. Inventory existing `setup-msys2.ps1`, Node and version-pin files.
2. Move reusable provisioning logic into a release-distributed helper or embed it in `streamfind-dev`; do not hardcode repository paths.
3. Define signed/trusted source URLs, exact versions, SHA-256 verification, download cache and retries.
4. Windows: install supported MSYS2 UCRT64 GCC, CMake, Ninja and Arrow development dependencies.
5. Linux: document supported distributions; use package manager or a vetted toolchain bundle, with explicit privilege prompts. Do not claim `~/.streamfind` contains every system dependency if APT installs are required.
6. Viewer: provision pinned Node/npm and consume lockfile-based packages.
7. Support offline mode with predownloaded artifacts and actionable errors.
8. Support interrupted setup, idempotency, concurrent setup lock, and `--dry-run`.
9. Never install Java, Python, Open Babel or other scientific tools unless required by the chosen development workflow; catalogue/semantic tooling dependencies must be explicitly inventoried.
10. Generate machine-readable environment report.

Deliverables: `streamfind dev setup`, `doctor`, toolchain lock.  
**Gate:** repeat setup is idempotent, verifies hashes and works on a clean extracted release.

### Phase 4 — Standalone `streamfind-dev` executable and dispatch
**Goal:** CLI and MCP share one developer implementation.

Tasks:
1. Add `cpp/dev` executable and typed request/response protocol.
2. Implement command parser and JSON output with stable error codes.
3. Integrate `streamfind dev ...` forwarding into existing CLI.
4. Use safe argv execution, environment isolation, timeouts, cancellation, logs and bounded output.
5. Define file-access policy: source projects only under approved user paths by default; canonicalize paths and reject traversal.
6. Keep toolchain management out of the core execution process.
7. Add unit tests for parsing, exit codes, process spawning and path handling.

Deliverables: dev executable, CLI forwarding, command reference.  
**Gate:** dev command works from arbitrary working directory without source checkout.

### Phase 5 — Native plugin scaffolding
**Goal:** one-command creation of a buildable plugin.

Tasks:
1. Adapt `scripts/dev/plugin_generator.py` and SDK minimal plugin into distributable templates.
2. Generate `extension.toml`/canonical manifest, CMake, source, include, tests, semantic definitions, README and license notes.
3. Generate operation skeleton with host callbacks, typed input/output contracts, cancellation/error handling.
4. Allow `add-operation` repeatedly; update manifest/CMake/semantic metadata atomically.
5. Validate identifiers and collisions; avoid overwriting user changes.
6. Include a deterministic example operation that computes a numerical output and publishes a typed result.
7. Generate source in `~/.streamfind/dev/projects/plugins/`.

Deliverables: plugin and operation generator.  
**Gate:** two generated operations compile and register under one native plugin.

### Phase 6 — Build, test, validate, package and install native plugins
**Goal:** fully reproducible plugin lifecycle.

Tasks:
1. Build with installed SDK and managed compiler profile; never link against arbitrary machine DLLs.
2. Use CMake presets/toolchain files and isolated build directories keyed by SDK/version/platform.
3. Invoke SDK validator and semantic catalogue validation.
4. Verify native dependency closure, ABI exports, manifest IDs, operation contracts and host callback compatibility.
5. Run CTest and optional worker-level integration tests.
6. Package binary, manifest, semantic resources, tests metadata, checksums, license and provenance.
7. Stage install to temporary directory; verify hashes; atomically rename into versioned location.
8. Update registry atomically with rollback; never overwrite active version in place.
9. Keep source and logs for rebuilds.
10. Add uninstall/deactivate rules that protect versions pinned by running jobs.

Deliverables: `build`, `test`, `validate`, `package`, `install`.  
**Gate:** fresh generated plugin installs, executes through service, produces published artifact and survives service restart.

### Phase 7 — Core discovery, activation and worker lifecycle
**Goal:** new operations become available without rebuilding or restarting the entire app.

Tasks:
1. Extend current dynamic plugin manager to scan bundled and user registry paths.
2. Validate manifests before loading and provide clear per-extension diagnostics.
3. Register operations and semantic metadata with the existing shared catalogue; specify how catalogue snapshots are refreshed and exposed through MCP. Keep per-plugin named graphs immutable and enforce ownership during catalogue merge.
4. On install, send registry-generation change notification to service.
5. Worker manager starts new workers with immutable plugin generation and version set; existing workers finish in-flight work.
6. New workflows use the new generation; never unload an in-use native library.
7. Store extension version, binary hash, operation ID and result contract in execution provenance.
8. Prevent duplicate IDs and incompatible schema registrations; define collision precedence.
9. Test rollback, failed plugin load, missing library, crashes and cancellation.
10. Preserve service-owned Arrow result publication and DuckDB metadata boundaries.

Deliverables: registry watcher/refresh, worker generation routing, execution provenance.  
**Gate:** install new operation while a workflow runs; existing workflow finishes unchanged and new workflow can select new operation.

### Phase 8 — Exportable viewer SDK and scaffolding
**Goal:** user-developed viewers load in the existing Model/Result Viewer.

Tasks:
1. Extract stable public interfaces from `frontend/src/framework/plugins`, viewers and result contracts into versioned `@streamfind/viewer-sdk`.
2. Preserve current bundled frontend plugin behavior.
3. Define viewer registration API, supported artifact contract/representation matching, data-access API, selection/filter state, and disposal lifecycle.
4. Generate viewer template with React component, manifest, package.json, lockfile, tests and sample typed result.
5. Build viewer with pinned Node toolchain, output ESM/CSS and hashed assets.
6. Validate imports, API version, contract support and bundle manifest.
7. Install under `~/.streamfind/extensions/viewers/...`.
8. Extend local app server to serve installed viewer bundles via same-origin, path-safe endpoints and dynamically generate a combined plugin manifest.
9. Extend loader to refresh available viewers; handle failed import without blanking the app.
10. Define trust policy: trusted JS can run in app origin; untrusted third-party code requires explicit trust or sandboxed iframe/isolated context with limited API. Same-origin alone is not sandboxing.

Deliverables: viewer SDK, generator, runtime loader integration.  
**Gate:** user viewer appears for a compatible artifact from a bundled operation; incompatible artifacts do not offer it.

### Phase 9 — MCP development tools and agent workflow
**Goal:** AI agents can scaffold, build, test and inspect without repository access.

Tasks:
1. Add MCP tools for dev environment status, create plugin, add operation, create viewer, build, test, validate, package, list and inspect.
2. Reuse dev dispatcher; no duplicate CMake/process logic.
3. Provide JSON Schema inputs and structured outputs with paths, errors, logs, hashes and diagnostics.
   Provide ontology term discovery and explicit namespace ownership guidance to agents.
4. Separate `build` from `install` and `activate`; require explicit approval for code execution/activation.
5. Restrict filesystem paths and package installation scope; audit log each agent request.
6. Support long-running build tasks with task IDs, progress, cancellation and bounded log retrieval.
7. Refresh `tools/list`/operation catalogue appropriately after activation.
8. Add MCP smoke tests against packaged release and generated extension.

Deliverables: agent-facing developer MCP interface.  
**Gate:** agent creates a native plugin and viewer, validates them, requests installation, then invokes the operation and sees a compatible viewer.

### Phase 10 — Upgrade resilience and compatibility doctor
**Goal:** upgrades preserve sources and installed extensions without pretending all binaries remain compatible.

Tasks:
1. Never delete user home on runtime upgrade.
2. Read registry on startup; report compatibility with new runtime and SDK.
3. Mark incompatible native plugins disabled until rebuilt; do not attempt unsafe loading.
4. Provide `streamfind dev doctor --json` and `rebuild` using retained source.
5. Preserve versioned package archives, rollback points and project pins.
6. Test upgrade from release N to N+1 with compatible viewer, incompatible native plugin and successful source rebuild.
7. Record migration actions and errors in audit logs.
8. Document that compiled binary compatibility is conditional and tested, not automatic.

Deliverables: upgrade doctor, rebuild workflow, migration documentation.  
**Gate:** upgrade preserves all source projects and metadata; incompatible plugin cannot crash startup; rebuild restores operation availability.

### Phase 11 — Release engineering and documentation
**Goal:** every published runtime release is an extension-development-ready distribution.

Tasks:
1. Extend Windows and Linux release scripts to stage `streamfind-dev`, SDK, viewer SDK/templates and setup manifests.
2. Keep official plugins/viewers bundled and discoverable using same contracts as custom extensions.
3. Validate checksums, dependency closure, no machine-local absolute paths and license notices.
4. Run clean-machine smoke tests from extracted runtime archives.
5. Add documentation for `setup`, `doctor`, plugin/viewer creation, API, compatibility, security, offline provisioning and upgrades.
6. Document SDK/API changes and deprecations.
7. Publish reproducible sample plugin and viewer sources and their expected output.
8. Verify native release can run normally without invoking `dev setup`.

Deliverables: updated release packages, docs, CI workflows.  
**Gate:** complete acceptance matrix below passes for Windows x64 and Linux x86_64.

## 7. Test matrix and quality gates

| Scenario | Windows x64 | Linux x86_64 | Required |
|---|---|---|---|
| Runtime launches without developer toolchain | Yes | Yes | P0 |
| `dev doctor --json` reports missing tools | Yes | Yes | P0 |
| `dev setup` downloads and validates tools | Yes | Yes | P0 |
| Setup re-run does not corrupt toolchain | Yes | Yes | P0 |
| Native plugin built from extracted release | Yes | Yes | P0 |
| Two operations registered in one plugin | Yes | Yes | P0 |
| Plugin installed atomically and executed | Yes | Yes | P0 |
| Service remains running during extension activation | Yes | Yes | P0 |
| Active workflow retains original plugin generation | Yes | Yes | P0 |
| Plugin crash does not terminate service | Yes | Yes | P0 |
| Viewer built outside frontend repository | Yes | Yes | P0 |
| Viewer loaded by typed result contract | Yes | Yes | P0 |
| Invalid/incompatible plugin refused with diagnostics | Yes | Yes | P0 |
| MCP creates/builds/tests extension | Yes | Yes | P1 |
| Upgrade preserves source and registry | Yes | Yes | P1 |
| Rebuild incompatible extension from source | Yes | Yes | P1 |
| Offline setup from predownloaded cache | Yes | Yes | P2 |

Tests must exercise the **extracted release**, not merely source-tree builds. Keep CTest for native unit tests and existing frontend tests for viewer SDK. Add end-to-end release smoke tests for CLI, MCP, registry and worker lifecycle.

## 8. Security, licensing and operational safeguards

- Native plugins execute arbitrary code; worker isolation protects service availability but is not necessarily a full OS security sandbox. Require trust/approval for third-party binaries and AI-generated code.
- Never auto-activate newly built AI code without user authorization.
- Downloads must use approved HTTPS sources, verified hashes, explicit pinned versions, controlled extraction and safe paths.
- Reject zip-slip/path traversal, symlink escapes and unexpected executables in packages.
- Use immutable package paths and atomic registry changes; avoid replacing loaded Windows DLLs.
- UI ESM executed in the main app has origin privileges. Trust or sandbox it; a same-origin URL is not a security boundary.
- Do not expose project DB handles directly to plugins; enforce host capabilities and typed result contracts.
- Track third-party dependency licenses, GPLv3 distribution obligations, SDK linkage and vendor attribution in release review.
- Keep build logs free of secrets; restrict environment variables forwarded to build subprocesses.
- Separate installation, activation, deactivation and removal so failures can be rolled back.

## 9. Suggested PR sequence (small, independently reviewable)

1. **PR-01:** ADR, contract inventory, baseline tests.
2. **PR-02:** canonical extension manifest and compatibility validator.
3. **PR-03:** installable native SDK included in extracted runtime.
4. **PR-04:** shared provisioning library and `streamfind dev setup/doctor`.
5. **PR-05:** standalone `streamfind-dev` + CLI dispatch.
6. **PR-06:** native plugin/operation templates and generator.
7. **PR-07:** native build/test/validate/package/install.
8. **PR-08:** service registry discovery, generation-aware workers and provenance.
9. **PR-09:** exportable viewer SDK and viewer generator.
10. **PR-10:** runtime viewer bundle hosting and typed viewer selection.
11. **PR-11:** MCP developer tools, approval boundaries and agent tests.
12. **PR-12:** upgrade doctor, rebuild, compatibility and rollback.
13. **PR-13:** release packaging, cross-platform E2E, documentation.

**Dependency chain:** PR-01 → PR-02 → PR-03 → PR-04/05 → PR-06/07 → PR-08. Viewer SDK work (PR-09) can proceed after PR-02, in parallel with native scaffolding; runtime viewer hosting follows PR-09. MCP tools follow the stable dev dispatcher. Upgrade/release gates come last.

## 10. First vertical slice: mass-spec peak quality

Implement a user plugin `org.example.peak_tools` with:
- `peak_quality` operation reading an existing typed mass-spec peak/feature artifact through host APIs;
- deterministic quality score computation on a small test fixture;
- output contract `mass_spec.peak_quality.v1`, published as an Arrow table by the existing service;
- semantic metadata for operation inputs/outputs and parameters;
- unit test with known expected score;
- custom viewer `org.example.peak_quality_viewer` rendering score distribution and selectable rows;
- browser Model/Result Viewer discovery by output contract, not producer operation ID.

**Demo script:**
```bash
streamfind dev setup --target all
streamfind dev plugin create peak_tools --domain mass_spec
streamfind dev plugin add-operation peak_quality --plugin peak_tools
streamfind dev build ~/.streamfind/dev/projects/plugins/peak_tools
streamfind dev test ~/.streamfind/dev/projects/plugins/peak_tools
streamfind dev validate ~/.streamfind/dev/projects/plugins/peak_tools
streamfind dev install ~/.streamfind/dev/projects/plugins/peak_tools
streamfind dev viewer create peak_quality_viewer --contract mass_spec.peak_quality.v1
streamfind dev build ~/.streamfind/dev/projects/viewers/peak_quality_viewer
streamfind dev install ~/.streamfind/dev/projects/viewers/peak_quality_viewer
streamfind dev doctor --json
```
*All commands are proposed target syntax.* Add a documented `activate` step if install is intentionally staged rather than immediately activated. The demo is complete only when the running service publishes the operation, a workflow executes it, and the browser selects the new viewer for its typed result.

## 11. Definition of done

The workstream is complete only if a user who has **never cloned the streamfind repository** can:
1. extract a normal runtime release and run the existing service;
2. invoke `streamfind dev setup` to provision all required native/viewer tools;
3. generate and modify C++ plugin source in `~/.streamfind/dev/projects/plugins/`;
4. add multiple operations to one native plugin;
5. compile/test/validate/install without rebuilding core;
6. activate new operations safely while the service is running;
7. discover and execute the new operation through CLI, MCP and browser;
8. generate a separate custom viewer and render the result through its declared contract;
9. upgrade streamfind without losing source, installed packages, project pins or logs;
10. receive explicit compatibility diagnostics and rebuild from source when needed;
11. reference existing bundled ontology vocabulary from a custom domain, while being prevented from redefining or expanding that bundled vocabulary.

## 12. References reviewed

- [`README.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/README.md)
- [`docs/plugin-development.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/docs/plugin-development.md)
- [`docs/releases.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/docs/releases.md)
- [`scripts/README.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/scripts/README.md)
- [`cpp/sdk/cmake/streamfindAddPlugin.cmake`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/sdk/cmake/streamfindAddPlugin.cmake)
- [`cpp/core/include/streamfind/plugin_loader.hpp`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/core/include/streamfind/plugin_loader.hpp)
- [`frontend/src/framework/plugins/FrontendPluginLoader.ts`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/frontend/src/framework/plugins/FrontendPluginLoader.ts)

This is an implementation plan grounded in the inspected branch structure and documentation. Specific new C++ interfaces and CLI command names remain design proposals requiring Phase 0 verification.
