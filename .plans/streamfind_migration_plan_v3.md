# StreamFind Future Development Roadmap

**Branch:** `dev_refactoring`
**Scope:** future work only for the current C++ operation-graph runtime.

## Target architecture

StreamFind persists one workflow per DuckDB project file:

```text
Workflow.operations[] + Workflow.connections[]
        -> typed-port validation
        -> operation-graph execution
        -> durable execution records
        -> immutable artifact inventory
```

Core owns project lifecycle, workflow persistence, validation, execution, DuckDB access, artifact lineage, and the generic MCP/service boundary. Plugins own domain operations, readers, semantic declarations, and result contracts. Turtle sources under `cpp/core/semantic` and `cpp/plugins/*/semantic` remain authoritative; generated catalogues are build outputs.

## Future work sequence

1. **Installed-package acceptance**
   - Extract Windows and Linux release archives into disposable directories.
   - Verify launcher-relative runtime lookup, bundled DuckDB/MinGW dependencies, plugin discovery, catalogues, and package confinement.
   - Run `initialize`, capability discovery, project creation, graph construction, validation, execution, and artifact inspection against the packaged host.

2. **Core data-plane hardening**
   - Replace remaining ad-hoc row and SQL paths with typed batch reads/writes at the plugin access boundary.
   - Add rollback, nullable-value, timestamp, decimal, binary, and authorization coverage.
   - Verify atomic artifact publication and cleanup after failed or cancelled operations.

3. **Plugin extensibility**
   - Keep plugin discovery, manifests, semantic resources, entry points, and CMake integration automatic.
   - Add new domain capabilities through plugin-owned operations and typed table/result contracts without generic-core changes.
   - Extend validator coverage for manifests, catalogue consistency, exported operations, and release staging.

4. **MassSpec public contracts**
   - Complete SCIEX/LCD inspection and malformed-input coverage.
   - Verify lazy indexed decoding, calibration, truncation/overflow handling, chromatogram persistence, and reopen behavior.
   - Resolve and implement the transformation-product result contract as a typed operation artifact.

5. **Cross-platform release hardening**
   - Build and test Windows UCRT64 and Linux GCC/Ninja packages from clean environments.
   - Verify all enabled-plugin selections, package checksums, runtime dependencies, and representative operation graphs.

6. **Frontend integration**
   - Connect the React application to the public C++ service/MCP boundary.
   - Render operation capabilities from the catalogue, edit typed ports and parameters, display validation diagnostics, monitor runs, and browse immutable artifacts.
   - Add visualization operations that publish typed visualization-spec artifacts.

7. **Deferred integrations**
   - Reopen R and other integrations only after the C++ contracts and release packages are stable.
   - Any future backend must consume the same semantic operation, port, result, and artifact contracts independently.

## Acceptance gates

- No operation executes unless its required typed inputs are connected or explicitly supplied by its contract.
- Execution records identify the workflow revision, operation instance, parameters, inputs, outputs, and status.
- Published artifacts are immutable, discoverable, lineage-linked, and safe to reopen.
- New plugin operations are discoverable through the generated catalogue and executable through the generic host.
- Release archives run without developer tools or manually configured dependency paths.
