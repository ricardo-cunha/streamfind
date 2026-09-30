# streamfind Frontend Plugin and Domain Viewer Implementation Plan

**Target baseline:** `dev_refactoring`\
**Primary proof-of-concept:** Mass-spec / NTA feature-table viewer and
curation UI\
**Status:** Proposed implementation plan

## 1. Objective

Extend the current streamfind React frontend toward a frontend plugin and
viewer architecture that complements the existing C++ domain plugin system.
The first implementation mission is a **mass-spec / NTA feature-table
viewer** inspired by the legacy R/Shiny NTS application. This plan
distinguishes the current static visualization implementation from the target
runtime plugin architecture; runtime loading is a future phase, not current
functionality.

The architectural rule is:

> **Workflow nodes compute. Viewers inspect. Editors propose changes.
> Backend operations validate and commit. Artifacts and audit/provenance
> records preserve state.**

The frontend web app is the authoritative visualization surface. MCP is an
execution and artifact-discovery interface, not the primary HTML/UI renderer.
MCP must return the persisted visualization artifact and enough semantic
metadata for an agent or harness to explain what was produced, while the
streamfind web app opens the workflow and renders the artifact interactively.
Do not make MCP HTML entry points or packaged MCP visualization assets a
requirement for visualization support.

------------------------------------------------------------------------

## 2. Current `dev_refactoring` baseline

The current frontend already contains useful foundations:

-   `frontend/src/app/CanvasShell.tsx`
    -   Builds workflow nodes from backend capabilities.
    -   Represents connections between node ports.
    -   Opens artifact content in the current artifact viewer.
    -   Has explicit handling for `visualizationSpecResult`.
-   `frontend/src/visualization/VisualizationRegistry.ts`
    -   Maps renderer IDs to React renderers.
    -   Provides the basic registry pattern needed for extensibility.
-   `frontend/src/visualization/VisualizationRenderer.tsx`
    -   Resolves the renderer declared by a visualization specification.
    -   Provides fallback behavior when a renderer is unavailable.
-   `frontend/src/visualization/VisualizationDataResolver.ts`
    -   Resolves `visualizationSpecResult` artifacts into validated
        visualization specifications.
-   `frontend/src/visualization/visualizationTypes.ts`
    -   Defines the current versioned visualization envelope.
    -   The current implementation is intentionally Plotly-focused.
-   `cpp/core/semantic/artifacts.ttl`
    -   Defines `visualizationSpecResult` as a bounded, versioned
        visualization envelope.
-   `cpp/core/src/project_table_store.cpp`
    -   Validates visualization artifacts before publication.
-   `cpp/plugins/mass_spec/semantic/operations.ttl`
    -   Domain operations already declare semantic input/output ports.
    -   Plot operations can emit `visualizationSpecResult`.

This means the new system should **generalize the existing
registry/semantic-contract approach**, not replace it.

------------------------------------------------------------------------

## Current implementation state and gaps

The current checkout already provides a usable backend/artifact foundation,
but it does not yet provide the viewer/plugin architecture described below.
Treat these as facts when implementing the plan:

### C++ / MCP already implemented

-   The C++ service uses persisted operation graphs with typed ports,
    immutable published artifacts, workflow revisions, lineage, execution
    records, and artifact-cache reuse.
-   MCP exposes stable project tools plus operation discovery and
    `run_operation`; domain operations are discovered through catalogue-backed
    query tools rather than one MCP tool per operation.
-   Artifact inventory and bounded artifact retrieval already exist, including
    table pagination/filtering controls and metadata-only inventory queries.
-   `visualizationSpecResult` is a validated JSON artifact contract in the
    C++ project table store.
-   The current MCP implementation still advertises `resources/list`,
    `resources/read`, `structuredContent`, and `ui://streamfind/visualization`
    HTML resources, and the C++ distribution currently requires
    `app/index.html`. These are transitional MCP-owned rendering paths that
    Phase 0b must remove.

### Frontend already implemented

-   `frontend/src/visualization/VisualizationRegistry.ts` is a static
    renderer registry, with renderer registration in
    `registerVisualizationRenderers.ts`.
