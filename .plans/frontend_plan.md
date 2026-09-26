# StreamFind React Frontend Implementation Plan

**Implementation branch:** `frontend_development`, worktree `.worktrees/frontend`
**Upstream integration source:** local `dev_refactoring` (import deliberately; no automatic commit/push)
**Frontend:** React + TypeScript + Vite
**Backend:** StreamFind C++ core through `streamfind_service` HTTP/WebSocket; MCP is separate
**Primary domains:** MassSpec first, Raman and Sensors as genericity validation
**Reference UI:** `cogniflow-playground/sandcastle/cf_web` and the existing StreamFind Shiny UI under `bindings/r`

---

## Active checkpoint and execution order

Phase 1 is recorded complete. Phase 2 transport and Phase 3 project lifecycle have
working slices; Phase 4 canvas interaction scaffolding and part of Phase 5 execution
exist, but they are NOT an artifact-bound workflow implementation. The current
canvas graph is local UI state, entries lack data ports, and the backend executes an
ordered method list. Do not mark Phase 4 or durable Phase 5 complete from those slices.

**Next work: Phase 3A, then 3B → 3C → 3D → 3E → revised Phase 4 → revised Phase 5.**
These intermediate phases replace the old method/operation split before more canvas
features. The target is one operation-only DAG, with immutable output artifacts,
JSON values or table references in a DuckDB inventory, and backend-owned execution.
This decision supersedes older method-chain and disconnected-entry proposals in
reference notes. Backend code described below is planned unless explicitly checked.

**Visualization work now follows the authoritative contract in Section 22.** Phase 7 and Phase 9 must be implemented
against `sfvis:VisualizationSpec`: backend operations create visualization artifacts; the shared client runtime renders
them. Do not add a second path where React reconstructs scientific traces directly from MassSpec tables or operation IDs.

Phases describe responsibility and acceptance order, not permission to leave broken
interfaces between commits. The ABI/registry replacement in 3B–3C and caller/plugin
conversion in 3D–3E are one coordinated cutover: update compile-dependent call sites
in the same slice, then finish each phase's behavioral gates. Do not temporarily
route operations through retained methods to keep an intermediate build green.

## Authoritative operation-framework decision

The following is the accepted target model from the operation/dataflow discussion and
the attached concept map. Later sections must be interpreted consistently with this
section; older text describing methods as a separate canvas node class, project nodes,
execution-only edges, implicit latest tables, or automatic entry execution is obsolete.

### Core rule

**Operations are the only computational workflow class.** A processing method is
converted into an operation; it is not retained as a parallel `Method` node, wrapper,
adapter, or second execution path. Operations may be entry, processing, extraction,
data-carpentry, summary, import, or visualization-preparation operations, but all use
the same lifecycle, parameter, port, artifact, provenance and cache contracts.

```text
Operation instance
├── operation capability/version
├── operation-specific parameters
├── zero or more named typed input ports
├── one or more named typed output ports
├── cacheability and execution policy
├── validation/execution implementation
└── node/run/artifact provenance
```

The active project/session is implicit execution context, not a visible project node.
Edges are data bindings from `source node + output port` to `target node + input port`.
They are not merely visual links or ordering edges. Execution order is derived from
the directed graph. An operation may have no data inputs only when it is a declared
entry operation; it still exposes explicit outputs.

### Project and entry behavior

Creating a project creates the project/system metadata and an initial saved workflow
definition, but no scientific data tables or source-file links. The backend may
materialize declared entry-operation node instances in that definition. It must not
execute them during create or open. Opening a project restores the graph and artifact
inventory without duplicating entry nodes or rerunning imports. Closing disconnects
the session and preserves the DuckDB file.

Entry operations have parameters and no upstream data inputs. Their outputs start
pipelines. Examples include MassSpec `add_analyses` and future finite-batch sensor
acquisition operations; OPC UA/MQTT continuous ingestion is not part of the first DAG
implementation. A domain without an entry operation still creates successfully with
an empty/unconfigured graph.

### Canonical reference graph

Use this as the first vertical acceptance slice. Names are conceptual contracts and
must be reconciled with the actual catalogue identifiers before implementation; do
not advertise an output that the native producer does not create.

```text
add_analyses(parameters: files, replicates, blanks)
  ├── analysesTable ───────────────┐
  ├── spectraHeadersTable ─────────┤
  └── chromatogramHeadersTable     │
                                    ▼
                         find_features(parameters)
                                    │ ntaFeaturesTable
                    ┌───────────────┴────────────────┐
                    ▼                                ▼
          filter_features(parameters)       get_features(parameters)
                    │                                │
       filtered ntaFeaturesTable               featuresResult (memory/JSON)
                    │                                │
                    ▼                                ▼
             count_table_rows                 plot_features(parameters)
             numericValue (memory/JSON)                 │
                                                        ▼
                                             visualizationSpec
                                           (sfvis:VisualizationSpec)

import_suspects(parameters: file/database)
  └── suspectsResult (memory/JSON or table according to size/contract)
                    │
                    ▼
             suspect_screening(parameters)
              ├── ntaFeaturesTable
              └── ntaSuspectsTable
```

`find_features` has two distinct required inputs—analyses and spectra headers—even
if both are persisted DuckDB table artifacts. `filter_features` consumes one feature
artifact and publishes a separate output artifact; the original feature artifact
remains inspectable. The two feature artifacts may share a semantic table contract,
but never share an ambiguous “current table” identity. `get_features` and row counts may produce bounded in-memory/JSON values. Scientific visualization is represented by
normal backend visualization-preparation operations that consume exact typed artifact bindings and publish an immutable,
versioned `sfvis:VisualizationSpec` JSON artifact. The operation owns scientific selection, aggregation, units, trace
semantics, labels and defaults; it never owns a browser DOM or rendering library instance. A client-side generic
visualization runtime consumes the resulting spec and performs the actual Plotly/D3 rendering. Visualization operations
do not mutate their scientific inputs, and a rendering client never infers a different scientific plot directly from
DuckDB rows.

### Artifact representations

An artifact is a logical value produced by a node output port. Its semantic contract
is independent of its storage representation:

```text
ArtifactDescriptor
  artifact_id, semantic_contract, schema_version, fingerprint
  storage_kind: 'table' | 'json' | 'memory'
  producer node/run/port and input lineage
  availability, retention/pinned state
  table locator OR JSON payload/serialization metadata
```

The inventory is the authority for artifact identity and location. Small structured
results can be persisted as schema-validated JSON in DuckDB and decoded when consumed;
large tables, feature sets, spectra and chromatogram points remain table-backed. A
runtime-only memory value may be used within a run but is unavailable after restart
unless persisted or reproducibly recomputed. JSON is a storage representation, not a
universal port type: port compatibility uses semantic contracts and schemas.

Output publication is atomic per node. Write private output tables or payloads,
validate every declared output, insert the inventory/output bindings, and commit as
one unit. Failed or cancelled nodes do not publish partial outputs. Published inputs
and outputs are immutable; storage reclamation is governed by references, pins and
retention, not by overwriting a global table.

### Cache and dynamic inventory

`sf:cacheable` remains the semantic authority for whether an operation may reuse a
successful result. A cache entry is an output manifest mapping named output ports to
existing artifact IDs; it is not a second copy of the data. The cache key includes
operation/plugin/schema version, normalized parameters/defaults, ordered named input
fingerprints, relevant execution settings and content-aware external-source
fingerprints. A cache hit must verify that every artifact still exists and satisfies
the contract. Missing/changed inputs, changed parameters or implementation versions
invalidate only that node and its descendants.

The dynamic inventory grows as operations publish outputs. It must support multiple
artifacts with the same semantic contract, branches, JSON values, persisted tables,
lineage, previews and retention. No core/SDK/plugin code may select an artifact by
“latest table of type”; execution always resolves the exact artifact binding.

### Workflow schema and execution modes

Persist the graph definition independently from run history and artifact inventory:

```text
WorkflowDefinition
  schema_version, workflow_id, revision
  nodes: node_id, operation_id/version, parameters
  connections: from_node/port → to_node/port
  output policies and presentation metadata

WorkflowRun / NodeExecution
  immutable definition revision, status/progress/cancellation
  resolved input artifact IDs, output artifact IDs, cache hit/miss, diagnostics
```

Validate ports, semantic compatibility, required inputs, entry-root reachability,
cardinality and cycles before mutation. Support the same scheduler for `run_node`,
`run_to_node`, full graph execution and explicit force-recompute. Initially serialize
execution and writes within one project while allowing independent projects to run.
The existing lifecycle states and cancellation/progress events must be attached to
node executions and runs, not to a synthetic ordered method step.

### Backend/frontend authority

The backend owns the persisted graph, capability contracts, validation, execution,
artifact inventory, cache, lineage and lifecycle. The frontend renders and edits the
same graph through typed service APIs. Workflow and Explorer are two views of that
graph: Workflow emphasizes processing/run controls, while Explorer emphasizes artifact
inspection, previews and plots. React must not maintain a disconnected execution graph,
access DuckDB, invent domain operations, or infer physical table names.

Work only in `C:/Users/cunha/Documents/GitHub/streamfind/.worktrees/frontend`.
Preserve existing dirty files and keep the main checkout unchanged. Do not commit,
push, create old/new feature flags, or add legacy workflow adapters. Existing-project
schema incompatibility must produce a clear version error, never silent data loss;
a released-format migration requires a separately approved scope. Leave `bindings/r`
and `integrations/cf-streamfind` untouched during this refactor.

## 1. Purpose

This plan defines the implementation sequence for the StreamFind React web application.

The application must:

* manage multiple StreamFind project DuckDB files;
* create and open projects;
* respect the immutable project domain;
* allow several projects to remain open simultaneously;
* create and edit workflows from ontology-derived capabilities;
* execute workflows through durable backend workers;
* display live workflow progress and worker state;
* inspect persisted table artifacts, JSON artifacts, in-memory result previews, and workflow outputs;
* render scientific results through reusable React components;
* allow plugins to declare preferred visual representations for typed artifacts;
* support future domains without adding domain-specific logic to the generic frontend;
* preserve workflow provenance and execution history;
* provide a consistent StreamFind visual identity from the first implementation stage.

The frontend must not:

* access DuckDB directly;
* become the owner of workflow execution state;
* duplicate operation definitions already contained in the semantic catalogue;
* contain hard-coded MassSpec workflow inventories;
* implement domain validation that belongs in the backend;
* depend on the R package or Shiny runtime.

The backend remains the source of truth for project state, workflow validation, execution lifecycle, plugins, tables, and capabilities.

---

# 2. Target frontend architecture

```text
React application
│
├── Bootstrap / Session
│
├── Project Hub
│   ├── Create project
│   ├── Open project
│   ├── Recent projects
│   └── Active project sessions
│
├── Project Workspace
│   ├── Workflow
│   ├── Explorer
│   ├── Results
│   ├── Data / Tables
│   ├── Runs
│   ├── Provenance
│   └── Project settings
│
├── Capability Registry
│   ├── Domains
│   ├── Operations
│   ├── Parameters
│   ├── Table contracts
│   └── UI metadata
│
├── Renderer Registry
│   ├── Generic result renderers
│   ├── VisualizationSpec renderer
│   └── Non-plot/domain detail renderers
│
├── Visualization Runtime
│   ├── VisualizationSpec schema/versioning
│   ├── Plotly renderer
│   ├── Declarative D3 renderer registry
│   ├── artifact-backed data resolver
│   └── text/static fallback adapters
│
├── Execution Store
│   ├── Workers
│   ├── Runs
│   ├── Node executions and artifact bindings
│   └── Live events
│
└── StreamFind Client
    │
    └── Typed HTTP / WebSocket application transport
             │
             ▼
       C++ StreamFind host
```

The existing C++ architecture already separates project persistence, workflow execution, SDK/plugin contracts, and dynamically discovered plugins. The frontend should mirror those boundaries rather than introduce another capability model.

---

# 3. Proposed frontend layout

Create a root-level frontend package:

```text
frontend/
├── src/
│   ├── app/
│   │   ├── App.tsx
│   │   ├── BootstrapGate.tsx
│   │   ├── AppShell.tsx
│   │   └── router.tsx
│   │
│   ├── backend/
│   │   ├── StreamFindClient.ts
│   │   ├── WebSocketTransport.ts
│   │   ├── protocol.ts
│   │   ├── requests.ts
│   │   └── subscriptions.ts
│   │
│   ├── capabilities/
│   │   ├── CapabilityProvider.tsx
│   │   ├── capabilityStore.ts
│   │   ├── types.ts
│   │   └── normalizers.ts
│   │
│   ├── projects/
│   │   ├── ProjectHub.tsx
│   │   ├── ProjectWorkspace.tsx
│   │   ├── ProjectSwitcher.tsx
│   │   ├── CreateProjectDialog.tsx
│   │   ├── OpenProjectDialog.tsx
│   │   └── projectStore.ts
│   │
│   ├── workflow/
│   │   ├── WorkflowCanvas.tsx
│   │   ├── WorkflowNode.tsx
│   │   ├── WorkflowEdge.tsx
│   │   ├── OperationLibrary.tsx
│   │   ├── NodeInspector.tsx
│   │   ├── ParameterEditor.tsx
│   │   ├── WorkflowToolbar.tsx
│   │   └── workflowStore.ts
│   │
│   ├── execution/
│   │   ├── ExecutionMonitor.tsx
│   │   ├── ExecutionTimeline.tsx
│   │   ├── WorkerMonitor.tsx
│   │   ├── RunDetails.tsx
│   │   └── executionStore.ts
│   │
│   ├── tables/
│   │   ├── TableExplorer.tsx
│   │   ├── TableSchema.tsx
│   │   ├── TableGrid.tsx
│   │   ├── TableFilters.tsx
│   │   └── tableStore.ts
│   │
│   ├── results/
│   │   ├── ResultExplorer.tsx
│   │   ├── ResultRenderer.tsx
│   │   ├── RendererRegistry.ts
│   │   └── renderers/
│   │       ├── TableRenderer.tsx
│   │       ├── MetricRenderer.tsx
│   │       ├── LineRenderer.tsx
│   │       ├── ScatterRenderer.tsx
│   │       ├── HeatmapRenderer.tsx
│   │       ├── SpectrumRenderer.tsx
│   │       ├── ChromatogramRenderer.tsx
│   │       └── FeatureMapRenderer.tsx
│   │
│   ├── visualization/
│   │   ├── VisualizationRenderer.tsx
│   │   ├── VisualizationRegistry.ts
│   │   ├── visualizationTypes.ts
│   │   ├── visualizationSchema.ts
│   │   ├── VisualizationDataResolver.ts
│   │   ├── interactions.ts
│   │   ├── plotly/
│   │   │   ├── PlotlyRenderer.tsx
│   │   │   └── plotlyAdapter.ts
│   │   └── d3/
│   │       ├── D3RendererRegistry.ts
│   │       └── ForceNetworkRenderer.tsx
│   │
│   ├── plugins/
│   │   ├── PluginProvider.tsx
│   │   ├── PluginRegistry.ts
│   │   ├── pluginTypes.ts
│   │   └── uiContract.ts
│   │
│   ├── provenance/
│   │   ├── ProvenanceView.tsx
│   │   └── ProvenanceGraph.tsx
│   │
│   ├── components/
│   │   ├── inputs/
│   │   ├── layout/
│   │   ├── feedback/
│   │   └── scientific/
│   │
│   ├── theme/
│   │   ├── tokens.ts
│   │   ├── palettes.ts
│   │   ├── styles.ts
│   │   └── theme.css
│   │
│   └── assets/
│       └── streamfind-logo.svg
│
├── tests/
├── package.json
├── tsconfig.json
└── vite.config.ts
```

---

# 4. Phase 1 — Frontend shell, theme, and splash screen

**Status: COMPLETE**

## Goal

Create the visual and structural foundation before implementing domain features.

## Tasks

### 4.1 Create frontend package

Initialize:

* React;
* TypeScript;
* Vite;
* routing;
* test framework;
* linting;
* formatting.

Avoid domain-specific dependencies at this stage.

### 4.2 Create application shell

Implement:

* top application bar;
* project switcher placeholder;
* navigation sidebar;
* project workspace outlet;
* global notification area;
* modal layer;
* error boundary.

Initial routes:

```text
/
/projects
/project/:sessionId/workflow
/project/:sessionId/explorer
/project/:sessionId/results
/project/:sessionId/data
/project/:sessionId/runs
/project/:sessionId/provenance
```

### 4.3 Theme system

Adapt the conceptual model used by CogniFlow:

```text
Theme
├── mode
│   ├── light
│   └── dark
├── palette
└── style
```

Use semantic CSS tokens instead of hard-coded component colors.

Minimum tokens:

```text
--sf-bg
--sf-bg-secondary
--sf-surface
--sf-surface-raised
--sf-surface-hover
--sf-border
--sf-border-strong

--sf-text
--sf-text-secondary
--sf-text-muted
--sf-text-inverse

--sf-accent
--sf-accent-hover
--sf-signal

--sf-success
--sf-warning
--sf-error
--sf-info

--sf-node-input
--sf-node-processing
--sf-node-output

--sf-domain-mass-spec
--sf-domain-raman
--sf-domain-sensors
```

The CogniFlow frontend already has a mature token-based theme model with palettes, light/dark modes, datatype colors, status colors, canvas colors, and node colors. Reuse that architectural approach rather than copying CSS ad hoc.

### 4.4 Initial StreamFind palette

Use the existing CogniFlow `wastewater` palette as the starting inspiration.

Primary identity:

```text
teal / blue-green primary accent
muted scientific backgrounds
ochre / amber signal accent
neutral technical surfaces
```

### 4.5 Splash screen

Implement `BootstrapGate`.

Initial state:

```text
full-screen clean background
        │
        ▼
StreamFind logo
        │
vertical Y-axis rotation
        │
        ▼
session initialization
```

The logo should rotate vertically around itself using CSS transform:

```text
rotateY()
```

Add:

* fade-in;
* fade-out transition;
* `prefers-reduced-motion` handling;
* failure state if backend initialization fails.

Do not show the application workspace until bootstrap is complete.

## Current implementation status

The current frontend foundation implements the following Phase 1 surfaces:

### Application control and appearance

* The top bar contains the `streamfind` identity, Home, backend state, notifications,
  and Settings controls.
* Settings is implemented as a right-side pane with persisted light/dark mode,
  `streamfind`, `playful`, and `matrix` palettes, and Classic, Studio, and Chrome
  style selections.
* Palette previews display the configured accent swatches.
* Light-mode content surfaces use a literal white base and dark-mode content surfaces
  use a literal black base; palette colors are used as accents.
* The landing splash has a white background, rotating logo, reduced-motion handling,
  and a bootstrap failure state.

### Notifications and backend state

* Notifications use a typed in-memory bus with bounded history, severity, unread state,
  drawer history, timestamps, and auto-dismissed toasts.
* Opening the notification drawer marks notifications read; there is no separate
  “mark all read” action.
* The backend indicator is a top-bar server icon with state-specific colors for
  disconnected, connecting, initializing, ready, reconnecting, and failed states.
* Selecting the backend icon opens a `Backend` details pane with endpoint, protocol,
  role, and current state.

### Project Hub home surface

* The no-project home is a Project Hub with fixed Create Project and Open Project
  action cards.
* Active projects are rendered as reusable playing-card-style project cards in the
  same responsive grid.
* Cards use the active palette accent for their thin edge/depth shading and keep the
  home surface visually separate from the top bar.
* Project cards show the database filename, domain, full path, preview, and open
  actions.
* The Project Hub activates a validated project workspace only after the backend
  accepts the project.

### Deliberate shell deviation

The original Phase 1 proposal included a visible navigation sidebar. The current
accepted design removes that visible sidebar and uses a full-width workspace below
the top bar. Project navigation is exposed only after a project is active, while
Home remains a global top-bar action.

## Phase 1 completion record

Phase 1 is complete. The acceptance checklist below is fully satisfied, including
the service-driven bootstrap gate, browser smoke coverage, and session-aware project
routes. This is the historical Phase 1 completion record; the active next milestone
is Phase 3A as recorded at the top of this plan.

## Acceptance criteria

* [x] React app builds successfully with `npm run build`.
* [x] Light and dark modes work.
* [x] Theme preferences can be persisted locally.
* [x] Settings, notifications, backend state, and Project Hub home controls render
  through the application shell.
* [x] Splash screen appears before the application workspace.
* [x] Theme colors use semantic tokens, with documented literal white/black base-surface exceptions.
* [x] Splash screen has explicit fade-in/fade-out transitions with reduced-motion
  handling.
* [x] Frontend unit test framework and foundational notification tests are present
  (`npm run test:run`).
* [x] Theme, Project Hub, and bootstrap tests are present in `src/app/App.test.tsx`.
* [x] Frontend linting gate is present and passes with `npm run lint`.
* [x] Frontend formatting gate is present and passes with `npm run format:check`.
* [x] Browser-level shell smoke check passes with `npm run browser:smoke`.
* [x] Bootstrap completion is driven by the real service initialization lifecycle,
  with a minimum three-second splash duration and retry handling.
* [x] Project context uses session-aware hash routes such as
  `#/project/<sessionId>/workflow`, with Project Hub fallback for invalid sessions.

---

# 5. Phase 2 — WebSocket/C++ client and session bootstrap

## Goal

Establish a single browser-facing communication layer between React and the C++ runtime.

## Tasks

### 5.0 First implementation slice — `streamfind_service`

Before implementing the React transport, establish a browser-facing C++ service over the
StreamFind Core Public Application API. Do not route the React application through
`streamfind_mcp` or the current stdio `streamfind_cli` target; MCP remains the agent and
external-client boundary.

The first service slice must:

1. Define application protocol DTOs for session state, capabilities, project sessions,
   connection state, errors, and event envelopes.
2. Implement a long-running `streamfind_service` executable under `cpp/service/`.
3. Add `/session` for service/session initialization and status.
4. Add `/capabilities` for normalized core/plugin capability discovery.
5. Add project runtime management for multiple simultaneously open project sessions.
6. Add a WebSocket event channel for project, workflow, worker, execution, and service
   state events.
7. Connect the React client to `streamfind_service` through a typed application API
   client, not MCP tool calls.
8. Replace the static `Service not connected` label with live states:
   `disconnected`, `connecting`, `initializing`, `ready`, `reconnecting`, and `failed`.

The service should own the following long-lived boundaries:

```text
streamfind_service
├── ProjectRuntimeManager
├── WorkerManager
├── WorkflowExecutionManager
├── PluginRegistry / CapabilityRegistry
└── EventBroker
```

The service must build on the C++ public APIs, including `streamfind::Project`,
`streamfind::api::run` and the unified `OperationRegistry` after Phase 3C. The existing
`MethodRegistry` is a refactoring input, not part of the target. The service must not access
DuckDB through a second frontend-specific persistence layer, invoke MCP internally, or
pass C++ implementation objects to React.

Initial application surface:

```text
/session
/capabilities
/projects
/events
```

The first slice does not need to implement the complete project, workflow, execution,
table, or result API. It must establish the service process, typed DTO boundary, live
connection lifecycle, multi-project runtime ownership, and event delivery path that those
later endpoints will extend.

#### First service-slice acceptance criteria

* `streamfind_service` builds as a separate C++ executable under `cpp/service/`.
* The service links against the C++ Core Public Application API and does not use MCP
  request handling internally.
* `/session` returns service and protocol state.
* `/capabilities` returns normalized capability data from the loaded core/plugin registry.
* Multiple project sessions can be represented without replacing one another.
* WebSocket events can be published to connected browser clients.
* React displays live service connection state rather than a hard-coded status.
* Service startup, connection loss, reconnect, and initialization failure are visible in
  the notification framework.

### 5.1 Add browser transport to the C++ host

Keep current stdio MCP support for agents and external MCP clients. Add the
browser-facing `streamfind_service` application protocol with HTTP request/response
endpoints and a persistent WebSocket event channel. Use the application endpoints
`/session`, `/capabilities`, and `/projects`; reserve `/events` for WebSocket upgrades.

The WebSocket channel carries service, project, workflow, worker, and execution events.
The service owns protocol DTO serialization and must not expose C++ implementation
objects or invoke MCP request handling internally.

### 5.2 Create frontend application API client

Implement a typed `StreamFindApiClient` that:

* bootstraps `/session` and `/capabilities`;
* opens `/events` as a WebSocket;
* exposes typed project/session operations as the API expands;
* reports `disconnected`, `connecting`, `initializing`, `ready`, `reconnecting`, and
  `failed` states;
* forwards event envelopes to the notification and workspace layers.

Keep transport details inside the client so application components do not depend on
HTTP, WebSocket, MCP, or C++ implementation details.

### 5.3 Session initialization contract

Bootstrap sequence:

```text
connect
↓
initialize
↓
backend version
↓
protocol version
↓
core capabilities
↓
enabled plugins
↓
domains
↓
semantic catalogue projection
↓
UI metadata
↓
active executions
↓
ready
```

### 5.4 Capability bootstrap

Retrieve:

* domains;
* operations;
* named typed input/output ports;
* parameters;
* table contracts;
* plugin information;
* supported result types;
* semantic labels and descriptions.

Do not parse Turtle in React.

The backend should provide normalized frontend-friendly DTOs.

### 5.5 Connection state

Support:

```text
disconnected
connecting
initializing
ready
reconnecting
failed
```

Add exponential reconnect behavior.

### 5.6 Event envelope

Define one stable event envelope:

```json
{
  "type": "workflow.node.progress",
  "project": "...",
  "workflow_revision": 1,
  "run_id": "...",
  "node_id": "...",
  "execution_id": "...",
  "sequence": 1,
  "timestamp": "...",
  "payload": {}
}
```

Event groups:

```text
session.*
project.*
workflow.*
workflow.node.*
worker.*
table.*
plugin.*
```

## Acceptance criteria

* React establishes a WebSocket connection to the C++ host.
* Protocol/version negotiation occurs during startup.
* Plugin and capability discovery is automatic.
* React contains no manually maintained MassSpec operation list.
* Connection loss and reconnection are handled visibly.
* Backend initialization failure is shown through the splash/error state.

---

