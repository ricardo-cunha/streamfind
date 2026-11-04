# streamfind Frontend Plugin and Domain Viewer Implementation Plan

**Target baseline:** `dev_refactoring`\
**Primary proof-of-concept:** Mass-spec / NTA feature-table viewer and
curation UI\
**Status:** Static viewers, GUI regression coverage, the MCP/web-app
visualization boundary, the versioned frontend plugin API, the controlled
loader boundary, packaged-manifest discovery, and the first bundled optional
mass-spec plugin are implemented; backend-mediated curation remains

The next consolidation phase is now active: `visualizationSpecResult` and
domain table artifacts are being brought under one plugin-owned artifact-viewer
architecture. Core and domain plugins must register viewers through the same
public API; `CanvasShell` may enumerate compatible registrations but must not
name domain contracts or viewer labels. Visualization-spec parsing and inner
renderer selection remain separate concerns, but both are plugin dependencies
with standardized registry and test boundaries.

First consolidation slice completed: canonical artifact-contract normalization
now lives in `frontend/src/artifacts/artifactContracts.ts`; the core
`visualizationSpecResult` viewer registers through `api.registerViewer()` like
domain plugins; plugin APIs expose the visualization registry and transactional
`registerVisualizationRenderer()` support; and renderer registration rollback
is covered by the plugin registry tests. The remaining structural work is to
relocate core artifact/visualization assets under `plugins/core/`, then remove
the transitional `viewers/` and `visualization/` ownership split.

The first relocation is complete: the visualization-spec artifact viewer and
the visualization envelope, resolver, registry, Plotly renderer, and related
tests now live under `frontend/src/plugins/core/`. `src/viewers/` retains only
the generic shell/resolver/registry boundary, while the mass-spec plugin
imports the shared visualization runtime from the core plugin.

The core artifact-viewer relocation has started as well: the virtualized generic
table implementation now lives at
`frontend/src/plugins/core/artifact-viewers/VirtualArtifactTable.tsx` instead
of inside `CanvasShell`. The remaining table viewer extraction must move its
artifact paging state and toolbar into the core plugin before the hardcoded
table action is replaced with a core viewer registration.

The generic table viewer extraction is now complete. `GenericTableViewer` owns
server-side paging, searching, sorting, page-size selection, and the virtualized
table presentation. It is registered as `core.table` by `streamfind.core`, and
`CanvasShell` resolves that viewer instead of rendering the table controls
directly. The transitional `registerCoreViewers` module and test were removed;
core viewer registration now has one plugin-owned path.

Core bootstrap ownership is now explicit in
`frontend/src/plugins/core/index.ts`. It owns the core plugin manifest and
registration of both `core.table` and `core.visualization`; `main.tsx` imports
that entry point directly. The former top-level core registration module was
removed so the core implementation and bootstrap boundary share one folder.

Added a core bootstrap contract test at
`frontend/src/plugins/core/index.test.ts`; it verifies that the core plugin
registers both generic artifact viewers and resolves the canonical table and
visualization contracts through the public registry.

Added `GenericTableViewer.test.tsx` to cover the typed artifact-data boundary:
the viewer requests the initial page and requests the next server-side page
when the user activates pagination. `VirtualArtifactTable` now tolerates test
and non-browser environments without a native `scrollTo` implementation.

Packaged validation served the production `dist/` payload and confirmed that
`/plugins.json` points to the emitted same-origin mass-spec chunk. The browser
successfully dynamically imported that hashed entry and found the default
`streamfind.mass-spec` plugin export. Full application activation reached the
backend initialization gate but could not proceed because no backend service
was running; the packaged viewer import was validated independently and no
frontend page error was observed before the expected backend failure state.

The plugin manifest now declares ownership metadata for domains and artifact
contracts. `streamfind.mass-spec` declares `mass_spec` and `featuresTable`,
while `streamfind.core` declares `visualizationSpecResult`. The runtime exposes
`providersForArtifact()` so future artifact routing can distinguish activated
core and domain providers without hardcoded domain checks.

Viewer compatibility is now split between semantic contracts and generic
representations. Domain plugins register semantic viewers such as
`featuresTable`; core registers the generic table viewer for the `table`
representation. `ViewerResolver` and the artifact menu query both values, so
`CanvasShell` no longer names `core.table` directly. Artifact contract helpers
only normalize and compare backend-provided identifiers; they no longer contain
a frontend-owned list of domain contracts.