-   `VisualizationDataResolver.ts` resolves a published
    `visualizationSpecResult` artifact, and `VisualizationRenderer.tsx`
    validates the spec and selects a registered renderer with a fallback.
-   `McpVisualizationApp.tsx` and `mcpVisualization.tsx` currently implement
    the MCP HTML entry point. They are not the target architecture and should
    be removed or repurposed only after the web-app artifact viewer path is
    available.
-   `CanvasShell.tsx` currently owns the generic artifact viewer and bounded
    table paging. A dedicated `ViewerShell`/`ViewerResolver` and domain viewer
    registry are still planned work.
-   There is no `frontend/src/viewers/` or `frontend/src/plugins/` runtime
    registry/loader yet, and no first-party mass-spec Feature Inspector yet.

### Consequence for sequencing

Do not start by implementing dynamic JavaScript loading. First move the
existing visualization renderer and generic artifact viewer into an explicit
web-app viewer boundary, remove the MCP HTML dependency, and prove that the
same persisted artifact can be inspected from the workflow UI. Only then
introduce a versioned frontend plugin API and runtime loader.

------------------------------------------------------------------------

## 3. Core architectural distinction

### 3.1 Workflow nodes

A workflow node represents reproducible computation or transformation.

Examples:

-   find features;
-   annotate features;
-   filter features;
-   calculate quality;
-   extract EIC;
-   generate a visualization artifact.

Nodes remain backend-defined through the capability/ontology system.

### 3.2 Viewers

A viewer is a frontend presentation of an existing artifact or port.

Examples:

-   generic table viewer;
-   NTA feature inspector;
-   spectrum viewer;
-   Raman spectrum inspector;
-   sensor time-series inspector.

Opening a viewer must **not modify the workflow graph**.

### 3.3 Editors

An editor is a viewer that additionally supports user-mediated curation.

Examples:

-   accept/reject a feature;
-   modify an annotation;
-   assign identification confidence;
-   manually adjust a classification;
-   add curator comments.

Editors must not write directly to DuckDB. They invoke backend
operations that validate and persist changes.

------------------------------------------------------------------------

## 4. Target interaction model

``` text
Backend workflow node
        |
        v
Output port / artifact
semantic contract
        |
        v
Core frontend
Viewer Registry
        |
        +---------------- Generic Table Viewer
        |
        +---------------- Mass-Spec Feature Viewer
        |
        +---------------- NTA Curation Editor
        |
        +---------------- Future third-party viewer
```

A port or artifact describes **what the data is**.

A frontend plugin declares **which semantic data contracts it can
handle**.

The core frontend determines the compatible viewers and mounts the
selected viewer inside a generic application-owned viewer shell.

------------------------------------------------------------------------

## 5. Generic viewer shell

Implement a core frontend component such as:

``` text
frontend/src/viewers/
    ViewerShell.tsx
    ViewerRegistry.ts
    ViewerResolver.ts
    viewerTypes.ts
```

`ViewerShell` should be owned entirely by the streamfind core frontend.

Initial responsibilities:

-   modal/overlay lifecycle;
-   approximately 95% viewport width and height;
-   title and artifact/port identity;
-   close/minimize behavior;
-   loading state;
-   error state;
-   viewer selection when several viewers are compatible;
-   consistent theme;
-   optional toolbar;
-   permission/curation state;
-   fallback to generic artifact presentation.

The shell must not contain mass-spec-specific logic.

------------------------------------------------------------------------

## 6. Viewer registry

Introduce a registry conceptually similar to the current
`VisualizationRegistry`.

A viewer registration should contain at least:

``` ts
type ViewerRegistration = {
  id: string;
  pluginId: string;
  label: string;
  accepts: string[];
  mode: "viewer" | "editor";
  component: ViewerComponent;
};
```

Example:

``` ts
viewerRegistry.register({
  id: "mass_spec.feature_table.viewer",
  pluginId: "mass_spec",
  label: "Feature Inspector",
  accepts: ["mass_spec.feature_table/v1"],
  mode: "viewer",
  component: FeatureTableViewer
});
```

