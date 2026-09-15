Yes. The cleanest design is to make the React application a **generic StreamFind shell whose behavior is driven by the C++ capability catalogue and plugin metadata**, rather than building MassSpec-specific screens into the main frontend. I reviewed the current `dev_refactoring` structure, the frontend decisions already recorded in the roadmap, the Shiny result modules, and the `cf_web` implementation you referenced.

The current backend architecture is already well suited to this: `core/` owns projects, DuckDB and workflows; `sdk/` defines the plugin boundary; and `plugins/` owns domain capabilities. Plugin packages already contain a manifest, semantic catalogue and shared library, so the UI should extend that same discovery mechanism rather than inventing a separate configuration system.

## 1. Overall architecture

I would target this:

```text
┌──────────────────────────────────────────────────────────────┐
│                    StreamFind React UI                       │
│                                                              │
│ Project Hub ── Project Workspaces ── Global Worker Monitor   │
│                    │                                         │
│     ┌──────────────┼──────────────────────┐                  │
│     │              │                      │                  │
│ Workflow       Results/Data         Lifecycle/Provenance     │
│ Canvas         Explorer             Monitor                  │
│     │              │                      │                  │
│     └────── UI Component + Renderer Registry ────────────────┤
│                        │                                     │
│                 Plugin UI Registry                           │
└────────────────────────┬─────────────────────────────────────┘
                         │
                     WebSocket
                     MCP/service
                         │
┌────────────────────────▼─────────────────────────────────────┐
│                   StreamFind C++ Host                        │
│                                                              │
│ Session Manager                                              │
│ Project Runtime Manager                                      │
│ Plugin / Capability Registry                                 │
│ WorkflowExecutionManager                                     │
│ Table Query / Result Service                                 │
│ WebSocket Event Broker                                       │
└────────────────────────┬─────────────────────────────────────┘
                         │
       ┌─────────────────┼────────────────────┐
       ▼                 ▼                    ▼
 MassSpec plugin     Raman plugin        Sensors plugin
       │                 │                    │
       ▼                 ▼                    ▼
 project A.duckdb    project B.duckdb     project C.duckdb
 worker A            worker B             worker C
```

I would **not** let React access DuckDB directly. The roadmap already makes MCP the orchestration boundary, requires a browser-capable transport, and explicitly says the frontend should not contain backend workflow state or direct DuckDB logic. It also establishes the model that one DuckDB file represents exactly one project/domain while different project files may run simultaneously.

For the browser transport, I would use a persistent **WebSocket connection to the C++ host**. Request/response messages can handle ordinary API/MCP calls, while the same connection delivers workflow progress, worker changes, table updates, logs and cancellation events.

---

# 2. Application startup

Your spinning StreamFind logo fits particularly well as a real bootstrap state rather than a cosmetic splash screen.

I would create a top-level `BootstrapGate`:

```text
Browser starts
     │
     ▼
StreamFind logo appears
3D rotateY animation
     │
     ▼
Connect to C++ backend
     │
     ├─ initialize session
     ├─ negotiate frontend/backend API versions
     ├─ retrieve core capabilities
     ├─ discover loaded plugins
     ├─ retrieve semantic catalogue
     ├─ retrieve UI renderer metadata
     ├─ preload plugin icons/assets
     ├─ initialize theme
     ├─ retrieve known/recent projects
     └─ subscribe to active executions/workers
     │
     ▼
Assets ready
     │
fade transition
     ▼
Project Hub
```

The logo animation would literally rotate around its vertical axis:

```css
transform: perspective(600px) rotateY(...);
```

with a `prefers-reduced-motion` alternative.

The background can remain completely clean. I would show no progress bar unless initialization takes unusually long; errors could replace the splash with a compact diagnostics screen.

Importantly, **the main React router should not mount until bootstrap succeeds**. That prevents pages briefly rendering without plugin information and subsequently rearranging themselves.

---

# 3. Project Hub

This becomes the application's starting page.

