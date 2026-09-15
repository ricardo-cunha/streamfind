# StreamFind React Frontend Implementation Plan

**Target branch:** `dev_refactoring`
**Frontend:** React + TypeScript + Vite
**Backend:** StreamFind C++ core + MCP/service boundary
**Primary domains:** MassSpec first, Raman and Sensors as genericity validation
**Reference UI:** `cogniflow-playground/sandcastle/cf_web` and the existing StreamFind Shiny UI under `bindings/r`

---

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
* inspect project tables and workflow outputs;
* render scientific results through reusable React components;
* allow plugins to declare preferred visual representations;
* support future domains without adding domain-specific logic to the generic frontend;
* preserve workflow provenance and execution history;
* provide a consistent StreamFind visual identity from the first implementation stage.

The frontend must not:

* access DuckDB directly;
* become the owner of workflow execution state;
* duplicate method definitions already contained in the semantic catalogue;
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
│   ├── Results
│   ├── Data / Tables
│   ├── Runs
│   ├── Provenance
│   └── Project settings
│
├── Capability Registry
│   ├── Domains
│   ├── Methods
│   ├── Operations
│   ├── Parameters
│   ├── Table contracts
│   └── UI metadata
│
├── Renderer Registry
│   ├── Generic renderers
│   └── Domain renderers
│
├── Execution Store
│   ├── Workers
│   ├── Runs
│   ├── Steps
│   └── Live events
│
└── StreamFind Client
    │
    └── WebSocket / browser-capable MCP transport
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
│   │   ├── MethodLibrary.tsx
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

## Acceptance criteria

* React app builds successfully.
* Light and dark modes work.
* No page contains hard-coded theme colors.
* Splash screen appears before application initialization.
* Theme preferences can be persisted locally.
* Application shell renders without backend functionality.

---

# 5. Phase 2 — WebSocket/C++ client and session bootstrap

## Goal

Establish a single browser-facing communication layer between React and the C++ runtime.

## Tasks

### 5.1 Add browser transport to the C++ host

Keep current stdio MCP support.

Add a browser-compatible persistent transport.

Preferred:

```text
WebSocket
```

Use one connection for:

* commands;
* responses;
* subscriptions;
* execution events;
* worker events;
* project state changes.

Do not add domain-specific API endpoints.

### 5.2 Create frontend transport abstraction

Implement:

```ts
interface StreamFindTransport {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  request<T>(method: string, params?: unknown): Promise<T>;
  subscribe(
    topic: string,
    handler: (event: unknown) => void
  ): () => void;
}
```

Initial implementation:

```text
WebSocketTransport
```

This allows future transport replacement without changing application components.

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
* methods;
* operations;
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
  "type": "workflow.step.progress",
  "project": "...",
  "execution_id": "...",
  "timestamp": "...",
  "payload": {}
}
```

Event groups:

```text
session.*
project.*
workflow.*
workflow.step.*
worker.*
table.*
plugin.*
```

## Acceptance criteria

* React establishes a WebSocket connection to the C++ host.
* Protocol/version negotiation occurs during startup.
* Plugin and capability discovery is automatic.
* React contains no manually maintained MassSpec method list.
* Connection loss and reconnection are handled visibly.
* Backend initialization failure is shown through the splash/error state.

---

# 6. Phase 3 — Project Hub and multi-project runtime

## Goal

Allow users to create, open, close, and switch between multiple project databases while keeping independent project runtimes active.

The current architecture defines one DuckDB file as exactly one project, with one immutable domain. Multiple project files may be open and execute independently.

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

# 7. Phase 4 — Ontology-driven workflow canvas

## Goal

Create workflows using the same ontology-driven concept used by CogniFlow, but backed by StreamFind capabilities.

CogniFlow already uses semantic step metadata, typed inputs/outputs, dynamic parameter editors, connections, viewport state, and node metadata. Reuse these interaction patterns.

## Tasks

### 7.1 Method definition contract

Frontend receives normalized method specifications:

```ts
MethodDefinition {
  id: string;
  label: string;
  description?: string;
  domain: string;
  category?: string;

  parameters: ParameterDefinition[];

  reads: TableReference[];
  writes: TableReference[];

  ui?: MethodUiMetadata;
}
```

### 7.2 Method library

Left panel groups methods by semantic metadata.

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
method label
status
input contracts
output contracts
```

