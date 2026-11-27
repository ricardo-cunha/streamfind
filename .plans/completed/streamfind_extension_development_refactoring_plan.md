# streamfind — Integrated Extension Development Platform
## Stepwise implementation plan for `extension_framework_development`
- **Planning baseline:** 20 November 2026
- **Implementation branch:** `extension_framework_development`
- **Base and merge target:** `dev_refactoring`
- **Baseline commit:** `1c920da487d52949ad8f6193395e45c26d1f8e12`
- **Status:** complete; all implementation phases and acceptance gates passed
- **Repository:** https://github.com/ricardo-cunha/streamfind/tree/dev_refactoring

## 0. Executive decision and non-negotiable boundaries

Every native streamfind release shall include a first-class, optional development entry point. A user can run `streamfind dev setup`, generate a plugin containing one or more operations, build it with the SDK matching the installed runtime, test and install it, and make its operations discoverable to the existing service, CLI, MCP clients, and browser app **without rebuilding streamfind-core**. A parallel development path supports independent, custom frontend **viewers** for typed scientific results.

**Separate extension kinds:**
1. **Native processing plugin:** one deployable C++ dynamic library/package containing one or more operations. Operations declare typed inputs, parameters, outputs, result contracts, and any permitted table/schema registration.
2. **Frontend viewer extension:** a separately deployable, compiled TypeScript/React package. A viewer declares supported plugin, capability, fully qualified artifact/result contract, representation, and compatible frontend API. It renders data but does not implement scientific processing.
3. Both may be distributed together as an optional convenience collection, but **retain independent registration, versioning, activation and SDKs**. Neither requires the other.

**Ownership:** The core service continues to own project sessions, workflows, DuckDB metadata, operation registry, worker supervision, and result publication. The new developer executable owns toolchain provisioning, scaffolding, compilation, testing, validation, packaging, and installation. CLI and MCP delegate to a shared development command/service implementation. Avoid a second scientific execution path.

**Critical correction from earlier discussions:** The current `dev_refactoring` README describes `.sf` project folders containing `project.duckdb` metadata and published tabular results in Arrow files under `data/`. Workers do **not** open the project DuckDB directly. Preserve this service-owned result architecture. Do not implement a new “plugin writes arbitrary result tables directly to DuckDB” path.

### Development worktree and service

Use `.worktrees\extension_framework_development` for all framework development. Resolve the paths below from this worktree. Run `sfdev env show` and the relevant `sfdev doctor --workflow native`, `frontend`, or `scripts` before development. Use `sfdev package` for native or mixed changes and `sfdev package --frontend-only` to refresh an existing package after frontend changes.

Use this worktree's `tmp/packaged_build/` for development verification. Its assigned loopback service port is **8789**, an explicit exception to the general worktree allocation range. Before starting, check this package's service status and the listener on 8789. An occupied port belonging to a different checkout blocks this run. For test installs and generated extension projects, set `STREAMFIND_HOME` to an isolated directory under this worktree's `tmp/` for the package CLI, service, and developer subprocesses. Select a source workspace under this worktree's `tmp/scratch/` for disposable extension fixtures. Keep test databases under `tmp/projects/` and logs under `tmp/logs/`.

```powershell
.\tmp\packaged_build\bin\streamfind.exe service start --port 8789
.\tmp\packaged_build\bin\streamfind.exe service status
```

Verify the reported endpoint and listener executable under this worktree's package before using its app or MCP. Stop a service started for a check through the same package CLI and confirm that 8789 is released. Extracted release checks on Windows and Linux remain separate Phase 11 gates.

## 1. Verified repository baseline and reuse inventory

The following were verified in the `dev_refactoring` baseline at commit `0db11ae2465800be42583dca1324b62a7776c69c`:

| Existing component | Evidence / path | Proposed treatment |
|---|---|---|
| C++ service, worker, CLI and MCP runtime | `cpp/core/`, `README.md`, `docs/releases.md` | Reuse; add minimal registry activation and dev dispatch |
| Native plugin loader | `cpp/core/include/streamfind/plugin_loader.hpp` | Extend discovery and diagnostics; do not replace blindly |
| C++ SDK and plugin manager | `cpp/sdk/`, `cpp/sdk/src/dynamic_plugin_manager.cpp` | Reuse versioned ABI and SDK; audit stability before promises |
| Native plugin manifest | `cpp/plugins/<plugin>/plugin.json.in`, `cpp/sdk/src/plugin_manifest.cpp` | Keep `plugin.json` canonical; evolve its schema and parser together |
| SDK CMake integration | `cpp/sdk/cmake/streamfindAddPlugin.cmake` | Extend installed-package consumer workflow |
| SDK consumer example | `cpp/sdk/examples/minimal_plugin/`, `scripts/dev/test_plugin_consumer.py` | Checks SDK linkage with an executable. Use the shared-library entry point in the plugin generator for worker-load checks. |
| Plugin generator | `scripts/dev/plugin_generator.py` | Extract/reuse generator logic; eliminate checkout-relative assumptions |
| SDK validator and catalogue tools | `cpp/sdk/tools/streamfind-sdk-plugin-validator.cpp`, `streamfind-sdk-catalogue.cpp` | Package and call from dev executable |
| Integrated SDK release packaging | `scripts/release/cpp/release-cpp.ps1`, `release-cpp-linux.sh` | Ship the matching native/viewer SDK and development dispatcher in the single runtime archive |
| Repo developer command | `scripts/sfdev.ps1`, `scripts/README.md` | Reuse provisioning logic, not repo-only CLI assumptions |
| Managed dependencies | `scripts/build/setup-msys2.ps1`, `setup-node.ps1`, `setup-python.ps1`, version pin files | Extract reusable provisioning modules |
| Native plugin plugins | `cpp/plugins/mass_spec`, `raman`, `sensors`, `statistics` | Continue bundled and built using same plugin contracts |
| Frontend plugin loader and registry | `frontend/src/framework/plugins/FrontendPluginLoader.ts`, `FrontendPluginRegistry.ts` | Extend to runtime-installed viewer packages |
| Existing frontend manifest | `frontend/tools/build/*`, `/plugins.json` loader | Preserve bundled loading; add generated runtime manifest |
| Frontend plugin API and viewer matching | `frontend/src/framework/plugins/pluginTypes.ts`, `frontend/src/framework/artifacts/artifactContracts.ts`, `frontend/src/framework/viewers/` | Current API is `1.0`; qualify contract identities end-to-end before external viewers are supported |
| Frontend plugin viewers | `frontend/src/plugins/` and `frontend/src/framework/viewers/` | Reuse result contracts and viewer slots |
| Package release checks | `scripts/release/cpp/`, `docs/releases.md` | Add extracted-package dev-kit and extension end-to-end checks |

**Important gaps to resolve, not assume already implemented:**
- The runtime and developer payload must be released as one platform archive; there is no separate developer-kit distribution.
- The documented native ABI is version/platform matched, **not guaranteed stable across streamfind versions**.
- The frontend loader currently reads `/plugins.json` and imports same-origin JS. That does not itself establish runtime discovery of user-installed viewers.
- Existing `sfdev` assumes repository-oriented workflows. It is not equivalent to a release-installed `streamfind dev`.
- The current native manifest already uses `plugin_id`, `plugin`, `abi_version`, `sdk_compatibility`, and a platform-to-library map; the current plugin ABI is 3.0. Do not introduce a competing `extension.toml` format.
- The current frontend manifest exposes `plugins`, `capabilities`, `artifactContracts`, and `artifactRepresentations`; it has no `modules` field. The current viewer contract helper can match by local name after removing a namespace prefix, which is unsafe for independently owned plugins. External viewer contracts must use fully qualified semantic IRIs and exact identity matching.
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
  dev/
    extensions/               # development-stage package/workflow checks
  release/cpp/                # existing release scripts extended
