# streamfind Artifact Cache and Current-Input Resolution Plan

> **For Hermes:** Implement this plan task-by-task with tests first. Keep one owning execution and resolution path; do not add compatibility shims or a second cache execution path.

**Goal:** Make immutable published artifacts the only persisted output data, make the cache a fingerprint-to-artifact index, and make ordinary workflow connections resolve the latest valid artifact for the current upstream computation without a manual input-artifact resolver.

**Architecture:** The C++ core owns computation fingerprints, cache lookup, artifact publication, and logical connection resolution. Workflow JSON stores typed logical connections; execution records retain the concrete artifact IDs consumed. The frontend and MCP layer request current results from the core and never independently choose historical artifacts.

**Tech Stack:** C++ core, DuckDB project databases, operation-graph execution, C++ contract tests, service/API JSON, TypeScript/React frontend, CMake/Ninja with MinGW-w64 UCRT64 on Windows.

---

## Current repository state

This plan is based on `dev_refactoring` at commit `76b00da5`.

### Already implemented

- Operation-graph workflows are persisted as `operations` plus typed `connections`.
- The frontend no longer exposes the manual input-artifact resolver or serializes `source_artifact_id` in normal canvas edges.
- `Project::resolve_workflow_inputs()` already resolves a connected input from published artifacts by source operation instance, source port contract, workflow revision, creation time, and artifact status.
- The resolver currently treats `WorkflowConnection::source_artifact_id` as an optional explicit filter in the C++ model.
- Published outputs are stored in `ARTIFACT_INVENTORY`; table artifacts and structured JSON artifacts have immutable IDs and lineage support.
- `ARTIFACT_LINEAGE` records source artifact relationships.
- `WORKFLOW_EXECUTION_STEP` stores `parameter_hash` and `cache_key`, but execution currently computes the key and still executes the operation; it does not perform artifact-cache lookup or reuse.
- The `CACHE` table stores a serialized blob in `data`, so it is still a payload cache rather than an artifact-reference index.
- The ontology currently declares `sf:cacheable`; SHACL requires it on operations, catalogue projection copies it into `catalogue_entries`, and C++/Rust/frontend operation models expose it.
- The legacy backend framework uses that flag to decide whether an operation participates in caching. That policy split is no longer wanted: every operation must pass through the same fingerprint, artifact-reference, and execution-recording framework.
- `clear_workflow_history()` marks or deletes revision-scoped artifacts and lineage; its interaction with reusable cache references must be defined before implementation.
- `get_artifact_inventory` and targeted `request_artifact` exist for inspection. The frontend artifact viewer is for current/result inspection, not connection binding.

### Current implementation anchors

- `cpp/core/include/streamfind/project.hpp`
  - `WorkflowConnection::source_artifact_id`
  - `CacheEntry`
  - `Project::get_cache`, `set_cache`, `delete_cache`
  - `Project::get_artifact_inventory`
  - `Project::publish_result_artifact`
  - `Project::resolve_workflow_inputs`
- `cpp/core/src/project.cpp`
  - project schema creation, including `ARTIFACT_INVENTORY`
  - cache CRUD
  - artifact inventory/publication
  - `resolve_workflow_inputs()` around the current artifact-selection loop
  - `run_operation_graph()` where `cache_key` is currently recorded but not used for reuse
- `cpp/core/src/project_table_store.cpp`
  - table artifact allocation/publication and lineage persistence
- `cpp/core/src/api.cpp` and `cpp/core/src/mcp.cpp`
  - cache/artifact commands and MCP discovery schemas
- `cpp/sdk/src/dynamic_plugin_manager.cpp`
  - plugin input artifact consumption and output artifact publication
- `cpp/tests/unit/project_lifecycle_contract.cpp`
  - project/artifact/cache lifecycle coverage
- `cpp/tests/unit/artifact_publication_contract.cpp`
  - structured and table artifact publication coverage
- `cpp/tests/unit/mixed_domain_workflow_artifact_contract.cpp`
  - connected artifact resolution and lineage coverage