Visually I would make it much closer to a desktop scientific application than to a conventional website:

| Area             | Function                                                    |
| ---------------- | ----------------------------------------------------------- |
| New Project      | project name/location + domain selection                    |
| Open Project     | select existing `.duckdb`                                   |
| Recent Projects  | cards with domain, last opened, status                      |
| Running Projects | show projects whose workers are still active                |
| Plugins          | small indicator of available MassSpec/Raman/Sensors plugins |

Creating a project would first ask for the domain because the domain is immutable after project creation in the current C++ architecture.

There is one important deployment consideration: a normal remote browser cannot safely manipulate arbitrary filesystem paths. For the local StreamFind application, I would therefore let the **native StreamFind launcher/backend own the file selection**. If you eventually wrap the React UI with Tauri, the same React application can use a native file dialog without changing the project APIs.

---

# 4. Multi-project UI

Once projects are opened, I would keep them alive independently.

Something like:

```text
┌───────────────────────────────────────────────────────────────┐
│ streamfind          Project A ●   Project B ▶   Project C ✓  │
├──────────────┬────────────────────────────────────────────────┤
│ Workflow     │                                                │
│ Results      │             active project                    │
│ Data         │                                                │
│ Runs         │                                                │
│ Provenance   │                                                │
│ Project      │                                                │
└──────────────┴────────────────────────────────────────────────┘
```

The global top project switcher becomes important because **changing from Project A to Project B must not stop Project A's workflow**.

The backend, not React, owns this.

I would introduce a C++ runtime layer roughly like:

```text
ProjectRuntimeManager
    │
    ├─ project-A.duckdb
    │     ├─ Project
    │     ├─ WorkflowExecutionManager
    │     ├─ Worker A
    │     └─ event stream
    │
    ├─ project-B.duckdb
    │     ├─ Project
    │     ├─ WorkflowExecutionManager
    │     ├─ Worker B
    │     └─ event stream
    │
    └─ project-C.duckdb
          ...
```

For analytical workflows, I would strongly favor **process workers rather than only frontend/background JS workers**. That gives better crash isolation and works naturally with computational C++/OpenMP workloads.

The durable execution data can contain:

```text
execution_id
project path / internal project identity
worker_id
worker_pid
workflow revision
status
created_at
queued_at
started_at
finished_at
heartbeat_at
progress
error
```

The existing C++ project already exposes persisted workflow-execution information and step execution rows, so this should extend that lifecycle rather than create another job subsystem.

---

# 5. Workflow page: reuse the CogniFlow concept

This is where I would reuse the `cf_web` interaction model most heavily.

CogniFlow already has the useful conceptual pieces:

* semantic step discovery;
* node specifications containing inputs, outputs and parameters;
* automatic parameter editors;
* data-type visual styles;
* connections between typed ports;
* viewport/navigation state;
* live workflow validation.

Its current editor loads semantic steps dynamically and constructs node metadata rather than manually defining every step.

For StreamFind the left side could contain:

```text
MASS SPECTROMETRY

Input
  Import analyses
  Load chromatograms

Processing
  Find features
  Group features
  Fill features
  Subtract blank
  Correct matrix suppression
  ...

Screening
  Suspect screening
  MetFrag screening
  Transformation products
```

But **that list must never exist as a TypeScript array**.

Instead:

```text
MassSpec semantic catalogue
        ↓
C++ capability projection
        ↓
WebSocket/MCP
        ↓
React MethodDefinition[]
        ↓
Workflow node factory
```

That matches the current roadmap requirement that the frontend must not maintain a separate copy of the MassSpec method inventory.

Parameters can then map automatically:

```text
boolean             → Toggle
integer             → NumberInput
number + min/max    → NumberInput / Slider
enumeration         → Select
string              → TextInput
table reference     → TablePicker
column reference    → ColumnPicker
analysis reference  → AnalysisPicker
file/directory      → ResourcePicker
structured object   → SchemaForm
```