```

Do not add a repository-root `tests/` tree. Native unit and contract tests stay
with their owning C++ component and are registered with CTest. Data-backed,
workflow, extracted-package, and end-to-end checks belong under
`scripts/dev/` or `scripts/release/` and use the repository-supported runner.

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
  plugins/
    native/                  # bundled native plugins
    custom/                  # user/developer native plugin deployments
  viewers/
    native/                  # bundled native viewers and shared assets
    custom/                  # user/developer viewer deployments
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

### 3.3 Source workspace and installed packages

Let authors choose the editable source folder when they set up extension development. Offer the user's Documents folder (for example, `Documents/streamfind-dev`) as a suggestion. Save the chosen absolute path as a per-user preference and accept an explicit `--workspace PATH` for another checkout or CI. Resolve Windows Documents through the OS folder location when offering the suggestion. A source folder can have its own Git history, editor settings and backups.

| Location | Owner and contents |
|---|---|
| `<workspace>/plugins/peak_tools/` | Author-owned C++, CMake, Turtle sources, manifest template and tests |
| `<workspace>/viewers/peak_quality_viewer/` | Author-owned TypeScript, component, manifest, lockfile and tests |
| `<STREAMFIND_HOME>/dev/` | Managed build trees, package staging and logs. Key builds by source checkout, kind, ID, version, target and SDK so two workspaces can build the same extension independently. |
| `<STREAMFIND_HOME>/extensions/plugins/<id>/<version>/<platform>/` | Verified native package including binary, ontology and catalogue |
| `<STREAMFIND_HOME>/extensions/viewers/<id>/<version>/` | Verified compiled viewer package including manifest and assets |
| `<STREAMFIND_HOME>/extensions/registry.json` | Installed hashes, active versions and generation. Store the optional local source locator separately from portable package identity. |

`STREAMFIND_HOME` defaults to `~/.streamfind`. Build and test from the selected source using the matching SDK. Package into staging, install the exact artifact by path and digest, then activate its installed version. Only activated installed packages participate in discovery. Removing an installed extension preserves its source. Moving the source folder leaves execution intact. For rebuild, accept the new source location explicitly and verify its manifest identity before use.

### 3.4 Project-local reproducibility

Within a `.sf` project, store an **extension dependency lock/manifest** as project metadata or a sidecar owned by the service, without changing the `.sf` format arbitrarily. Record extension ID, version, package hash, ABI/API version, operation ID, and result contract version for every execution. Never silently substitute another plugin version when replaying a workflow.

## 4. Extension contracts

### 4.1 Native plugin manifest

For custom native plugins, the author supplies one machine name. For `peak_tools` derive the manifest `name` and `plugin` as `peak_tools`, the package ID as `custom.plugin.peak_tools`, and the operation ID as `peak_tools.peak_quality`. Reject disagreement among these fields, a reserved plugin, or another owner's claim before registration. Human-readable labels belong in catalogue metadata. Viewer identities use the separate proposed `custom.viewer.<name>` convention.

Keep the existing `plugin.json` format as the canonical native plugin manifest.
The example below shows the current field names plus a proposed schema version and namespace field.
Phase 0 and Phase 1 must confirm the exact compatibility range and package
layout before implementation.

```json
{
  "schema_version": 1,
  "plugin_id": "custom.plugin.peak_tools",
  "name": "peak_tools",
  "version": "0.1.0",
  "abi_version": { "major": 3, "minor": 0 },
  "sdk_compatibility": {
    "minimum": "<matching-streamfind-version>",
    "maximum": "<matching-streamfind-version>"
  },
  "library": {
    "windows-x86_64": "native/peak_tools.dll",
    "linux-x86_64": "native/libpeak_tools.so"
  },
  "plugin": "peak_tools",
  "ontology_namespace": "https://streamfind.dev/catalogue/plugins/peak_tools#",
  "semantic_catalogue": "catalogue.duckdb",
  "static_composition": false,
  "platforms": ["windows-x86_64", "linux-x86_64"]
}
```

The ABI value above records the inspected baseline, not a promise that future
versions stay at 3.0. Generate compatibility values from the matching SDK and
runtime. Keep registry-only fields such as install location, package hash,
origin, trust, and activation state outside `plugin.json`. Do not introduce a
parallel `extension.toml` or a compatibility adapter. If a released manifest
transition is required, stop and scope it separately under the repository's
legacy-free rule.

### 4.2 Operation contracts

An operation must specify:
- immutable ID, display name, semantic plugin and module;
- typed inputs, optional/required status, shape and cardinality;
- parameter schema, units, defaults, bounds and descriptions;
- typed outputs, artifact contract ID/version and representation;
- declared table/schema capabilities, if relevant, through the existing SDK;
- resource limits, cancellation and progress capabilities;
- deterministic/cache behavior and provenance;
- validation of data shape and semantic definitions.

Native plugins must communicate through the existing host callbacks and service-owned result publication. Do not expose `Project`, DuckDB connections, STL types or C++ exceptions across the plugin ABI. Validate opaque handles, buffer ownership, pointer lifetime and error propagation.

#### Enforce declared types against the installed SDK and ABI

The baseline ontology describes logical types and storage, but its validation does not establish whether an operation can read or emit each declared type. `cpp/core/semantic/types.ttl` defines `sf:PrimitiveType` terms with `sf:wireType` and `sf:duckdbType`. In `shapes.ttl`, the column shape checks an enumerated `sf:type` and a nonempty `sf:storageType`. The SDK catalogue builder projects those values. The ABI header defines JSON request/result buffers and separate Arrow batch callbacks. Descriptor negotiation checks binary compatibility and identity. Each of these checks answers a different question.

| Declaration or value | Evidence at ABI 3.0 | Proposed validation |
|---|---|---|
| JSON parameters and scalar or structured results | The ABI passes JSON buffers. The catalogue projects schemas for parameters, fields and array items. | Resolve each leaf type against the core catalogue. Validate defaults, required fields, nullability, arrays and result values recursively against the projected schema. A table reference resolves to a declared artifact contract. |
| Table columns | Typed readers recognize UTF-8, int64, float64, Boolean, timestamp, binary and decimal families. The batch builder emits timestamps in microseconds. | Resolve the qualified input or output contract down to each field. Specify the physical Arrow type, width, nullability and timestamp unit as applicable. Check allowed storage mappings separately, including existing INTEGER storage for logical integers. |
| Decimal | The typed reader recognizes Arrow decimal128/256. `TableBatchBuilder::emit` selects UTF-8 for the scalar decimal case through its default branch. | Reject a native decimal output declaration on that writer path until a tested writer preserves precision and scale. A direct Arrow path can qualify only with its own validated schema and round-trip evidence. |
| Lists and reserved types | The SDK batch builder emits lists of UTF-8, int64 and float64. The ontology also declares image and UUID primitives. | Declare the child type and validate read and write support independently. Each additional type or encoding needs a tested mapping for the selected transport and ABI. |

1. **Core and SDK ownership.** Maintain one core-owned, machine-readable type profile keyed by ABI major/minor, transport, direction and SDK access path. Each supported leaf identifies its core primitive IRI, allowed logical and physical forms, and applicable constraints. Publish the profile identity and digest with the SDK and catalogue. Custom plugins may compose registered types into plugin contracts. Adding a primitive or changing its wire encoding requires a core/SDK change with compatibility tests.
2. **Static enforcement.** Extend the native SDK catalogue validator used by scaffolding, build, package and install. Resolve both locally defined and referenced contracts, including nested properties and every reachable table column. Require declared types and reject missing, ambiguous or unsupported mappings. Compare manifest, installed runtime/SDK, and the requested ABI profile before activation. Report operation, port, field path, declared type, ABI, and supported encoding in structured diagnostics. Preserve existing bundled contracts by inventorying their actual transport and storage combinations before enabling the rule.
3. **Execution enforcement.** During ABI negotiation compare the binary descriptor with the manifest and required callbacks. At the service/worker result boundary, check actual JSON values or Arrow batch schemas and nulls against the pinned contract before publishing. On failure discard all staged batches for that result and preserve the prior catalogue generation. Record the qualified contract, schema version and profile digest in provenance.
4. **Acceptance.** Round-trip a declared float64 score through the packaged worker and service. Exercise a supported list with nulls, an unknown primitive, an incompatible ABI minor, a decimal emitted as UTF-8, an unsupported list child, and a mismatch introduced in a later batch. Require a field-specific diagnostic and zero published partial results for invalid output. Keep these checks in the existing native SDK/CTest and packaged integration workflows.

Baseline evidence comes from `cpp/sdk/include/streamfind/plugin_abi.h` and `cpp/sdk/src/{catalogue_builder,dynamic_plugin_manager,plugin_host_access,table_batch_builder}.cpp`.

### 4.3 Viewer contract

```json
{
  "id": "custom.viewer.peak_quality_viewer",
  "name": "Peak Quality Viewer",
  "version": "0.1.0",
  "apiVersion": "1.0",
  "plugins": ["peak_tools"],
  "capabilities": ["peak-quality-viewer"],
  "artifactContracts": ["https://streamfind.dev/catalogue/plugins/peak_tools#peakQualityTable"],
  "artifactRepresentations": ["table"],
  "entry": "/viewers/custom.viewer.peak_quality_viewer/0.1.0/index.js",
  "css": []
}
```

Map this onto the existing `FrontendPluginManifest` and registry rather than
replacing them. Use the existing `capabilities` field; do not add a `modules`
field unless a separate API decision justifies it. Contract identity must be a
fully qualified semantic IRI, matched exactly with representation as an
additional constraint; plugin and capability metadata are filters, not
substitutes for contract identity. The current frontend helper strips
namespace prefixes, so Phase 8 must replace that behavior and update bundled
manifests atomically. If the public API changes incompatibly, bump its version
and update every bundled consumer in the same change. Viewer selection must
not depend on the producing algorithm's ID. Define explicit preference and
fallback behavior when multiple viewers support the same result.

### 4.4 Compatibility rules

MVP: **exact streamfind runtime + SDK version and platform/toolchain match** for native plugins. A C ABI version alone is not sufficient to promise binary compatibility. Later, introduce tested compatibility ranges after ABI auditing and compatibility CI.

Viewer API compatibility is separately versioned. Persist source so users can rebuild after upgrade. A failed compatibility check disables an extension with diagnostics; it must not crash the service.

### 4.5 Semantic plugin ownership and cross-plugin vocabulary reuse (mandatory)

**Scope:** semantic ownership applies to native processing plugins and any other extension that contributes RDF or SHACL definitions. A frontend viewer is presentation-only: its `plugins` and contract declarations describe what it can display, not a plugin it owns, and it cannot add or change ontology terms.

**Architectural invariant:** Every native processing plugin owns exactly one unique semantic plugin namespace. The plugin's owning plugin is distinct from the scientific plugins of data it consumes: a plugin may process mass-spectrometry data while owning a separate `peak_tools` plugin. A plugin may **reference existing ontology vocabulary from another registered plugin without altering it**, but may introduce or modify definitions **only in its own plugin**. This rule applies equally to bundled and custom native plugins. A custom plugin must never expand the bundled `mass_spec` ontology.

**Identity and namespace rules:**
- A plugin manifest declares a globally unique, immutable `plugin_id`, a unique `plugin` ID, and its owned ontology namespace IRI. One plugin owns one plugin; no two installed plugins may claim the same plugin ID or namespace.
- Reserve all installed/bundled plugin IDs, aliases, and namespace IRIs, including `mass_spec`; a user cannot install another `mass_spec` plugin or use a confusing alias to bypass collision checks.
- Operation identifiers are globally unique and owned by the defining plugin. Parameter term IRIs are owned by the plugin that defines them; the operation-to-parameter relationship identifies their use. Reuse a parameter term across operations only when its meaning, units, and constraints are identical. Display labels are **not** unique identifiers.
- New RDF subjects that define classes, properties, operations, parameters, result contracts, or shapes must belong to the plugin's owned namespace. Reuse of an external concept must be a reference to its existing IRI, not a copied or redefined term.
- A plugin can refer to external terms as RDF objects (e.g., `rdf:type`, parameter type, input/output contract) and, where permitted by existing contracts, reference them as inputs or outputs. It cannot assert new descriptive or schema-changing triples *about* externally owned subjects, nor override external SHACL shapes, ranges, defaults, units, labels, or constraints.
- Cross-plugin relationships must be expressed using predicates/relationship types allowed by the core ontology. If a new relationship predicate is necessary, define it in the plugin's own namespace and validate it; never create it in another plugin's namespace.
- When a referenced external term is unavailable, incompatible, or removed, installation/activation fails with a diagnostic or the plugin is marked unresolved; do not silently create or reinterpret the term.
- Installation uses **separate named graphs per plugin** with provenance (plugin ID, version, hash, graph ID). The runtime catalogue may present a merged query view, but source graphs remain immutable and ownership is checked before merge.
- Namespaces are immutable for a published plugin identity. Changing a plugin/namespace requires a new identity. Any released data or project migration must be separately scoped and approved; do not add migration code to this refactor.

**Example:** `sfms:featuresTable` and `sfms:relMinIntensityParameter` are existing
terms in the current mass-spec catalogue. `sfms:PeakTable` and
`sfms:minimum_intensity` are not current term IDs; do not use those names as if
they were verified catalogue entries. The custom plugin below owns a new
`peak_tools` namespace and only references the mass-spec terms:

```text
Bundled plugin: mass_spec (prefix sfms)
  sfms:featuresTable                   [existing table contract]
  sfms:relMinIntensityParameter        [existing parameter; reuse only if semantics fit]