The frontend directory boundary has now been separated into application,
framework, plugin, deployment, and tooling areas. Application screens live
under `src/application/`; transport, artifact matching, plugin lifecycle,
viewer hosting, visualization envelopes, and theme infrastructure live under
`src/framework/`; plugin implementations remain under `src/plugins/`;
deployment-time plugin metadata is sourced from `deployment/`; and build,
development, and browser tooling live under `tools/`. `public/plugins.json` is
now a generated development/runtime copy of the deployment manifest rather
rather than the source of plugin ownership metadata.

Each plugin now owns `plugin.manifest.json` beside its entry point. The build
and development manifest tools discover `src/plugins/*/plugin.manifest.json`,
filter registrations by activation mode, and resolve each declared source
entry through the Vite manifest. They no longer name `streamfind.mass-spec`,
`featuresTable`, or any other plugin/domain in build code. The bundled-entry
graph uses a Vite glob with the core bootstrap excluded, so adding a plugin
requires adding its folder and registration file rather than editing a central
manifest generator.

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

The current checkout has moved beyond the original proposal. It now provides a
working static viewer boundary and a first-party mass-spec Feature Inspector,
including the GUI refinements captured from manual review. The remaining work
is to harden that boundary, close the read-only viewer contract, and only then
extract the stable plugin API and runtime loader. Treat the following as the
current source of truth when implementing the plan:

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
-   The MCP implementation no longer advertises resource capabilities, accepts
    `resources/list`/`resources/read`, emits `structuredContent`, or exposes a
    `ui://streamfind/visualization` HTML resource. Visualization results remain
    persisted artifacts with a bounded text/metadata fallback. The packaged
    `app/index.html` used by the native browser application is independent of
    MCP and remains required.

### Frontend already implemented

-   `frontend/src/visualization/VisualizationRegistry.ts` is a static
    renderer registry, with renderer registration in
    `registerVisualizationRenderers.ts`.
-   `VisualizationDataResolver.ts` resolves a published
    `visualizationSpecResult` artifact, and `VisualizationRenderer.tsx`
    validates the spec and selects a registered renderer with a fallback.
-   No frontend MCP HTML visualization entry point is part of the current
    source path. Rich visualization is owned by the packaged web app and
    persisted artifact viewer.
-   `frontend/src/viewers/ViewerShell.tsx`, `ViewerResolver.tsx`, and
    `viewerTypes.ts` (which owns `ViewerRegistry`) provide the static viewer
    boundary, resolution, fallback, and shell lifecycle.
-   `registerCoreViewers.ts` registers the generic visualization artifact
    viewer and the mass-spec Feature Inspector. This is a static first-party
    registration, not runtime JavaScript plugin loading.
-   `frontend/src/plugins/mass-spec/FeatureInspector.tsx` is a read-only
    mass-spec/NTA viewer. It loads all
    bounded artifact pages, renders the Feature Explorer with Plotly WebGL,
    supports analysis-scoped feature/component selection and cross-analysis
    feature-group selection, and coordinates details, EIC, MS1, MS2, and
    persisted relationship-network views.
-   The manual GUI improvements are implemented and must remain regression
    requirements: 320px filter sidebar; responsive/resizable detail panel;
    full-width detail plots; in-plot EIC legend; rich EIC hover metadata;
    deterministic trace/legend/fill colors; boolean filter label-before-
    checkbox layout; selection-mode filtering of null/empty group/component
    rows; persisted relationship-based network rendering; and centered
    loading/error/empty states.
-   `featureSelection.ts` centralizes selection identity and availability
    rules. Changing selection mode clears stale selection, and unavailable
    dimensions are expressed by filtering rows rather than notification-only
    messages.
-   Generic artifact tables remain domain-agnostic and retain server-side
    search, sorting, filtering, pagination, and virtualization. The Feature
    Inspector's complete-row load is intentional for its cross-row WebGL
    explorer and does not replace the generic table contract.
-   `frontend/src/plugins/` now contains the versioned public API contract,
    registry validation, and static core-plugin registration. It does not yet
    contain a runtime loader. Do not introduce dynamic loading until the
    remaining static viewer contract is complete.