# 6. Phase 3 — Project Hub and multi-project runtime

## Goal

Allow users to create, open, close, and switch between multiple project databases while keeping independent project runtimes active.

The current architecture defines one DuckDB file as exactly one project, with one immutable domain. Multiple project files may be open and execute independently.

## 6.0 Two sister canvases per project

Each project has two views of the same backend-owned operation graph. Workflow
emphasizes processing and execution; Explorer emphasizes artifacts, inspection and
visualization. They share node identities, bindings and connection semantics.

### Project-card actions

Every project card exposes three actions:

* **Preview** — opens a read-only project overview with project identity, available tables, workflow summary, lifecycle state, and a read-only workflow preview. It does not activate a canvas.
* **Workflow canvas** — opens `#/project/<sessionId>/workflow`.
* **Explorer canvas** — opens `#/project/<sessionId>/explorer`.

The existing external-link action becomes the workflow action and a second action is added for the explorer canvas. All three controls require explicit accessible labels.

### Workflow canvas

The active project is implicit context, never a graph node. Entry operations have
parameters and output ports but no upstream data inputs:

```text
Entry operation → typed artifacts → processing operation → typed artifacts
```

Processing methods become operations. Edges bind named output ports to named input
ports; they are data dependencies, not an execution-order chain. An output can be
used by multiple operations and inspected in either canvas.

Workflow lifecycle state is rendered on nodes and edges:

```text
idle | validated | queued | running | cancelling | completed | failed | cancelled
```

### Explorer canvas

The explorer canvas is the data interaction and visualization surface. It contains backend operations and frontend-native nodes:

```text
Backend operation → ontology result → domain-specific plot
Domain result → primitive column extraction → generic frontend node
```

For example:

```text
getRawSpectraEic → eicResult → plotEIC
```

Backend extraction, filtering and transforms use the same operation contract as
processing. Plots are explicit frontend renderers of typed artifacts. Do not create
a second browser-only scientific execution engine. Frontend-only rendering must be
identified explicitly and reported as not rendered during headless execution.

### Shared shell and typed connections

Keep pan, zoom, grid, snapping, dragging, selection and theme in `CanvasShell`.
Keep `WorkflowCanvas` and `ExplorerCanvas` as presentation modes. Capability DTOs
describe operation ports and renderer contracts, not method-versus-operation rules:

```ts
Operation: parameters + named typed inputs + named typed outputs + project_entry
Artifact: semantic contract + identity + storage representation
Renderer: accepted semantic contracts + frontend execution target
```

Port compatibility is semantic and backend-validated. Canvas mode must not prohibit
an otherwise valid data connection. Plain wheel/two-finger movement pans; Ctrl/Cmd
wheel or pinch zooms only the canvas through a non-passive wheel listener. Do not
restore device-detection thresholds.

### Workflow and explorer service contracts

Use one graph service and artifact inspection surface. The routes below are target
contracts to implement in Phase 3E, not assertions that all exist already:

```text
GET  /projects/<sessionId>/workflow
GET  /projects/<sessionId>/workflow/state
PUT  /projects/<sessionId>/workflow
POST /projects/<sessionId>/workflow/validate
POST /projects/<sessionId>/workflow/run
POST /projects/<sessionId>/workflow/cancel

GET /projects/<sessionId>/artifacts
GET /projects/<sessionId>/artifacts/<artifactId>/schema
GET /projects/<sessionId>/artifacts/<artifactId>/preview
```

The inspector shows parameters, named ports, bound artifact IDs, semantic types,
storage kind, availability, provenance and compatible renderers. Physical locators
remain backend-owned. Pause stays unsupported unless separately implemented; do not
render a working pause action over the current 501 response.

### Implementation order

1. Preserve implemented routes, project-card actions and shared canvas mechanics.
2. Complete intermediate Phases 3A–3E below before expanding graph UI behavior.
3. Complete revised Phase 4 against the real graph and artifact DTOs.
4. Complete Phase 5 recovery/history and later scientific renderer phases.

## Tasks

### 6.1 Project Hub page

Create:

```text
Project Hub

[ New Project ]   [ Open Project ]

Recent Projects

Active Projects

Available Domains
```

### 6.2 Create project flow

User selects:

```text
project name
database path
domain
optional description
```

Domain list must come from capability discovery.

Do not hard-code:

```text
mass_spec
raman
sensors
```

### 6.3 Open project flow

Backend validates:

* file exists;
* valid StreamFind project;
* exactly one project record exists;
* domain is installed;
* project schema is compatible.

### 6.4 Project session model

Frontend session:

```ts
ProjectSession {
  sessionId;
  databasePath;
  metadata;
  domainId;
  status;
}
```

`sessionId` is UI/session identity only.

`databasePath` remains the backend project locator.

### 6.5 Multiple open projects

Maintain:

```text
Project A
Project B
Project C
```

Switching visible projects must not close inactive ones.

### 6.6 Backend ProjectRuntimeManager

Implement or extend the C++ runtime manager:

```text
ProjectRuntimeManager
├── open()
├── create()
├── close()
├── list_open()
└── get_runtime()
```

Each runtime owns:

```text
Project
WorkflowExecutionManager
worker association
event subscriptions
```

### 6.7 Project switcher

Top application bar:

```text
Project A ●
Project B ▶
Project C ✓
```

Status icon should reflect:

* idle;
* running;
* completed;
* failed.

## Acceptance criteria

* User can create projects.
* User can open existing projects.
* Domain is selected only during creation.
* Multiple projects remain active at once.
* Closing one project does not affect others.
* Switching UI project does not affect backend execution.

---

# 6A. Phase 3A — Freeze operation/port/artifact contracts

**Status: TODO. Backend-first prerequisite to further Phase 4 development.**

## Implementation sequence

1. Inventory `MethodDefinition`, `MethodRegistry`, `OperationDefinition`,
   `OperationRegistry`, `WorkflowStep` and `Workflow` in
   `cpp/core/include/streamfind/project.hpp`. Trace each definition through
   `cpp/core/src/`, `cpp/core/include/streamfind/catalogue_binding.hpp`, SDK loading,
   plugin registration, public API, CLI/MCP and service. Record affected files before
   changing signatures; the paths here identify existing owners, not new APIs.
   operation's implementation can be invoked by the backend when its declared input
   artifacts and parameters are supplied. Workflow execution supplies those inputs,
   normalizes parameters, manages DuckDB-backed artifact publication, and returns an
   execution envelope containing status, diagnostics, and output-artifact bindings.
   Operations are not required to declare transport/session properties such as
   `sf:invocationModel` or `sf:requiresConnection`; those belong to the API boundary,
   not the operation data contract. A standalone backend operation remains valid: for
   example, `create` can be called directly with a database path and domain to create
   a project, without being placed in a workflow. The workflow framework begins when
   operations are composed into a project graph.
   `sf:returns` is optional for legacy/public response schemas and must not duplicate
   workflow output ports. Workflow data outputs are declared through reusable table or
   result contracts under `sf:hasOutputPort`; execution status, messages and diagnostics
   are returned in the framework execution envelope. Conditional control flow may use
   explicit reusable signal/result contracts later, with conditions on workflow
   connections, rather than treating log messages as data dependencies.
2. Define the target operation schema in `cpp/core/semantic/vocabulary.ttl` and
   `shapes.ttl`; domain declarations stay under `cpp/plugins/<domain>/semantic/`.
   An operation has stable ID/version, parameters, named input/output ports,
   `sf:projectEntry`, `sf:cacheable`, mutation/execution metadata and an executor.
   Input and output ports directly reference reusable `sf:ArtifactContract` resources;
   the initial concrete contracts are existing `sf:Table` and `sf:Result` resources.
   Do not create operation-specific port resources. The referenced table/result owns its
   label, definition, schema, storage metadata and semantic identity, so the same table
   or result can be reused as an input of one operation and an output of another.
   Direction comes from the operation predicate (`sf:hasInputPort` or
   `sf:hasOutputPort`), while workflow connections still bind concrete node and port
   identifiers in the project workflow schema. Cardinality and optionality belong to
   the operation contract only when the reusable artifact resource cannot express them;
   do not duplicate table/result metadata in per-operation port declarations.
   Reuse existing vocabulary where equivalent; explicitly declare any new predicates and validate them with SHACL.
3. Treat `sf:reads`/`sf:writes` as aggregate effects of precise named port contracts,
   not a second independent dependency authority. Carry conditional read semantics
   forward, evaluating conditions from this node's normalized parameters. Never
   infer predecessor IDs or choose the most recently created table of a type.
4. Extend `cpp/sdk/src/catalogue_builder.cpp`, `cpp/core/src/catalogue.cpp` and
   catalogue binding/serialization together. Project one canonical operation/port
   shape; transfer method defaults, validation, cacheability and provenance rather
   than dropping them during the rename. Regenerate projections, never hand-edit
   generated catalogue JSON. Keep the SDK and core free of domain-specific names.
5. Define the following target documents and validators before implementing storage.
   Field names below are the proposed contract to implement consistently, not
   currently available endpoints or structs:

```text
WorkflowDefinition
  schema_version, workflow_id, revision
  nodes[]: node_id, operation_id, operation_version, parameters
  edges[]: from_node, from_port, to_node, to_port
  output_policies[]: node_id, output_port, persistence
  presentation: positions and per-view display metadata (not execution semantics)

WorkflowRun
  run_id, workflow_id, definition_revision, immutable definition snapshot, status
NodeExecution
  execution_id, run_id, node_id, normalized_parameters, status, cache_key
  input_bindings[]: port_id, ordinal, artifact_id
  output_bindings[]: port_id, ordinal, artifact_id
```

6. Required roots are catalogue-declared zero-input entry operations. Multiple entry
   roots and disconnected valid components are allowed; an empty graph is a valid
   unconfigured project. Control-plane commands (create/open/close/workflow editing)
   stay outside the computational node palette. Do not require project/session IDs
   in operation parameters: inject project context and resolved bindings at dispatch.

## Phase 3A reconnaissance result — current migration map

**Status: COMPLETE as an inventory; target contract implementation remains TODO.**
This map was produced by tracing the current frontend worktree after the imported
chromatogram commits. It is the baseline for the next implementation slice.

### Current core ownership

| Current owner | Current responsibility | Required destination |
| --- | --- | --- |
| `cpp/core/include/streamfind/project.hpp:168` `MethodDefinition` | method ID/version/domain, `reads`, conditional reads, parameters, cacheability and `writes` | unified `OperationDefinition` with named ports and output policies |
| `project.hpp:198` `Method` | parameter resolution, validation, executor/context-executor dispatch and method errors | unified operation executor with resolved artifact handles |
| `project.hpp:229` `MethodRegistry` | method registration, lookup and catalogue listing | remove as workflow authority; use one operation registry |
| `project.hpp:242` `OperationDefinition` / `Operation` | lightweight parameter-only operation metadata and `Project` executor callback | expand to the complete operation contract; do not preserve as a second model |
| `project.hpp:263` `OperationRegistry` | separate operation registration/listing | become the only computational registry |
| `project.hpp:276` `WorkflowStep` | `{method, parameters}` ordered invocation, JSON serialization | node instance with node ID, operation ID/version, parameters and presentation metadata |
| `project.hpp:288` `Workflow` | ordered `steps`, version/domain, method-based validation and serialization | persisted graph definition with nodes, port connections and revision |
| `project.hpp:457,499,504` `Project` | set/get workflow, ordered `run_workflow`, direct `run_method`, and direct `run_operation` | graph save/validate/schedule/run APIs with node and artifact bindings |

### Current implementation call paths

1. `cpp/core/src/project.cpp:876–904` implements method registry lookup/listing;
   `:906–959` independently implements operation registry lookup/listing.
2. `cpp/core/src/project.cpp:967–982` serializes/deserializes method workflow
   steps. `:984–1060` validates steps in order, resolves method parameters, checks
   aggregate table availability, then adds each method's `writes` to an in-memory
   available-table list. This is the current ordered-table dependency authority.
3. `cpp/core/src/project.cpp:1568–1681` executes `Workflow.steps` sequentially,
   computes a previous-step hash, restores cache snapshots, invokes `Method::run`,
   snapshots declared write tables and records `WORKFLOW_EXECUTION_STEP`. It does
   not resolve named input/output ports or artifact IDs.
4. `cpp/core/src/project.cpp:1709–1720` implements `run_method` by appending a
   method step to the persisted workflow before running it. This is a legacy direct
   workflow mutation path to replace with an operation-node run mode.
5. `cpp/core/src/catalogue_binding.cpp:44–67` projects separate method and operation
   definitions. `:71+` registers both module kinds. It currently maps effects only
   into method `reads`, conditional reads and `writes`; operation definitions receive
   parameters but not effects/ports.
6. `cpp/sdk/src/dynamic_plugin_manager.cpp:272–308` dispatches catalogue entries by
   `kind`: methods are registered into `MethodRegistry`, operations into
   `OperationRegistry`, both using the current dynamic invocation callback. This is
   the principal plugin registration cutover point.
7. `cpp/core/src/api.cpp:125–182` exposes `get_available_methods`, `add_method`,
   `set_workflow`, `validate_workflow` and `run_method` around the ordered method
   model. `cpp/core/src/mcp.cpp:32–138` advertises and dispatches the same method
   tools. These must consume the graph/operation contract while MCP remains a
   separate transport from the browser service.