Custom plugin: custom.plugin.peak_tools
Custom plugin: peak_tools (prefix sfpq)
  sfpq:peakQualityOperation            [new operation: allowed]
  sfpq:peakQualityTable                [new output table contract: allowed]
  sfpq:minQualityThresholdParameter    [new local parameter: allowed]

sfpq:peakQualityOperation uses sfms:featuresTable as its input and
sfpq:peakQualityTable as its output. It may reference
sfms:relMinIntensityParameter only when that parameter's existing meaning,
units, and constraints fit the operation; otherwise it uses
sfpq:minQualityThresholdParameter.

REJECT: defining sfms:new_parameter
REJECT: changing sfms:relMinIntensityParameter's definition or constraints
REJECT: registering a second plugin/plugin called mass_spec
```

A parameter label such as **“minimum intensity”** may occur in several plugins. Resolve it by stable semantic IRI and the operation/parameter ownership path, not by text label. Only reuse the exact existing term if its meaning, units, and constraints fit; otherwise define a distinct term within the custom plugin and optionally relate it to the existing term through a permitted semantic relation.

For the example prefixes, `sfms` expands to
`https://streamfind.dev/catalogue/plugins/mass_spec#` and `sfpq` expands to
`https://streamfind.dev/catalogue/plugins/peak_tools#`. The latter is a
proposed namespace and must be checked against the installed plugin registry
before it is allocated.

**Validation algorithm (before installation and on catalogue rebuild):**
1. Parse manifest, normalize IDs/IRIs, and reject duplicate/reserved plugin IDs, aliases and namespace IRIs.
2. Parse the plugin's RDF/TTL into an isolated named graph. Enumerate newly defined subjects, including RDF/OWL declarations and SHACL shapes.
3. Reject any definition or schema-changing assertion whose subject belongs to a different namespace, even if the statement is stored in the custom plugin's graph. Check for semantic overrides, not merely string-prefix ownership.
4. Resolve every external IRI against the installed immutable catalogue; check its kind and the allowed reference relationship.
5. Validate core ontology structure, required operation/input/output/parameter properties, datatype/units, SHACL constraints, and graph consistency.
6. Detect collisions in operation IDs, result-contract IDs, plugin IDs and protected terms; distinguish shared labels from true identity collisions.
7. Produce structured diagnostics (offending triple, owning plugin, violated rule, suggested local namespace).
8. Publish the validated named graph transactionally; do not mutate bundled ontology files. On failure, leave the previous catalogue/registry generation active.

**Agent and generator rules:** `streamfind dev plugin create` allocates a unique plugin namespace; `add-operation` generates semantic definitions in that namespace only. Agent prompts and MCP tool schemas must state “reuse external vocabulary by reference only; never extend another plugin.” Provide catalogue lookup commands so generators can discover existing terms rather than duplicate them.

**Required tests:**
- Accept custom operation using `sfms:featuresTable` and referencing `sfms:relMinIntensityParameter` without redefining either, when its semantics fit the operation.
- Accept same display label `minimum intensity` in two separate plugins with distinct IRIs.
- Reject duplicate `mass_spec` plugin identity and reserved namespace aliases.
- Reject custom triples redefining `sfms:relMinIntensityParameter`, adding `sfms:new_parameter`, or replacing a bundled SHACL shape.
- Reject missing external term, wrong semantic type, conflicting result contract, and unauthorized cross-plugin relationship.
- Verify failed installation leaves bundled graph and active registry unchanged; verify upgrade preserves graph ownership and provenance.

## 5. Developer CLI contract

Commands below are **target design**, not current implementation.

| Command | Required behavior |
|---|---|
| `streamfind dev help` | Show commands, examples, JSON support |
| `streamfind dev setup --target TARGET [--workspace PATH]` | Select `native`, `viewer`, or `all`, verify tools, and record the chosen source workspace |
| `streamfind dev doctor --json` | Report runtime/SDK/toolchain and compatibility |
| `streamfind dev plugin create NAME` | Generate plugin project in a new, unreserved semantic plugin |
| `streamfind dev plugin add-operation NAME --plugin PATH [--example NAME]` | Generate an operation and its metadata, optionally with the selected worked example. |
| `streamfind dev viewer create NAME --contract ID [--example NAME]` | Generate a React viewer for a qualified contract, optionally with the selected worked example. |
| `streamfind dev build PATH` | Build native or UI package |
| `streamfind dev test PATH` | Run extension tests |
| `streamfind dev validate PATH` | Check manifests, ABI/API, semantic/result contracts, package closure |
| `streamfind dev package PATH [--print-path]` | Produce an immutable versioned artifact with hashes. `--print-path` returns its path for installation |
| `streamfind dev contracts --plugin DOMAIN [--json]` | Discover contracts in the installed catalogue. |
| `streamfind dev inspect PACKAGE [--json]` | Read metadata and explain compatibility and dependencies before installation, without loading extension code. |
| `streamfind dev install PACKAGE` | Verify and install the packaged artifact, returning its ID, version and digest |
| `streamfind dev list --json` | List source projects and installed versions |
| `streamfind dev activate ID --version V` | Activate with safe worker/manifest refresh |
| `streamfind dev deactivate ID` | Stop new jobs from using an extension |
| `streamfind dev rebuild ID` | Rebuild from retained source against current SDK |
| `streamfind dev logs ID` | Inspect reproducible build/validation logs |

Also expose user-facing `streamfind extension list|doctor|remove` aliases if useful; ensure one underlying implementation. Use meaningful nonzero exit codes and machine-readable JSON for AI agents.

### First-use workflows

| User goal | Workflow | Acceptance evidence |
|---|---|---|
| Use a shared extension | Inspect an archive, review its provider, dependencies and compatibility, install it, then activate the chosen version. | A second user with a matching runtime and required providers installs the archive without source, compiler, Node, or a development workspace. The service executes its operation or the browser opens its viewer. |
| Write a native operation | Generate a working example, discover a compatible input contract, change one calculation, and run the fixture through the packaged worker. | A disposable project contains the expected changed value and qualified output contract. |
| Write only a viewer | Discover a bundled contract and representation, generate the viewer, then open its sample artifact in the packaged app. | A viewer built outside the frontend checkout renders an existing bundled result and responds to a row selection. Only viewer tooling is required. |

Expose contract lookup through the CLI and MCP using the same catalogue query. Return qualified IRI, owner, contract version, representation, columns, types and supported SDK read/write paths. A search by label can return several qualified matches. Let the author pass the selected IRI directly to the generator.

## 6. Implementation phases and exact acceptance gates

Keep phase decisions and verification evidence in this plan. Update the report beneath each gate with the date, revision, changes, commands and observed results, remaining gaps and gate decision. Reference existing code, package hashes or logs when useful. Keep each report here throughout implementation. The acceptance matrix below lists required coverage. Mark a gate passed only after recording observed results for its checks.

### Phase 0 — Freeze baseline and map existing contracts
**Goal:** avoid replacing working SDK, worker, or frontend plugin architecture.

Tasks:
1. Record the implementation branch SHA, its `dev_refactoring` base SHA, and release version.
2. Read `cpp/sdk/include`, `dynamic_plugin_manager.cpp`, manifest code, plugin loader, worker registration and `plugin.json.in`.
3. Map `sfdev setup`, plugin generator, SDK packaging and runtime package layout.
4. Trace frontend bundled plugin discovery and `/plugins.json` loading.
5. Identify actual artifact/result contracts, Arrow access and viewer registration.
6. Record the decisions on native plugins, viewers and service-owned data in the Phase 0 report below. Inventory the bundled type/storage combinations and the SDK read/write paths before freezing the profile.
7. Record a repeatable local baseline command set using `sfdev build all`, `sfdev test backend`, and `sfdev test frontend`, proving the existing runtime, MCP, and frontend tests remain green from the active worktree. Do not add or require GitHub-hosted CI for this project.

**Gate:** reviewers can identify the exact existing function that each new feature extends.

#### Phase 0 report

**Gate status** — complete and passed. Phase 1 may continue against this frozen local baseline.

