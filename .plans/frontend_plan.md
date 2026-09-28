# StreamFind Frontend Future Development Plan

**Stack:** React, TypeScript, Vite
**Backend boundary:** C++ service and MCP host
**Scope:** future frontend work against the current operation-graph and artifact contracts.

## Product model

The frontend is a generic StreamFind shell. It discovers capabilities from the semantic catalogue and never reads DuckDB directly or imports native implementation details.

```text
project file
  -> persisted operations and typed connections
  -> backend validation and execution
  -> execution status and artifact inventory
  -> tables, JSON results, and visualization-spec renderers
```

## Work packages

### 1. Project workspace

- Open or create a DuckDB project file.
- Show the saved workflow revision and artifact inventory.
- Keep project context implicit in service/MCP requests.
- Preserve unsaved graph edits separately from the persisted workflow.

### 2. Operation graph editor

- Discover operation IDs, versions, parameters, input ports, output ports, and result contracts from the catalogue.
- Create operation nodes with persisted parameters.
- Connect only compatible typed ports.
- Surface missing inputs, invalid parameters, cycles, and unavailable capabilities before execution.
- Treat connections as data bindings; derive execution order from the graph.

### 3. Execution and provenance

- Submit a validated workflow revision to the backend.
- Display durable run and operation-instance status, progress, cancellation, diagnostics, and timestamps.
- Show exact input/output artifact lineage and cache state.
- Never infer data from a global “latest table”; resolve artifacts by inventory identity.

### 4. Results and visualization

- Browse typed table and JSON artifacts with schema-aware columns.
- Render `sfvis:VisualizationSpec` artifacts through registered generic renderers.
- Support MassSpec chromatograms and spectra first, while keeping the renderer registry domain-neutral.
- Keep visualization state separate from scientific artifact identity.

### 5. Plugin-driven UI

- Derive forms and operation cards from semantic parameter metadata.
- Display plugin labels, descriptions, versions, and availability from manifests/catalogues.
- Add no domain-specific capability list to the frontend source.
- Verify the same UI works for MassSpec, Raman, and Sensors discovery.

## Validation

- Unit-test parameter, port, connection, artifact, and visualization data models.
- Run browser smoke tests against a packaged C++ host.
- Verify create → add operations → connect → validate → run → inspect artifacts.
- Verify invalid graphs are rejected before execution and failed operations publish no partial outputs.