8. `cpp/service/src/project_runtime_manager.cpp:97–131` starts the current worker
   with `project->run_workflow(*methods_, ...)`; `:134–155` already has a separate
   direct operation invocation path. The worker must be redirected to the unified
   graph scheduler, and direct operation invocation must use the same binding and
   publication path rather than remain a table-mutating bypass.
9. `cpp/core/semantic/vocabulary.ttl` and `shapes.ttl` currently declare both
   `sf:Method` and `sf:Operation`. `sf:cacheable`, `sf:reads`, and `sf:writes` are
   still method-oriented in important shape rules. `sf:nextOperation` is not a
   replacement for typed port connections and must not be used as a workflow edge.
   The ontology cutover must add named port/artifact contracts and remove the old
   method authority atomically. For a project, the persisted workflow schema owns
   connections as `from_node/from_port → to_node/to_port`; those connections bind
   output artifacts to input ports for that workflow revision.
10. MassSpec currently registers chromatogram processing callbacks as methods in
    `cpp/plugins/mass_spec/src/plugin_entrypoint.cpp`, while extraction callbacks
    remain operations. The imported commits therefore provide algorithm/source
    behavior but are not yet operation-framework ports; Phase 3D must migrate them
    without rewriting their numerical behavior.

### Identified migration risks

* `Operation` currently has only `Project& + Json` execution and cannot receive
  resolved input artifacts or output writers.
* Existing cache restoration snapshots physical tables and uses a previous-step hash;
  it cannot represent multiple same-contract artifacts, JSON manifests or branches.
* `WORKFLOW_EXECUTION_STEP` records ordered methods and must be replaced or separated
  from node-execution history; administrative metadata must not be confused with the
  artifact inventory.
* Existing semantic shapes allow methods and operations to coexist. The target must
  avoid a compatibility shim: update catalogue projection, registries, plugins,
  service, API/MCP handlers and tests as one coordinated cutover.
* The current service has lifecycle/cancellation plumbing that should be retained,
  but its progress payload says `current_step` and must become node/run progress.

### Reconnaissance acceptance

* [x] Core definitions and owning implementations traced.
* [x] Catalogue binding and dynamic plugin registration traced.
* [x] Core API/MCP workflow callers traced.
* [x] Service worker and direct-operation paths traced.
* [x] Semantic method/operation/read/write/cache declarations located.
* [ ] Unified operation schema, artifact inventory and graph executor implemented.

## Acceptance gate

* [ ] Generated catalogue expresses two differently named inputs of the same type.
* [ ] Invalid/missing ports, unknown schema versions and malformed conditions fail
  with structured diagnostics; no silent schema guessing or arbitrary JSON matching.
* [ ] A documented migration checklist covers every registered method and its callers.
* [ ] C++ contract fixtures cover the canonical operation, port, workflow-connection
  and validation fields and behavior.
* [ ] Rust remains intentionally stale during this C++-first refactor. Record this as
  a deliberate scoped exception in the test plan; do not update Rust fixtures, add
  Rust compatibility shims, or claim cross-backend parity until the C++ contract is
  accepted and a separate Rust migration is approved.

---

# 6B. Phase 3B — Artifact inventory and bound plugin data access

**Status: TODO. Depends on Phase 3A.**

## Storage contract

Create project system metadata only; do not eagerly create plugin data tables or
source-file links. Preserve project identity/domain and administrative metadata.
Domain entry nodes may be saved immediately but must not execute during create/open.

Persist an artifact inventory with the following logical fields. Choose physical
system-table names once in core's existing schema owner; do not duplicate that schema
in the service or plugins:

```text
artifact_id (primary key), semantic_contract, schema_version
storage_kind ('json' | 'table'), json_payload, table_schema, table_name
producer_execution_id, producer_node_id, producer_port_id
fingerprint, created_at, availability, retention/pinned state
```

For `json`, payload is required and table locator fields are null; for `table`, the
locator is required and payload is null. A JSON null value must be distinguishable
from SQL NULL/missing payload. Record lineage through execution input bindings rather
than duplicating whole parent values. Enforce project-local references and identity
uniqueness. Physical table names are generated and safely quoted by core, never
accepted as arbitrary frontend/operation SQL identifiers.

An inventory JSON value is persisted data, not a surviving process object. Decode
it on demand into runtime memory. Define schema-aware round trips for timestamps,
large integers and non-finite numbers; reject unsupported values rather than
silently changing them. Keep large row sets and spectra arrays table-backed. Do not
serialize entire feature tables into JSON merely because an operation returns data.

## Implementation sequence

1. Locate the project DDL/create/open code and implement inventory persistence there.
   Add version checks; never open a pre-refactor DB and silently reinterpret old
   global tables as graph artifacts. Use disposable new fixtures for this development
   cutover; do not alter user project files to make tests pass.
2. Extend generic host access in `cpp/sdk/include/streamfind/sdk/plugin_host_access.hpp`,
   `plugin_data_service.hpp` and `cpp/sdk/src/plugin_host_access.cpp`. Give executors
   read-only input handles resolved by port and output writers allocated by runtime.
   Catalogue table contracts describe schemas, not singleton physical table names.
3. Trace `dynamic_plugin_manager` and plugin ABI ownership before changing callbacks.
   Update host/plugin ABI version and all loaded C++ plugins atomically where needed;
   reject mismatched versions, do not install forwarding or compatibility shims.
4. Make output publication transactional: create/write private outputs, validate all
   schemas, insert inventory rows and execution bindings, then commit them together.
   Failure/cancellation rolls back unpublished tables and metadata. Published inputs
   are immutable; never grant a downstream writer access to an input table.
5. Add core artifact list/describe/read APIs with bounded JSON/row previews and
   pagination. Lookup by artifact ID, not a semantic type's global table name.
6. Implement retention references from saved bindings, run history and pins. Garbage
   collection removes only unreferenced artifacts and their owned tables. Removing a
   cache entry must not delete a still-referenced artifact. Do not drop input tables
   on node deletion or project disconnection.

## Acceptance gate

* [ ] New project has no scientific tables/source links; entry execution creates them.
* [ ] Two artifacts of the same table contract coexist and return different rows.
* [ ] JSON and table artifacts survive close/reopen; JSON null round-trips correctly.
* [ ] Cross-project/stale/missing artifact references and mismatched schemas fail.
* [ ] Failed multi-output execution leaves no partially published inventory/output.
* [ ] SDK/core contain no plugin-specific columns, contracts or branching.

---

# 6C. Phase 3C — Unified operation executor, DAG scheduler and cache

**Status: TODO. Depends on Phases 3A–3B.**

1. Move validation, processing callbacks, cooperative cancellation/progress,
   provenance and caching from the method execution path into the operation executor.
   Replace `Workflow.steps` and method-registry dispatch at their owning boundary.
   There must be one execution path, not an operation wrapper calling a retained
   method engine. Update all affected native registrations/callers in the cutover.
2. Validate node/port IDs, parameter schemas/defaults, cardinality, semantic types,
   conditional inputs and entry roots. Detect cycles and dangling edges before any
   write. Derive a deterministic topological schedule (stable node-ID tie break).
3. Initially serialize node execution and project writes; allow different projects
   to run independently. Freeze the graph revision and input bindings for each run.
   Reject conflicting save requests via expected revision; never mutate a running
   snapshot. Graph branching does not imply parallel writes are safe.
4. Implement `run_node` (requires available current inputs), `run_to_node` (ancestor
   subgraph), `run_workflow` (all components) and explicit force recompute as modes of
   the same scheduler. Missing inputs fail clearly rather than reading project-global
   data or implicitly rerunning an import. Failed parents block descendants while
   recording why; use fail-fast scheduling initially and retain completed outputs.
5. Keep definition validity, execution status and output freshness separate. Parameter
   or edge edits stale only affected descendants; old outputs remain inspectable by
   execution ID. Renaming/repositioning a node does not invalidate scientific results.
6. Compute cache keys from operation/plugin implementation and schema versions,
   canonical parameters with defaults, named/ordered input fingerprints and relevant
   execution settings. File-backed entries require content-aware source fingerprints
   (including directory contents); path alone is insufficient. If a trustworthy
   fingerprint is unavailable, disable reuse rather than claim a cache hit.
7. Use `sf:cacheable` for eligibility. Cache values are manifests mapping output ports
   to artifact IDs. Verify every artifact exists and is compatible on hit; otherwise
   recompute. Never duplicate JSON/table payloads in a second cache store. Outputs
   backed by external raw files must retain validated source identity/provenance.
8. Persist run/node events and final output bindings. Cancellation cannot publish
   half a node; handle cancellation before commit or completed publication explicitly.
   Report interrupted runs after service restart; no automatic resume claim yet.

## Acceptance gate

* [ ] Headless graph with two roots, a multi-input node and two branches executes.
* [ ] Cycle/type/missing-input failures occur before mutation; two same-type inputs
  bind to the intended separate artifacts, independent of table creation order.
* [ ] Unchanged cacheable node reuses artifact IDs; changed parameters invalidate only
  descendants; layout changes do not recompute; force recompute bypasses reuse.
* [ ] Changed source files, missing cached outputs and implementation version changes
  invalidate reuse. Cancelled/failed runs never register successful cache entries.
* [ ] Run node/to-node/full graph share validation and publication behavior.

---

# 6D. Phase 3D — Port domain processing, chromatograms first

**Status: algorithm sources imported into this worktree; operation/artifact conversion
TODO. Importing algorithms is not this gate.**

## Upstream algorithm baseline

Local `dev_refactoring` contains `04a61f0b` (persisted chromatograms/native peaks) and
`5c176cc4` (baseline correction/smoothing). Their algorithm/source changes are now
present in this worktree's uncommitted state and compile in the current plugin path.
They are deliberately not called “ported to operations” yet: current registration
still exposes the processing callbacks through the legacy method boundary until
Phases 3A–3C provide named ports, artifact handles and the unified executor. Do not
claim the new framework conversion from this import alone. Import missing changes without replacing local `project_entry`,
picker metadata, service work or generic SDK fixes. Do not merge the branch blindly
or cherry-pick commits over dirty overlapping files. Review its processing plan as
algorithm evidence, not as authority to retain methods in the new architecture.

Primary conversion owners:

* `cpp/plugins/mass_spec/src/methods/chromatograms_processing_methods.cpp/.hpp`
* `cpp/plugins/mass_spec/src/utils/chromatogram_peaks.cpp/.hpp`
* `cpp/plugins/mass_spec/src/operations/chromatograms.cpp`, `operations.hpp`, `base.cpp`
* `cpp/plugins/mass_spec/src/plugin_entrypoint.cpp` and plugin `CMakeLists.txt`
* `cpp/plugins/mass_spec/semantic/{methods,operations,parameters,tables,columns,results}.ttl`

Inspect real registrations and canonical table/column identifiers before editing.
These conceptual port labels must map to the existing semantic contracts; do not
invent aliases for established table/column names:

| Operation | Required data inputs | Output artifacts |
| --- | --- | --- |
| `add_analyses` | None; file/replicate/blank parameters | Actual declared analyses/header datasets |
| `load_chromatograms` | Analyses and any headers the implementation reads | Persisted chromatogram point dataset and other actual writes |
| `correct_chromatogram_baseline` | Chromatogram points | New corrected chromatogram dataset |
| `smooth_chromatograms` | Chromatogram points | New smoothed chromatogram dataset |
| `find_chromatogram_peaks` | Processed chromatogram points and any additional actual reads | Peak dataset |
| `find_features` | Analyses and spectra headers, plus audited conditional dependencies | Feature dataset |
| `filter_features` | Feature dataset | Distinct filtered feature dataset |

Do not claim `add_analyses` materializes spectra/chromatogram points until its reader
path does so. Headers and points are different contracts. If an intended output is
missing, implement the producer at its domain boundary or expose the required load
operation explicitly; never advertise an empty/nonexistent table as successful data.

## Porting recipe (repeat for every processing capability)

1. Capture parameters/defaults, numeric output and declared/actual reads/writes in a
   small deterministic regression fixture before moving the dispatch code.
2. Move each capability declaration from `sf:Method` to `sf:Operation`, preserving
   canonical IDs where possible, defaults and `sf:cacheable`. Add precise named ports.
   Remove its old method declaration/registration in the same change.
3. Move executor plumbing into the plugin's operation owner; preserve utility
   algorithms. Read from the supplied input handle and write a runtime-allocated
   output table. Remove hard-coded singleton table lookup and UPDATE-on-input paths.
   Do not leave forwarding method files, duplicate implementations or aliases.
4. Preserve the chain `load → baseline → smooth → find_peaks`. Baseline/smoothing
   consume the preceding `intensity`; retain `raw_intensity` for provenance. Peak
   finding reads processed `intensity`, not `raw_intensity`. Preserve imported ALS
   first-difference penalty and auto-scaled lambda; do not rewrite numerical logic.
   Preserve RT matching tolerance (0.01 s) rather than float-string equality.