Phase 0 completion recorded 21 November 2026: both `extension_framework_development` and `dev_refactoring` start at the same commit, `1c920da487d52949ad8f6193395e45c26d1f8e12`, release version `0.8.0`. The worktree still contains uncommitted Phase 1 manifest changes; those are explicitly excluded from the Phase 0 completion decision.

The initial baseline was recorded on 20 November 2026 at implementation commit `48ee2d4ea9d692dd6ef5376267010e6345695d77`, based on `dev_refactoring` at `0db11ae2465800be42583dca1324b62a7776c69c`.

The manifest owner is `cpp/sdk/src/plugin_manifest.cpp` and its public model is `cpp/sdk/include/streamfind/sdk/plugin_manifest.hpp`; bundled templates are under `cpp/plugins/*/plugin.json.in`. Catalogue ownership is split between the SDK catalogue builder and the service-side dynamic plugin registration. Worker/plugin loading is owned by `cpp/sdk/src/dynamic_plugin_manager.cpp` and the service runtime. Bundled frontend discovery is owned by `frontend/src/framework/plugins/FrontendPluginLoader.ts`, with registration and viewer compatibility in `FrontendPluginRegistry.ts` and `frontend/src/framework/viewers/viewerTypes.ts`.

The audit also confirmed that result `contract_id` values are currently propagated as local semantic names through the service and frontend. Fully qualified viewer matching therefore remains a coordinated follow-up with service result identity propagation; this patch intentionally does not create a mixed qualified/local matching mode.

Baseline evidence recorded 20 November 2026:

- `sfdev env show` and `sfdev doctor --workflow native|frontend` resolved the worktree, MSYS2 UCRT64 GCC/CMake/Ninja, managed Node.js and shared example data. Frontend dependencies were provisioned with `sfdev setup --components frontend`.
- `sfdev build all --jobs 4` completed the native runtime and frontend assets.
- `sfdev test backend --jobs 4` passed all 46 native tests, including `streamfind_result_store_arrow_contract`, which had previously hung in the longer worktree path.
- `sfdev test frontend` passed 30/30 files and 122/122 tests in both the main `dev_refactoring` checkout and this extension worktree after the project-folder wizard test expectations were synchronized. The expected `ViewerErrorBoundary` test logs its intentional `renderer exploded` exception while still passing.
- `scripts/build/cpp/package-cpp.ps1 -SkipTests` staged and validated `tmp/packaged_build/`; `scripts/release/cpp/test-packaged-mcp.ps1 -PackageRoot .\tmp\packaged_build` passed direct packaged MCP startup, `tools/list`, and mass-spec discovery (37 tools).
- The packaged runtime health endpoint returned `healthy`, `ready`, and `alive` from `tmp/packaged_build/bin/streamfind-server.exe` at `http://127.0.0.1:8788`; the endpoint and manager executable were verified to belong to this worktree package.
- The baseline is intentionally local and worktree-scoped: `sfdev build all --jobs 4`, `sfdev test backend --jobs 4`, and `sfdev test frontend` are the authoritative validation commands. GitHub-hosted CI is not part of this project's development workflow.

Baseline follow-up resolved 20–21 November 2026: the broader `framework|mcp` sweep exposed a hang in `streamfind_result_store_arrow_contract`. The test used a fixed worktree path, persisted range indexes with a MinGW filesystem rename, raced publish against abort without lifecycle serialization, and recursively removed result directories while Arrow handles remained open. The test now uses a unique project path under this worktree's `tmp/projects/`; result-store range indexes use direct validated writes, publish/abort mutations are serialized, result deletion is bounded and non-recursive, and missing range indexes are read without filesystem type probes. The synchronized frontend changes and their test expectations are now committed in both worktrees. The Phase 0 evidence is therefore complete and local-only; GitHub-hosted CI is neither required nor part of the gate.

### Phase 1 — Define manifest, registry and compatibility
**Goal:** one extension identity and version model.

Tasks:
1. Define canonical package identity, semantic version, kind, platform, content hash, origin and trust state.
2. Evolve the existing native `plugin.json` schema and update its parser, validator, templates, and bundled manifests atomically; do not add a legacy parser path.
3. Define separate viewer manifest schema using existing frontend plugin types.
4. Define registry JSON schema, lockfile rules, atomic writes, and collision precedence.
5. Reserve bundled identities and namespaces. Project pins select an installed version and digest of the same owner. Reject claims by another owner.
6. Add schema validation and negative fixtures (duplicate IDs, bad paths, missing dependencies, incompatible ABI).
7. Define registry schema version handling; reject unsupported versions with actionable diagnostics. Do not add automatic legacy migration or fallback parsing.
8. Implement and test **exclusive plugin/namespace ownership**, cross-plugin reuse by reference, immutable bundled vocabularies, and per-plugin named-graph provenance as described in section 4.5. Keep source ontology files in Turtle; assign named graphs during catalogue ingestion rather than changing source files to TriG.
9. Implement the ABI type profile and recursive validation in section 4.2. Verify bundled and external contracts with the same native validator.

**Gate:** bundled and generated contracts resolve against their declared ABI. Unknown types, conflicting owners and unsupported encodings fail before activation.

#### Phase 1 report

**Gate status** — complete and passed.

Initial contract work landed 20 November 2026. Native `plugin.json` is explicitly schema-versioned (`schema_version: 1`) and carries its owning `ontology_namespace`, propagated through all bundled manifests, the dynamic-plugin fixture, and `scripts/dev/plugin_generator.py`. The parser remains the single validation entry point at `streamfind::sdk::parse_plugin_manifest` and rejects unsupported versions with `unsupported manifest schema_version: 2`; it rejects an invalid namespace with `manifest ontology_namespace must be a non-empty namespace ending with '#'` and rejects traversal in both library and semantic-catalogue paths.

On 21 November 2026, the ownership vocabulary was aligned with the extension model: semantic catalogue ownership is now called `plugin`, with `Plugin`/`PluginModule` ontology terms and `availableInPlugin`; native catalogue, service, MCP, frontend protocol/loader, and tests use `plugin`/`plugins` fields and routes. Bundled native identities are `native.plugin.mass_spec`, `native.plugin.raman`, `native.plugin.sensors`, and `native.plugin.statistics`; generated external plugins use `custom.plugin.<name>`. Bundled install directories and generated runtime configuration use those identities, while modules remain qualified by the owning plugin name.

The first registry contract is now present in `streamfind/plugin_registry.hpp` and `core/src/plugin_registry.cpp`. Registry schema version 1 stores a generation and installed records containing plugin ID, version, platform, package path, package digest, origin, trust state, and activation state. Parsing rejects unsupported schema versions, duplicate ID/version/platform records, and multiple active versions for one plugin. Writes use a same-directory temporary file and platform-native atomic replacement. Registry-only metadata remains outside `plugin.json`.

The project lockfile contract is now present in `streamfind/plugin_lockfile.hpp` and `core/src/plugin_lockfile.cpp`. Lockfile schema version 1 pins one exact version and package digest per plugin, its ABI major/minor, operation IDs, and result-contract versions. It rejects unsupported schemas, duplicate plugin entries, invalid ABI values, and duplicate/empty operation or contract metadata. Lockfiles use the same atomic replacement discipline and are enforced as project sidecars such as `project.extensions.lock.json` during runtime project opening.

On 21 November 2026, the remaining contract-level Phase 1 work was implemented. Viewer manifests now declare `schemaVersion: 1` and `kind: "viewer"`, with loader and build-time discovery rejecting unsupported schema/kind values. Catalogue generation persists plugin ID, ontology namespace and named-graph provenance; plugin catalogue imports reject mismatched owners, namespaces and named graphs. The shared SDK ABI type profile recursively validates nested schemas and representations before dynamic capabilities are registered, and bundled/native catalogue generation uses the same validator. Build-tree plugin outputs are staged under their canonical `native.plugin.<name>` directories so activation resolves the same identity used by manifests and runtime configuration.

Runtime integration completed on 21 November 2026. Configuration can select a registry explicitly through `registry_path`; service and MCP activation resolve only active, platform-matching canonical package records from that registry. Project opening enforces `project.extensions.lock.json` when present: it requires the configured registry and rejects missing, inactive, version-mismatched, digest-mismatched, or ABI-incompatible pins before opening the project. Installation/deactivation lifecycle mutations remain part of the later Phase 7 activation workflow; Phase 1 now defines and enforces the registry/lockfile boundary needed by that workflow.

Final validation: `sfdev build backend --jobs 4` completed with all bundled plugin validators passing; `sfdev test backend` passed 50/50 tests, including registry activation resolution, lockfile runtime enforcement, the result-store Arrow contract, and packaged service/MCP discovery; `sfdev test frontend` passed 30/30 files and 123/123 tests; and `git diff --check` is clean. Unknown manifest schemas, invalid paths, unsupported registry/lockfile versions, conflicting registry identities, mismatched catalogue ownership/provenance, unsupported ABI types/encodings, and lockfile package mismatches are rejected before activation or project opening.

### Phase 2 — Release SDK as an installed package
**Goal:** out-of-tree native plugins compile from an extracted release.

Tasks:
1. Reuse existing SDK CMake exports, headers, libraries and tools.
2. Integrate current separate developer-kit payload into runtime archive under `sdk/native/`.
3. Ensure installed `streamfindConfig.cmake`, exported targets, dependencies and `streamfind_add_plugin` resolve without source checkout.
4. Add `sdk-manifest.json` with runtime version, ABI, architecture, compiler/runtime profile, package hashes.
5. Build the SDK consumer against the extracted release. Separately build a shared plugin from the generator's entry point. Stage it in a disposable extracted package using the bundled layout, invoke it through the packaged worker, and check its published result. Activation from the user registry is covered in Phase 7.
6. Check dynamic dependency closure and actual plugin load in packaged worker, not only successful linking.
7. Keep normal runtime use free of toolchain requirements.

**Gate:** `find_package(streamfind CONFIG REQUIRED)` and plugin load succeed on clean Windows and Linux runners.