Clicking selects node and opens inspector.

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

Use existing backend workflow contract.

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

* Method palette is generated entirely from backend capability metadata.
* Nodes can be added to the canvas.
* Parameters are generated automatically.
* Workflow can be validated by the backend.
* Workflow can be persisted into the project.
* Reloading a project reconstructs the workflow.
* No domain-specific workflow logic exists in generic canvas components.

---

# 8. Phase 5 — Durable execution lifecycle, workers, and live events

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

workflow.step.started
workflow.step.progress
workflow.step.completed
workflow.step.failed

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

Expand into step history.

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

---

# 9. Phase 6 — Generic table and data explorer

## Goal

Provide a domain-independent way to inspect every project-owned table.

## Tasks

### 9.1 Generic backend table services

Expose safe operations:

```text
table.list
table.describe
table.query
table.count
table.distinct
table.export
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
workflow step that last produced it
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

# 10. Phase 7 — Renderer registry and MassSpec renderers

## Goal

Introduce the extensible scientific visualization layer.

## Tasks

### 10.1 RendererRegistry

Implement:

```ts
RendererRegistry.register(id, renderer);
RendererRegistry.resolve(tableMetadata);
```

Core renderer IDs:

```text
core.table
core.metric
core.line
core.scatter
core.heatmap
```

MassSpec renderer IDs:

```text
mass_spec.feature_map
mass_spec.chromatogram
mass_spec.spectrum
mass_spec.suspect_table
mass_spec.compound_match
```

### 10.2 Renderer contract

```ts
interface ResultRendererProps {
  projectSessionId: string;
  table: TableDescriptor;
  query: QueryState;
  metadata: RendererMetadata;
}
```

Renderers request data through backend services.

They do not receive DuckDB connections or raw database access.

### 10.3 Feature map

Implement:

```text
x = retention time
y = m/z
color = intensity or configurable metadata
size = optional intensity
```

Interactions:

* zoom;
* hover;
* feature selection;
* filtering;
* linked details.

### 10.4 Chromatogram viewer

Reusable component supporting:

* TIC;
* BPC;
* EIC;
* selected chromatograms;
* multiple analyses;
* zoom;
* range selection.

### 10.5 Spectrum viewer

Support:

* MS1;
* MS2;
* peak labels;
* mirrored comparison later;
* selected feature linkage.

### 10.6 Suspect results table

Support:

* compound structure;
* score;
* formula;
* m/z;
* RT;
* evidence;
* MS2 summary.

### 10.7 Linked selections

Shared selection model:

```text
FeatureMap
    ↓ selected feature
Chromatogram
Spectrum
Metadata
Compound match
```

## Acceptance criteria

* Renderer is selected through registry metadata.
* ResultExplorer contains no `if domain === mass_spec` rendering branches.
* Feature map works with real FEATURES data.
* Spectrum and chromatogram components are reusable.
* Table fallback renderer always remains available.

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
  preferredRenderer → mass_spec.feature_map
  xRole → retention_time
  yRole → mz
  colorRole → intensity
```

The backend catalogue generator exposes normalized UI metadata.

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

Do not duplicate method/table semantics in an additional JSON configuration.

### 12.4 Generic UI extension rules

A new plugin should be able to introduce:

* new domain;
* new methods;
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
* No duplicated method metadata exists in the frontend.

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
Raman methods appear in workflow library
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
static result table
and
stream-updated result table
```

without changing core ResultExplorer behavior.

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
workflow step
↓
method
↓
parameters
↓
plugin
↓
plugin version
↓
input tables
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
* Method Library;
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
add methods
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
Phase 4
Workflow canvas
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
Phase 4
```

Success:

```text
ontology-driven workflows can be created and saved
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
* Workflows are created using ontology-derived methods.
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
* React contains no hard-coded central domain or method inventory.
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