### Consequence for sequencing

Do not start by implementing dynamic JavaScript loading or curation writes.
First consolidate the implemented viewer boundary, verify the manual GUI
contract in the browser, and close the read-only artifact-binding and MCP/web-
app boundary tests. Only then introduce a versioned frontend plugin API and
runtime loader.

------------------------------------------------------------------------

## 3a. Implemented GUI contract to preserve

The following behavior came from manual viewer review and is part of the
acceptance contract, not optional polish:

### Feature Explorer

- Load the complete feature artifact through bounded pages and render the
  overview with Plotly `scattergl`; do not silently fall back to SVG for the
  large feature plot.
- Keep `featuresTable` and `spectraHeadersTable` on the same generic,
  domain-agnostic virtualized table path. Feature-specific controls belong in
  the Feature Inspector, not in the generic renderer.
- Keep server-side search, sorting, filtering, and pagination for generic
  tables. Do not move those responsibilities into a frontend-only copy of
  the data.
- Treat feature and component identities as analysis-scoped. Treat feature
  group identity as cross-analysis. Use persisted relationship columns for
  network edges; never infer edges from m/z or retention-time proximity.
- When selection mode is Feature group, Feature component, or Group +
  component, filter out rows whose required value is null, empty, or only
  whitespace. Show the resulting count directly in the table/plot state and
  clear the previous selection when the mode changes.

### Coordinated detail views

- Selecting a point or table row updates feature details and the dependent
  EIC/MS1/MS2/network tabs without creating a workflow node.
- EIC/MS1/MS2 plots fill the available right-side panel width. The EIC legend
  is inside the plot at the top-right. Trace colors, marker colors, fills, and
  legend swatches come from one deterministic label/category mapping in both
  light and dark themes.
- EIC hover text includes feature, analysis, replicate, component/group,
  m/z, retention time, intensity/area, polarity/adduct, annotation/formula,
  and the hovered point values when present.
- Network rendering consumes persisted partner/correlation/loss-chain
  relationships and preserves explicit loss chains; it must not invent
  relationships from visual proximity.

### Layout and interaction

- Keep the 320px filter sidebar and constrain filter controls so the viewer
  does not acquire avoidable horizontal scrolling.
- Keep the details splitter bounded by usable minimum widths and ensure plots
  resize with the panel.
- Keep centered loading, error, and empty states; do not show misleading
  selection-availability notifications when filtering can make the absence
  visible directly.
- File/folder selection controls must preserve existing JSON-array values,
  append newly selected paths without duplicates, keep the explorer visible,
  and support click, Shift-click range, Ctrl/Cmd-click toggle, and right-click
  selection. Do not render a large selected-path list below the explorer.
- JSON editor modals remain horizontally and vertically resizable, with the
  editor fitting the modal.

These rules apply to future refactors of the viewer even if the component is
later moved into a first-party frontend plugin package.

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

## 19. Revised implementation phases

The first three phases below are the immediate development sequence. They
describe work still required; the old proposal's boundary-extraction and
first-party-viewer phases are recorded as implemented rather than repeated as
future work.

### Phase 0 --- Baseline and contract audit (implemented, maintain)

Keep a short inventory synchronized with the code:

-   artifact semantic types and table schemas used by the viewer;
-   stable feature identity, analysis/replicate identity, group/component
    identity, and persisted relationship columns;
-   backend operations required for feature detail, EIC, MS1, MS2, and network
    data;
-   the `ViewerShell`/`ViewerResolver`/registry contract and generic fallback;
-   MCP artifact identity and web-app guidance;
-   the manual GUI contract in section 3a.

Do not broaden this inventory into a new domain-specific backend API when the
generic artifact and operation APIs already provide the required data.

### Phase 1 --- Read-only viewer hardening (next implementation slice)

Finish and verify the implemented static path before plugin loading:

1. Audit `ViewerShell`, `ViewerResolver`, and `registerCoreViewers` for
   artifact/port binding, close/reopen behavior, missing-viewer fallback, and
   session-aware API usage.
2. Ensure the Feature Inspector does not depend on MCP window messages,
   `structuredContent`, or a hard-coded workflow node ID.