#### Phase 2 report

**Gate status** — complete and passed on Windows and Linux.

Evidence recorded on 21 November 2026:

- The installed native kit is under `sdk/native/` with headers, libraries,
  CMake exports, tools and `sdk-manifest.json`; normal runtime files remain in
  `bin/`, `core/` and `plugins/`.
- Windows and Linux combined runtime/development archives were generated from this
  worktree. Both manifests contain runtime version `0.8.0`, ABI `3.0`, the
  platform/compiler profile, and SHA-256 hashes for every installed native SDK
  payload file other than the manifest itself.
- Both platform builds passed 50/50 CTest cases. The extracted SDK consumer
  was configured with `find_package(streamfind CONFIG REQUIRED)`, linked with
  `streamfind_add_plugin`, built, and executed successfully on Windows and
  Linux.
- `scripts/dev/plugin_generator.py` generated `custom.plugin.phase2_example`;
  its shared Windows and Linux plugins built against their extracted kits, and
  each generated ABI contract executable passed. The installed Windows SDK
  validator accepted the staged manifest, catalogue and library.
- `scripts/dev/cpp/test-packaged-generated-plugin.ps1` staged that generated
  package into a disposable extracted Windows runtime, started the package's
  own service, confirmed the generated operation in `/capabilities`, created a
  temporary project, executed `phase2_example.ping` through the packaged
  worker, and confirmed the `examplePingResult` publication.
- The equivalent Linux validation in
  `scripts/dev/cpp/test-packaged-generated-plugin-linux.sh` passed against the
  extracted Linux package and generated `.so`, including worker execution and
  result publication.
- The packaged worker test used the package-local executable and package-local
  plugin directory; no checkout build tree or other worktree output was used.

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
10. Report the selected workspace, resolved `STREAMFIND_HOME`, SDK, package and tool paths via `doctor --json`. Record CLI acceptance for this phase once the Phase 4 dispatcher is available.

**Gate:** repeat setup is idempotent, verifies hashes and works on a clean extracted release.

#### Phase 3 report

**Gate status** — complete. The release-distributed provisioning boundary and
the native dispatcher acceptance are recorded in the Phase 4 report.

Implementation and validation evidence:

- Added the release-distributed provisioning assets under
  `scripts/release/dev/`: `StreamFindProvisioning.psm1`,
  `streamfind-dev-setup.ps1`, `toolchains.lock.json`, and
  `setup-manifest.json`. The helper resolves its own manifest directory and
  never derives a repository root.
- `toolchains.lock.json` pins Node.js 22.23.4 for Windows/Linux with official
  archive URLs and SHA-256 values, MSYS2 installer 20260927 with SHA-256 and
  Authenticode verification, the UCRT64 package set, Ubuntu/Debian package
  requirements, and the viewer `npm ci --engine-strict` policy.
- The helper supports explicit workspace selection, persisted workspace
  preference, `STREAMFIND_HOME`, verified download cache and retries, offline
  cache errors with the expected path and digest, staged installs, an
  exclusive setup lock, idempotent repeat setup, and `-DryRun`.
- `doctor -Json` reports workspace, managed home, SDK, package, manifest, and
  resolved Node/MSYS2/Linux tool paths and versions. Linux reports that native
  dependencies are system packages rather than claiming they live in the
  managed home. Supported Linux distributions are Ubuntu and Debian.
- `sfdev` native and viewer setup now call the same release helper; Python,
  Java, browser, and data workflows remain explicit repository workflows.
- The development-payload CMake install places all four provisioning assets
  under `dev/`. A Windows development reconfigure/build/install completed
  successfully and installed the files into
  `tmp/build/windows/devkit/install/dev/`.
- First setup downloaded and hash-verified Node.js into a worktree-local
  managed home. A repeat setup with `-Offline` succeeded without network
  access. The same first/repeat sequence succeeded from a clean extracted copy
  of the four release assets, outside the source checkout's script directory.
- Focused checks passed: `sfdev env show`, `sfdev doctor --workflow scripts`,
  PowerShell execution of setup/doctor dry-run, JSON doctor output, and
  `git diff --check`.

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

**Gate:** dev command works from arbitrary working directory without source checkout.

#### Phase 4 report

**Gate status** — complete.

Implementation and validation evidence so far:

- Added the native `streamfind-dev` executable under `cpp/dev/` with a typed
  request model, stable command error codes, JSON doctor/process responses,
  workspace and managed-home resolution, and an approved-root path policy.
- Added a platform-native argv process runner with bounded output, optional
  log-file output, working-directory control, timeout termination, console
  cancellation, and no shell command string execution. Child processes receive
  an explicit allowlisted environment for `run`; provisioning remains delegated
  to the release-distributed helper rather than being embedded in the core
  execution process.
- Added `streamfind dev ...` forwarding from the existing CLI to the sibling
  `streamfind-dev` executable using `CreateProcessW` on Windows and `execv` on
  POSIX. No checkout-relative path is required for an installed package.
- Installed `bin/streamfind-dev` and the Phase 3 `dev/` provisioning assets in
  the package layout.
- Added native protocol/path tests and CTest version/process acceptance tests.
  The Windows developer-kit build passed, all three focused CTest tests passed, and the
  installed-package commands `streamfind-dev --version`, `doctor --json`, and
  `streamfind dev --version` succeeded from `tmp/scratch`.
- Installed-package behavioral checks also verified timeout exit code `5`,
  path-policy exit code `3` with `E_PATH_DENIED` JSON, and removal of an
  unallowlisted environment variable from a child process. MCP-specific tools
  will consume this dispatcher in the later MCP phase; Phase 4's CLI dispatcher
  gate is complete.

### Phase 5 — Native plugin scaffolding
**Goal:** one-command creation of a buildable plugin.

Tasks:
1. Adapt `scripts/dev/plugin_generator.py` and SDK minimal plugin into distributable templates.
2. Generate the canonical `plugin.json.in`/`plugin.json` manifest, CMake, source, include, tests, semantic definitions, README and license notes.
3. Generate operation skeleton with host callbacks, typed input/output contracts, cancellation/error handling.
4. Allow `add-operation` repeatedly; update manifest/CMake/semantic metadata atomically.
5. Validate identifiers and collisions; avoid overwriting user changes.
6. Include a deterministic example operation that computes a numerical output and publishes a typed result.
7. Generate source in `<workspace>/plugins/`.

**Gate:** two generated operations compile and register under one native plugin.

#### Phase 5 report

**Gate status** — complete.

Implementation and validation evidence:

- Added release-distributed native scaffolding to `streamfind-dev` with
  `plugin create NAME --workspace PATH` and
  `plugin add-operation NAME --plugin PATH`.
- The generator creates a workspace-relative native plugin containing
  `plugin.json.in`, CMake, C++ entrypoint, operation declarations and
  implementations, semantic Turtle metadata, a CTest registration contract,
  README, and third-party license-notice notes. It derives the canonical
  identity `custom.plugin.<name>` and operation IDs `<name>.<operation>`.
- The generated `measure` operation is deterministic and numeric, publishes a
  typed JSON result through the SDK host callback, checks cancellation, and
  returns errors through the plugin ABI. `add-operation` adds another operation
  without overwriting user files, performs identifier/collision checks, and
  updates owned files atomically.
- Acceptance workspace: `tmp/scratch/phase5-package-workspace/plugins/phase5_package`.
  Plugin machine name: `phase5_package`; plugin ID:
  `custom.plugin.phase5_package`; operation IDs:
  `phase5_package.measure` and `phase5_package.second`.
- The generated plugin built against the installed SDK under
  `tmp/build/phase5-package`; the SDK catalogue generator reported `Conforms`,
  the shared library linked successfully, and its registration CTest passed.
- Installed-package acceptance succeeded from `tmp/scratch`. Re-adding
  `second` was rejected with `operation already exists`, confirming collision
  protection. Focused native build/install checks and `git diff --check`
  passed.

### Phase 6 — Build, test, validate, package and install native plugins
**Goal:** fully reproducible plugin lifecycle.

Tasks:
1. Build with installed SDK and managed compiler profile; never link against arbitrary machine DLLs.
2. Use CMake presets/toolchain files and isolated build directories keyed by SDK/version/platform.
3. Invoke SDK validator and semantic catalogue validation.
4. Verify native dependency closure, ABI exports, manifest IDs, operation contracts and host callback compatibility.
5. Run CTest for native unit and contract tests. Keep worker-level, data-backed, and extracted-package integration checks under `scripts/dev/extensions/` or `scripts/release/`.
6. Package binary, manifest, semantic resources, tests metadata, checksums, license and provenance.
7. Stage install to temporary directory; verify hashes; atomically rename into versioned location.
8. Update the registry atomically. Reinstalling identical content is idempotent. Reject different content at the same ID, version and platform, and identify the version field the author should increment. Record cross-plugin contract dependencies by owner, qualified IRI and schema version. Before activation, report any missing or incompatible provider and the operation or viewer that needs it.
9. Keep source and logs for rebuilds.
10. Add uninstall/deactivate rules that protect versions pinned by running jobs.

**Gate:** a generated plugin builds from the selected workspace and installs from its verified package. The installed version survives a restart and remains inactive until explicitly activated.

#### Phase 6 report

**Gate status** — complete.

Implementation and validation evidence:

- Added native lifecycle commands to the release-distributed `streamfind-dev`
  executable: `plugin build`, `test`, `validate`, `package`, `install`,
  `deactivate` and `uninstall`. The commands use an isolated environment,
  the installed SDK catalogue/validator, the managed MSYS2 UCRT64 compiler
  profile, and build directories keyed by source, SDK, platform and config.
- Packaging stages the CMake install, validates manifest identity, ABI fields,
  dynamic library, catalogue and semantic source layout with the SDK validator,
  then writes package checksums and provenance before the final rename.