- `frontend/src/app/CanvasShell.tsx`
  - logical edge loading/serialization
  - current output preview, which currently filters the locally loaded inventory for the latest matching artifact
- `frontend/src/backend/StreamFindApiClient.ts` and related protocol types
  - service calls for workflow, artifact inventory, and artifact data

### Important correction to the earlier plan

The frontend resolver UI has already been removed. The remaining work is not to remove a picker; it is to make the backend's existing logical resolver authoritative, use it during fingerprinting and execution, and provide a current-artifact query for frontend inspection. Do not reintroduce a manual resolver or make the frontend's `.at(-1)` inventory selection the execution authority.

---

## Remove `sf:cacheable` and the legacy cacheability framework

`sf:cacheable` is not part of the target architecture. It models an obsolete per-operation opt-in, while the required backend behavior is universal: every operation gets one fingerprint, one artifact-reference registration path, and one execution-recording path. The removal must be complete across ontology, catalogue generation, native models, Rust parity surfaces, frontend types, fixtures, and tests.

### Ontology removal

Remove the property and its validation/field projection from:

- `cpp/core/semantic/vocabulary.ttl` — delete the `sf:cacheable` resource.
- `cpp/core/semantic/fields.ttl` — delete the catalogue field declaration for `cacheable`.
- `cpp/core/semantic/shapes.ttl` — remove operation-shape cardinality/type constraints and SPARQL assertions involving `sf:cacheable`.
- `cpp/plugins/mass_spec/semantic/operations.ttl` — remove every `sf:cacheable true/false` predicate. Do not replace it with a universal boolean; absence is intentional.

Run the repository Turtle formatter only if needed, then regenerate the authoritative native catalogue and verify that no operation catalogue entry contains a `cacheable` field.

### Native catalogue and execution removal

Remove the legacy boolean and all defaulting/serialization branches from:

- `cpp/core/include/streamfind/project.hpp` — operation definition/catalogue structures containing `bool cacheable`.
- `cpp/core/src/project.cpp` — operation JSON serialization and parsing of `cacheable`.
- `cpp/core/src/catalogue.cpp` — SQL projection and JSON projection of the column.
- `cpp/core/src/catalogue_binding.cpp` — catalogue binding assignments.
- `cpp/sdk/src/catalogue_builder.cpp` — RDF extraction, DuckDB schema column, and insert value for `cacheable`.
- `cpp/sdk/src/dynamic_plugin_manager.cpp` — runtime catalogue binding assignment.

Remove any execution conditional that uses this flag. `run_operation_graph`, `run_operation`, and plugin execution must enter the same fingerprint/cache/artifact path for every operation. Do not retain a `cacheable=false` fallback, a bypass mode, or a second non-cached executor.

### Rust and frontend parity removal

Apply the same removal to the preserved parity surfaces rather than leaving stale schema fields:

- `rust/crates/core/src/project.rs` — remove the field, JSON representation, catalogue handling, and cacheability branches.
- `rust/crates/core/src/catalogue.rs` — remove the column extraction and assignments.
- `rust/crates/core/tests/project_smoke.rs` and `rust/crates/mass-spec/tests/nta_interface_smoke.rs` — remove or replace assertions that set/read `cacheable`.
- `frontend/src/backend/protocol.ts` — remove `cacheable` from operation/capability types and discriminated unions. Do not touch unrelated npm packages whose names contain `cacheable`.

The C++ and Rust implementations must expose the same operation catalogue shape after removal. If Rust remains intentionally stale during this migration, record the deliberate exception in the paired test plan rather than preserving the field in the target C++ contract.

### Test and documentation cleanup

Add a catalogue contract asserting that `cacheable` is absent from operations and that all operations still execute through the same cache-registration path. Update cache, catalogue, MCP, and operation documentation that describes opt-in cacheability. Search the repository after the change; remaining matches should be limited to historical plans/changelogs or unrelated dependency names, and those exceptions must be documented.

---

## Target invariants