The exact TypeScript API should be finalized during implementation
rather than treating this example as a fixed ABI.

### Resolution behavior

Given a selected port/artifact:

1.  obtain its semantic contract;
2.  query the registry for compatible viewers;
3.  if exactly one preferred viewer exists, open it;
4.  if several exist, allow selection;
5.  if none exist, use a generic fallback when possible.

This allows one artifact type to support multiple independent views.

------------------------------------------------------------------------

## 7. Frontend plugin architecture

Generalize the registry concept into a `FrontendPluginRegistry`.

Suggested structure:

``` text
frontend/src/plugins/
    FrontendPluginRegistry.ts
    FrontendPluginLoader.ts
    FrontendPluginApi.ts
    frontendPluginTypes.ts
```

A frontend plugin may contribute:

-   artifact/port viewers;
-   curation editors;
-   visualization renderers;
-   parameter editors;
-   node contextual actions;
-   domain-specific panels;
-   future domain UI extensions.

The existing `VisualizationRegistry` can initially remain intact and
later become a sub-registry exposed through the broader frontend plugin
API.

------------------------------------------------------------------------

## 8. Runtime plugin loading

The long-term target is **runtime loading**, analogous in spirit to the
C++ plugin system.

A frontend plugin should be distributable as:

``` text
plugin manifest
JavaScript/ESM bundle
optional CSS
optional static assets
```

Conceptual manifest:

``` json
{
  "plugin_id": "mass_spec",
  "plugin_version": "1.0.0",
  "frontend_api": "streamfind.frontend/v1",
  "requires_backend": {
    "plugin_id": "mass_spec"
  },
  "entrypoint": "mass_spec-ui.js"
}
```

The implementation should initially favor a **manifest + versioned
frontend SDK + dynamically imported ESM bundle** unless technical
evaluation during implementation identifies a blocking limitation.

Do not make the C++ shared library contain executable frontend code.
Backend and frontend packages remain separately buildable even if
distributed together.

------------------------------------------------------------------------

## 9. Frontend plugin API

Runtime plugins must not receive unrestricted access to React
application internals.

The following is a target, versioned plugin API rather than an existing
frontend service. Until Phase 3, viewers should use the typed
`StreamFindApiClient`/viewer context and must not import private `CanvasShell`
state.

Target services:

``` text
artifacts
operations
project context
workflow/node/port context
selection state
notifications
theme/UI primitives
viewer registry
visualization registry
```

Candidate operations:

``` text
getArtifact()
getArtifactData()
executeOperation()
subscribeToArtifact()
registerViewer()
registerEditor()
registerVisualizationRenderer()
registerParameterEditor()
```

Plugins must not depend directly on private components such as
`CanvasShell.tsx`.

React itself should be host-controlled/shared to avoid multiple React
runtimes and hook incompatibilities.

------------------------------------------------------------------------

## 10. Backend/frontend coupling

Backend and frontend plugins must remain loosely coupled.

A backend plugin can function without its specialized frontend plugin.

A frontend plugin declares the semantic contracts and backend
capabilities it requires.

Therefore:

``` text
backend capability
       |
semantic contract
       |
frontend extension
```

is preferred over:

``` text
C++ class <----> React component
```

There should not be a mandatory one-to-one relationship between backend
and frontend plugins.

A frontend plugin may understand artifacts produced by several backend
plugins, and a backend plugin may require no specialized frontend at
all.

------------------------------------------------------------------------

## Visualization boundary: MCP produces, web app renders

The MCP interface and the web app have deliberately different
responsibilities:

| Surface | Responsibility |
| --- | --- |
| MCP server | Assemble/run workflow nodes, publish immutable artifacts, return artifact references, semantic contracts, summaries, and explicit guidance that rich visualization is available in the web app. |
| External MCP harness | Display the returned summary or its own fallback; it must not be expected to execute streamfind's HTML entry points. |
| streamfind web app | Open the saved workflow, resolve the published artifact, select a compatible viewer/renderer, and provide the interactive plot. |
| Visualization node | Compute or transform scientific data and publish a typed visualization artifact; it is not itself an MCP UI surface. |