The canvas therefore becomes reusable for Raman, Sensors, or future domains.

---

# 6. Ontology-driven frontend contract

There is an especially valuable lesson to take from CogniFlow: **do not make the React app parse the ontology itself**.

`cf_web` explicitly normalizes semantic information in the backend service and makes the browser consume a UI-friendly contract.

I would do exactly that here.

For example, the frontend receives:

```ts
interface MethodDefinition {
  id: string;
  domain: string;
  label: string;
  description?: string;
  category?: string;

  parameters: ParameterDefinition[];

  reads: TableContract[];
  writes: TableContract[];

  ui?: {
    icon?: string;
    category?: string;
    nodeRenderer?: string;
  };
}
```

The ontology remains authoritative, but C++ projects it into this normalized representation.

That means React knows nothing about Turtle/SPARQL/SHACL.

---

# 7. Plugin-driven result rendering

This is probably the most important frontend abstraction.

Instead of:

```text
if domain === "mass_spec"
   show MassSpecResultsPage
```

create a generic:

```text
ResultRendererRegistry
```

with reusable renderers.

For example:

```text
core.table
core.metric
core.scatter
core.line
core.heatmap

mass_spec.feature-map
mass_spec.chromatogram
mass_spec.spectrum
mass_spec.suspect-table
mass_spec.compound-match

raman.spectrum
raman.peak-table

sensors.timeseries
sensors.status-grid
sensors.anomaly-timeline
```

A plugin table definition might semantically declare:

```text
FEATURES
  preferredRenderer → mass_spec.feature-map

roles:
  retention_time → x
  mz             → y
  intensity      → size/color
```

Then:

```text
DuckDB table
    ↓
table semantic contract
    ↓
renderer ID
    ↓
ResultRendererRegistry
    ↓
FeatureMapRenderer
```

This makes result visualization **data-contract-driven rather than page-driven**.

---

# 8. How I would extend StreamFind plugins for UI

I would avoid a completely separate frontend plugin manifest.

Instead, extend the plugin semantics with an optional `sfui:` vocabulary:

```text
sfui:preferredRenderer
sfui:icon
sfui:category
sfui:xRole
sfui:yRole
sfui:colorRole
sfui:sizeRole
sfui:detailsRenderer
sfui:defaultColumns
```

For example conceptually:

```text
mass_spec:FeaturesTable
    sfui:preferredRenderer "mass_spec.feature-map" ;
    sfui:xRole mass_spec:retentionTime ;
    sfui:yRole mass_spec:mz ;
    sfui:colorRole mass_spec:intensity .
```

The backend catalogue generator then exposes those fields.

That preserves the existing principle that semantic resources are the authoritative capability contract.

The physical plugin package could additionally contain only static UI assets:

```text
cpp/plugins/mass_spec/
├── plugin.json
├── catalogue.duckdb
├── semantic/
├── src/
└── ui/
    ├── icons/
    ├── images/
    └── plugin-ui.json
```

`plugin-ui.json` should initially describe **assets only**, not duplicate method/table metadata.

---

# 9. Dynamic UI plugins: two levels

I would deliberately implement this in two stages.

**Level 1 — recommended initially**

Plugins can dynamically introduce:

* methods;
* tables;
* parameters;
* icons;
* renderer selection;
* column roles;
* display metadata.

But all renderer implementations come from StreamFind's reusable React component library.

That is safe and simple.

**Level 2 — later**

Allow a trusted plugin to ship its own JavaScript renderer:

```text
ui/
   plugin-ui.json
   streamfind-mass-spec-ui.js
```

with something like:

```text
ui_api_version: 1
```

and dynamic ES-module loading.

That provides true third-party visualization extensibility but introduces versioning and security concerns. I would not make arbitrary executable JS plugins necessary for v1.

---

# 10. Results page: evolve the Shiny design rather than discard it