1. `ARTIFACT_INVENTORY` owns published analytical output data and immutable artifact identity.
2. The cache stores no duplicate analytical payload.
3. A normal workflow connection is logical: source operation instance/port to target operation/port.
4. A normal connection never requires a historical artifact ID.
5. For a given active workflow, resolution means the latest valid artifact for the source node's current effective computation—not simply the newest artifact globally.
6. Execution, single-operation calls, cache fingerprinting, frontend current-output display, and MCP result reporting use the same core resolution semantics.
7. Execution history records the exact artifact IDs consumed and produced.
8. Cache hits reuse the existing artifact IDs and do not invoke plugin computation or publish duplicate artifacts.
9. A stale, missing, invalid, or incomplete cache reference is a cache miss, never a successful result.
10. Every operation uses the same cache-registration and artifact-reference framework; operation metadata does not opt out of the framework.
11. Artifact lineage describes original production; cache-hit execution history describes later reuse without copying artifacts.
12. Do not introduce a legacy fallback, dual execution path, transitional cache adapter, or operation-level cacheability branch.

---

## Data model

### Canonical computation fingerprint

Add one core-owned, versioned fingerprint function. Its input must include:

```text
fingerprint_schema_version
canonical_operation_id
operation/plugin implementation version
normalized parameters
ordered (input port, resolved artifact/content identity) pairs
relevant external-state identity, when applicable
```

Do not include node coordinates, display name, canvas zoom, execution ID, timestamps, or workflow revision by itself. Parameter canonicalization must sort object keys, preserve meaningful array order, normalize explicit nulls, and follow declared semantics for unordered collections and file/path inputs.

Use a stable representation such as:

```text
streamfind.compute/v1:<hash>
```

The first implementation may use immutable artifact IDs as input identities if artifact IDs are guaranteed stable for the represented content. If not, add a persisted content hash to artifact metadata and use it explicitly.

### Artifact-reference cache tables

Replace payload-bearing `CACHE.data` for the execution path with a core-owned reference index. Prefer a normalized design because operations can emit multiple outputs:

```sql
CREATE TABLE ARTIFACT_CACHE (
    fingerprint VARCHAR PRIMARY KEY,
    operation_id VARCHAR NOT NULL,
    implementation_version VARCHAR,
    fingerprint_version VARCHAR NOT NULL,
    status VARCHAR NOT NULL,
    created_at TIMESTAMP NOT NULL,
    last_used_at TIMESTAMP
);

CREATE TABLE ARTIFACT_CACHE_OUTPUT (
    fingerprint VARCHAR NOT NULL,
    output_port_id VARCHAR NOT NULL,
    ordinal INTEGER NOT NULL,
    artifact_id VARCHAR NOT NULL,
    PRIMARY KEY (fingerprint, output_port_id, ordinal)
);
```

Add indexes for fingerprint and artifact ID. Validate that every referenced artifact exists, is published/valid, has the expected contract and output port, and that the complete required output set is present before declaring a hit.

Do not add a compatibility cache layer. Replace the current payload-cache execution semantics at the owning boundary. If the old public cache commands remain during implementation, they must be redefined to report cache-index metadata rather than payloads in the same change, or removed according to the current API contract.

### Execution records

Extend the existing workflow execution row or its associated JSON to expose:

```text
fingerprint
cache_status: hit | miss
executed: boolean
resolved_input_artifact_ids
produced_artifact_ids
```

Retain `parameter_hash` only if it remains useful for diagnostics; it must not substitute for the full computation fingerprint.

---

## Canonical resolution semantics

### Logical connections

The persisted workflow connection should be sufficient to resolve an input:

```text
source_operation + source_port
        -> target_operation + target_port
```

`source_artifact_id` must not be required for ordinary workflow definitions. If the C++ field is retained for an explicitly approved historical/replay mode, it must be isolated from the normal path and never be emitted by the frontend's ordinary workflow model.

### Resolution algorithm

Implement or rename one core resolver with a precise contract, for example `resolve_operation_inputs(operation_instance)`:

1. Load the active workflow and target operation.
2. For each incoming connection, identify the source operation instance and source port.
3. Determine the source operation's effective parameters and resolved upstream inputs.
4. Compute or look up the source operation fingerprint.
5. Resolve the complete current output set for that fingerprint.
6. If no valid output exists and execution is allowed, execute the source operation first through the same executor/cache path.
7. Validate the selected artifact contract against the target port.
8. Return input descriptors containing artifact IDs, contract IDs, representations, and physical table names where needed.
9. Record the concrete selected IDs in the consuming execution record.

The current `resolve_workflow_inputs()` implementation chooses the greatest workflow revision and timestamp among published artifacts. Replace that selection rule with fingerprint-based current computation. Revision/timestamp ordering may remain a deterministic tie-breaker only inside an explicitly defined same-computation case.

A browsing operation must not change which artifact is current. Historical artifact inspection belongs to inventory/provenance APIs.

### Frontend and MCP

- `CanvasShell.tsx` should retain logical edges only.
- The output viewer should request the backend's resolved current artifact for `(operation_instance, output_port)` instead of independently filtering inventory with `.at(-1)`.
- Add a typed client/API method for current output resolution if the existing artifact endpoints do not provide it.
- MCP workflow construction must require only logical typed connections. Returned run results and provenance may expose concrete artifact IDs.
- `request_artifact` remains an explicit inspection operation; it must not become an input-binding mechanism.

---

## Cache execution algorithm

For every operation:

```text
inputs = resolve_operation_inputs(node)
key = fingerprint(operation, normalized_parameters, inputs)

cached = lookup_complete_valid_output_set(key)
if cached:
    record cache_hit, executed=false, consumed/produced artifact IDs
    return cached artifacts

claim key atomically
if another worker already completed the claim:
    re-read and return its complete artifact set

record cache_miss
execute operation once
publish all outputs atomically
register key -> output artifacts atomically
record executed=true and artifact IDs
return artifacts
```

There is no operation-level cacheability branch. Every operation computes a fingerprint and registers or resolves artifact references through the same framework. If an operation depends on external state, randomness, current time, or mutable files, that state must be included in the fingerprint or normalized into an explicit input identity; otherwise the operation contract is incomplete and must be corrected rather than bypassing the framework. Failed operations must never register a reusable successful mapping. Partially published output must be invalidated or rolled back before the operation is reported as failed.

Concurrent identical requests must not create competing canonical output sets. Use a unique fingerprint claim/transaction or an equivalent project-scoped lock; do not use an unprotected check-then-execute sequence.

---

## Implementation tasks

### Task 0: Remove `sf:cacheable` and cacheability branches

**Files:**
- Modify: `cpp/core/semantic/vocabulary.ttl`, `cpp/core/semantic/fields.ttl`, `cpp/core/semantic/shapes.ttl`
- Modify: `cpp/plugins/mass_spec/semantic/operations.ttl`
- Modify: C++ catalogue/model files listed in the removal section above
- Modify: Rust catalogue/model/test files and `frontend/src/backend/protocol.ts`

Write the catalogue contract that expects no `cacheable` field, remove the ontology property and all generated/native/frontend/Rust representations, then regenerate the catalogue. Remove every execution conditional based on the flag before implementing the universal cache path. Verify with a repository search that remaining matches are only this plan, historical documentation explicitly retained, or unrelated dependency names.

### Task 1: Capture current contracts and add failing tests

**Files:**
- Modify: `cpp/tests/unit/project_lifecycle_contract.cpp`
- Modify: `cpp/tests/unit/artifact_publication_contract.cpp`
- Modify: `cpp/tests/unit/mixed_domain_workflow_artifact_contract.cpp`
- Create or modify: a focused cache contract under `cpp/tests/unit/`

Add tests that initially fail for:

- two identical executions reusing one artifact ID;
- a changed parameter producing a different fingerprint;
- a changed upstream artifact invalidating only downstream computations;
- a logical connection resolving the latest artifact for the current source fingerprint;
- an old artifact not being selected merely because it remains in inventory;
- missing/stale cache output causing a miss;
- complete multi-output hit versus incomplete output miss;
- execution history recording cache hit/miss and exact artifact IDs;
- two concurrent identical requests producing one canonical output set.