Remove MCP-specific visualization HTML entry points and packaged `app/`
visualization assets from the release path. The release must not require a
browser-capable MCP Apps harness to make visualization support usable. This
is not a removal of visualization data: the MCP response still exposes the
artifact ID, semantic visualization contract, producer node/port, workflow
revision, bounded metadata, and a text fallback describing what the web app
can render.

The implementation must preserve one artifact contract across both surfaces:

``` text
workflow visualization operation
        |
        v
immutable sfvis:VisualizationSpec artifact
        |
        +---- MCP: artifact reference + semantic summary + web-app guidance
        |
        +---- web app: ViewerResolver -> registered renderer/plugin
```

MCP tool and operation descriptions should say, in substance, that a
visualization result is persisted for inspection in the streamfind web app.
They should not promise inline HTML, an MCP resource URL, or a harness-specific
interactive plot. If a harness cannot render the artifact, it still receives a
useful result summary and a stable artifact reference.

Acceptance criteria for this boundary:

1. A plotting operation can run through MCP without an MCP HTML resource or
   packaged visualization app entry point.
2. The response identifies the published visualization artifact and its
   semantic contract.
3. The response explains that the interactive visualization is opened in the
   streamfind web app from the saved workflow/artifact.
4. The web app can open the same artifact and choose the registered renderer
   without creating a second workflow node or recomputing scientific data.
5. A non-rendering MCP harness still receives a meaningful text/metadata
   fallback.

------------------------------------------------------------------------

## First mission: mass-spec feature-table viewer

The first end-to-end implementation should be a feature inspector
inspired by the legacy R/Shiny NTS workflow.

The legacy development application demonstrates access to functionality
including:

-   feature tables;
-   feature EIC data;
-   feature MS1 data;
-   feature MS2 data;
-   feature plotting;
-   feature mapping;
-   feature intensity views;
-   groups and group profiles;
-   suspect-screening results;
-   internal standards;
-   fold-change analysis.

The first React viewer should **not attempt to reproduce every legacy
function immediately**.

### Phase-1 feature inspector

Initial layout:

``` text
+--------------------------------------------------------------+
| Feature Inspector                                            |
+---------------------------+----------------------------------+
| Feature table             | Selected feature                 |
|                           |                                  |
| searchable/filterable     | metadata                         |
| sortable                  |                                  |
| selectable rows           +----------------------------------+
|                           | EIC / chromatographic view        |
|                           +----------------------------------+
|                           | MS1 spectrum                      |
|                           +----------------------------------+
|                           | MS2 spectrum                      |
+---------------------------+----------------------------------+
```

The detailed layout can evolve during implementation, but the key
behavior is coordinated selection: selecting a feature updates all
dependent views.

### First acceptance criteria

The user can:

1.  open a feature-table artifact from its workflow port;
2.  select the domain-specific Feature Inspector;
3.  inspect the feature table;
4.  select a feature;
5.  see available metadata;
6.  request/render relevant EIC data;
7.  request/render MS1 data when available;
8.  request/render MS2 data when available;
9.  return to the workflow without changing the graph.

The viewer should use existing backend operations where suitable rather
than reimplementing analytical calculations in TypeScript.

------------------------------------------------------------------------

## 12. Semantic contract for feature data

The feature-table viewer should resolve from a semantic data contract
rather than from a hard-coded node ID.

For example:

``` text
mass_spec.feature_table/v1
```

The exact ontology identifier should follow the conventions already used
by the mass-spec plugin.

The contract should describe enough information for the frontend to
determine:

-   artifact/table type;
-   domain;
-   schema/version;
-   feature identifier;
-   analysis identifier where applicable;
-   m/z;
-   retention-time fields;
-   intensity fields;
-   links or operations for dependent EIC/MS1/MS2 data.

Do not encode React component names in the ontology.