The current Shiny MassSpec/NTA application already contains useful interaction concepts: full-height plots, collapsible filter panels, controls above plots, separate details panes, feature exploration, suspect tables and MS/MS-related views.  The current result modules also have dedicated chromatogram, MassSpec explorer and NTA views.

I would preserve those **analytical workflows**, not their Shiny implementation.

For NTA, for example:

```text
RESULTS — Non-Target Screening

┌─────────────────────────────────────────────────────────┐
│ Summary | Features | Suspects | Internal Standards      │
├──────────────┬──────────────────────────┬───────────────┤
│ Filters      │                          │ Details       │
│              │      Feature map         │               │
│ analysis     │                          │ m/z           │
│ group        │   RT × m/z × intensity  │ RT            │
│ intensity    │                          │ formula       │
│ flags        │                          │ MS2           │
│              │                          │ annotation    │
└──────────────┴──────────────────────────┴───────────────┘
```

Click a feature and the renderer registry could compose:

```text
FeatureDetails
 + ChromatogramViewer
 + SpectrumViewer
 + CompoundStructure
 + IdentificationEvidence
```

Those components can later be reused anywhere else in StreamFind.

---

# 11. Generic Data / Tables page

This should exist separately from polished domain visualizations.

Every project gets a generic database explorer:

```text
Tables
 ├─ PROJECT
 ├─ WORKFLOW_EXECUTION
 ├─ WORKFLOW_EXECUTION_STEP
 ├─ ANALYSES
 ├─ FEATURES
 ├─ SUSPECTS
 └─ ...
```

Selecting a table shows:

```text
Schema | Data | Visualization | Provenance
```

For this I would expose generic C++ services such as:

```text
table.list
table.describe
table.query
table.count
table.export
```

with server-side paging/filtering/sorting.

Do **not** expose arbitrary SQL as the normal UI interface.

---

# 12. Workflow lifecycle visualization

I would represent execution at two levels simultaneously.

On the workflow canvas:

```text
○ Not executed
◌ Queued
◉ Running
✓ Completed
✕ Failed
⊘ Cancelled
```

Nodes can show:

```text
Find features
████████░░ 82%
34.2 s
```

And the separate **Runs** page can show the durable history:

```text
Run #17               COMPLETED
─────────────────────────────────────
Created       10:43:01
Queued        10:43:02
Worker 3      PID 18342
Started       10:43:04
Finished      10:48:37

Import analyses                ✓
Find features                  ✓
Group features                 ✓
Fill features                  ✓
Suspect screening              ✓
MetFrag screening              ✓
```

Useful execution states would be:

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

The backend is always the source of truth. React just subscribes to it.

---

# 13. Provenance becomes a first-class UI feature

Because StreamFind already persists workflow, execution and audit information, I would give every result a route back to:

```text
Result table/row
       ↓
workflow execution
       ↓
workflow step
       ↓
method + parameters
       ↓
input tables
       ↓
plugin + version
```

You could even have a small **"Produced by"** action on results:

```text
FEATURES

Produced by:
Find Features
Run #31
MassSpec plugin 0.2.0

[Show in workflow]
```

That would make the FAIR/provenance aspect of StreamFind very visible.

---

# 14. Styling: reuse the CogniFlow theme architecture

I would reuse this nearly directly.

CogniFlow already separates:

```text
mode
palette
style
```

and uses semantic CSS tokens for surfaces, accent colors, status colors, datatype colors, node colors, canvas colors and so on.

Interestingly, its current default palette is already called **`wastewater`**, with a teal primary accent around `#11696f`, muted green/blue backgrounds and ochre signal colors. That is an excellent basis for StreamFind.

I would establish from day one:

```text
Theme
 ├─ Mode
 │   ├─ Light
 │   └─ Dark
 │
 ├─ Palette
 │   ├─ StreamFind / Wastewater
 │   ├─ Neutral Scientific
 │   └─ High Contrast
 │
 └─ Style
     ├─ Classic
     ├─ Compact
     └─ Spacious
```