Run the narrow contract targets and confirm failures are caused by missing cache/resolution behavior, not fixture setup.

### Task 2: Implement canonical fingerprinting

**Files:**
- Add a core-owned helper in `cpp/core/include/streamfind/` and `cpp/core/src/`
- Modify: `cpp/core/src/project.cpp`
- Modify: operation/catalogue structures if implementation-version metadata is not exposed

Implement canonical JSON normalization and the versioned fingerprint function. Add unit coverage for key order, parameter changes, input identity changes, UI-only workflow changes, and implementation-version changes.

### Task 3: Replace payload cache with artifact-reference storage

**Files:**
- Modify: `cpp/core/src/project.cpp` project schema initialization
- Modify: `cpp/core/include/streamfind/project.hpp`
- Modify: `cpp/core/src/api.cpp`
- Modify: `cpp/core/src/mcp.cpp` if cache discovery/output schemas expose payload semantics

Create the reference tables and lookup/register/invalidate operations. Ensure referenced artifacts are validated and that cache index deletion never deletes artifact data. Make public cache inspection return references and metadata, not serialized analytical payloads.

### Task 4: Integrate cache lookup into operation-graph execution

**Files:**
- Modify: `cpp/core/src/project.cpp` (`run_operation_graph`, `run_operation`, execution-row helpers)
- Modify: `cpp/sdk/src/dynamic_plugin_manager.cpp` only where output publication needs an atomic/cache-aware boundary

Resolve inputs before fingerprinting, perform cache lookup, claim misses safely, reuse complete output sets, publish/register misses atomically, and record exact execution outcomes. Preserve plugin ownership of computation while keeping persistence in core.

### Task 5: Make current-artifact resolution core-authoritative

**Files:**
- Modify: `cpp/core/src/project.cpp`
- Modify: `cpp/core/include/streamfind/project.hpp`
- Modify: `cpp/tests/unit/mixed_domain_workflow_artifact_contract.cpp`
- Modify: workflow serialization/validation code if `source_artifact_id` is still required

Remove historical selection from the normal resolver. Use the source operation's effective fingerprint and current output set. Keep explicit artifact IDs only in execution/provenance data or a separately specified replay contract. Ensure table and structured-result inputs follow the same path.

### Task 6: Align service, MCP, and frontend inspection

**Files:**
- Modify: `cpp/service/src/project_runtime_manager.cpp` and service headers/routes as needed
- Modify: `frontend/src/backend/StreamFindApiClient.ts` and protocol types
- Modify: `frontend/src/app/CanvasShell.tsx`
- Modify: frontend tests if present

Add a current-output query or equivalent backend response. Replace frontend inventory sorting/filtering for current output with that query. Keep historical inventory browsing and targeted artifact retrieval explicit and read-only. Verify MCP-created and frontend-created workflows produce identical logical connections and resolution results.

### Task 7: Remove obsolete payload-cache paths

**Files:**
- Modify/remove owning cache APIs in `cpp/core/include/streamfind/project.hpp`, `cpp/core/src/project.cpp`, `cpp/core/src/api.cpp`, and `cpp/core/src/mcp.cpp`
- Modify: tests, README, and public documentation that describe cache blobs

After all callers use the artifact-reference index, remove payload-cache writes and any duplicate execution path. Do not retain a speculative legacy fallback or dual-read compatibility layer.

### Task 8: Add cleanup and retention rules

**Files:**
- Modify: project maintenance/history-clearing implementation
- Modify: artifact/cache contract tests

Define how workflow-history clearing, artifact retention, cache-reference invalidation, and provenance retention interact. Add a maintenance operation that removes orphaned cache references without deleting retained artifacts. Ensure a historical execution can still report the exact artifact IDs it consumed.

---

## Validation matrix

Run from the repository root using the repository's `.venv` only for Python tooling and MinGW-w64 UCRT64 for native builds.

### Focused C++ validation