3. Extract small pure helpers from `FeatureInspector.tsx` where they improve
   testability, but keep domain-specific logic out of `CanvasShell` and keep
   the generic table domain-agnostic.
4. Add regression tests for every section 3a rule that can be tested without a
   browser, especially selection identity, null/empty filtering, trace-color
   parity, relationship-network construction, and paged artifact loading.
5. Run a fresh browser inspection of the real workflow with light and dark
   themes, resizing the detail splitter and checking the EIC/MS1/MS2 tabs.

**Acceptance:** a persisted feature artifact opens in the web app with no
workflow mutation; selecting a point or row updates all available dependent
views; the generic artifact fallback remains usable; and the manual GUI
contract passes automated tests plus browser inspection.

### Phase 2 --- MCP/web-app boundary completion (implementation complete;
package smoke remains)

The MCP boundary implementation is complete:

-   MCP no longer advertises resource capabilities or accepts
    `resources/list`/`resources/read`;
-   visualization results no longer use MCP `structuredContent` or an HTML
    entry point;
-   persisted visualization artifacts and bounded text responses remain
    available;
-   initialize guidance directs rich inspection to the streamfind web app;
-   the MCP discovery contract covers the removed resource surface and the
    web-app artifact guidance;
-   the packaged archive retains `app/index.html`, the native launcher, and
    the service independently of MCP.

Remaining closure check: retain the packaged MCP smoke and let the user
continue manual Feature Inspector browser validation against their populated
project fixture during subsequent viewer changes.

**Acceptance:** MCP is useful for execution and artifact discovery, while the
streamfind web app is the only rich interactive visualization authority. The
packaged web app still launches normally on Windows and Linux.

### Phase 3 --- Versioned frontend plugin API

The first static API slice is implemented in `frontend/src/plugins/`. It
extracts only the interfaces needed to establish the extension boundary:

-   `FrontendPluginApi` with registration, artifact access, operation request,
    theme/layout tokens, and notification/error boundaries;
-   plugin ID, API version, semantic-contract compatibility, viewer mode, and
    capability declarations;
-   duplicate registration and incompatible-version rejection;
-   core viewers registered through `streamfind.core` without changing their
    current rendering behavior.
-   plugin setup is transactional for viewers registered through the public
    API; failed setup removes partial viewer registrations and the plugin
    remains inactive.

The registry exposes a typed `StreamFindApiClient`, bounded theme tokens,
notifications, artifact summaries, and the existing `ViewerRegistry` through
`FrontendPluginApi`. The Feature Inspector continues to receive its existing
typed viewer context, now with a plugin API carrying the active typed client;
its artifact paging reads use that public API. Its domain CSS is loaded by the
core plugin from `frontend/src/plugins/mass-spec/FeatureInspector.css` rather
than the global theme stylesheet.

**Current acceptance:** the application starts with the core plugin, viewers
still resolve by semantic contract, and the registry rejects duplicate,
unsupported-version, and incompatible-contract registrations. Failed plugin
setup rolls back partial public-API viewer registrations. Runtime loader
failure isolation remains a Phase 4 concern.

### Phase 4 --- Runtime plugin loader

The controlled loader boundary is implemented in
`frontend/src/plugins/FrontendPluginLoader.ts`. It currently provides:

-   strict manifest and API-version validation;
-   same-origin entry enforcement by default;
-   controlled dynamic ESM import through an injectable importer;
-   activation through the transactional plugin registry;
-   failure containment with warning notifications;
-   duplicate-load suppression.

The packaged web app owns one optional same-origin manifest at
`/plugins.json`, sourced from `frontend/public/plugins.json` and copied into
the production `dist/`/native `app/` payload. Bootstrap discovers and
activates its manifests after the core plugin is registered. A missing or
invalid optional manifest does not block core startup. The mass-spec viewer
source lives under `frontend/src/plugins/mass-spec/` and
is emitted as a Vite dynamic ESM chunk. Development uses the source entry;
production rewrites `dist/plugins.json` to the hashed chunk path.

**Current acceptance:** valid same-origin manifests can be loaded through the
controlled boundary, while invalid, cross-origin, malformed, or setup-failing
plugins cannot break generic/core viewers. Optional activation is packaged
manifest-driven; no remote manifest discovery or arbitrary cross-origin
loading is enabled.