Ontology describes data/capabilities; the frontend plugin manifest and
registry map those capabilities to executable UI.

------------------------------------------------------------------------

## 13. Data-access model

A viewer receives context, not raw backend internals.

Suggested viewer context:

``` ts
type ViewerContext = {
  sessionId: string;
  nodeId?: string;
  portId?: string;
  artifactId: string;
  semanticType: string;
  api: StreamFindApiClient;
};
```

Large tables should not necessarily be transferred in full.

Plan for:

-   pagination;
-   sorting;
-   filtering;
-   column projection;
-   targeted feature lookup;
-   lazy retrieval of spectra/chromatograms.

This is especially important for NTA datasets.

------------------------------------------------------------------------

## 14. Curation architecture

After the read-only feature viewer is working, extend it into a
curation-capable editor.

Possible actions:

-   accept feature;
-   reject feature;
-   flag feature;
-   update annotation;
-   assign confidence;
-   add comment;
-   manually classify a feature.

The frontend should collect the proposed change and require an explicit
user action before persistence.

``` text
Feature artifact
      |
      v
Curation editor
      |
proposed change
      |
explicit user apply
      |
      v
backend curation operation
      |
validation
      |
      v
persisted curation result
      |
      +---- provenance/audit
```

### No direct database writes

Frontend plugins must never directly mutate DuckDB.

All mutations go through backend plugin operations/contracts.

------------------------------------------------------------------------

## 15. Provenance and artifact versioning

Prefer non-destructive curation.

At minimum retain:

-   source artifact ID;
-   affected feature/entity ID;
-   action;
-   previous value/state;
-   resulting value/state;
-   originating operation;
-   timestamp;
-   project/workflow context;
-   optional curator comment.

Evaluate two persistence strategies:

### A. Curation layer/table

Keep computed feature results immutable and persist curation separately.

Advantages:

-   clean provenance;
-   easy rollback;
-   separates computation from human interpretation.

### B. New artifact revision

Each committed curation produces a new artifact/revision.

Advantages:

-   explicit immutable snapshots;
-   simple reproducibility semantics.

The implementation should select the strategy that best fits the
existing artifact and project-table model. Avoid silent in-place
mutation.

------------------------------------------------------------------------

## 16. Permission and trust boundary

There are two distinct permission concerns.

### User mutation permission

A viewer can read according to normal project access.

An editor must request an explicit user action before committing
changes.

### Plugin trust

Runtime frontend plugins execute JavaScript in the application context
and therefore represent trusted code unless sandboxed.

Initial implementation should treat installed frontend plugins as
trusted extensions and enforce:

-   manifest validation;
-   frontend API version compatibility;
-   backend capability compatibility;
-   controlled plugin source/discovery;
-   no implicit loading of arbitrary remote URLs.

Sandboxing through iframe/worker boundaries can be evaluated later if
untrusted third-party plugins become a requirement.

------------------------------------------------------------------------

## 17. Compatibility model

Each frontend plugin should declare:

-   plugin ID;
-   plugin version;
-   frontend API version;
-   required backend plugin/capability versions;
-   supported semantic contract versions.

Failure modes must be graceful.

Examples:

-   backend plugin installed, frontend plugin missing:
    -   backend remains usable;
    -   generic viewer/fallback remains available.
-   frontend plugin incompatible:
    -   do not load;
    -   report compatibility reason.
-   viewer cannot understand artifact version:
    -   fall back instead of crashing the canvas.

------------------------------------------------------------------------

## 18. Preserve the web-app Plotly path; remove MCP HTML rendering

Do not make MCP-side HTML rendering the visualization authority. The
`visualizationSpecResult`/`sfvis:VisualizationSpec` artifact remains the
stable data contract, and the streamfind web app remains responsible for
resolving and rendering it.

Keep the web-app path:

``` text
backend plot operation
       |
visualizationSpecResult artifact
       |
web app VisualizationDataResolver
       |
ViewerResolver / VisualizationRenderer
       |
registered Plotly or domain renderer
```