And use semantic variables everywhere:

```css
--sf-bg
--sf-surface
--sf-surface-raised
--sf-border
--sf-text
--sf-text-muted

--sf-accent
--sf-signal

--sf-success
--sf-warning
--sf-error

--sf-domain-mass-spec
--sf-domain-raman
--sf-domain-sensors
```

No React component should contain arbitrary hard-coded colors.

---

# 15. Suggested frontend structure

I would start the new root `/frontend` like this:

```text
frontend/
├── src/
│   ├── app/
│   │   ├── App.tsx
│   │   ├── BootstrapGate.tsx
│   │   └── router.tsx
│   │
│   ├── backend/
│   │   ├── StreamFindClient.ts
│   │   ├── WebSocketTransport.ts
│   │   ├── protocol.ts
│   │   └── subscriptions.ts
│   │
│   ├── projects/
│   │   ├── ProjectHub.tsx
│   │   ├── ProjectWorkspace.tsx
│   │   ├── ProjectSwitcher.tsx
│   │   └── projectStore.ts
│   │
│   ├── workflow/
│   │   ├── WorkflowCanvas.tsx
│   │   ├── WorkflowNode.tsx
│   │   ├── MethodLibrary.tsx
│   │   ├── ParameterEditor.tsx
│   │   └── workflowStore.ts
│   │
│   ├── execution/
│   │   ├── ExecutionMonitor.tsx
│   │   ├── ExecutionTimeline.tsx
│   │   ├── WorkerMonitor.tsx
│   │   └── executionStore.ts
│   │
│   ├── results/
│   │   ├── ResultExplorer.tsx
│   │   ├── RendererRegistry.ts
│   │   └── renderers/
│   │       ├── TableRenderer.tsx
│   │       ├── ScatterRenderer.tsx
│   │       ├── LineRenderer.tsx
│   │       ├── SpectrumRenderer.tsx
│   │       ├── ChromatogramRenderer.tsx
│   │       └── FeatureMapRenderer.tsx
│   │
│   ├── tables/
│   │   ├── TableExplorer.tsx
│   │   └── DataGrid.tsx
│   │
│   ├── plugins/
│   │   ├── PluginRegistry.ts
│   │   ├── PluginProvider.tsx
│   │   └── uiContract.ts
│   │
│   ├── provenance/
│   │   └── ProvenanceView.tsx
│   │
│   ├── components/
│   │   └── reusable UI primitives
│   │
│   ├── theme/
│   │   ├── tokens.ts
│   │   ├── palettes.ts
│   │   └── themes.css
│   │
│   └── assets/
│       └── streamfind-logo.svg
│
├── tests/
├── package.json
└── vite.config.ts
```

The existing roadmap already reserves `/frontend` for exactly this backend-neutral React/TypeScript client.

---

# 16. Implementation order

I would develop it in this sequence:

1. **Frontend shell + theme + splash screen.** Establish visual identity before individual pages start accumulating local CSS.
2. **WebSocket/C++ client and session bootstrap.** Capability/plugin discovery becomes foundational.
3. **Project Hub + multi-project runtime manager.**
4. **Workflow canvas using ontology-derived methods**, based on the CogniFlow canvas interaction pattern.
5. **Durable execution lifecycle + workers + live events.**
6. **Generic table/data explorer.**
7. **Renderer registry and MassSpec renderers.**
8. **Port the useful NTA interactions from Shiny** into reusable React components.
9. **Plugin UI metadata contract.**
10. **Raman/Sensors as the test that the architecture is truly generic.**

The key architectural rule I would enforce from the beginning is:

> **Core React knows how to render StreamFind concepts; plugins describe what exists and how it should preferably be presented.**

That gives you one React application, one workflow canvas, one result framework and one theme system, while MassSpec, Raman, Sensors and future plugins can expand the application without adding domain-specific conditionals throughout the frontend. It also fits the direction already established in `dev_refactoring` much better than porting the Shiny UI page-for-page.