Packaged validation is complete: the production preview served
`/plugins.json`, the manifest resolved the emitted mass-spec chunk, and the
browser imported `streamfind.mass-spec` with API version `1.0` without page
errors. The preview process was stopped after validation.

The first live dynamic-plugin reproduction also exposed and fixed the module
export boundary: the loader accepts `default`/`plugin` exports, so the bundled
mass-spec entry now provides a default plugin export. Without it, the hardcoded
artifact renderer menu still displayed `Features Explorer`, but registry
resolution returned no viewer and the modal fell back to a `null` payload.

Artifact renderer menus now enumerate compatible registrations from the shared
viewer registry. The canvas retains only the generic table action for table
representations; plugin-provided viewer labels and IDs are discovered from the
artifact semantic contract and selected by viewer ID, so unavailable optional
plugins no longer leave dead domain-specific buttons.

### Phase 5 --- Curation operations

After the read-only viewer is stable, add backend-mediated curation. Begin
with one narrow operation such as `unreviewed -> accepted/rejected`.

**Acceptance:** explicit user confirmation invokes a validated backend
operation, records provenance/audit data, and restores the curated state after
project reload. The viewer never writes DuckDB directly.

### Phase 6 --- Generalize extension points

Only after the Feature Inspector proves the lifecycle, consider registries for
parameter editors, node actions, domain panels, and specialized renderers.
Each extension must preserve the generic artifact fallback and section 3a GUI
contract.

------------------------------------------------------------------------

## 20. Suggested frontend organization

The current implementation remains under `frontend/src/visualization/`,
`frontend/src/viewers/`, and `frontend/src/app/`. The following organization
is the current static shape plus the target plugin boundary; runtime plugin
files are future work:

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
        ViewerResolver.tsx
        viewerTypes.ts              # ViewerRegistry and shared types
        registerCoreViewers.ts
        VisualizationArtifactViewer.tsx

    plugins/
        pluginTypes.ts
        FrontendPluginRegistry.ts
        registerCoreFrontendPlugin.ts
        FrontendPluginLoader.ts
        mass-spec/
            FeatureInspector.tsx
            FeatureInspector.test.tsx
            FeatureInspector.css
            index.ts
            register.ts

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
-   complete multi-page artifact loading and short/empty page termination;
-   selection;
-   analysis-scoped feature/component identity;
-   cross-analysis feature-group identity;
-   null/empty/whitespace selection-mode filtering;
-   EIC update;
-   MS1 update;
-   MS2 update;
-   deterministic trace, marker, fill, and legend color parity;
-   persisted partner/correlation/loss-chain network construction;
-   missing optional data;
-   large-table pagination/filtering;
-   viewer close/reopen state behavior.

### Manual GUI regression checks

Run these against a fresh browser session after viewer-layout changes:

-   inspect the Feature Inspector in light and dark themes;
-   resize the details splitter and verify EIC/MS1/MS2 plots use the available
    panel width;
-   verify the EIC legend remains inside the plot at top-right and its swatches
    match the traces;
-   select by feature, group, component, and group+component, including an
    artifact where the selected dimension is entirely empty;
-   confirm the feature count, plot, table/details, and dependent views all
    reflect the filtered rows;
-   click a point and a table row, then switch tabs without creating a canvas
    node;
-   exercise file/folder click, Shift-click, Ctrl/Cmd-click, right-click, and
    JSON-array append behavior;
-   resize JSON editor modals horizontally and vertically.

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

The next smallest slice that meaningfully advances the current architecture is:

1. audit the implemented `ViewerShell`, `ViewerResolver`, core registrations,
   and generic fallback against persisted artifact identity;
2. add missing unit tests for the section 3a selection, color, relationship,
   and pagination contracts;
3. run the fresh browser GUI regression matrix in the testing section;
4. fix only concrete viewer-boundary or GUI-contract failures, without adding
   a second rendering path or moving domain logic into `CanvasShell`;
5. remove the remaining transitional raw viewer-client field after all static
   viewers use the plugin API;
6. design the manifest and controlled runtime loader around the now-tested
   transactional registration boundary.

This sequencing separates four risks:

-   read-only viewer binding and GUI regressions;
-   MCP/web-app boundary and release packaging;
-   public plugin API compatibility;
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