Remove any MCP release packaging that exists only to expose an HTML entry
point for that plot. MCP may return the artifact reference and bounded
semantic summary, and may state that the result is viewable in the web app,
but it should not advertise a harness-dependent interactive HTML resource.
The generic viewer/plugin path and the visualization renderer should consume
the same persisted artifact rather than maintaining separate MCP and React
plot implementations.

The new path exists alongside the generic artifact viewer:

``` text
table/artifact port
       |
semantic contract
       |
ViewerResolver
       |
FrontendPluginRegistry
       |
domain-specific viewer
```

Later, visualization renderers can be exposed through the same frontend
plugin SDK, but the rendering location remains the web app.

------------------------------------------------------------------------

## 19. Proposed implementation phases

### Phase 0 --- Contract inventory

Before coding:

-   document current artifact types;
-   identify the current mass-spec feature table contract/table;
-   identify operations required to retrieve feature EIC/MS1/MS2;
-   map relevant legacy R/Shiny behavior to current C++ capabilities;
-   identify missing backend operations;
-   inventory the release/MCP HTML entry points and `app/` assets that
    currently attempt to render visualization inside an MCP harness;
-   define the MCP result envelope for artifact ID, semantic contract,
    producer node/port, workflow revision, bounded summary, and web-app
    guidance;
-   identify the exact MCP tool descriptions and release packaging rules that
    must be changed so visualization is described as web-app rendered.

**Deliverable:** explicit feature-viewer data contract and backend
capability matrix, plus an MCP/web-app visualization boundary specification.

### Phase 0b --- Remove MCP-owned visualization assets

Implement the boundary before building specialized viewers:

-   remove MCP-only HTML entry points and visualization `app/` assets from
    the release/package manifests;
-   retain the semantic visualization artifact and bounded artifact retrieval
    through MCP;
-   update MCP operation/tool descriptions and result summaries to identify
    the artifact and direct users to the streamfind web app;
-   keep non-rendering harnesses useful through text and metadata fallback;
-   add an MCP contract test proving that visualization support does not
    depend on an HTML resource while the artifact reference remains present.

Likely files to inspect or modify:

-   `cpp/core/src/mcp.cpp`;
-   `cpp/service/src/service_server.cpp`;
-   `cpp/sdk/src/catalogue_builder.cpp`;
-   `cpp/tests/unit/mcp_discovery_contract.cpp`;
-   `scripts/release/cpp/release-cpp.ps1` and related package manifests;
-   `frontend/src/visualization/VisualizationDataResolver.ts`;
-   `frontend/src/visualization/VisualizationRenderer.tsx`.

**Acceptance:** a fresh packaged MCP server exposes artifact-based
visualization guidance without shipping or requiring MCP HTML entry points,
and the web app remains the only rich interactive rendering authority.

### Phase 1 --- Extract the web-app viewer boundary

Implement the first target boundary around existing code rather than creating
parallel rendering paths:

-   introduce `frontend/src/viewers/ViewerShell.tsx` and
    `viewerTypes.ts`;
-   introduce `ViewerResolver` and adapt the existing static
    `VisualizationRegistry`/`VisualizationRenderer` to it;
-   move generic artifact-viewer lifecycle out of `CanvasShell.tsx` while
    preserving the current session-aware `StreamFindApiClient` boundary;
-   resolve visualization specs from persisted artifacts, not MCP
    `structuredContent` or window message state;
-   keep a generic table/JSON fallback for artifacts without a specialized
    viewer;
-   remove the web app's dependency on `McpVisualizationApp` for normal
    workflow inspection.

Do not add runtime JavaScript loading yet.

**Acceptance:** a statically registered test viewer opens from a compatible
workflow output/artifact in the web app, and the same behavior works when MCP
returns only the artifact reference and summary.

### Phase 2 --- First-party mass-spec feature viewer

Implement the mass-spec Feature Inspector as a frontend extension using
the registry.

Start statically packaged while keeping its API identical to the future
runtime plugin API.

**Acceptance:** feature selection drives coordinated feature
metadata/EIC/MS1/MS2 views.

### Phase 3 --- Frontend plugin API