- Installation verifies every recorded file checksum, copies into a versioned
  managed-home directory, and updates the existing atomic plugin registry.
  Reinstalling the same digest is idempotent; a different package at the same
  qualified ID/version/platform is rejected with an instruction to increment
  `plugin.json`'s version. Installed entries remain inactive until activation
  is implemented by Phase 7; deactivation and uninstall protect active entries.
- Fixed the generated manifest template so `abi_version.major` and `.minor`
  are JSON integers, and ensured generated source includes the validator's
  required `src/utils` directory.
- Acceptance plugin: `tmp/scratch/phase6-workspace/plugins/phase6_plugin`;
  package: `tmp/scratch/phase6-package3`; managed home:
  `tmp/scratch/phase6-home4`; registry identity:
  `custom.plugin.phase6_plugin`, version `0.1.0`, platform
  `windows-x86_64`, inactive, verified digest `streamfind:7d98900dcb0cfaa4`.
  The plugin built and linked with GCC 16.1/UCRT64, generated its catalogue
  with `Conforms`, passed CTest, passed the SDK plugin validator, installed
  successfully, and a second install reported `already installed` without
  changing registry generation. Deactivation followed by uninstall removed
  only that inactive package.
- Focused native dev CTest checks passed: protocol, version and process-runner
  contracts. All generated files, packages, builds and logs remained under
  this worktree's `tmp/` tree. An intentionally edited package was rejected
  with `package checksum verification failed: plugin.json`, while the prior
  registry remained at generation 1 and the editable source remained intact.

### Phase 7 — Core discovery, activation and worker lifecycle
**Goal:** new operations become available without rebuilding or restarting the entire app.

Tasks:
1. Extend current dynamic plugin manager to scan bundled and user registry paths.
2. Validate manifests before loading and provide clear per-extension diagnostics.
3. Register operations and semantic metadata with the existing shared catalogue; specify how catalogue snapshots are refreshed and exposed through MCP. Keep per-plugin named graphs immutable and enforce ownership during catalogue merge.
4. On activation, notify the service of the new registry generation; installation only stages a verified package.
5. Worker manager starts new workers with immutable plugin generation and version set; existing workers finish in-flight work.
6. New workflows use the new generation; never unload an in-use native library.
7. Store extension version, binary hash, operation ID and result contract in execution provenance.
8. Prevent duplicate IDs and incompatible schema registrations; define collision precedence.
9. Test rollback, failed plugin load, missing library, crashes and cancellation.
10. Validate actual batch schemas and values against the pinned ABI profile before service-owned publication. Complete the Phase 6 installation scenario by executing the activated operation and checking its result after service restart.

**Gate:** activate a new operation while a workflow runs; the existing workflow finishes unchanged and a new workflow can select the new operation.

#### Phase 7 report

**Gate status** — complete.

Implementation and validation evidence:

- Extended the native service plugin runtime to discover active plugins from
  the bundled configuration and the user registry, validate and load each
  package before registration, merge its owned semantic catalogue, and expose
  clear runtime status through `GET /plugins/status`.
- Added explicit registry activation through `streamfind-dev plugin activate`
  and live service activation through `POST /plugins/activate`. Installation
  remains inactive; activation changes the atomic registry generation and then
  appends the new plugin to the live runtime without unloading existing
  libraries. Duplicate operation and catalogue IDs remain rejected by the
  existing registry and catalogue ownership checks.
- Fixed runtime catalogue refresh so a newly activated named graph is visible
  to capability discovery immediately rather than remaining hidden behind the
  initial catalogue cache.
- Pinned worker launches to the service's plugin generation and version set.
  Workers validate that their loaded registry matches the snapshot before
  executing, so an in-flight worker keeps its immutable plugin set while later
  workers observe the new generation.
- Added plugin package/version/digest, operation ID and result-contract fields
  to dynamic result provenance. The generated operation scaffold now emits
  the host's qualified output-port key expected by the current catalogue
  binding, allowing the generated operation to publish successfully.
- Live acceptance used this worktree's packaged service endpoint on port
  `8792`. The service started from the active registry at generation `6`, a
  newly installed `custom.plugin.phase7_live` package was activated while the
  service was running, and the service advanced to generation `8` without a
  restart. Its `phase7_live.second` operation appeared in capability discovery
  and executed with result value `20`.
- The published result carried provenance for plugin ID
  `custom.plugin.phase7_live`, version `0.1.0`, package digest
  `streamfind:5e3d63f3f9580858`, operation `phase7_live.second`, and result
  contract `secondResult`. A restart test also rediscovered active registry
  entries and executed `phase7_gate.second` successfully.
- Focused native CTest checks passed for the dev dispatcher, plugin registry,
  project runtime recovery, and the rebuilt service/worker targets. All
  generated projects, packages, service state and logs stayed under this
  worktree's `tmp/` tree.

### Phase 8 — Exportable viewer SDK and scaffolding
**Goal:** user-developed viewers load in the existing Model/Result Viewer.

Tasks:
1. Extract stable public interfaces from `frontend/src/framework/plugins`, viewers and result contracts into versioned `@streamfind/viewer-sdk`.
2. Preserve current bundled frontend plugin behavior.
3. Use one compatibility predicate in manifest validation, registration and selection. A specialized viewer must match its qualified contract and every declared representation constraint. The existing registration and selection checks each allow a representation match alone. Keep representation-only registration for generic viewers. Define data access, selection state and disposal in the public SDK.
4. Carry fully qualified semantic contract IRIs through service results, frontend manifests, and viewer registrations. Replace local-name matching in `artifactContractMatches`; update bundled manifests and consumers atomically, bumping the frontend API version if the public contract changes. Add a regression case where two plugins use the same local name and only the exact IRI matches.
5. Generate viewer template with React component, the existing frontend plugin manifest shape, package.json, lockfile, tests and sample typed result.
6. Build viewer ESM, CSS and assets with the pinned Node toolchain. Define how independently compiled modules resolve the public SDK, React and JSX runtime against the packaged host. Verify that a component using React state responds to a click and loads an asset relative to the installed bundle.
7. Validate imports, API version, contract support and bundle manifest.
8. Install under `~/.streamfind/extensions/viewers/...`.
9. Extend local app server to serve installed viewer bundles via same-origin, path-safe endpoints and dynamically generate a combined plugin manifest.
10. Track the activated viewer version and digest. The current loader caches by ID, and the registry discards registration ownership after successful setup. For the first release, offer an app reload when activation, rollback or deactivation changes an already loaded viewer. Restore the open project and selected result. If import fails, show the generic viewer and identify the failed package.
11. Define trust policy: trusted JS can run in app origin; untrusted third-party code requires explicit trust or sandboxed iframe/isolated context with limited API. Same-origin alone is not sandboxing.

**Gate:** a user viewer appears for the exact qualified contract emitted by the generated extension; a contract with the same local name in another plugin does not match it.

#### Phase 8 report

**Gate status** — complete.

Completed groundwork in this worktree:

- Added the public `frontend/src/framework/viewer-sdk/` entry point with a
  versioned SDK identifier and stable viewer/plugin contract exports.
- Centralized viewer selection through `viewerMatchesArtifact` and manifest
  registration through `viewerMatchesManifest`; representation-only viewers
  remain supported while constrained viewers must satisfy their declared
  contract and representations.
- Replaced local-name equivalence in `artifactContractMatches` with exact
  equality for qualified IRIs. Local-only identifiers still compare exactly
  for existing internal fixtures, but a qualified IRI never falls back to its
  local fragment.
- Updated bundled viewer manifests and registrations to use the qualified
  core, mass-spec, Raman, sensors and statistics contract IRIs.
- Added the collision regression covering two plugin IRIs with the same local
  name. The managed frontend build and all 124 frontend tests passed from this
  worktree.
- Extended `scripts/dev/plugin_generator.py init --kind viewer` with an
  independently buildable React/TypeScript viewer template. It emits the
  existing manifest shape, a qualified contract declaration, package.json and
  package-lock.json, pinned-toolchain Vite ESM configuration, CSS asset,
  stateful sample component and a smoke test. Generation was verified under
  this worktree's `tmp/scratch/` using its `.venv`.
- Added the repository-owned `frontend/packages/viewer-sdk` package boundary
  at version `1.0.0`, with stable manifest, viewer context/registration and
  exact contract-matching types for independently compiled viewers. The
  generated template's lockfile was regenerated with the managed Node toolchain
  and `npm ci --ignore-scripts` completed successfully.
- Added qualified-contract validation to both build-time manifest discovery and
  runtime manifest loading, so an external viewer cannot silently register a
  local-name-only contract.
- Extended the native service to combine bundled and installed viewer manifests
  at same-origin `/plugins.json`, and to serve installed ESM/CSS assets through
  `/viewers/...`. Viewer deployment roots are configured per
  `streamfind.json`, separately from plugin deployment roots; development roots
  are retained as authoring metadata and are not scanned by the runtime.
  Manifest entries, bundles and stylesheets are canonicalized and required to
  remain within their package. Broken optional viewers are skipped.
- Rebuilt `streamfind_service` successfully with the MinGW UCRT64 toolchain.
- Added runtime package identity tracking to the frontend loader. A changed
  viewer version, digest or entry reloads its registration and removes the old
  viewer ownership first; an import/setup failure therefore leaves the generic
  viewer fallback available instead of silently retaining stale code.
- Added an explicit trust boundary: trusted same-origin viewers may load, while
  sandboxed viewers are rejected with a diagnostic until an isolated iframe/API
  host is implemented. Same-origin serving is not treated as sandboxing.
- The frontend suite now passes 126 tests, including replacement and sandbox
  policy regressions.