5. Match catalogue-normalized parameter names in C++ (`baseline_algorithm`,
   `window_size`, etc.); verify defaults from imported declarations and R reference
   without modifying R. Test defaults and non-defaults, not only capability presence.
6. Convert all remaining active domain methods and extraction operations to bindings.
   Raman/Sensors must compile against the same generic interface. No plugin-aware
   SDK exception is allowed. Future live sensor clients require finite acquisition
   batches first; continuous-stream scheduling is explicitly outside this DAG slice.

## Acceptance gate

* [ ] Native headless chromatogram chain produces distinct baseline/smoothed/peak
  artifacts, preserves the loaded table and uses processed intensity for peaks.
* [ ] Two smoothing parameter branches coexist; rerunning one leaves the other and
  all upstream rows unchanged. A cache hit reuses the correct output artifacts.
* [ ] Numeric regression matches imported behavior within justified tolerances.
* [ ] Separate ungrouped plots show raw, baseline-corrected, smoothed and detected
  peak results for user review before analytical algorithm changes proceed.
* [ ] Feature detection's multi-input graph and two filter branches retain inspectable
  original/filtered tables. If an operation is not implemented, mark this gate blocked;
  never stub plausible data to pass it.
* [ ] No active method registry/declarations or global scientific-table fallback
  remains in migrated native execution. Deferred R/integration code is untouched.

---

# 6E. Phase 3E — Public graph service and end-to-end backend gate

**Status: TODO. Depends on Phases 3A–3D.**

1. Update core public API, CLI/MCP handlers and service dispatch to use the same graph
   schema/executor; do not create service-only graph persistence. Inspect actual
   operation-discovery schemas and nested input schemas before replacing method tools.
2. In `cpp/service/src/project_runtime_manager.cpp` and its header, replace ordered
   method execution with graph runs while preserving worker ownership, safe join on
   close, cooperative cancellation and independent project sessions. Update
   `service_protocol.hpp`, `service_server.cpp` and `service_plugin_runtime.cpp`.
3. Implement the graph/artifact routes in section 6.0. Run requests carry mode, optional
   target node, expected definition revision and force-recompute flag. Replies carry
   run identity, node statuses, output bindings and structured diagnostics. Existing
   direct computational operation invocation must resolve the same bindings and
   executor; do not leave an untracked table-mutating bypass.
4. Version typed HTTP/WebSocket DTOs. Events identify project, workflow revision, run,
   node execution and artifact outputs. Use monotonic event sequence IDs; snapshot and
   sequence-based reconnect must recover missed events without duplicating work.
5. Add bounded artifact schema/preview/row retrieval with validated filters and sort
   columns. Do not expose arbitrary SQL or trust client-provided physical locators.
6. Only then update `frontend/src/backend/protocol.ts` and
   `StreamFindApiClient.ts`. Reject incompatible service versions visibly rather than
   masking them with frontend defaults or a legacy transport.

## Backend-first acceptance fixture

Create a disposable project under `tmp/projects/` without a browser. Save and run an
entry-rooted graph, branch processed tables, produce one JSON count/summary, then
close/reopen. Verify graph revision, inventory, JSON payload, both table branches,
lineage and cache reuse. Run through the public service as well as core tests. Add
negative tests for cycles, missing inputs, invalid ports, stale revisions, cancellation
and cache references to deleted/unavailable artifacts. Browser rendering is not proof
of these properties.

## Verification instructions for each implementation slice

* Read the owning definitions and usages; write a focused failing C++ test, implement,
  run it, then run the affected native target and official C++ test gate. Rust is
  intentionally stale for this operation-framework slice; do not modify it and keep
  the exception documented until a later approved migration.
* Use `.venv/Scripts/python.exe` for repository Python and verify its executable first.
  Build/test/log outputs belong in worktree `tmp/`, not main checkout or system temp.
* Regenerate catalogue using the configured native build target
  `streamfind_aggregate_catalogue`. This worktree has used `tmp/build/core-default`
  and `tmp/build/core-static-runtime`; inspect their CMake configuration before reuse.
  Build affected plugins and `streamfind_service`; use the official build wrapper
  if compiler environment initialization is required. Capture logs in `tmp/logs/`.
* After changing runtime/catalogue, restart the managed service and read `/session`
  and exact `/capabilities` fields. A stale running binary is not validation.
* Run frontend checks in this worktree's `frontend/`: `npm run format:check`,
  `npm run lint`, `npm run test:run`, `npm run build`, then live browser validation
  and `npm run browser:smoke` when the service/UI are available. Do not claim browser
  coverage from unit tests if browser tooling is unavailable.
* End each slice with `git diff --check`, a main-checkout cleanliness check and a
  completed/remaining acceptance list. Do not mark an entire phase complete after
  merely finishing a proposed subtask or compiling one target.

---

# 7. Phase 4 — Backend-owned operation and artifact canvas

**Status: interaction scaffold exists; graph integration TODO after Phases 3A–3E.**

## Goal

Create workflows using the same ontology-driven concept used by CogniFlow, but backed by StreamFind capabilities.

CogniFlow already uses semantic step metadata, typed inputs/outputs, dynamic parameter editors, connections, viewport state, and node metadata. Reuse these interaction patterns.

## Tasks

### 7.1 Operation definition contract

Frontend receives the canonical normalized Phase 3A operation contract. The sketch
below shows required concepts; import matching DTOs from `protocol.ts`, not a second
component-local capability schema:

```ts
OperationDefinition {
  id: string;
  label: string;
  description?: string;
  domain: string;
  category?: string;

  parameters: ParameterDefinition[];

  inputs: PortDefinition[];
  outputs: PortDefinition[];
  project_entry: boolean;
  cacheable: boolean;

  ui?: OperationUiMetadata;
}
```

### 7.2 Operation library

The palette groups operations by semantic metadata. Remove the method node class
and method-only picker. Entry operations expose parameters and output ports; add
their initial instances once through the backend project/workflow creation path,
not on each React mount. Adding another permitted entry is an explicit graph edit.

Example:

```text
Input
Processing
Filtering
Screening
Output
```

Categories originate from plugin metadata.

### 7.3 Canvas

Support:

* pan;
* zoom;
* node drag;
* node selection;
* connection rendering;
* canvas minimap later if needed.

Reuse the interaction model from CogniFlow rather than copying its ontology format.

### 7.4 Workflow nodes

Each node displays:

```text
icon
operation label
status
input contracts
output contracts
```

Clicking selects node and opens inspector.

Resolve output bindings to artifact cards/previews with semantic type, storage kind,
availability, cache-hit provenance and execution ID. Table outputs of the same type
must remain distinguishable by producer and execution. Never display an old output
as current after a parameter edit. Artifacts are values, not executable table nodes.

### 7.5 Parameter rendering

Generic parameter component mapping:

```text
boolean → switch
integer → number input
float → number input
enum → select
string → text input
multiline → textarea
table → table selector
column → column selector
file → file selector
directory → directory selector
```

Respect:

* minimum;
* maximum;
* allowed values;
* required;
* defaults.

### 7.6 Workflow validation

Debounce backend validation.

Frontend should show:

```text
valid
warning
invalid
```

But backend remains authoritative.

### 7.7 Persist workflow

Use the new backend graph contract from Phases 3A–3E, not the old ordered list.
`CanvasShell.tsx` owns interaction only; load/save via `StreamFindApiClient`, preserving
server node IDs, named edges, revision and artifact bindings. UI edits may be local
drafts, but save/validation responses remain authoritative. Show revision conflicts
and restore a rejected connection instead of silently overwriting backend edits.

Support:

```text
load
edit
validate
save
run
```

### 7.8 Node UI state

Persist frontend-only metadata separately from scientific workflow semantics when needed:

```text
x
y
expanded
selected
```

Do not let layout coordinates influence workflow execution.

## Acceptance criteria

* Operation palette is generated entirely from backend capability metadata.
* Nodes can be added to the canvas.
* Parameters are generated automatically.
* Workflow can be validated by the backend.
* Workflow can be persisted into the project.
* Reloading a project reconstructs the workflow.
* No domain-specific workflow logic exists in generic canvas components.
* Backend-authored graphs render identically in Workflow and Explorer with the same
  node IDs/bindings. Closing/reopening does not duplicate entry nodes or rerun them.
* Entry → multi-input processing → branching transforms works through typed ports;
  incompatible connections display the backend's structured reason.
* Run node, run to node, run workflow and force recompute use the same service runtime.
* JSON outputs and original/filtered table outputs can be inspected after reload.
* Plain wheel/two-finger pan and Ctrl/Cmd-wheel/pinch zoom retain current behavior;
  pinch never zooms the browser page when the pointer is over the canvas.

---

# 8. Phase 5 — Durable execution lifecycle, workers, and live events

**Status: basic worker/cancellation slices exist; durable graph recovery TODO.**
**Prerequisites:** Phases 3A–3E and the revised Phase 4; do not rebuild a second
scheduler here. Extend the operation-graph executor and events already introduced.

## Goal

Represent backend-owned long-running workflow execution reliably in the UI.

## Tasks

### 8.1 Final execution state model

Use durable states such as:

```text
draft
validated
queued
starting
running
cancelling
cancelled
completed
failed
interrupted
```

### 8.2 Worker identity

Persist or expose:

```text
worker_id
process_id
started_at
heartbeat_at
status
```

### 8.3 Project concurrency

Rules:

```text
one active workflow execution per project
multiple projects may execute concurrently
one worker may own one active project execution
```

### 8.4 Execution events

Stream:

```text
workflow.queued
workflow.started
workflow.progress
workflow.completed
workflow.failed
workflow.cancelled

workflow.node.started
workflow.node.progress
workflow.node.completed
workflow.node.failed
workflow.node.cache_hit
artifact.published

worker.started
worker.heartbeat
worker.stopped
worker.failed
```

### 8.5 Canvas integration

Node execution indicators:

```text
○ idle
◌ queued
◉ running
✓ completed
✕ failed
⊘ cancelled
```

Active node:

```text
Find Features
████████░░ 82%
34 s
```

### 8.6 Runs page

Create durable execution history view.

Example:

```text
Run #21
Completed
Started 10:42
Finished 10:48
Worker sf-worker-2
PID 18342
```

Expand into node-execution history with immutable graph revision, normalized
parameters, exact input/output artifact IDs and cache provenance. Distinguish
historical outputs from the latest valid bindings; do not flatten branches into a
misleading step-order narrative.

### 8.7 Global Worker Monitor

Application-level panel:

```text
Project A    Running     Worker 1
Project B    Running     Worker 2
Project C    Idle
```

### 8.8 Reconnection

When the browser reconnects:

```text
retrieve active projects
retrieve active executions
retrieve latest execution state
subscribe again
```

Workflow execution must continue even if the browser is closed.

## Acceptance criteria

* Browser disconnection does not stop workflows.
* Multiple projects can execute concurrently.
* Workflow state is reconstructed after reconnect.
* Worker PID/identity is visible.
* Canvas nodes reflect live execution.
* Execution history survives application restart.
* Restart marks unfinished runs interrupted; outputs committed before the crash
  remain inspectable and unpublished outputs are cleaned safely. Resume is not
  implied by reconnect or by the existence of cached outputs.
* Event replay/snapshot reconciliation preserves graph revision and node identity;
  a late event from an older run never overwrites current-run status.
* Cooperative cancellation is verified during a real processing node, including
  rollback/publication boundaries; Pause remains unsupported until separately built.

---

# 9. Phase 6 — Generic table and data explorer

**Updated dependency:** inspect Phase 3B artifact inventory, not a singleton table
list. All table services below resolve an artifact ID to an authorized table handle.
List JSON outputs alongside tables; use bounded payload previews, pagination and
producer/run labels. Multiple artifacts of the same semantic type are expected.
Administrative metadata (`PROJECT`, audit, workflow definitions and execution
records) remains a separate read-only administration surface; it is not an operation
artifact and is not addressed by artifact-table locators.

## Goal

Provide a domain-independent way to inspect every project-owned table.

## Tasks

### 9.1 Generic backend artifact services

Expose safe operations:

```text
artifact.list
artifact.describe
artifact.query
artifact.count
artifact.distinct
artifact.export
```

Avoid arbitrary SQL as the normal UI contract.

### 9.2 Table list

Show:

```text
core tables
plugin-owned tables
```

Example:

```text
PROJECT
WORKFLOW_EXECUTION
WORKFLOW_EXECUTION_STEP
AUDIT_TRAIL

ANALYSES
FEATURES
SUSPECTS
...
```

### 9.3 Table page

Tabs:

```text
Data
Schema
Visualization
Provenance
```

### 9.4 Data grid

Support backend-side:

* pagination;
* sorting;
* filtering;
* column selection;
* search.

Avoid loading large tables into browser memory.

### 9.5 Schema inspection

Display:

```text
column
datatype
nullable
semantic meaning
unit
role
description
```

### 9.6 Table provenance

Show:

```text
owner plugin
node execution that produced this immutable artifact
execution
timestamp
```

## Acceptance criteria

* Any registered table can be inspected without domain-specific frontend code.
* Large tables are paginated server-side.
* Plugin tables appear dynamically.
* Column metadata comes from table contracts.
* Generic data explorer works for MassSpec, Raman, and Sensors tables.

---