Extract the APIs used by the feature viewer into a stable
`FrontendPluginApi`.

Add:

-   plugin registration lifecycle;
-   version declaration;
-   capability declaration;
-   compatibility validation.

**Acceptance:** the mass-spec viewer does not import private core
frontend modules.

### Phase 4 --- Runtime plugin loader

Implement:

-   manifest discovery;
-   manifest validation;
-   dynamic ESM loading;
-   plugin activation;
-   error isolation;
-   compatibility checks;
-   controlled asset/CSS loading.

Convert the mass-spec feature viewer from static registration to runtime
registration.

**Acceptance:** the application can start without the feature UI plugin,
then expose it when the compatible plugin is installed/discovered.

### Phase 5 --- Curation operations

Add backend curation contracts and frontend editor actions.

Implement at least one end-to-end curation operation, for example:

``` text
feature status: unreviewed -> accepted/rejected
```

**Acceptance:** user-confirmed curation is persisted through the backend
with provenance and is restored after project reload.

### Phase 6 --- Generalize extension points

Once the viewer/editor model is proven, add optional registries for:

-   parameter editors;
-   node contextual actions;
-   domain panels;
-   specialized visualization renderers.

Avoid designing all extension points before the Feature Inspector proves
the common plugin lifecycle.

------------------------------------------------------------------------

## 20. Suggested frontend organization

The current implementation remains under `frontend/src/visualization/` and
`frontend/src/app/`, including the transitional `McpVisualizationApp.tsx`.
The following is the target organization after Phase 1, not a claim about the
current checkout:

``` text
frontend/src/
    app/
        CanvasShell.tsx

    visualization/
        VisualizationRegistry.ts
        VisualizationRenderer.tsx
        ...

    viewers/
        ViewerShell.tsx
        ViewerRegistry.ts
        ViewerResolver.ts
        GenericTableViewer.tsx
        viewerTypes.ts

    plugins/
        FrontendPluginApi.ts
        FrontendPluginRegistry.ts
        FrontendPluginLoader.ts
        frontendPluginTypes.ts

    core-ui/
        ...

frontend-plugins/                 # final location to decide
    mass_spec/
        manifest.json
        src/
            index.ts
            FeatureTableViewer.tsx
            FeatureInspector.tsx
            ...
```

The exact monorepo location for first-party frontend plugins should be
decided alongside the build/distribution strategy.

------------------------------------------------------------------------

## 21. Backend work likely required

The C++ operation/artifact/MCP foundations are already present. Add only the
viewer-facing contracts and narrowly required domain operations; do not create
viewer-specific C++ APIs when the generic artifact/operation API is sufficient.

### Already available and to reuse

-   semantic type/contract metadata for published artifacts and ports;
-   metadata-only artifact inventory and bounded table paging/filtering via
    the service API;
-   immutable JSON/table artifact publication and lineage/provenance records;
-   generic operation execution and artifact retrieval through MCP and the
    typed service client.

### Remaining backend work

-   expose a stable visualization artifact envelope with renderer metadata,
    semantic type, provenance, interaction keys, fallback text, and bounded
    inline or artifact-backed data bindings;
-   retrieve feature detail by stable feature ID;
-   retrieve EIC for a selected feature;
-   retrieve MS1 for a selected feature;
-   retrieve MS2 for a selected feature;
-   add curation mutation operations only after the read-only viewer works;
-   expose any missing artifact/port metadata needed by the web-app resolver;
-   remove MCP resource/HTML packaging from the release contract;
-   add MCP descriptions and structured summaries that point to the web app,
    without making `structuredContent` or a UI resource the only path.

------------------------------------------------------------------------

## 22. Testing strategy

### Core frontend tests

Existing coverage to preserve and extend:

-   `frontend/src/visualization/VisualizationDataResolver.test.ts` for
    visualization artifact validation;
-   `frontend/src/app/App.test.tsx` and shell tests for workflow/artifact
    navigation;
-   renderer registration/resolution and fallback behavior.

Planned additions:

-   viewer registration/resolution;
-   duplicate IDs;
-   semantic compatibility;
-   missing viewer fallback;
-   plugin API compatibility;
-   failed plugin load isolation;
-   removal of MCP-window/message dependencies from normal web-app viewers.

### Feature viewer tests

-   table loading;
-   selection;
-   EIC update;
-   MS1 update;
-   MS2 update;
-   missing optional data;
-   large-table pagination/filtering;
-   viewer close/reopen state behavior.

### MCP/web-app boundary tests

-   MCP `tools/list` and operation descriptions explain that visualization
    artifacts are inspected in the streamfind web app;
-   MCP results contain stable artifact identity, semantic contract, producer
    node/port, workflow revision, bounded summary, and text fallback;
-   packaged MCP output contains no MCP-only visualization HTML entry point or
    required `app/` visualization asset;
-   the web app opens the returned artifact and resolves a registered renderer;
-   a harness that cannot render HTML can still report what was produced.

### Curation tests

-   read-only viewer cannot mutate;
-   edit action requires explicit commit;
-   invalid mutation rejected by backend;
-   successful mutation persisted;
-   provenance/audit recorded;
-   project reload reproduces curated state.

### Integration tests

-   application with no frontend plugins;
-   mass-spec backend without mass-spec frontend plugin;
-   compatible backend + frontend plugin;
-   incompatible frontend API version;
-   incompatible semantic contract version.

------------------------------------------------------------------------

## 23. Architectural guardrails

1.  **Do not make viewers workflow nodes merely because they display
    node data.**
2.  **Do not let frontend plugins access DuckDB directly.**
3.  **Do not place domain-specific feature logic in `CanvasShell`.**
4.  **Do not encode React component names into ontology.**
5.  **Do not require specialized UI for backend functionality to remain
    usable.**
6.  **Do not break `visualizationSpecResult` while introducing
    viewers.**
7.  **Do not expose private React application internals as the plugin
    API.**
8.  **Do not allow arbitrary remote JavaScript loading by default.**
9.  **Prefer semantic contracts over node IDs for viewer
    compatibility.**
10. **Prefer provenance-preserving curation over destructive table
    edits.**

------------------------------------------------------------------------

## 24. Recommended first implementation slice

The smallest slice that meaningfully validates the architecture is:

1. inventory the current `visualizationSpecResult` artifact and MCP resource
   behavior;
2. remove the release/package dependency on MCP HTML assets;
3. add a web-app `ViewerShell` around the existing static renderer registry;
4. resolve compatible viewers from a workflow output/artifact, not from an MCP
   resource or a hard-coded node ID;
5. implement a statically registered generic artifact viewer;
6. implement the first mass-spec `FeatureTableViewer` against bounded artifact
   retrieval;
7. select one feature and request one dependent view, preferably EIC;
8. ensure no new workflow node is created by opening the viewer;
9. preserve the existing Plotly renderer behavior in the web app;
10. only after this works, define the versioned frontend plugin API and runtime
    loader.

This sequencing separates three risks:

-   MCP/web-app boundary and release packaging;
-   viewer architecture and artifact binding;
-   runtime JavaScript plugin loading.

------------------------------------------------------------------------

## 25. Definition of success

The architecture is successful when a developer can add a new
domain-specific frontend package that:

-   declares which semantic artifacts it understands;
-   is discovered and loaded without editing the core frontend;
-   registers one or more viewers/editors;
-   receives only the stable streamfind frontend API;
-   reads data through standard artifact/backend operations;
-   optionally requests validated user-authorized mutations;
-   survives independent evolution of `CanvasShell` and other private
    frontend internals.

For the first proof-of-concept, success means the mass-spec NTA feature
table can be opened from its existing workflow data anchor in a rich
Feature Inspector, with coordinated feature data views comparable in
purpose to the legacy R/Shiny application, **without representing that
inspector as another analytical workflow node**. Visualization operations
also satisfy the cross-surface boundary: MCP produces and describes the
persisted artifact, while only the streamfind web app provides the rich
interactive plot; no MCP HTML entry point is required or shipped.