```text
cmake --build <active-cmake-build> --target <cache/resolution-contract-target>
ctest --test-dir <active-cmake-build> -R "cache|artifact|workflow" --output-on-failure
```

The paired C++/Rust test policy applies if an official Rust counterpart is introduced or materially changed. Do not add a redundant backend-specific test solely to increase counts.

### Full native validation

```text
cmake --build <active-cmake-build> --target all
ctest --test-dir <active-cmake-build> --output-on-failure
```

On Windows, use CMake + Ninja + `C:/msys64/ucrt64/bin/gcc.exe` and `g++.exe`; stop running service/plugin processes before relinking DLLs.

### Frontend validation

```text
cd frontend
npm run lint
npm run build
```

Use the live browser only to verify that logical connections display current outputs and that no manual input-artifact resolver appears. Do not treat browser-side inventory ordering as proof of backend resolution.

### MCP/service validation

- Start the service from the built/package-relative entry point.
- Call `initialize` and `tools/list`.
- Create a project and persist a complete operation graph with logical connections only.
- Run the graph twice and verify the second run reports cache hits, the same artifact IDs, and no duplicate output artifacts.
- Change one upstream parameter and verify only the affected downstream subgraph recomputes.
- Use `request_artifact` only to inspect returned IDs/data; do not use it to bind an input.
- Verify frontend and MCP runs select the same current artifact for the same project state.

### Release validation

Use the authoritative Windows release entry point only after focused and full tests pass:

```text
scripts/release/cpp/release-cpp.ps1 -Version <version> -Config Release
```

Stop running StreamFind/Vite processes first. Verify the extracted archive with the packaged MCP smoke test, compare every archive against `tmp/release-output/sha256sums.txt`, and report exact hashes. Local packaging is not publication; do not tag, push, or publish without explicit instruction.

---

## Risks and decisions

- **Current artifact versus newest artifact:** newest `created_at` is not sufficient. The source's effective fingerprint is authoritative; timestamps are only deterministic tie-breakers.
- **Artifact IDs versus content hashes:** artifact IDs are acceptable only if immutable and content-stable. Otherwise persist and hash content identity.
- **Revision cleanup:** deleting old revision artifacts can invalidate reusable cache results. Define retention and cache-reference behavior before enabling aggressive cleanup.
- **Multi-output operations:** a cache hit requires the complete output set, not one convenient output.
- **External inputs:** paths, remote resources, random seeds, and current-time dependencies need explicit identity in the fingerprint; they must not create an untracked bypass path.
- **Concurrency:** the project database must serialize claims for the same fingerprint.
- **Historical reproducibility:** normal connections remain logical, but execution records must retain exact consumed artifact IDs.
- **No compatibility scaffolding:** because this repository is in an active refactor, implement the target boundary directly rather than preserving the payload cache as a fallback.

---

## Acceptance criteria

**Status: complete for the C++-first target architecture.** The implementation and
verification below satisfy the criteria; Rust-side legacy cacheability remains
outside this plan's deferred backend scope.

The work is complete when all of the following are true:

- An unchanged deterministic operation invoked twice executes once and returns the same artifact ID on the second invocation.
- The cache stores only fingerprint/output-artifact references and metadata, never a duplicate analytical payload.
- A parameter or effective input change changes the fingerprint and recomputes only the affected downstream subgraph.
- UI-only workflow changes do not invalidate analytical results.
- A normal logical connection resolves the current valid upstream artifact automatically; no frontend input-artifact resolver is required.
- The frontend, MCP, single-operation path, and graph executor share the same core resolver.
- Historical artifacts remain inspectable but cannot silently become normal current inputs.
- Execution history records cache status, exact input artifact IDs, and produced artifact IDs.
- Missing, invalid, or incomplete references safely cause recomputation.
- Concurrent identical requests do not create competing canonical output sets.
- Failure and partial-publication paths cannot register reusable successful cache mappings.
- History cleanup and cache-reference maintenance preserve required provenance.
- The focused tests, full CTest suite, frontend checks, packaged MCP smoke test, and checksum verification pass.