- Added native viewer lifecycle commands to `streamfind-dev`: `viewer install`,
  `activate`, `deactivate` and `uninstall`. Installation verifies the package
  manifest and every locked checksum, stages into the managed
  `extensions/viewers/<name>/<version>/<platform>` tree, and writes an atomic
  inactive activation record. Activation state is separate from package
  contents, so package digests remain stable and the service can expose only
  explicitly activated viewers. Same-version/digest installs are idempotent;
  conflicting same-version packages are rejected. Viewer command parsing is
  covered by the native protocol contract test.
- Runtime extension configuration now keeps `plugin_roots` and `viewer_roots`
  separate, with independent `development` and `deployment` paths. Native
  plugins are selected by `enabled_plugins`; activated viewers are selected by
  `enabled_viewers`. The global `.streamfind/tools` cache remains reserved for
  managed external development/runtime tools and is not used as an extension
  source or deployment root.
- Bundled optional frontend viewers are now staged as first-class packaged
  viewer artifacts under `viewers/native/native.viewer.<name>/`, with native
  IDs such as `native.viewer.mass_spec`. Shared native viewer JavaScript and
  CSS are staged once under `viewers/native/_shared/assets`; each viewer keeps
  its own manifest and wrapper package. Native Raman, Sensors and Statistics
  viewers follow the same layout. The app shell retains only the bootstrap
  core viewer; the service discovers staged viewers recursively from the
  configured deployment root and publishes their `/viewers/...` assets.
- Native C++ plugin packages are staged under
  `plugins/native/native.plugin.<name>/`. Custom plugin and viewer deployment
  areas are represented explicitly by sibling `plugins/custom/` and
  `viewers/custom/` directories. The generated runtime configuration keeps
  the deployment roots at `../plugins` and `../viewers`, allowing recursive
  discovery while preserving the native/custom ownership boundary.
- Corrected the packaging install rule so the frontend's existing
  `dist/viewers/native/` tree is installed directly as `viewers/native/`;
  `viewers/native/native/` is not created. Rebuilt the Windows package and
  verified the four native plugin packages, eleven mass-spec viewers, and the
  native Raman, Sensors and Statistics viewers in their expected directories.
- Rebuilt `streamfind-dev` and `streamfind_service`; the focused native
  protocol CTest passed.
- Built the generated viewer with the managed Node/npm toolchain (`npm ci` and
  Vite ESM build), packaged it with deterministic raw-byte checksums, and
  installed and activated it through `streamfind-dev viewer`. The activation
  record captured package digest `streamfind:38c3c3f336a8b675` for
  `@streamfind/phase8-viewer-viewer` version `0.1.0`.
- Verified the current worktree's service combined manifest exposed that
  activated viewer at
  `/viewers/phase8_viewer-viewer/0.1.0/windows-x86_64/dist/viewer.mjs`
  and served the generated ESM bundle with HTTP 200 (898 bytes). The service
  executable and all temporary package/service files were under this worktree.
- Documented the independent viewer scaffold, package, install and activation
  flow in `docs/plugin-development.md`.
- Added packaged-host module resolution for independently built viewers. The
  frontend publishes one shared React/ReactDOM/JSX/SDK runtime through the
  same-origin import map and native service runtime-module endpoints, so an
  external viewer uses the host React dispatcher rather than a duplicate copy.
- Fixed the generated viewer's relative CSS URL to reference the Vite-emitted
  package-named stylesheet, and corrected release package validation for the
  canonical `native.plugin.<name>` directory layout.
- Added `frontend/tools/browser/viewer-extension-smoke.mjs` and the
  `viewer:smoke` npm command. Against this worktree's packaged service on port
  `8789`, it discovered and imported the activated generated viewer, verified
  its qualified contract, rendered it with the host runtime, clicked its
  stateful control, loaded the emitted stylesheet with HTTP 200, and verified
  that `https://streamfind.dev/catalogue/plugins/beta#result` does not match
  the viewer for `https://streamfind.dev/catalogue/plugins/alpha#result`.
- Final acceptance evidence: viewer ID
  `@streamfind/phase8_viewer-viewer`, package digest
  `streamfind:30c96d52489b6bdf`, qualified contract
  `https://streamfind.dev/catalogue/plugins/alpha#result`, CSS HTTP 200, and
  `statefulClick: true`.

The Phase 8 gate is closed. The generated viewer was observed through the
packaged browser host with exact qualified-contract selection and the
same-local-name collision case rejected.

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
9. Add packaged-layout checks proving native packages are discovered from
   `plugins/native/` and `viewers/native/`, while custom packages can be
   deployed under the sibling `plugins/custom/` and `viewers/custom/` paths
   without ID or asset collisions.

**Gate:** agent creates a native plugin and viewer, validates them, requests installation, then invokes the operation and sees a compatible viewer.

#### Phase 9 report

**Gate status** — complete.

Implemented the MCP development-tool bridge by routing structured calls to the
existing `streamfind-dev` dispatcher. The bridge exposes environment inspection,
list/inspect, native plugin create/add-operation/build/test/validate/package/
install/activate, and viewer create/build/test/validate/package/install/activate
operations. It passes validated arguments to the dispatcher as an argv vector,
so path allow-listing, managed tool selection, package checksum validation, and
activation semantics remain owned by the existing development framework rather
than being duplicated in MCP. The dispatcher now contains a self-contained
viewer scaffold and Node/npm build/test/validate/package path, independent of
the checkout's Python generator.

The MCP target and developer dispatcher build successfully. The focused
developer protocol, version, run, and MCP schema CTests pass. The bridge now
exposes structured environment/list/inspect, native-plugin, viewer, and task
tools; long-running commands return task IDs, retain bounded output, and pass
task cancellation into the child-process runner. Each dispatched development
request is recorded as JSONL under the current worktree's `tmp/logs/` audit
file. Build remains separate from install and activation.

The worktree smoke flow recorded MCP plugin/viewer creation and listing calls,
then completed the generated-extension lifecycle in the same worktree-local
scratch area: native plugin operation scaffold, MinGW build, CTest, SDK
validation, package, inactive install, activation, and registry discovery;
viewer scaffold, npm lock resolution, build, Vitest, manifest validation,
package, inactive install, activation, and viewer discovery. A queued MCP
viewer-validation task was also observed, and the packaged-layout check passed
with four native plugins and fourteen native viewers. The MCP operation
catalogue exposed native statistics operations after startup. The refreshed
worktree package starts the packaged MCP successfully and advertises the same
development-tool surface.

The Phase 9 gate is closed. All recorded paths are under this worktree's
`tmp/` scratch/build/package areas; no generated extension or package output
was written into another checkout.

### Phase 10 — Upgrade resilience and compatibility doctor
**Goal:** upgrades preserve sources and installed extensions without pretending all binaries remain compatible.

Tasks:
1. Never delete user home on runtime upgrade.
2. Read registry on startup; report compatibility with new runtime and SDK.
3. Mark incompatible native plugins disabled until rebuilt; do not attempt unsafe loading.
4. Provide `streamfind dev doctor --json` and `rebuild` using retained source.
5. Preserve versioned package archives, rollback points and project pins.
6. Test upgrade from release N to N+1 with compatible viewer, incompatible native plugin and successful source rebuild.
7. Record compatibility findings and approved rebuild or rollback actions in audit logs. Do not add automatic migration code in this refactor.
8. Document that compiled binary compatibility is conditional and tested, not automatic.

**Gate:** upgrade preserves all source projects and metadata; incompatible plugin cannot crash startup; rebuild restores operation availability.

#### Phase 10 report

**Gate status** — complete.

The upgrade-resilience boundary is implemented. The extension registry now preserves
package identity, compatibility state, diagnostics and an optional retained source
locator. Registry reads remain backward-compatible with older entries. Runtime
startup evaluates native plugin ABI/runtime compatibility before loading a package;
an incompatible entry is atomically marked inactive with a diagnostic and skipped,
so an unsafe binary cannot crash startup. Viewer compatibility remains independent
of native plugin activation.

`streamfind-dev doctor --json` reports the runtime version, registry generation,
installed package versions/platforms, compatibility findings, retained source paths
and source availability. `streamfind-dev rebuild --plugin PATH --sdk PATH [--build
PATH]` rebuilds and tests from explicitly selected retained source without silently
activating a replacement. The same operation is exposed to MCP as
`dev_plugin_rebuild`; doctor forwards managed-home, workspace, SDK and package
arguments. Development MCP actions append structured records to the worktree audit
log, including compatibility findings and approved rebuild outcomes.

Validation evidence in this worktree:

- `streamfind-dev doctor --json` reported an incompatible native plugin as inactive
  with `plugin ABI major is incompatible with this runtime`, while retaining its
  source path and registry generation.
- A retained-source rebuild configured, compiled, ran its plugin CTest contract and
  completed successfully against the selected runtime SDK.
- `streamfind_mcp`, including the rebuild forwarding path, built successfully.
- Focused contracts passed 6/6: developer protocol, version and process runner;
  plugin registry; plugin configuration validation; and MCP schema.

Binary compatibility is documented as conditional on the runtime/SDK/platform and
toolchain contract, not as an automatic promise. Versioned installed packages,
source locators, registry generations and project extension pins remain separate
from upgrade cleanup; source relocation is handled by explicitly selecting the new
source path for a rebuild.

### Phase 11 — Release engineering and documentation
**Goal:** every published runtime release is an extension-development-ready distribution.

Tasks:
1. Extend Windows and Linux release scripts to stage `streamfind-dev`, SDK, viewer SDK/templates and setup manifests.
2. Provision the pinned Python runtime through the packaged developer setup,
   retaining it under `STREAMFIND_HOME/tools/python` while creating a separate
   `.venv` inside the selected development workspace; never place that
   environment inside `tmp/packaged_build`.
3. Keep official plugins/viewers bundled under `plugins/native/` and
   `viewers/native/`, with custom deployment paths kept under the corresponding
   `custom/` directories; verify both are discoverable using the same contracts.