# 10. Phase 7 — VisualizationSpec runtime and MassSpec visualization operations

## Goal

Introduce an extensible scientific visualization layer with one strict responsibility split:

```text
typed scientific artifact
        ↓
backend visualization operation
        ↓
sfvis:VisualizationSpec JSON artifact
        ↓
generic client visualization runtime
        ↓
Plotly.js or registered declarative D3 renderer
```

The backend decides what the scientific plot means. The client decides how a validated visualization specification is
drawn. Section 22 is authoritative for the schema, lifecycle, MCP interoperability, data binding, security and testing
rules used in this phase.

## Tasks

### 10.1 ResultRenderer and VisualizationRegistry

Keep the generic result registry for tables, metrics, JSON and visualization artifacts:

```ts
ResultRendererRegistry.register("core.table", TableRenderer);
ResultRendererRegistry.register("core.metric", MetricRenderer);
ResultRendererRegistry.register("core.json", JsonRenderer);
ResultRendererRegistry.register("core.visualization", VisualizationRenderer);
```

A result with semantic contract `sfvis:VisualizationSpec` resolves to `core.visualization`. The
`VisualizationRenderer` validates the spec envelope and delegates to a renderer registered by
`renderer.renderer_id + renderer.spec_version`.

Initial visualization renderer IDs:

```text
core.plotly
d3.force_network
d3.provenance_graph       # later, only if needed
```

Do not register separate React plot renderers such as `mass_spec.chromatogram`, `mass_spec.spectrum`, or
`mass_spec.feature_map` when those views are expressible as Plotly specs. Their MassSpec semantics belong in backend
visualization operations, not in frontend branching.

### 10.2 Generic result contract

```ts
interface ResultRendererProps {
  projectSessionId: string;
  artifact: ArtifactDescriptor;
  query: QueryState;
  metadata: RendererMetadata;
}
```

`ArtifactDescriptor` discriminates `storage_kind: 'json' | 'table'` and carries semantic contract, schema,
availability, producer/run identity and lineage. The visualization renderer obtains the bounded JSON spec through the
typed artifact API. Artifact-backed datasets declared by the spec are resolved through the dedicated visualization-data
API described in Section 22.

Renderers never receive DuckDB connections, physical table names, arbitrary SQL, or native implementation objects.

### 10.3 Backend MassSpec visualization operations

Implement scientific views as normal operations with typed inputs and an
`sfvis:VisualizationSpec` output. Initial operations should map closely to the proven R/Plotly behavior in
`bindings/r/R`, while using exact current C++ artifact contracts.

Recommended first set:

```text
mass_spec.plot_chromatogram
mass_spec.plot_spectrum
mass_spec.plot_feature_map
mass_spec.plot_feature_profile
mass_spec.plot_feature_count
mass_spec.plot_heatmap
mass_spec.plot_mirrored_ms2
```

Later candidates:

```text
mass_spec.plot_fold_change
mass_spec.plot_3d_surface
mass_spec.plot_transformation_network
```

Each operation is responsible for scientific selection, filtering, grouping, aggregation, units, labels, hover fields,
trace grouping and appropriate downsampling policy. It must not embed StreamFind light/dark theme colors or executable
browser code.

### 10.4 Plotly as the primary scientific renderer

Use Plotly.js for views that map naturally to declarative traces. Preserve analytical behavior from the legacy R package
rather than recreating every chart in custom D3 code.

Expected mappings include:

```text
chromatograms / EIC / TIC / BPC  -> scattergl lines
feature chromatograms            -> lines + peak-region fill
feature maps                     -> scattergl
MS1 / MS2 spectra                -> scattergl stick traces
mirrored MS2                     -> positive/negative stick traces
feature counts                   -> bar/scatter
feature profiles                 -> lines + markers/error bars
binned data                      -> heatmap/heatmapgl
3D binned surfaces               -> surface
fold-change                      -> scatter
```

Start with `scattergl`/WebGL where the R implementation already relies on it or where point counts justify it. Do not
use D3 merely to avoid Plotly; use D3 only for visualization structures Plotly does not represent cleanly.

### 10.5 Declarative D3 renderers

D3 support is renderer-specific and declarative. Never send executable D3/JavaScript from C++, plugin code, DuckDB, or
MCP tool results.

A D3 specification names a known renderer and supplies data/configuration validated against that renderer's schema, for
example:

```text
renderer_id: d3.force_network
payload:
  nodes[]
  links[]
  node_encoding
  link_encoding
```

The frontend owns the implementation of `d3.force_network`. Backend operations own the scientific meaning of nodes,
links and encodings.

### 10.6 Shared selections and interactions

The visualization runtime exposes normalized local interaction events:

```ts
type VisualizationInteraction =
  | { type: "select"; visualizationId: string; keys: string[] }
  | { type: "range"; visualizationId: string; axis: "x" | "y"; min: number; max: number }
  | { type: "hover"; visualizationId: string; key?: string };
```

Backend operations should place stable scientific identifiers such as `feature_id`, `analysis_id`, spectrum ID, or
chromatogram ID in safe trace metadata/custom-data fields so clients can link views without parsing hover text.

Shared selection model:

```text
Feature map
    ↓ feature_id
Chromatogram
Spectrum
Metadata
Compound evidence
```

Selections are client/session state unless an explicit workflow operation persists them. Zooming or clicking a chart must
not silently mutate project data.

### 10.7 Non-plot detail renderers

Tables, molecular structures, compound evidence cards and other rich result details may remain normal result renderers
when they are not plots. Examples include:

```text
mass_spec.suspect_table
mass_spec.compound_match
core.table
core.json
```

Do not force every result into `VisualizationSpec`.

## Acceptance criteria

* A backend visualization operation publishes a validated immutable `sfvis:VisualizationSpec` artifact.
* The React result explorer renders that artifact through `core.visualization` with no MassSpec-specific branching.
* Plotly is the default scientific plotting engine and matches the analytical meaning of the corresponding R plots.
* D3 receives only validated declarative payloads for explicitly registered renderer IDs.
* Scientific inputs remain unchanged after plot-spec generation.
* Theme selection is applied by the client runtime rather than baked into the backend artifact.
* Stable datum identifiers support linked feature/chromatogram/spectrum selections.
* JSON/table fallback renderers remain available for non-visual or unsupported results.

---

# 11. Phase 8 — Port useful NTA interactions from Shiny

## Goal

Reproduce the useful analytical behavior of the existing R/Shiny application while replacing its implementation with reusable React components.

The current Shiny UI already contains feature plots, NTA summary views, filtering controls, detail panes, suspect tables, chromatogram views, spectrum views, and MS2-related views. Preserve those analytical interaction patterns rather than porting the implementation literally.

## Tasks

### 11.1 NTA results workspace

Tabs:

```text
Summary
Features
Suspects
Internal Standards
```

### 11.2 Summary

Provide:

* number of analyses;
* number of features;
* feature distribution;
* analysis/group overview;
* filtering status;
* workflow completion status.

### 11.3 Feature explorer

Layout:

```text
Filters | Feature Map | Feature Details
```

Filters:

* analysis;
* group;
* intensity;
* m/z;
* retention time;
* annotations;
* flags.

### 11.4 Feature details

Compose existing reusable components:

```text
Feature metadata
ChromatogramViewer
SpectrumViewer
MS2Viewer
Annotations
```

### 11.5 Suspect explorer

Display:

```text
structure
compound name
formula
score
feature
chromatographic evidence
MS2 evidence
fragment similarity
```

### 11.6 Internal standards

Provide:

* standards table;
* status;
* feature match;
* expected vs observed values;
* analytical diagnostics.

### 11.7 Export

Allow controlled exports:

```text
selected rows
filtered table
feature list
suspect list
```

Backend performs data export where practical.

## Acceptance criteria

* Core Shiny NTA workflows are available in React.
* Shared visualization components are used instead of NTA-specific plot implementations.
* Selected feature links all relevant views.
* Result state is project-scoped.
* NTA views remain thin compositions of reusable components.

---

# 12. Phase 9 — Plugin UI metadata contract

## Goal

Allow plugins to influence workflow and result presentation without changing generic frontend code.

## Tasks

### 12.1 Extend semantic UI vocabulary

Introduce a small UI metadata vocabulary, for example:

```text
sfui:preferredVisualizationOperation
sfui:preferredRenderer
sfui:detailsRenderer
sfui:icon
sfui:category
sfui:xRole
sfui:yRole
sfui:colorRole
sfui:sizeRole
sfui:defaultColumns
```

### 12.2 Keep ontology authoritative

Example concept:

```text
Features table
  preferredVisualizationOperation → mass_spec.plot_feature_map
  xRole → retention_time
  yRole → mz
  colorRole → intensity

mass_spec.plot_feature_map
  output semantic contract → sfvis:VisualizationSpec
  default renderer → core.plotly
```

The backend catalogue generator exposes normalized UI metadata. Roles such as `xRole` and `yRole` are scientific
metadata/defaults available to visualization operations and inspectors; React must not use them to independently
reconstruct a scientific Plotly trace when a visualization operation exists.

React does not interpret Turtle.

### 12.3 Plugin manifest UI assets

Allow optional plugin assets:

```text
plugin/
├── plugin.json
├── catalogue.duckdb
├── semantic/
├── shared library
└── ui/
    ├── icons/
    └── assets/
```

Do not duplicate operation/port/artifact semantics in an additional JSON configuration.

### 12.4 Generic UI extension rules

A new plugin should be able to introduce:

* new domain;
* new processing operations;
* new parameters;
* new tables;
* icons;
* categories;
* preferred renderers.

Without modifying:

```text
App.tsx
WorkflowCanvas.tsx
ResultExplorer.tsx
TableExplorer.tsx
```

### 12.5 Custom JavaScript UI plugins

Defer arbitrary dynamically loaded JavaScript renderers.

Initial implementation should rely on known reusable StreamFind renderer components.

Future optional contract:

```text
ui_api_version
frontend bundle
trusted plugin loading
```

This should only be implemented when a concrete external plugin distribution requirement exists.

## Acceptance criteria

* UI metadata originates from semantic/plugin metadata.
* Adding metadata does not require frontend TypeScript edits.
* Renderer preference changes through metadata.
* Missing renderer automatically falls back to generic table view.
* No duplicated operation metadata exists in the frontend.

---

# 13. Phase 10 — Raman and Sensors genericity test

## Goal

Prove that the frontend architecture is truly domain-independent.

Do not treat this as only feature expansion. Treat it as an architectural acceptance test.

---

## 13.1 Raman test

Once Raman capabilities exist, verify that:

```text
Raman domain
↓
automatically appears in New Project
↓
Raman operations appear in the operation library
↓
parameters render automatically
↓
Raman tables appear in Data Explorer
↓
Raman results resolve through renderer registry
```

Expected renderer examples:

```text
raman.spectrum
raman.peak_table
raman.baseline_view
```

No changes should be required in generic project/workflow components.

---

## 13.2 Sensors test

Sensors provides a more important genericity test because its data model differs significantly from MassSpec.

Verify:

```text
Sensors project
↓
OPC UA / MQTT operations
↓
sensor ingest
↓
time-series tables
↓
processing workflow
↓
anomaly detection
↓
live result views
```

Potential renderers:

```text
sensors.timeseries
sensors.status_grid
sensors.anomaly_timeline
sensors.device_overview
```

### Live-data requirement

Ensure renderer infrastructure can support:

```text
static result artifact
and
successive immutable batch artifacts for live views
```

without changing core ResultExplorer behavior. A live view may refresh its binding to
the newest completed batch, but published artifacts remain immutable; do not update a
published table in place or invent an unbounded streaming executor in this DAG phase.

---

## Acceptance criteria

The frontend passes the architecture test only if:

* Raman can be added without editing generic workflow logic.
* Sensors can be added without editing generic workflow logic.
* Both domains appear through backend discovery.
* Their tables appear dynamically.
* Their result views resolve through the same renderer registry.
* Project creation automatically reflects installed domains.
* No central domain switch statement exists.

---

# 14. Cross-cutting provenance requirements

Provenance should be implemented progressively throughout all phases.

Every result should be traceable to:

```text
table / row
↓
workflow execution
↓
node execution and artifact bindings
↓
operation
↓
parameters
↓
plugin
↓
plugin version
↓
input artifacts (JSON values or tables)
```

Results should support an action such as:

```text
Produced by:
Find Features
Run #21
MassSpec plugin 0.2.0

[Show in workflow]
```

The existing project persistence already contains workflow execution and audit information, so this UI should consume those contracts instead of maintaining its own history.

---

# 15. State ownership rules

Maintain a strict separation between frontend state and backend state.

## Backend-owned

```text
project identity
domain
workflow definition
workflow validity
execution
worker ownership
progress
project tables
plugin capabilities
artifact inventory and lineage
node input/output bindings and cache manifests
audit trail
```

## Frontend-owned

```text
current route
selected project tab
canvas viewport
selected node
expanded panels
table column widths
current filters
theme
temporary unsaved input values
```

Frontend stores must never become the authoritative source for workflow execution.

---

# 16. Testing strategy

Each phase must add tests at the correct boundary.

## Frontend unit tests

Test:

* capability normalization;
* parameter renderer selection;
* workflow node generation;
* renderer resolution;
* project store;
* execution event reduction;
* theme selection.