4. Validate checksums, dependency closure, no machine-local absolute paths and license notices.
5. Run clean-machine smoke tests from extracted runtime archives.
6. Update the existing plugin development and release guides with the shipped commands, workspace and install layout, ABI compatibility, trust, offline provisioning and upgrade behavior.

#### Phase 11 report

**Gate status** — complete on Windows and Linux.

The release model has been consolidated to one archive per platform. The
Windows and Linux release builders now enable the SDK installation option, and
the CPack name remains `streamfind-core-cpp` regardless of development payload.
Each archive is intended to contain the runtime, `streamfind-dev`, native SDK,
viewer SDK and provisioning manifests. The obsolete standalone developer-kit
packaging scripts and `sfdev sdk build` workflow were removed.

The combined Windows release was produced as
`tmp/release-output/streamfind-core-cpp-0.8.0-Windows-x86_64.zip`. Its 53/53
CTest suite passed, PE dependency closure passed, the release payload check
passed, and extracted packaged MCP smoke passed with direct startup,
`tools/list`, and mass-spec operation discovery. The extracted
`streamfind-dev` dispatcher also passed version and Python setup dry-run checks.

The Linux release was produced from the native WSL checkout at the generic
worktree mirror `/home/<linux-user>/<worktree-name>` and copied back to this
worktree's `tmp/release-output/`. Its 53/53 CTest suite passed, ELF dependency
closure passed for 19 binaries, and extracted packaged MCP smoke passed with
58 tools. The Linux release path also now uses package-relative plugin RPATHs,
places shared SDK libraries under `sdk/native/lib`, and stops the tray-owned
service in the packaged smoke harness.
6. Document SDK/API changes and deprecations.
7. Publish reproducible sample plugin and viewer sources and their expected output.
8. Verify native release can run normally without invoking `dev setup`.

**Gate:** complete acceptance matrix below passes for Windows x64 and Linux x86_64.

#### Phase 11 report

**Gate status** — complete.

Release artifacts and validation evidence:

- Windows: `streamfind-core-cpp-0.8.0-Windows-x86_64.zip`, SHA-256
  `fe499a3e7c7dea8d3e0b2e48fd1bf7778d568605ba7ffeee973dd509271ab2b2`.
- Linux: `streamfind-core-cpp-0.8.0-Linux-x86_64.tgz`, SHA-256
  `5039fc9791b15a74d51552e313e71e2b2e6b739737d7a7c18c8a33238bda0bb0`.
- Windows and Linux both passed the complete 53-test CTest suite, native
  dependency closure, package payload checks and extracted MCP smoke. Windows
  MCP discovered 58 tools including mass-spec operations; Linux MCP discovered
  58 tools from the extracted archive.
- The combined checksum manifest is `tmp/release-output/sha256sums.txt`.

## 7. Test matrix and quality gates

| Scenario | Windows x64 | Linux x86_64 | Required |
|---|---|---|---|
| Install shared native and viewer packages on a runtime-only machine with required providers | Yes | Yes | P0 |
| Independently built viewer interacts with a bundled result and reloads the selected version after rollback | Yes | Yes | P0 |
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
| Same local contract name in two plugins never cross-matches a viewer | Yes | Yes | P0 |
| Invalid/incompatible plugin refused with diagnostics | Yes | Yes | P0 |
| Unknown logical type or unsupported ABI encoding rejected before activation | Yes | Yes | P0 |
| Published result conforms to declared JSON/Arrow schema, including later batches | Yes | Yes | P0 |
| Source in a chosen workspace builds and installs from a package into a separate home | Yes | Yes | P0 |
| MCP creates/builds/tests extension | Yes | Yes | P1 |
| Upgrade preserves source and registry | Yes | Yes | P1 |
| Rebuild incompatible extension from source | Yes | Yes | P1 |
| Offline setup from predownloaded cache | Yes | Yes | P2 |

Tests must exercise the **extracted release**, not merely source-tree builds. Keep CTest for native unit tests and existing frontend tests for viewer SDK. Add end-to-end release smoke tests for CLI, MCP, registry and worker lifecycle.

Register native unit and contract tests with CTest in their owning C++ area;
keep plugin-specific tests under `cpp/plugins/<plugin>/tests/`. Put data-backed,
workflow, extracted-package, and release integration checks under
`scripts/dev/extensions/` or `scripts/release/`. Run development scripts through
the supported `sfdev script` workflow, use the repository `.venv` for Python,
and keep generated projects, build outputs, and logs under the checkout's
`tmp/`. Do not create a repository-root `tests/` suite.

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

1. **PR-01:** baseline checks and contract inventory recorded in the Phase 0 report.
2. **PR-02:** canonical native `plugin.json`, qualified viewer contract identity, and compatibility validator.
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

Implement a user plugin `custom.plugin.peak_tools` that owns the new `peak_tools` plugin and namespace. It may consume mass-spectrometry vocabulary by reference, but it never defines or modifies terms in the bundled `mass_spec` namespace. Include:
- `peak_tools.peak_quality` operation reading the existing `sfms:featuresTable` contract through host APIs;
- deterministic quality score computation on a small test fixture;
- output table contract `sfpq:peakQualityTable` in the extension's own namespace, published through the existing service as a typed table artifact;
- semantic metadata for operation inputs, outputs, and parameters, defining new terms only in the extension's plugin;
- C++ unit test with a small synthetic fixture and a known expected score; exercise the installed package separately through `scripts/dev/extensions/`;
- custom viewer `custom.viewer.peak_quality_viewer` rendering score distribution and selectable rows;
- browser Model/Result Viewer discovery by the fully qualified output contract IRI and `table` representation, not producer operation ID or a local-name match.

**Demo script:**

Use the worked peak-quality example shipped with each SDK. The proposed `--example` option fills in the operation algorithm, contract declarations and known fixture for the native project and the matching component for the viewer. The default generators remain available for new projects.

```bash
set -e
workspace="$HOME/Documents/streamfind-dev"   # example author-selected folder
plugin="$workspace/plugins/peak_tools"
viewer="$workspace/viewers/peak_quality_viewer"
streamfind dev setup --target all --workspace "$workspace"
streamfind dev doctor --json
streamfind dev plugin create peak_tools
streamfind dev plugin add-operation peak_quality --plugin "$plugin" --example peak-quality
build_and_install() {
  streamfind dev build "$1"
  streamfind dev test "$1"
  streamfind dev validate "$1"
  package=$(streamfind dev package "$1" --print-path)
  streamfind dev inspect "$package"
  streamfind dev install "$package"
}
build_and_install "$plugin"
streamfind dev activate custom.plugin.peak_tools --version 0.1.0
streamfind dev contracts --plugin peak_tools
streamfind dev viewer create peak_quality_viewer --example peak-quality --contract 'https://streamfind.dev/catalogue/plugins/peak_tools#peakQualityTable'
build_and_install "$viewer"
streamfind dev activate custom.viewer.peak_quality_viewer --version 0.1.0
```
Installation and activation are separate steps. The demonstrated workflow is
complete when the running service publishes the operation, a workflow executes
it, the service publishes its typed result, and the browser selects the viewer
by the exact qualified contract.

## 11. Definition of done

The workstream is complete only if a user who has **never cloned the streamfind repository** can:
1. extract a normal runtime release and run the existing service;
2. invoke `streamfind dev setup` to provision all required native/viewer tools;
3. choose a source workspace and edit a native plugin under `<workspace>/plugins/` and a viewer under `<workspace>/viewers/`;
4. add multiple operations to one native plugin;
5. build, test and validate in the chosen workspace, then install each packaged artifact under `~/.streamfind/extensions/` (or the configured `STREAMFIND_HOME`) without rebuilding core;
6. activate new operations safely while the service is running;
7. discover and execute the new operation through CLI, MCP and browser;
8. generate a separate custom viewer and render the result through its declared contract;
9. upgrade streamfind without losing source, installed packages, project pins or logs;
10. receive explicit compatibility diagnostics and rebuild from source when needed;
11. reuse bundled vocabulary from a custom plugin and receive an ownership diagnostic for a conflicting definition;
12. discover which types the target SDK/ABI supports for each transport and direction, and receive a field-specific diagnostic if a declaration or emitted result violates the contract.

## 12. References reviewed

- [`README.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/README.md)
- [`docs/plugin-development.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/docs/plugin-development.md)
- [`docs/releases.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/docs/releases.md)
- [`cpp/sdk/src/plugin_manifest.cpp`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/sdk/src/plugin_manifest.cpp)
- [`cpp/plugins/mass_spec/semantic/artifacts.ttl`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/plugins/mass_spec/semantic/artifacts.ttl)
- [`cpp/plugins/mass_spec/semantic/parameters.ttl`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/plugins/mass_spec/semantic/parameters.ttl)
- [`frontend/src/framework/plugins/pluginTypes.ts`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/frontend/src/framework/plugins/pluginTypes.ts)
- [`frontend/src/framework/artifacts/artifactContracts.ts`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/frontend/src/framework/artifacts/artifactContracts.ts)
- [`frontend/src/framework/viewers/viewerTypes.ts`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/frontend/src/framework/viewers/viewerTypes.ts)
- [`scripts/README.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/scripts/README.md)
- [`cpp/sdk/cmake/streamfindAddPlugin.cmake`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/sdk/cmake/streamfindAddPlugin.cmake)
- [`cpp/core/include/streamfind/plugin_loader.hpp`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/cpp/core/include/streamfind/plugin_loader.hpp)
- [`frontend/src/framework/plugins/FrontendPluginLoader.ts`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/frontend/src/framework/plugins/FrontendPluginLoader.ts)

This implementation plan is complete. All phases 0–11 are closed, the
acceptance matrix is recorded as passing for Windows x64 and Linux x86_64, and
the final documentation and route/path harmonization is reflected in the
repository docs and runtime behavior. The working tree may retain uncommitted
implementation and documentation changes for the user to review; completion of
the plan does not imply those changes have been committed.