## Component tests

Test:

* Project Hub;
* Operation Library;
* Workflow Node;
* Parameter Editor;
* Table Grid;
* execution status components;
* scientific renderers.

## Backend contract tests

Test:

* WebSocket initialization;
* capability discovery;
* multiple project sessions;
* project close/open;
* execution subscriptions;
* reconnect behavior;
* table query contract;
* plugin UI metadata.

## End-to-end tests

Minimum flows:

### Flow A — project creation

```text
launch
create MassSpec project
open workspace
verify domain
```

### Flow B — workflow

```text
open project
add operations and typed data connections
edit parameters
save
reload
verify workflow reconstructed
```

### Flow C — execution

```text
run workflow
receive live events
switch to another project
return
execution state remains correct
```

### Flow D — multi-project

```text
open A
open B
run A
run B
verify independent workers
```

### Flow E — results

```text
workflow completes
open FEATURES
feature map renders
select feature
linked details render
```

### Flow F — generic plugin expansion

```text
enable second domain plugin
restart host
domain automatically appears
no frontend code changes
```

---

# 17. Dependency order

Implementation dependencies:

```text
Phase 1
Frontend foundation
    │
    ▼
Phase 2
Backend connection
    │
    ▼
Phase 3
Projects
    │
    ▼
Phase 3A — operation/port/graph contracts
    │
    ▼
Phase 3B — artifact inventory and bound plugin I/O
    │
    ▼
Phase 3C — unified executor, DAG scheduling and cache
    │
    ▼
Phase 3D — domain operation conversion (chromatograms first)
    │
    ▼
Phase 3E — graph service and headless acceptance
    │
    ▼
Phase 4
Backend-owned operation/artifact canvas
    │
    ▼
Phase 5
Execution
    │
    ├───────────────┐
    ▼               ▼
Phase 6         Phase 7
Tables          Renderers
    │               │
    └───────┬───────┘
            ▼
         Phase 8
          NTA UI
            │
            ▼
         Phase 9
       Plugin UI contract
            │
            ▼
         Phase 10
    Raman / Sensors validation
```

Some work may proceed in parallel after Phase 5:

* generic data explorer;
* scientific renderer components;
* backend UI metadata design.

But the plugin UI metadata contract should be finalized only after real MassSpec renderers reveal what metadata is actually necessary.

---

# 18. Milestone gates

## Milestone A — usable shell

Includes:

```text
Phase 1
Phase 2
```

Success:

```text
StreamFind UI launches
backend connects
plugins discovered
theme works
```

---

## Milestone B — project client

Includes:

```text
Phase 3
```

Success:

```text
projects created/opened/closed
multiple projects active
```

---

## Milestone C — workflow editor

Includes:

```text
Phases 3A–3E
Phase 4
```

Success:

```text
backend-authored operation DAGs can be created, saved, run headlessly and rendered
JSON artifacts and distinct table outputs survive reopening and remain inspectable
```

---

## Milestone D — runnable application

Includes:

```text
Phase 5
```

Success:

```text
workflow execution survives browser sessions
multiple projects execute concurrently
```

---

## Milestone E — scientific application

Includes:

```text
Phase 6
Phase 7
Phase 8
```

Success:

```text
tables inspectable
MassSpec results rendered
NTA usable in React
```

---

## Milestone F — plugin-driven UI

Includes:

```text
Phase 9
```

Success:

```text
plugins control discoverable UI behavior through metadata
```

---

## Milestone G — architecture acceptance

Includes:

```text
Phase 10
```

Success:

```text
Raman and Sensors integrate without changes to generic frontend architecture
```

---

# 19. Definition of done for the frontend milestone

The StreamFind frontend milestone is complete when:

* React launches as the primary StreamFind graphical interface.
* Startup uses the StreamFind logo bootstrap animation.
* All capabilities are discovered dynamically.
* Multiple project databases can remain open simultaneously.
* Projects execute independently through durable backend workers.
* Workflows are created using ontology-derived operations and typed artifact ports.
* Parameters are generated automatically.
* Workflow validation is backend-driven.
* Execution progress is streamed live.
* Browser disconnect does not terminate processing.
* Project tables can be inspected generically.
* Result renderers are selected through a renderer registry.
* MassSpec NTA workflows have usable scientific visualizations.
* Plugins can declare UI metadata.
* Raman and Sensors can expand the application without modifying generic frontend components.
* React contains no direct DuckDB access.
* React contains no hard-coded central domain or operation inventory.
* Backend state remains authoritative.

---

# 20. Guiding architectural rule

The core rule for implementation is:

> **The React framework knows how to render StreamFind concepts. The backend and plugins declare which concepts exist, how they are validated, and how they should preferably be presented.**

This keeps:

```text
core generic
frontend generic
plugins domain-specific
semantics authoritative
execution backend-owned
visual components reusable
```

and allows StreamFind to grow from MassSpec into Raman, Sensors, and future domains without repeatedly redesigning the web application.

---

# 21. Optional domain entry operations

A domain may declare zero or more entry operation templates. Creating a project
creates metadata and the initial saved graph only, never scientific data. The generic
creation flow must support both cases:

```text
domain declares entry operation
    -> create metadata-only project and initial unexecuted entry node(s)
    -> open workspace immediately
    -> edit entry parameters in node
    -> explicitly run entry/node/subgraph; publish its typed outputs

domain declares no entry operation
    -> create project
    -> open the project workspace
    -> expose later domain setup through discovered operations/settings
```

Mass spectrometry and Raman may declare `mass_spec.add_analyses` and
`raman.add_analyses` respectively. Sensors may omit an entry operation because
sensor-network setup can require a separate configuration workflow after project
creation.

The catalogue marks entries, not a hard-coded frontend mapping. If absent, creation
still succeeds with an empty graph; a runnable data pipeline must eventually have
an available declared entry. The frontend renders parameters and output ports from
metadata. Entries have no upstream data inputs. Preserve file-versus-directory
metadata, including `.d` folders and native service pickers; inject `database_path`
from project context rather than making it an editable parameter.

An absent or unexecuted entry is not a failed project creation. A failed entry is a
failed node execution with diagnostics and no partial publication. Open restores
the saved graph and available artifacts without rerunning entries; Close disconnects
without deleting the DuckDB file. OPC UA/MQTT entries remain future capabilities,
not functionality to fake or special-case in the frontend.

---

# 22. VisualizationSpec, shared rendering runtime, and MCP interoperability

This section is the **authoritative visualization contract** for backend, SDK/plugin, service, React and MCP work.
Earlier references to “plot nodes”, renderer metadata, or Plotly specs must be interpreted according to this section.

The architectural rule is:

```text
scientific artifact(s)
        │
        ▼
backend visualization operation
        │
        ▼
immutable sfvis:VisualizationSpec artifact
        │
        ├──────────────────────────────┐
        ▼                              ▼
React application                    MCP server
        │                              │
        ▼                              ├── structured VisualizationSpec
shared visualization runtime           ├── MCP App/UI when host supports it
        │                              └── text/static fallback otherwise
   ┌────┴─────┐
   ▼          ▼
Plotly.js   registered D3 renderer
```

The C++ core and plugins never render a browser plot. They generate a validated StreamFind envelope whose payload is
renderer-specific. Plotly is the primary scientific renderer; D3 is available for a small number of registered
declarative structures.

## 22.1 Design goals

The visualization system must:

* make scientific plots normal workflow outputs with artifact identity, provenance and cache behavior;
* keep scientific data selection/aggregation in backend operations;
* keep browser rendering, theme and interaction implementation in the client;
* reproduce the useful analytical semantics of the legacy R/Plotly implementation under `bindings/r`;
* support Plotly.js without making raw Plotly JSON the entire StreamFind contract;
* permit selected D3 visualizations without transporting executable JavaScript;
* allow the same visualization artifact to be consumed by React, MCP-capable AI clients, tests and future bindings;
* scale from small inline plots to large artifact-backed datasets;
* remain versioned, validated and safe to deserialize;
* provide meaningful fallback information when an interactive renderer is unavailable.

The visualization system must not:

* expose DuckDB directly to React or MCP clients;
* make React infer scientific semantics from physical table columns;
* store executable JavaScript, HTML callbacks, function bodies or arbitrary event handlers in a plot artifact;
* bake light/dark mode or StreamFind UI theme colors into scientific backend operations;
* require Node, Chromium, Plotly or D3 inside the C++ core;
* duplicate a visualization-specific persistence system outside the normal artifact inventory.

## 22.2 VisualizationSpec semantic contract

Introduce a core semantic contract:

```text
sfvis:VisualizationSpec
```

It is normally persisted as a bounded JSON artifact and participates in the same immutable artifact inventory as any
other operation result.

Canonical envelope:

```json
{
  "schema": "streamfind.visualization/v1",
  "visualization_id": "vis_42",
  "semantic_type": "mass_spec.chromatogram",

  "renderer": {
    "engine": "plotly",
    "renderer_id": "core.plotly",
    "spec_version": "1"
  },

  "title": "Extracted ion chromatogram",

  "data_mode": "inline",

  "payload": {
    "data": [],
    "layout": {},
    "config": {}
  },

  "data_bindings": [],

  "interaction": {
    "selection_key": "feature_id"
  },

  "provenance": {
    "source_artifact_ids": ["artifact_91"],
    "producer_operation_id": "mass_spec.plot_chromatogram",
    "producer_node_id": "node_8",
    "workflow_revision": 12
  },

  "fallback": {
    "description": "EIC for m/z 301.071 ± 5 ppm from 0–18 min"
  }
}
```

Required top-level fields:

```text
schema
visualization_id
semantic_type
renderer.engine
renderer.renderer_id
renderer.spec_version
data_mode
payload and/or data_bindings
provenance.source_artifact_ids
fallback.description
```

The StreamFind envelope is stable across renderer families. The inner `payload` schema is validated according to
`renderer_id + spec_version`; a Plotly payload and a D3 network payload are not expected to be interchangeable.

## 22.3 Renderer descriptor and versioning

Renderer identity is explicit:

```text
RendererDescriptor
  engine
  renderer_id
  spec_version
```

Initial values:

```text
engine: plotly
renderer_id: core.plotly
spec_version: 1

engine: d3
renderer_id: d3.force_network
spec_version: 1
```

The client rejects unknown renderer IDs or unsupported spec versions with an actionable diagnostic and still shows the
fallback description/data where possible. Never silently reinterpret a v2 payload as v1.

Schema evolution rules:

* additive optional fields may remain within a compatible spec version only when old clients can safely ignore them;
* breaking payload changes require a new renderer `spec_version`;
* breaking envelope changes require a new `streamfind.visualization/vN` schema;
* persisted visualization artifacts keep the exact version that produced them;
* renderers support a small explicit compatibility range rather than “best effort” parsing.

## 22.4 Plotly payload

For `core.plotly`, payload is deliberately close to Plotly.js:

```json
{
  "data": [
    {
      "type": "scattergl",
      "mode": "lines",
      "x": [0.1, 0.2, 0.3],
      "y": [123, 415, 302],
      "name": "Sample 1",
      "customdata": [["analysis_1"], ["analysis_1"], ["analysis_1"]]
    }
  ],
  "layout": {
    "xaxis": { "title": { "text": "Retention time [min]" } },
    "yaxis": { "title": { "text": "Intensity" } }
  },
  "config": {
    "responsive": true
  }
}
```

The backend operation prepares the Plotly traces because it owns scientific grouping, units and defaults. The React
renderer stays thin: validate DTO -> apply client theme/safe defaults -> pass `data/layout/config` to Plotly.js.

Keep the allowed Plotly surface constrained. Backend validation rejects unsupported trace types, executable properties,
HTML/script injection, function-valued fields and unbounded arrays.

## 22.5 Declarative D3 payloads

D3 is not itself a portable plot-spec format. StreamFind therefore registers named D3 renderers with explicit schemas.

Example:

```json
{
  "schema": "streamfind.visualization/v1",
  "semantic_type": "mass_spec.transformation_network",
  "renderer": {
    "engine": "d3",
    "renderer_id": "d3.force_network",
    "spec_version": "1"
  },
  "data_mode": "inline",
  "payload": {
    "nodes": [
      { "id": "F1", "label": "m/z 301.071" },
      { "id": "F2", "label": "m/z 317.066" }
    ],
    "links": [
      { "source": "F1", "target": "F2", "type": "oxidation" }
    ],
    "node_encoding": {
      "size": "intensity",
      "label": "label"
    }
  }
}
```

No D3 callback, JavaScript source, DOM selector or function body is allowed in the artifact. The client implementation of
`d3.force_network` is trusted code shipped with the visualization runtime.

## 22.6 Inline and artifact-backed data modes

Support two data modes.

### Inline

Use for bounded plots whose renderer data is small enough to serialize, cache, inspect and send through MCP directly:

```text
data_mode: inline
payload contains renderer data arrays
```

### Artifact-backed

Use for dense chromatograms, feature maps, long sensor series, large heatmaps or other data that should not be copied
into a large JSON artifact:

```json
{
  "data_mode": "artifact",
  "payload": {
    "layout": {},
    "config": {}
  },
  "data_bindings": [
    {
      "binding_id": "chromatogram_points",
      "artifact_id": "artifact_982",
      "semantic_contract": "sfms:ChromatogramPoints",
      "columns": ["rt", "intensity", "analysis_id"],
      "query": {
        "filters": []
      },
      "sampling": {
        "method": "lttb",
        "max_points": 10000
      }
    }
  ]
}
```

Rules:

* the visualization operation chooses the binding, columns, filters, grouping and sampling policy;
* the client resolves only the declared binding through a typed service endpoint;
* the client never converts a binding into arbitrary SQL;
* the service verifies project/session authorization and that the requested artifact matches the published binding;
* server responses are bounded and may be chunked/streamed later without changing the visualization envelope;
* downsampling must preserve the analytical purpose of the plot and be declared in provenance/metadata;
* an MCP client may request a smaller bounded representation than the React app, but must not silently change scientific
  aggregation semantics.

Recommended service surface:

```text
visualization.get_spec(artifact_id)
visualization.get_data(visualization_artifact_id, binding_id, viewport?, limit?)
```

This may internally reuse generic artifact-query infrastructure, but visualization clients receive a constrained
visualization-data contract rather than arbitrary SQL capability.

## 22.7 Backend operation contract

Visualization preparation is a normal operation class, not a special side channel.

Conceptual operation:

```text
mass_spec.plot_chromatogram

inputs:
  chromatograms : sfms:ChromatogramResult

parameters:
  analyses
  chromatogram_type
  normalization
  rt_min
  rt_max

output:
  visualization : sfvis:VisualizationSpec
```

Conceptual C++ DTO:

```cpp
struct VisualizationSpec {
    std::string schema;
    std::string visualization_id;
    std::string semantic_type;
    RendererDescriptor renderer;
    std::string data_mode;
    nlohmann::json payload;
    nlohmann::json data_bindings;
    nlohmann::json interaction;
    nlohmann::json provenance;
    nlohmann::json fallback;
};
```

Exact ABI representation follows the existing SDK opaque-buffer/C-ABI rules. Do not expose C++ STL/JSON types across a
binary plugin boundary merely because the conceptual DTO uses them here.

Publication rules:

* resolve exact input artifact IDs from node bindings;
* build the visualization spec from immutable inputs;
* validate envelope + renderer payload + referenced bindings;
* publish the JSON artifact atomically;
* record source artifact lineage and producing node/run;
* on validation, cancellation or operation failure publish no partial visualization artifact.

## 22.8 Provenance, fingerprinting and cache behavior

Visualization artifacts use the normal artifact lifecycle.

A visualization fingerprint/cache key includes at least:

```text
producer operation/plugin/version
VisualizationSpec envelope version
renderer_id + renderer spec_version
normalized plot-operation parameters
ordered named source artifact fingerprints
scientifically relevant sampling/aggregation settings
```

Theme, browser width, current zoom, hover state and other presentation-only client state must not invalidate the backend
scientific visualization artifact.

Artifact provenance makes it possible to answer:

* which workflow node produced this plot;
* which source artifacts it visualizes;
* which operation parameters were used;
* whether downsampling/aggregation occurred;
* which renderer contract/version is required.

## 22.9 Validation and security

Validate before publication and again at the trust boundary where appropriate.

Minimum validation:

* envelope schema/version is known;
* `renderer_id + spec_version` is registered;
* all source artifact IDs exist and belong to the resolved project/run context;
* artifact-backed binding IDs are unique and authorized;
* Plotly trace arrays have compatible lengths;
* numeric arrays contain finite values or explicit nulls according to schema;
* D3 node/link references are internally consistent;
* units/labels required by the operation contract are present;
* payload bytes, trace count, point count, text length and nested depth are bounded;
* no arbitrary JavaScript, HTML callbacks, event-handler source, DOM selectors or executable expressions are accepted;
* URLs/resources, if ever added, require a separate allowlisted resource contract rather than arbitrary renderer fields.

Invalid specs are operation failures with diagnostics, not “best effort” plots.

## 22.10 Theme and presentation authority

Backend visualization operations emit semantic presentation information:

```text
axis meaning
axis label
unit
series identity
legend meaning
annotation meaning
scientific color category/role where necessary
```

They do not emit UI-theme decisions such as:

```text
darkMode
application background
global font
grid color
StreamFind panel color
selection highlight color
```

The shared visualization runtime applies those from StreamFind theme tokens. This allows the same persisted spec to render
in light/dark mode, the React app, an MCP App, static export, or another future client without recomputation.

When a scientific convention truly requires a color identity, encode it as semantic metadata and document it separately
from UI-theme styling.

## 22.11 Shared visualization runtime

Keep one implementation of StreamFind visualization rendering.

Target internal package:

```text
frontend/
├── src/
│   └── visualization/                 # app integration
└── packages/
    └── visualization-runtime/
        ├── src/
        │   ├── VisualizationSpec.ts
        │   ├── VisualizationRenderer.tsx
        │   ├── VisualizationRegistry.ts
        │   ├── VisualizationDataResolver.ts
        │   ├── interactions.ts
        │   ├── plotly/
        │   │   ├── PlotlyRenderer.tsx
        │   │   └── plotlyAdapter.ts
        │   └── d3/
        │       ├── D3RendererRegistry.ts
        │       └── ForceNetworkRenderer.tsx
        └── schemas/
            └── visualization-v1.schema.json
```

The main React app consumes this package. An MCP visualization UI/app consumes the same package rather than
reimplementing chart semantics.

If introducing a workspace package is premature, first implement these boundaries under `frontend/src/visualization`
but keep imports/API boundaries extraction-ready. Do not fork renderer code for MCP.

## 22.12 MCP transport contract

Expose visualization results to MCP as structured data first.

Recommended MCP-facing tools/resources:

```text
streamfind.get_visualization
streamfind.get_visualization_data
```

A successful result exposes the bounded `VisualizationSpec` as structured content so an AI agent can inspect semantic
type, axes, series, labels, provenance and fallback description even when it cannot render the chart.

For artifact-backed plots, the MCP layer may expose bounded data retrieval through
`streamfind.get_visualization_data`; it must preserve the published binding and service limits rather than grant
arbitrary database access.

Do not make PNG the primary contract. The structured spec is the canonical machine-readable result.

## 22.13 MCP interactive UI

When an MCP host supports an embedded MCP App/UI resource, provide a generic StreamFind visualization application that
uses the shared visualization runtime:

```text
MCP host
   ↓ calls StreamFind tool
StreamFind MCP
   ├── structured VisualizationSpec
   └── StreamFind visualization UI resource
                ↓
      shared visualization runtime
          ├── Plotly.js
          └── registered D3 renderer
```

The MCP UI contains no MassSpec-specific plotting logic. It receives the same spec that the React application uses.

Treat embedded UI support as an optional host capability. Tool results remain useful without it.

## 22.14 MCP and non-interactive fallbacks

Every visualization spec includes a concise semantic fallback description. Where useful, the MCP/service layer may also
offer:

```text
text summary
PNG
SVG
WebP
```

Static image generation is an adapter/service concern, not a C++ core dependency. Do not add Chromium/Node/Plotly
rendering dependencies to the scientific core merely to create an image.

Fallback priority:

```text
1. structured VisualizationSpec
2. interactive shared runtime when supported
3. static PNG/SVG when available and useful
4. concise semantic text description
```

This allows terminal-oriented MCP harnesses and AI agents to consume the result even when rich UI is unavailable.

## 22.15 Client interactions and linked views

The visualization spec may advertise stable selection keys:

```json
{
  "interaction": {
    "selection_key": "feature_id",
    "hover_key": "feature_id"
  }
}
```

Plotly `customdata` or D3 datum fields carry stable identifiers. The runtime translates renderer-specific events into
normalized `VisualizationInteraction` events.

Linked-view state belongs to the client project/session:

```text
selected feature
   ├── feature map highlight
   ├── chromatogram request/view
   ├── spectrum request/view
   └── metadata/evidence panel
```

A click/zoom/hover does not mutate workflow artifacts. If an interaction should launch a new backend operation, that
transition must be explicit and auditable.

## 22.16 Legacy R plotting reference and migration order

Use `bindings/r/R/utils_plots.R`, `class_ProjectNonTargetAnalysis.R`, chromatogram result modules, spectrum modules and
related Plotly code as the analytical reference. Preserve meaning, not implementation details.

Recommended migration order:

```text
1. chromatogram / EIC / TIC / BPC
2. MS1/MS2 spectrum
3. feature map
4. feature profile and feature count
5. heatmap / grouped heatmap
6. mirrored suspect/MS2 comparison
7. fold-change plot
8. 3D surface
9. network/provenance D3 views where Plotly is not appropriate
```

For each port compare:

* input selection semantics;
* axes and units;
* grouping/legend behavior;
* normalization;
* peak-region representation;
* hover fields;
* label behavior;
* WebGL use/point density;
* analytical defaults.

Do not mechanically port R/Shiny theme code such as `darkMode`; those concerns belong to the client runtime.

## 22.17 First vertical slice

The first end-to-end implementation is deliberately narrow:

```text
Chromatogram artifact
        ↓
mass_spec.plot_chromatogram
        ↓
validated sfvis:VisualizationSpec
        ↓
artifact inventory
        ↓
visualization.get_spec
        ↓
core.visualization
        ↓
core.plotly
        ↓
Plotly.js in React
```

Acceptance for this slice requires a real disposable project and real backend-produced chromatogram data. Do not treat a
hard-coded frontend fixture as completion.

After the React slice is stable, exercise the same spec through MCP structured output. Only then add additional plot
operations and optional MCP interactive UI/static render adapters.

## 22.18 Testing strategy

### Backend/schema tests

Cover:

* valid Plotly spec;
* empty-but-valid plot;
* unsupported trace type;
* mismatched x/y/customdata lengths;
* NaN/infinite numeric values;
* missing source artifact;
* stale/invalid binding;
* oversized inline payload;
* excessive point count;
* unknown renderer/spec version;
* D3 dangling node/link IDs;
* attempted executable/script field;
* atomic failure with no published artifact.

### Frontend/runtime tests

Cover:

* schema validation;
* renderer resolution;
* Plotly mount/update/unmount;
* responsive resize;
* light/dark theme application without changing backend spec;
* inline data;
* artifact-backed data loading;
* empty state;
* loading/error state;
* unsupported renderer/version fallback;
* normalized selection events;
* no console/page errors in browser smoke test.

### Integration tests

Cover:

```text
workflow artifact
  → plot operation
  → VisualizationSpec artifact
  → service fetch
  → React render
```

Then cover:

```text
VisualizationSpec artifact
  → MCP tool result structured content
  → fallback description
  → interactive MCP UI when available
```

Golden/regression fixtures compare scientific meaning and contract structure, not pixel-perfect screenshots as the only
correctness criterion.

## 22.19 Implementation order for another agent

Implement in this order unless a compile/runtime dependency forces a smaller coordinated slice:

1. Define `sfvis:VisualizationSpec` semantic contract and JSON schema.
2. Add renderer descriptor/version rules and validation.
3. Add visualization artifact publication through the existing artifact inventory.
4. Implement `mass_spec.plot_chromatogram` against exact current artifact contracts.
5. Add `visualization.get_spec` and bounded artifact-preview/service DTOs.
6. Add `frontend/src/visualization` with `VisualizationRenderer`, registry and `core.plotly`.
7. Render the real chromatogram spec end to end in React.
8. Add artifact-backed `visualization.get_data` only when the first real dense plot requires it; do not overbuild first.
9. Add normalized selection interactions and linked-view hooks.
10. Port spectrum and feature-map operations from the R analytical reference.
11. Extract the renderer into `frontend/packages/visualization-runtime` if not done initially.
12. Expose `VisualizationSpec` from MCP as structured content.
13. Reuse the shared runtime in an optional MCP visualization UI/app.
14. Add optional static image adapters outside the C++ core.
15. Add D3 only for a concrete visualization whose structure is not cleanly represented by Plotly.
16. Continue the R migration list with regression tests for every visualization operation.

Do not implement every plot type before validating the first end-to-end contract.

## 22.20 Completion criteria

This visualization architecture is complete enough to expand when all of the following are true:

* backend operations, not React, generate scientific plot specifications;
* `VisualizationSpec` is immutable, versioned, schema-validated and stored in the normal artifact inventory;
* Plotly.js renders a backend-generated real MassSpec plot through one generic client component;
* large-data plots have a bounded artifact-backed path with no arbitrary SQL exposure;
* client theme changes do not recompute or mutate the visualization artifact;
* no executable D3/JavaScript is transported in visualization specs;
* D3, when used, resolves through an explicit registered renderer schema;
* source artifact lineage, producer node/run and sampling/aggregation metadata are inspectable;
* invalid/oversized/stale specs fail visibly and publish no partial artifact;
* React contains no MassSpec-specific plot reconstruction branch;
* MCP can return the same visualization spec as structured content;
* MCP clients without rich rendering still receive a useful fallback description and, where available, static image;
* interactive MCP rendering, when supported, reuses the same visualization runtime as React;
* the first migrated plots preserve the analytical meaning, axes, units, grouping, hover data and relevant interactions of
  the legacy R/Plotly views.
