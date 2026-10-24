# Service-Owned Result Runtime and Unified Execution Plan

## Status

The service discovery/control-plane, supervised worker, service data-proxy,
typed output-sink, and Result Runtime slices are implemented. The result store
creates the project `.data` sidecar, allocates isolated staging leases,
promotes immutable payloads, and records deterministic payload size and digest
metadata. Workers execute through the service boundary and do not open the
project DuckDB; no fallback runtime is being added. The service now exposes
checksum-validated lookup by
`computation_key`. The project schema now uses the canonical result tables
`RESULT_REGISTRY`, `RESULT_LINEAGE`, `RESULT_CACHE`, and
`RESULT_CACHE_OUTPUT`; the core registry inspection commands now use
`get_result_registry` and `get_current_result_registry`. Remaining legacy
payload field names are migration targets, not a second persistence model. The
versioned data-proxy envelope and service endpoint are implemented. The API
result-data path now reads tables through the service and preserves
JSON result payloads, and operation-input resolution now runs through the
service-owned project session.
The service result inventory now also includes integrity-validated manifests
from the `.data/results` store, so worker-published typed results are visible
without requiring a DuckDB registry row.
Service cache lookup now validates and returns all matching result manifests
for a computation key, rather than assuming one cached payload per operation.
This supports complete multi-output cache reuse; runtime cache registration and
hit selection are the next step before removing the legacy DuckDB mappings.
The workflow scheduler now forwards its deterministic computation key to the
worker. Service-owned dynamic table and JSON result descriptors record the
key, output port, and cacheability, establishing the provenance needed for
service-side cache registration and reuse.
Service-owned workflow scheduling now queries the result store by that key,
accepts complete multi-output result sets as cache hits, forwards their
`result_id` references downstream, and skips legacy DuckDB cache writes for
those runs. The remaining direct-operation cache path is the next boundary.
The operation execution context no longer exposes the `Project` facade. Direct
execution now receives only the narrow host services it needs: table-domain
preparation, a scoped table transaction, a database-path query, and runtime
logging. Dynamic plugins therefore use the same context boundary for direct and
service-owned execution, while the project remains private to the host
implementation. The native build and all 26 CTest contracts pass after this
slice.
Core operation queries and statements now use the data proxy whenever they run
inside a worker. The service handles the corresponding `query` and `execute`
requests against its project session, so worker-side in-memory project state
cannot be mistaken for the real project database. The remaining proxy slice is
to replace the SDK's direct-mutation branches and add streaming/batch coverage
for large result transfers.
The SDK mutation boundary is now proxy-capable as well: table clears and
appends use service requests, while update/delete operations send validated
statements and obtain affected-row counts through the service. The service owns
the resulting DuckDB writes. The next implementation slice is replacing JSON
row batches with a bounded binary/Arrow transport and adding explicit proxy
integration coverage for these mutation paths.
The proxy now transports bounded row batches as base64-wrapped Arrow IPC
streams inside the existing control envelope. Each stream contains typed
Arrow columns for booleans, integers, floating-point values, and JSON-backed
string/structured values, preserving the current schema-neutral plugin ABI
while avoiding JSON serialization for ordinary scalar cells. Worker-side reads
use this for ordinary tables and result-store tables, and the native contract
test covers mixed-type, null, and structured-value round trips. Table outputs
now use one runtime-owned `write_rows` sink operation: the operation supplies
typed rows, the sink encodes bounded Arrow IPC frames, and the service owns
forwarding, staging, publication, and result registration. Published Arrow
batch results are readable through the existing table query and binary batch
interfaces. The next step is end-to-end service/worker coverage and replacing
the remaining legacy result handles and direct-output code with the same
result-oriented boundary.
The native `streamfind_result_store_arrow_contract` now exercises the complete
service-boundary path: result-store initialization, sink allocation, framed
Arrow writes, publication, typed reads, and computation-key lookup.
Direct operation execution now uses the same runtime-owned table output sink
as worker execution; the service-owned flag remains separate so local
transactions are not mistaken for service execution. The table contract
coverage now consumes emitted result references instead of requiring legacy
`artifact_id` lookup rows.
Workflow execution now accepts an explicit `execution_mode` (`detailed` or
`pipeline`) through the service, SDK client, and MCP run command. In detailed
mode every output is published. In pipeline mode only terminal operation
outputs are published; intermediate table outputs use the same typed Arrow
sink but are held as run-scoped transient results in the service and are
resolved directly from the current graph run. Transient results are removed
when the workflow finishes or is cancelled, and are never added to the
published result registry. The next slice is to add end-to-end workflow
coverage for transient cleanup and expose the mode in the app controls.

### Completion audit — 2026-10-24

The plan is **partially complete**. The following slices are implemented and
have native contract coverage:

- service discovery, attachment, transparency, and the public `streamfind
  service` command surface;
- supervised worker execution with process cancellation and worker metadata;
- project `.data` result-store creation, staging leases, immutable promotion,
  checksums, manifests, and typed result descriptors;
- service-owned data-proxy requests for table inspection, bounded reads,
  mutations, JSON results, and format-neutral binary ranges;
- runtime-owned output sinks and bounded Arrow IPC contract tests;
- worker startup from a service-supplied workflow snapshot without a usable
  project database path;
- operation execution through `OperationExecutionContext` rather than the old
  public `Project&` executor contract;
- service-side workflow input resolution and computation-key lookup;
- API and MCP attachment paths using the same service session and workflow
  endpoints;
- explicit `detailed` and `pipeline` execution modes in the service runtime.

The following areas are **implemented only in a bootstrap or partial form**:

- some dynamic-plugin table output still uses bounded JSONL; Arrow is available
  for the proxy contract, but the complete large-table production path must
  still be converted and benchmarked;
- cache lookup and multi-output hit selection exist, but complete cache
  registration, claim coordination, invalidation, garbage collection, and
  crash recovery are not complete;
- direct host execution still retains a service-private project/table-store
  adapter;
- pipeline scheduling and transient cleanup exist, but end-to-end coverage and
  frontend/MCP run-mode controls remain;
- new interfaces use result terminology, but legacy names such as
  `artifact_id`, old registry tables, and some frontend/internal paths still
  require final migration.

The following work is **still required before declaring the refactor complete**:

1. Remove the remaining direct host/table-store execution path and migrate all
   plugin domains and operation emitters to the service-owned result sink.
2. Replace the remaining JSONL large-table path with bounded Arrow/Parquet
   transport, including backpressure, size limits, and large-dataset tests.
3. Finish cache registration, idempotent claims, provenance invalidation,
   recovery, retention, and concurrent identical-run tests.
4. Complete pipeline-mode end-to-end tests, endpoint-only publication, and
   transient-result cleanup after success, failure, cancellation, and restart.
5. Implement sidecar locate/attach, fork/clone, degraded metadata-only open,
   explicit reset, and startup reconciliation for copied or damaged projects.
6. Remove obsolete artifact-facing schema/API/frontend names after the
   migration boundary is complete; do not add compatibility fallbacks.
7. Add full app/MCP shared-run, event replay, inspection-during-run, parallel
   branch, and no-DuckDB-worker tests.
8. Finish Windows, Linux, WSL, packaged-build, forced-cancellation, and clean
   release validation.

The current native build and focused result/data-proxy contract tests pass,
but that is not yet evidence that the complete multi-domain workflow and
release acceptance criteria pass.

### ABI audit — 2026-10-24

A targeted source audit confirmed that the worker executable does not include
or construct `ProjectTableStore`, does not receive a usable project database
path, and routes operation reads, writes, and output publication through the
service data proxy. Plugin operation sources do not expose `Project&` or
`ProjectTableStore`; they use the operation execution context. The worker had
one unused direct table-store include, which was removed.

The remaining `ProjectTableStore` references are host-side project/schema
management and direct in-process test infrastructure. They are not available
to worker execution, but remain a planned removal target for the final direct
host-execution cleanup. The remaining JSONL result readers are required for
currently published bootstrap results; they should be removed only after the
Arrow/Parquet migration and result migration are complete.

Validation after this audit: the worker and service rebuilt successfully, and
the data-proxy, output-sink, Arrow result-store, plugin ABI, dynamic-schema,
and execution-lifecycle contracts all passed.

## Objective

Refactor StreamFind so that:

1. The service is the single owner of project metadata, result registration,
   workflow state, and DuckDB access.
2. Workers never open the project DuckDB or compete with the service over its
   file lock.
3. Large result payloads are stored outside DuckDB in a sibling data folder
   named from the project database, for example:

   ```text
   ACC1_27803.duckdb
   ACC1_27803.data/
   ```

4. Workers can produce immutable result payloads concurrently, including for
   parallel workflow branches, without exposing incomplete results.
5. The app and MCP use exactly the same service runtime interface.
6. A running operation does not block inspection of the workflow, metadata, or
   previously published artifacts.
7. Workflow execution can run in either detailed mode, which retains
   intermediate artifacts, or pipeline mode, which retains only declared
   endpoints and removes run-scoped intermediates.

## Result terminology

The canonical term for data produced by an operation or workflow is **result**.
It is broad enough to cover tables, JSON, images, logs, plots, and binary
outputs, while avoiding confusion with an operation's declared output port.

Use these terms in new APIs, UI, documentation, and implementation code:

| Concept | Canonical term |
|---|---|
| Concrete data produced by an execution | Result |
| Large or persistent result payload location | Result store |
| DuckDB metadata and registration | Result registry |
| Result used only within one run | Transient result |
| Result retained as a user-visible project output | Published result |
| Declared operation interface | Output port |
| Result selected as a workflow endpoint | Endpoint result |

New identifiers should use `result_id`, `result_store`, `result_registry`,
`result_manifest`, and `result_handle`. New event and API payloads should use
`result` terminology rather than introducing more `artifact` names.

This rule applies to new code, HTTP paths, MCP tools, CLI commands, events,
UI labels, and documentation. `artifact` is retained only in unmigrated
legacy schemas or explicitly documented migration references; new code must
not introduce artifact terminology.

Existing persisted schemas and clients may contain names such as
`artifact_id`, `ARTIFACT_INVENTORY`, or `artifact_query`. These are legacy
storage/API fields to be migrated or translated at the boundary; they are not
a second execution model. The migration must define the canonical result
registry and remove old names from new generated schemas and user-facing
documentation when the transition is complete.

## Non-goals

- Do not preserve the current worker-local DuckDB execution path as a fallback.
- Do not add plugin-specific cancellation requirements.
- Do not make the frontend or MCP implement scheduling or artifact publication.
- Do not force JSON, images, or other non-tabular outputs into Parquet.
- Do not make workers write arbitrary files directly into the final project
  artifact namespace.

## Target architecture

```text
                    HTTP / event stream
Frontend  ───────────────────────────────┐
                                         │
MCP adapter ─────────────────────────────┤
                                         ▼
                              StreamFind service
                              ├─ project sessions
                              ├─ workflow scheduler
                              ├─ worker supervisor
                              ├─ result store
                              ├─ DuckDB metadata/catalogue owner
                              └─ event broker
                                         │
                              IPC data/runtime protocol
                                         │
                         operation workers and plugins
```

The service owns the only DuckDB handles. A worker receives an operation
context and uses a data proxy for reads, writes, result publication, progress,
and logs. The plugin ABI remains the operation boundary, but its data-service
implementation becomes proxy-backed instead of opening DuckDB in the worker.

## Service discovery, attachment, and transparency

The app-launched service is a shared local control plane, not an invisible
private process. The service writes a short-lived, authenticated discovery
record when it starts and removes or invalidates it during a clean shutdown.
The record contains:

- service instance ID and protocol version;
- loopback HTTP/WebSocket endpoint or named-pipe endpoint;
- process ID, start time, version, and capability summary;
- authenticated local connection token or equivalent capability;
- currently attached project identities and active run IDs;
- health and shutdown state.

The record must be stored in an OS-appropriate per-user runtime location, with
permissions restricted to the current user. It is a locator and handshake
mechanism, not a substitute for service authentication. A stale record is
detected by a health handshake and process-liveness check before it is shown
or reused. The service must never be discovered only by guessing a port.

### App visibility

The app displays whether its service is starting, ready, degraded, stopping, or
unavailable. A service details action shows the instance ID, endpoint type,
process ID, version, start time, attached projects, active runs, and the
connection mode. The project UI also shows which service instance owns the
current project session, so the user can tell when an MCP client is attached
to the same runtime.

### MCP attachment

The MCP adapter discovers the active service record or receives an explicit
endpoint/token handoff. It performs a handshake, verifies the protocol and
service instance, then attaches by canonical project identity. The service
returns a project session ID and current workflow revision. MCP never opens
the DuckDB file, reads the result folder directly, or starts a duplicate
service for an already-owned project.

If no compatible service is available, MCP reports that clearly and may request
the app or a standalone service to start one. If a project is owned by another
service instance, the response identifies that instance and offers attach or
an explicit conflict; it does not silently create a second owner.

### CLI inspection and lifecycle management

The distribution should provide transparent service-management commands with
machine-readable output, for example:

```text
streamfind service list
streamfind service status
streamfind service status --json
streamfind service stop --instance <service-instance-id>
```

`streamfind` is the public terminal command. With no arguments it keeps its
existing role as the application launcher; with the `service` subcommand it
forwards to the packaged service-management implementation. The internal
`bin/streamfind_cli` executable is an implementation detail and must not be
presented as the user-facing command in documentation, help text, or support
instructions.

`list` shows all discoverable local services and whether each responds to a
health handshake. `status` shows endpoint, PID, version, attached projects,
active runs, worker count, and stale-record diagnostics. `--json` is intended
for MCP, scripts, and support diagnostics and must not expose database
credentials or long-lived secrets.

`stop` requests a graceful service shutdown through the authenticated control
API. The service first stops accepting new commands, reports its stopping
state, allows active workers to finish or be cancelled according to policy,
releases project sessions, and then removes its discovery record. A force
option, if provided, must be explicit, restricted to local authorized users,
and clearly warn that active runs may be cancelled and require recovery.
Stopping a service must not delete project results or metadata.

The CLI must distinguish these outcomes: service not found, stale discovery
record, healthy service, shutdown requested, shutdown completed, and shutdown
blocked by active work. It should return stable exit codes for each category.

### Discovery and lifecycle tests

- app startup creates a discoverable healthy service record;
- MCP attaches to the app-owned service and receives the same project session;
- a second service cannot silently claim an already-owned project;
- stale records are detected and cleaned without terminating unrelated
  processes;
- CLI list/status output exposes endpoint and ownership information;
- JSON status output is stable and excludes secrets;
- graceful stop refuses or coordinates active runs according to policy;
- forced stop leaves recoverable run metadata and no live discovery record;
- service restart creates a new instance ID and does not reuse stale state.

## Design corrections and pitfalls

The initial concept is sound, but several boundaries must be explicit before
implementation. These are architectural invariants, not optional optimizations.

### 1. File publication and metadata registration are separate transactions

A filesystem rename and a DuckDB commit cannot be one atomic transaction. There
are two crash windows:

- the payload is published but the service crashes before registration;
- the metadata row is committed but the payload is missing or incomplete.

The artifact lifecycle therefore needs a durable `staging`, `sealed`, and
`published` state plus a startup reconciler. Registration must include the
payload checksum and the final relative path. A published row is usable only
when the checksum and file metadata still validate. Orphan payloads and orphan
metadata rows must be recoverable without guessing.

### 2. The `.data` folder is a storage convention, not the project identity

The sidecar path must be derived from the canonical database path and must not
be based on a user-editable display name. The service must reject ambiguous
paths, handle case-insensitive filesystems, and define behavior when a legacy
`.data` path already exists. Database moves and copies need an explicit policy:
either the sidecar moves with the database or the project is opened as
metadata-only until the sidecar is located or repaired.

### 2a. Database copies need explicit fork semantics

Copying `name.duckdb` to `copy.duckdb` does not copy `name.data`, and blindly
deriving `copy.data` would make the copied project appear to have no payloads.
The database must therefore contain a stable project identity and an artifact
store identity. On open, the service checks both identities and the sidecar
layout manifest before exposing artifacts.

The service must distinguish these cases:

| Situation | Default behavior |
|---|---|
| Expected sidecar exists and matches | Open normally |
| Sidecar is missing | Open in degraded metadata-only state; do not delete registry rows |
| Sidecar identity mismatches | Refuse attachment until the user chooses an action |
| Payload is missing but registry row exists | Mark artifact unavailable and retain metadata |
| User chooses reset | Explicitly archive/clear unavailable registrations |

The UI and API should offer:

- **Locate/attach** an existing compatible sidecar;
- **Fork project** by copying the sidecar into a new sibling store and assigning
  the copy a new project/store identity;
- **Open metadata only** while preserving the registry for later repair;
- **Reset artifact registry** as an explicit, confirmed, destructive action.

The service must never silently attach the original project's data store to a
copied database, because two projects could then publish into the same artifact
namespace. A normal filesystem copy should therefore be documented as a
metadata-only copy unless the `.data` directory is copied through the fork or
clone workflow.

### 3. “One project, one workflow” does not mean “one project, one run”

The project stores one current workflow definition, but it may have historical
runs, diagnostic runs, and a future need for concurrent read-only runs. Every
artifact, staging directory, event, and lease must therefore be scoped by
`project_id` and `run_id`, not only by the workflow or operation instance.

The first implementation may allow one mutating run per project, but this must
be an explicit scheduler policy rather than an accidental global lock.

### 4. The worker must not receive a usable DuckDB path

Passing the database path to a worker while relying on the worker not to open
it is not ownership isolation. The worker request must contain service and run
capabilities, not the project database path. The worker must not construct
`Project::open`, `ProjectTableStore`, or any other project-DuckDB owner.

### 5. Worker writes still require service-issued leases

Allowing a worker to write directly to the final `.data` directory would avoid
DuckDB contention but introduce path collisions, partial-file visibility,
symlink/path traversal risks, and stale files after termination. Workers write
only to unique service-issued staging leases. The service validates and
promotes them; workers never choose final artifact paths.

### 6. Inspection must not share the worker’s transaction

The service must commit artifact metadata in short transactions and keep
workflow state in a service-owned run registry. A worker's long computation or
streaming write must never hold a DuckDB transaction open. Inspection requests
must use independent read operations and must remain available while another
worker is active.

### 7. Streaming and durable payloads are different paths

Arrow IPC is a transport or temporary exchange format; Parquet is a durable
columnar payload. The plan must not require every result to be materialized as
Parquet before a downstream node can start. Small results may stream directly
through the proxy, while large results may be sealed as Parquet and referenced
by a handle.

### 8. Validation must be content-based and bounded

The service cannot trust a worker-reported schema, size, or checksum. It must
validate the sealed payload using bounded resource limits, canonical metadata,
and a checksum calculated from the closed file. Validation must avoid executing
arbitrary code or following paths outside the assigned staging root.

### 9. Cleanup needs retention and quotas

Transient output cleanup cannot depend only on the normal completion path. The
service needs run retention, maximum staging size, abandoned-run detection,
startup cleanup, and a user-visible diagnostic policy. Detailed artifacts need
an explicit retention/deletion policy as well.

### 10. The current synchronous HTTP operation route must become asynchronous

The existing operation route waits for `run_operation` to return. A unified
runtime API should acknowledge a run quickly and expose status/events separately.
Otherwise MCP and the frontend will have different timeout and cancellation
behavior even if they call the same endpoint.

### 11. Event replay needs a durable cursor contract

An in-memory event list is insufficient across service restarts. Events need a
monotonic per-project/run sequence, timestamps, terminal state, and a replay
cursor. On restart, clients must be able to distinguish replayed events from
new events and learn whether a run was recovered, failed, or cancelled.

### 12. MCP access needs an explicit security boundary

The common runtime API should work for the local app and MCP, but MCP must not
automatically gain unrestricted filesystem access. Project capabilities,
artifact read permissions, run control, and external path access must be
validated by the service. Artifact responses should use opaque handles or
relative URIs rather than exposing arbitrary host paths unless explicitly
requested by a trusted local client.

### 13. Existing artifacts require a migration and compatibility policy

Current projects contain artifacts whose payloads may live in DuckDB tables.
The new store needs a read-only legacy representation during migration or an
explicit conversion command. The migration must be resumable, checksummed, and
must not delete the original table until the external payload is validated.

### 14. Backups must include both halves of a project

Backing up only `name.duckdb` will lose external payloads. Export, copy, and
restore operations must treat the database and `name.data` as one project
bundle, with a manifest/version that can detect mismatched halves.

### 15. Parquet readers must not see files before sealing

Readers may open a file while a worker is still writing it unless the service
uses staging paths that are inaccessible to readers and promotes only after the
writer closes the file. Rename semantics differ across Windows and POSIX, so
the promotion protocol must be tested on both and must not overwrite an
existing artifact.

## Non-negotiable invariants

The implementation should enforce these invariants in code and tests:

1. Only the service opens the project DuckDB.
2. Only the service creates final artifact registrations.
3. A payload is immutable after publication.
4. No reader can resolve a staging or incomplete payload as published.
5. Every artifact belongs to exactly one project and run.
6. Every worker write is covered by a service-issued lease.
7. Every terminal run state has deterministic cleanup/recovery behavior.
8. App and MCP use the same request, response, event, and authorization model.
9. Historical inspection does not wait for a worker computation transaction.
10. A project backup/restore includes DuckDB metadata and the sidecar store.

## Project storage layout

For a database at `/path/name.duckdb`, the service derives the sibling data
directory `/path/name.data`. The derivation must be centralized and use the
canonical database filename stem, not an arbitrary project display name.
Existing projects without a data directory receive it lazily when the first
external payload is published. The service must record the storage layout
version and verify that the database and sidecar belong together when opening
or restoring a project.

Suggested layout:

```text
name.duckdb
name.data/
  results/
    <artifact-id>/
      manifest.json
      payload.parquet
  runs/
    <run-id>/
      staging/
      transient/
      manifests/
  locks/
```

The service owns the directory. Workers receive service-issued staging leases
and may write only within the assigned run and artifact staging directory.
Final artifact paths are never worker-selected.

## Artifact model

DuckDB stores authoritative metadata and registration, while payload bytes are
stored in the data directory.

### Payload formats

| Artifact kind | Payload format |
|---|---|
| Table | Parquet |
| Streaming table batch | Arrow RecordBatch / Arrow IPC |
| JSON | Canonical UTF-8 JSON |
| Log | UTF-8 text or JSON Lines |
| Image | PNG, SVG, or another registered image MIME type |
| PDF | PDF |
| Arbitrary binary | Binary blob with an explicit MIME type |

The renderer is selected from registered artifact metadata, never inferred
solely from a filename extension.

### Artifact lifecycle

```text
allocated → writing → sealed → validated → published
                                      └→ failed / abandoned
```

1. The service allocates an artifact ID, run ID, format, schema contract, and
   staging location, and records a durable staging lease.
2. The worker writes the payload to its isolated staging location.
3. The worker closes the payload and reports size, checksum, schema, and
   content metadata.
4. The service validates the payload against the declared output contract.
5. The service atomically promotes the payload into the final artifact folder
   without replacing an existing payload.
6. The service registers the published artifact in DuckDB in a short metadata
   transaction, including checksum, byte size, relative path, and storage
   layout version.
7. Readers resolve only metadata rows in the valid `published` state and then
   verify the referenced payload before exposing it.

If validation or execution fails, the service marks the staging result failed
and removes it during run cleanup. A partial payload is never registered. If the
service crashes between promotion and registration, startup reconciliation
either completes registration after validation or quarantines the orphan; it
does not silently treat the file as a valid artifact.

## Result identity and content-addressed reuse

The current cache and the current published-result registry are separate
concepts. `ARTIFACT_CACHE` and `ARTIFACT_CACHE_OUTPUT` store lookup metadata in
DuckDB: a fingerprint maps an operation output port to an existing published
artifact ID. The payload itself is currently still stored in the DuckDB-backed
artifact representation (for example, a table or JSON payload), rather than in
an independent cache blob. Clearing the cache removes these lookup mappings;
it does not remove published artifacts. Removing published artifacts is a
separate, explicit cleanup operation.

The new result store should preserve that distinction while moving payloads to
immutable files. A reusable result needs three identities:

| Identity | Meaning |
|---|---|
| `computation_key` | The normalized conditions under which an operation may be reused |
| `payload_digest` | The checksum of the sealed result payload and manifest |
| `result_id` | The logical registered result and its provenance |

`computation_key` is derived from canonical, semantically relevant inputs:

- operation identity, implementation/plugin version, and declared cache schema;
- normalized parameters and resolved input result identities or payload digests;
- catalogue, ontology, algorithm, and external-resource versions that affect
  the output;
- source-file identity and relevant imported-data versions;
- deterministic execution settings and the execution mode where it changes
  semantics.

It must not include run IDs, timestamps, node positions, UI-only workflow
layout, or other incidental metadata. A workflow revision is included only for
semantically relevant changes, not for a canvas move or display-only edit.
Operations that depend on current time, randomness, network state, or
unversioned external resources must be declared non-cacheable or must include
an explicit stable version in the key.

The service computes and validates `payload_digest` from the closed staged
payload; it never trusts a worker-reported checksum by itself. A cache hit is
valid only when every expected output is present, published, schema-compatible,
and checksum-valid, and the computation key and input identities still match.
The full parameters and provenance remain in the result registry rather than
being exposed through the hash.

On a valid hit, the service creates a new run/history reference but reuses the
immutable result handles. It must not duplicate the payload files or silently
rewrite a previous result. Multiple runs may therefore refer to one payload;
retention and garbage collection must protect it until no retained result or
run reference needs it.

Identical concurrent requests use a service-owned claim keyed by
`computation_key`. One worker computes while other requests wait for, attach
to, or receive the completed result. Claims have leases, bounded recovery, and
an abandoned-claim state so a crashed worker cannot block future execution.
Publication follows this order: stage and seal payload, calculate and validate
the digest, atomically promote the immutable payload, then commit the result
registry and cache mapping in a short transaction. Startup reconciliation
handles crashes between any two steps and never returns an unverified hit.

Detailed mode keeps reusable intermediate results published. Pipeline mode may
use content-addressed transient results during the run and promote only
declared endpoints; transient entries are removed when no run reference or
recovery record remains. Explicit cache clearing removes reuse mappings while
leaving published results available for inspection. A separate, explicit
retention/garbage-collection action may later delete unreferenced payloads.

For large outputs, the digest is calculated while sealing or by one bounded
sequential read of the closed file; the service must not copy a table merely to
hash it. Parquet footer/schema validation and streaming Arrow paths should be
benchmarked on native Windows, Linux, and WSL storage. The implementation must
also define whether numerical/platform differences are part of the operation
version or the computation key.

## Execution modes

Every operation and workflow run receives an explicit execution mode.

### Detailed mode

- Every operation output is sealed and published.
- Intermediate artifacts remain visible in the project inventory.
- Existing operation-by-operation behavior maps to this mode.
- Users can inspect each result after every step.

### Pipeline mode

- Intermediate results are run-scoped and stored under `name.data/runs/<run-id>`.
- Downstream workers consume transient result handles.
- Only declared workflow endpoint outputs are promoted to persistent artifacts.
- Transient results are removed after successful completion or cancellation.
- Run metadata and logs may be retained independently of payloads.

The service must not create a persistent artifact row for a transient result.
It may keep an internal run manifest so that recovery and cleanup remain
deterministic.

## Unified service runtime interface

The app and MCP must call the same service operations. MCP should be an adapter
that maps tool calls to this API; it must not invoke plugins or create workers
directly.

### Project and workflow operations

```text
open_project
close_project
get_project_state
get_workflow_definition
save_workflow
validate_workflow
start_workflow
get_workflow_run
cancel_workflow
```

### Operation operations

```text
run_operation
get_operation_state
cancel_operation
```

### Artifact operations

```text
list_artifacts
get_artifact_metadata
query_artifact
read_artifact_payload
get_artifact_stream
publish_artifact
```

### Event operations

```text
subscribe_to_project_events
subscribe_to_run_events
```

Every execution request should return a stable run or operation identifier,
for example:

```json
{
  "run_id": "run-123",
  "project_id": "ACC1_27803",
  "mode": "pipeline",
  "state": "queued"
}
```

The app and MCP then observe the same state and events. Starting a second
client does not create a second runtime for the same request.

## Shared execution semantics

### Idempotency

Submission requests accept a client-provided idempotency key. Repeating a
request returns the existing run instead of launching duplicate workers.

### Revisions

Workflow edits carry the workflow revision observed by the client. A stale
revision is rejected with a conflict instead of silently overwriting changes.

### Concurrent clients

- Multiple clients may read project state and historical artifacts while a run
  is active.
- Multiple clients may subscribe to the same run event stream.
- Cancellation is idempotent and may be requested by either client.
- Workflow mutation is serialized by the service and guarded by revision.
- A project may have at most one active mutation run unless an explicit future
  policy allows independent run scopes.

### App and MCP control-plane contract

The app and MCP are equal clients of the service. Neither client may open the
project database, read result files directly, schedule workers, or maintain a
second workflow state machine. MCP tools are adapters over the same service
commands and snapshot queries used by the frontend.

#### Snapshot reads

State reads return a coherent snapshot identified by a project revision or run
sequence. The service must expose snapshots for:

```text
get_project_state(project_id)
get_workflow_state(project_id, revision)
get_run_state(project_id, run_id)
get_operation_state(project_id, run_id, operation_id)
list_results(project_id, run_id, consistency_token)
get_result(result_id)
```

An active worker may continue producing a result while clients inspect the
workflow and previously published results. A result is included in a published
snapshot only after payload validation and registry commit. In-progress and
staging results are represented explicitly as runtime state, never as usable
published results.

#### Commands and conflict handling

All mutating requests use a common envelope containing `project_id`, a client
identity, an idempotency key, and the client's expected workflow revision where
applicable:

```json
{
  "project_id": "ACC1_27803",
  "command_id": "cmd-123",
  "idempotency_key": "mcp-session-7:run:hash",
  "expected_revision": 41,
  "type": "start_workflow",
  "payload": {"mode": "pipeline"}
}
```

The service is the traffic controller for each project. It serializes
workflow mutations and run-control commands, rejects stale revisions with a
conflict response, and records the accepted command before dispatching work.
Repeating an idempotency key returns the original command result and run ID;
it never launches a second worker. Commands such as cancellation are safe to
repeat and return the current state when the run has already completed,
failed, or been cancelled.

The app and MCP may therefore start, inspect, or cancel the same run. The
service records the initiating client but does not give that client exclusive
ownership of the run unless an explicit authorization policy requires it.

#### WebSocket event stream and MCP observation

The frontend subscribes to a project or run WebSocket with its last received
event cursor. The service first replays events after that cursor, then streams
new events in order. Each event contains a monotonically increasing
`sequence`, `project_id`, optional `run_id` and `operation_id`, event type,
timestamp, state, worker metadata, and result references. The client ignores
duplicate sequences and reconnects with the last acknowledged cursor after a
disconnect.

MCP uses the same event journal through subscription or wait-for-state tools.
It may request a snapshot followed by events after the snapshot's cursor, so
it cannot miss a transition between polling and subscribing. Event cursors
are scoped to the project/run and are opaque to clients. The service retains
enough history for reconnects or returns a fresh snapshot plus a new cursor
when the requested history has expired.

The service remains the sole publisher of lifecycle events. Workers emit
operation progress through the worker protocol, but they do not mutate the
project event journal directly. This gives the app terminal, WebSocket
consumers, and MCP tools the same ordered view of starts, progress, cache hits,
result publication, failures, and cancellation.

## IPC data proxy

The worker protocol must distinguish control events from bulk data. JSON is
appropriate for control messages, metadata, errors, and progress. It is not
appropriate for large tables.

### Control messages

Each message has:

```json
{
  "protocol": "streamfind.data/v1",
  "request_id": "req-123",
  "run_id": "run-123",
  "operation_instance": "operation-6",
  "type": "read_batch",
  "payload": {}
}
```

### Required proxy operations

- resolve input artifact handles;
- inspect table schemas and row counts;
- read selected columns in bounded batches;
- append or update output batches;
- create and seal typed output artifacts;
- emit operation logs and progress;
- report operation errors and completion;
- query cancellation state for graceful cleanup only.

Large batch data should travel through Arrow IPC frames or a service-issued
Parquet handle. The protocol must apply backpressure and enforce column,
row-count, and byte-size limits.

## Service-owned DuckDB access

Introduce an internal service database facade around the existing project
metadata and table-store code. Its responsibilities are:

- opening and closing DuckDB connections;
- short metadata and registration transactions;
- workflow state and revision updates;
- artifact inventory and manifest registration;
- read-only inspection queries while operations run;
- recovery of abandoned runs and staging leases.

The service must not hold a long write transaction while a worker is computing.
Payload writes occur outside DuckDB, and only publication metadata is committed
after the payload has been sealed and validated.

## Worker execution changes

The existing worker supervisor remains responsible for process isolation and
cancellation. It should be extended so that:

- the worker receives a service endpoint and run token, not a project database
  path for execution data;
- worker startup includes protocol version and capability negotiation;
- all plugin data callbacks use the proxy context;
- worker output is a typed result envelope or a completed artifact lease;
- worker termination releases leases through the service;
- no worker opens `Project::open()` for the project database;
- no worker-side `ProjectTableStore` is constructed for project data.

The transition must be atomic at the execution boundary. Do not retain a
worker-local DuckDB fallback after the proxy implementation becomes active.

## API and event payloads

All clients should receive the same lifecycle events:

```json
{
  "event": "worker.started",
  "run_id": "run-123",
  "operation_instance": "operation-6",
  "worker": {
    "id": "worker-abc",
    "process_id": 1234,
    "job_id": "job-xyz",
    "state": "running"
  },
  "timestamp": "..."
}
```

Artifact events should include the artifact ID, representation, persistence
mode, URI or opaque handle, status, and schema summary. The physical path may
be omitted from remote-facing responses if it is not needed by the client.

The event broker must retain a bounded event history per run so that a client
connecting after startup can replay events and reach a consistent view.

## Inspection while a run is active

The service must keep project inspection responsive while a worker runs:

- workflow definition and node state are read from service memory/metadata;
- published artifacts remain queryable;
- completed artifact payloads are readable immediately after publication;
- transient outputs are visible only through the owning run unless explicitly
  exposed for diagnostics;
- artifact queries use short-lived read connections or the service database
  facade, never the worker database;
- inspection requests must not wait for the active worker to finish except for
  a narrow metadata commit conflict.

## Recovery and cleanup

On normal completion:

- publish detailed-mode outputs;
- promote endpoint outputs in pipeline mode;
- remove transient staging data;
- mark the run complete;
- release worker leases.

On cancellation or failure:

- terminate the worker process tree;
- wait for worker exit;
- mark active leases abandoned;
- remove incomplete payloads;
- preserve diagnostics and failure events;
- release the project run lock;
- leave previously published artifacts intact;
- make the project immediately inspectable again.

On service restart, the recovery pass scans run manifests and staging folders,
then removes or reconciles entries that have no live service-owned run.

## Implementation sequence

### Phase 0 — Baseline and contract inventory

- Commit the current worker supervisor and worker executable as a reproducible
  baseline before changing execution semantics.
- Record the current HTTP, MCP, worker, plugin ABI, artifact, and workflow
  contracts.
- Identify all direct project-DuckDB opens in service, SDK, worker, CLI, and
  plugin runtime code.
- Add a design fixture describing detailed and pipeline execution modes.
- Define the project storage-layout version and the database/sidecar bundle
  identity rules before writing payload files.
- Define terminal run states, lease states, event sequence semantics, and
  recovery outcomes before changing the scheduler.
- Document the existing cache behavior and distinguish cache mappings,
  published results, payload storage, and explicit cache/artifact cleanup.

### Phase 1 — Result-store abstraction

- Add a service-owned `ResultStore` interface.
- Implement project `.data` path derivation and safe directory creation.
- Implement staging leases, atomic promotion, checksums, MIME/format metadata,
  and cleanup.
- Add typed payload support for Parquet, JSON, text, images, and binary blobs.
- Use the canonical DuckDB result tables (`RESULT_REGISTRY`,
  `RESULT_LINEAGE`, `RESULT_CACHE`, and `RESULT_CACHE_OUTPUT`) for new project
  schemas; do not create new `ARTIFACT_*` cache or inventory tables.
- Keep the current C++ method names and public legacy fields only until their
  result-oriented API boundary is replaced in a later atomic slice; they must
  not introduce another storage path.
- Define result-registry fields for `computation_key`, `payload_digest`,
  cacheability, provenance, and immutable result handles.
- Add content-addressed staging and promotion without making the hash depend
  on run identity or UI-only workflow changes.
- Expose service-side cache lookup only after manifest and payload digest
  validation; do not let workers decide cache hits.
- Add a resumable migration path for legacy DuckDB-backed table payloads and
  verify database/sidecar backup and restore as a pair.

### Phase 2 — Service runtime control plane

- Consolidate direct operation and workflow submission under one service runtime
  manager.
- Add run IDs, idempotency keys, revisions, execution mode, and event replay.
- Expose identical state and cancellation behavior to HTTP and MCP adapters.
- Make the frontend and MCP consume the same event and snapshot shapes.
- Acknowledge operation/workflow submissions asynchronously; do not make the
  HTTP request lifetime equal the worker lifetime.
- Add authorization and capability checks before exposing project paths,
  artifact reads, or run control through MCP.
- Move cache lookup, claim acquisition, hit validation, and cache-mapping
  commits into the service runtime; workers only compute and seal leased
  payloads.

#### Operation output contract

Operations do not choose whether an output is written to disk, forwarded to a
downstream operation, cached, or kept transient. They receive one runtime-owned
`OperationOutputSink` through `OperationExecutionContext`. An operation opens a
typed output descriptor, submits bounded encoded batches, and finishes or
aborts the output. Tabular batches will use Arrow IPC encoding; JSON, images,
models, MFT, and other binary payloads use the same bounded byte-batch
interface with format and media-type metadata. The service selects the
delivery policy from the run mode, output size, memory pressure, cache state,
and whether the output is a final endpoint. This keeps plugin code independent
of Parquet, filesystem paths, DuckDB, and downstream scheduling.

### Phase 3 — Proxy protocol

- Define the versioned control envelope and Arrow/batch transport.
- Implement service endpoints for schema, batch reads, writes, output leases,
  artifact sealing, logs, and progress.
- Add protocol limits, backpressure, request cancellation, and error mapping.
- Add worker handshake and protocol diagnostics to runtime transparency.
- Implement stream-versus-materialize selection so small results can flow as
  batches while large results become sealed Parquet handles.
- Make protocol requests scoped by project, run, operation instance, and lease;
  reject stale or reused leases.

Current progress: the `streamfind.data-proxy/v1` envelope supports request,
response, and structured failure messages with project/run/operation/lease
scope. The service exposes bounded `has_table`, `describe`, `count_rows`, and
`read_batch` requests at `/projects/<session>/data-proxy`. It also exposes
format-neutral `describe_result` and `read_result_range` requests. A result
descriptor carries an opaque `payload_ref`, format, media type, size, digest,
provenance, and capabilities; the bounded range response is base64 encoded so
JSON, images, MFT, binary, encoded, and model payloads use one transport
contract. The format string is deliberately extensible rather than an enum.
Tables can advertise batch capabilities while model packages can advertise
range reads and later receive a dedicated streaming capability. These reads
are service-owned. Workers now receive the service port and project session
and install a typed proxy callback on their Project context; the callback
speaks the same envelope over loopback HTTP. Dynamic-plugin table reads and
schema discovery use that callback, so the worker does not query the project
database for those operations. Service-owned dynamic-plugin invocation also
skips the worker-side `ProjectTableStore` transaction wrapper; only the direct
host execution path still opens that wrapper while the remaining ABI mutation
surface is migrated. The endpoint now also supports service-issued result leases, bounded sequential base64 chunk
writes, and publication of typed immutable payloads. Lease responses expose no
filesystem path and accept arbitrary format, kind, and media-type metadata, so
the same lifecycle covers tables, JSON, images, MFT/binary data, and model
packages. Publication remains service-controlled: workers can write only to a
lease and cannot register or promote a result themselves.

The core now exposes `OperationOutputSink` and a data-proxy-backed
`DataProxyOutputSink`. It allocates an opaque result lease, sends ordered
bounded batches, and seals or aborts the lease without exposing a destination
path to operation code. The worker now provides it through the SDK operation
context. Dynamic-plugin JSON result emission uses the sink when present: it
writes a bounded JSON payload to a service lease, seals it, and returns a
`result_id`/`payload_ref` reference with the small payload retained for
downstream parameter inputs. The workflow layer accepts that reference
without requiring a DuckDB registry row and carries it to the next operation.
The worker now carries the run identity, binds the proxy-backed sink to its
project execution context, and exposes the sink to the SDK plugin data-service
context. The remaining emitters are intentionally not switched yet: table
completion, cache registration, and workflow input resolution for large table
payloads must first use service-owned descriptors rather than DuckDB registry
rows. The SDK read side has now taken its first proxy slice: worker-side
`has_table`, `count_rows`, and bounded `read_batch` requests use the service
data proxy and reconstruct the existing ABI batches. The service owns the
DuckDB query, while direct in-process calls retain the same SDK behavior
without a proxy session. Table output batches now take the corresponding
write slice: worker dynamic plugins serialize batches as bounded JSONL chunks,
publish a typed table result, and pass a virtual `result:<id>` reference to
downstream operations. The service can read those JSONL batches through the
same `read_batch` proxy method, so downstream plugins retain the existing ABI
without reopening the project database for the produced table. Parquet/Arrow
is still the planned high-volume representation; JSONL is the bootstrap
representation for this incremental boundary.
Direct service operation execution now uses the same computation-key lookup as
workflow execution. It resolves effective inputs through the service-owned
project context, reuses a complete set of validated published results, and
passes the effective inputs and key to the worker only on a cache miss. JSON
cache hits additionally reload bounded JSON payloads for downstream parameter
inputs; table hits remain opaque `result:<id>` handles. This keeps cache reuse
typed without copying large payloads into the operation request.
The service result query now renders published JSONL table results directly
from validated result-store manifests and batches, including pagination state,
without requiring a legacy `RESULT_REGISTRY` row. Legacy identifiers still use
the existing metadata path until all callers have moved to result descriptors.
Service-side workflow input resolution now selects the latest published result
descriptor for each connection, including result-store table handles and JSON
payload loading for parameter inputs. Direct operation requests therefore no
longer call the project-local resolver when inputs are omitted; workers receive
the same service-owned descriptors used by workflow scheduling.
The workflow graph now accepts an injected service input resolver. Service
sessions install this resolver when they open a project, so workflow scheduling
uses the same result-store inventory as direct operation calls rather than
silently switching back to project-local registry resolution.
When a worker has a service result-cache callback, `Project::run_operation`
no longer performs local `RESULT_CACHE` reads or writes. The service decides
reuse before launch and the worker only executes the operation and delivers
typed outputs through the proxy sink.
The service inventory also skips virtual `result:<id>` table handles when
inspecting legacy DuckDB tables, so published result-store tables are not
incorrectly marked stale or probed through DuckDB.
Dynamic-plugin lineage now records `result_id` for proxy-published inputs and
falls back to the legacy identifier only for legacy registry inputs, keeping
provenance connected across the new result boundary.
Service-owned dynamic-plugin contexts no longer expose a database path; the
nullable table-store pointer remains unavailable in worker execution and all
table access is routed through the data proxy.
Dynamic-plugin schema discovery for worker inputs now also uses the service
`describe`/`describe_result` proxy paths, including virtual `result:<id>`
tables, so worker plugin setup no longer needs to issue direct DuckDB
`DESCRIBE` queries.
The worker startup boundary now creates an in-memory Project context from a
service-supplied workflow snapshot instead of opening the project DuckDB. The
worker request no longer carries a usable project database path. Operation
execution uses the supplied inputs and skips the local workflow lock for that
in-memory context; proxy-published outputs satisfy the operation output
contract without requiring a local result-registry row. The service remains
the owner of project metadata, locks, registry updates, and result payloads.
The remaining standalone API/MCP worker launchers must now be routed through
the same service session and data-proxy endpoint; they must not reintroduce a
database path merely to supply the missing session context.
Those launchers now pass the workflow snapshot as well, so the worker has the
same execution definition without reopening the project. The shared runtime
client now discovers the active local service, reuses an existing project
session when the canonical database path matches, or attaches the project once
before starting the worker. This gives standalone API and MCP operation
launches the same service-owned proxy route as app-launched workers. The
remaining client-integration work is to remove their local project handles
from execution setup and make the service command surface the sole workflow
scheduler.

The MCP direct-operation paths now obtain the workflow definition through the
attached service session before launching a worker, so they no longer open the
project database locally merely to construct the worker request. The API
workflow and MCP workflow commands now submit through the service session as
well. The service returns the accepted run state immediately and remains the
sole workflow scheduler; clients observe progress and results through the
existing service state and event endpoints.

MCP project connection, workflow validation, and workflow updates now use the
same attached session endpoints. MCP keeps only session metadata locally; it
does not open or mutate the project database for these operations.

The shared API facade now routes workflow retrieval, validation, and updates
through those same service endpoints. These commands no longer open DuckDB in
the API process; service sessions are the single metadata boundary for the
workflow graph.

API metadata, domain, and result-registry reads now use the attached service
session as well. The service client retrieves project descriptions and current
or historical result manifests through the service HTTP boundary, so these
inspection paths no longer query DuckDB directly from the API process.

The shared attachment client and service-discovery implementation are linked
through the core runtime boundary so API, MCP, CLI, workers, and native test
executables use one transport implementation. Platform socket dependencies are
part of that boundary rather than repeated in individual launchers.

### Phase 4 — Plugin ABI integration

- Replace worker-local `PluginDataServiceContext.tables` access with a proxy
  implementation at the SDK boundary.
- Replace `WorkflowOperationExecutor(Project&, ...)` with the new
  `OperationExecutionContext` boundary. The operation algorithm remains the
  executor body; it must no longer receive a DuckDB-backed `Project` handle.
- Define the context as the only operation-facing runtime interface. It must
  provide workflow revision and input resolution, typed data-proxy requests,
  result publication, cache lookup, provenance, progress/log/event emission,
  cancellation state, and operation identity. It must not expose a database
  path, DuckDB connection, filesystem result path, or worker process handle.
- Provide service-backed and worker-backed implementations of that context
  through the same protocol, with the service implementation owning metadata
  and the worker implementation forwarding data requests to the service.
- Make every plugin output a typed result descriptor plus an opaque handle;
  table, JSON, image, binary/MFT, encoded, and model formats must not require
  separate scheduling or registry APIs.
- Add service-issued read/write leases and streaming transport for large
  payloads; bounded base64 range reads are only the bootstrap transport, not a
  reason to load complete models or tables into JSON.
- Let MCP and visualization request the same descriptor and payload ranges,
  selecting renderers from media type, format, schema, and capabilities.
- Preserve existing plugin operation contracts and column authorization rules.
- Route output table/result emission through service-issued artifact leases.
- Remove worker project-DuckDB opening and local project table-store creation.
- Remove the old direct execution path once all entry points use the proxy.

Progress completed in the current ABI slice:

- `WorkflowOperationExecutor` and `OperationBinding` now receive
  `OperationExecutionContext&`; the old `Project&` executor signature has been
  removed from the operation ABI.
- Core operation registration and dynamic-plugin registration now invoke through
  the context boundary, so the scheduler has one executor contract.
- The project-backed context centralizes direct execution services, including
  query/execute delegation, input resolution, proxy requests, output sinks,
  progress, events, cancellation, and result publication.
- The remaining core table utilities still use the context's project facade
  while their table-store transactions are converted to typed proxy requests.
  That conversion is the next slice and must not reintroduce a `Project&`
  callback into the public operation binding.

The next implementation slice has now also moved the core table operations onto
the runtime output boundary:

- `read_csv`, `bind_table_rows`, and `bind_table_columns` query through the
  execution context and emit JSONL table batches through `OperationOutputSink`.
- Direct project execution uses a local context-owned sink that publishes the
  same immutable table result shape; worker execution continues using the
  service-owned result sink.
- Table allocation, appending, lineage, and publication are no longer called
  from those operation bodies through `ProjectTableStore`.
- Dynamic-plugin execution now obtains workflow revision, cancellation,
  progress, output sinks, computation keys, and service-owned data requests
  from `OperationExecutionContext`; service-owned schema and result reads no
  longer reach through the project callback.
- The remaining `Project` facade is now limited to the direct-execution host
  adapter and direct table transaction setup. Removing that final host-only
  facade, together with the corresponding direct proxy implementations, is
  the next ABI cleanup slice.

Migration order for operations:

1. Introduce the context contract and migrate the core operations that only
   read/write typed payloads.
2. Migrate dynamic plugin invocation and the SDK callbacks so operation logic
   continues to receive the same typed batches and result callbacks.
3. Move workflow input resolution, cache decisions, and result publication to
   the service-side context.
4. Remove `Project&` from the executor ABI and delete worker-side
   `Project::open()` and all direct table-store construction.
5. Keep operation algorithms and semantic definitions unchanged except where
   they currently perform persistence directly; those calls become context
   calls with equivalent typed inputs and outputs.

### Phase 5 — Pipeline mode

- Add detailed/pipeline mode to operation and workflow requests.
- Keep intermediate output handles run-scoped in pipeline mode.
- Publish only endpoint outputs and clean up transient payloads.
- Ensure operation-by-operation execution defaults to detailed mode.
- Add frontend/MCP controls and clear run summaries.
- Reuse content-addressed transient results within a run while publishing only
  endpoint results, then apply retention rules to unreferenced intermediates.

### Phase 6 — Parallel workflow execution

- Allow independent ready nodes to run concurrently.
- Allocate isolated output leases per node and output port.
- Preserve dependency ordering and prevent downstream execution after failure or
  cancellation.
- Verify that service metadata commits remain short and non-overlapping with
  payload writes.

### Phase 7 — Legacy removal and release hardening

- Remove worker-side DuckDB execution code and obsolete service-side direct
  invocation code.
- Remove duplicate MCP/HTTP scheduling logic.
- Update release packaging, startup scripts, docs, and example workflows.
- Complete migration/recovery handling for projects created before `.data`
  storage and document the backup/restore bundle format.
- Build and test Windows, Linux, and WSL-compatible deployments.

## Testing plan

### Protocol and artifact tests

- artifact path derivation;
- copied database with missing sidecar opens degraded without deleting rows;
- compatible sidecar attach and incompatible sidecar rejection;
- fork/clone copies the sidecar and assigns a new store identity;
- explicit registry reset archives or clears only after confirmation;
- staging lease isolation;
- atomic promotion;
- checksum and schema validation;
- mixed Parquet/JSON/image/binary payloads;
- cleanup after failure, cancellation, and service restart;
- stale run recovery.
- identical inputs produce a cache hit without recomputation;
- changed parameters, input result digests, plugin versions, schemas, and
  external-resource versions invalidate the hit;
- UI-only workflow layout changes do not invalidate a reusable result;
- missing, corrupted, incomplete, or schema-incompatible payloads never count
  as cache hits;
- concurrent identical requests share one computation claim and result;
- crash recovery repairs or quarantines promotion/registration gaps;
- clearing cache mappings retains published results until explicit garbage
  collection;
- multiple run histories can safely reference the same immutable payload.

### Runtime tests

- app and MCP submit the same operation and observe one run;
- duplicate idempotent submission does not create a second worker;
- MCP cancels an app-started run and vice versa;
- event replay reconstructs the full run state;
- inspection queries work while a worker is active;
- workflow revision conflicts are deterministic.

### Concurrency tests

- parallel branches write separate artifacts;
- readers inspect published artifacts while other workers write staging files;
- no worker opens the project DuckDB;
- no DuckDB lock race occurs during active execution;
- failed branch prevents dependent nodes from starting;
- pipeline-mode intermediates are removed and endpoint artifacts remain.

### Platform tests

- Windows worker/job-object termination;
- Linux process-group termination;
- WSL service and worker connectivity;
- release package with no development-only paths;
- restart after forced worker termination.

## Acceptance criteria

The refactor is complete when:

1. A worker process cannot open the project DuckDB because it has no execution
   database path or local table-store implementation.
2. The service can inspect workflow state and historical artifacts while an
   operation is running.
3. App and MCP requests produce identical run, worker, artifact, and event
   behavior.
4. Parallel workers can write independent artifacts without file or DuckDB
   races.
5. Detailed mode preserves all intermediate outputs.
6. Pipeline mode publishes only endpoints and cleans transient outputs.
7. JSON, tables, images, logs, and binary artifacts use the same catalog and
   lifecycle despite different payload formats.
8. Repeated deterministic operations reuse validated immutable results without
   recomputation, while non-deterministic or stale inputs are never reused.
9. Cancellation, restart recovery, CTest, frontend tests, and release-package
   validation pass on Windows and Linux/WSL.

## Migration audit — 2026-10-24

This audit separates the new runtime paths that are already active from the
remaining MCP and payload-format migration work. The audit is intentionally
based on the current source tree, not on the earlier development harness.

### MCP migration completed in this pass

`cpp/core/src/mcp.cpp` now routes the project and execution commands below
through the service runtime client:

- `get_workflow_execution`;
- `create_workflow_execution`;
- `get_execution`;
- `list_executions`;
- `transition_execution`;
- `cancel_execution`;
- `describe`, `validate`, `get_project_domains`, `get_metadata`,
  `set_metadata`, `get_audit_trail`, `close`, `add_operation`,
  `connect_operations`, and `copy`.

`get_workflow_execution`, `create_workflow_execution`, `get_execution`,
`list_executions`, `transition_execution`, `cancel_execution`, project
metadata/audit operations, workflow edits, close/copy, workflow snapshots,
result inventory, result selection, bounded result reads, and input resolution
all use the same attached service session as the app. The generic `api::run()`
fallback has been removed from MCP; an unhandled project command now fails
explicitly instead of reopening DuckDB in the MCP process.

### JSONL migration status

The active worker output sink already emits table results as bounded Arrow IPC
frames (`arrow-ipc-batches`). JSONL remains in the code as a read fallback in
`ResultStore::read_jsonl_batch()` and in service-side format dispatch for
older or manually-created result manifests. It is not the desired output
format for new table results.

The source audit found no active worker/service writer for JSONL; new table
outputs are emitted as Arrow IPC. The remaining work is therefore:

1. enumerate existing JSONL writers and remove any writer still reachable from
   worker or service execution;
2. make Arrow IPC the only newly published table representation;
3. retain one bounded, read-only JSONL migration reader for explicitly
   migrated legacy results until the migration tool has converted or quarantined
   those payloads;
4. add a result-store migration check that rejects JSONL as a new lease format;
5. update frontend, MCP, operation contracts, demos, and tests to assert
   `arrow-ipc-batches` for new table results;
6. remove the JSONL reader and its compatibility branch after the conversion
   and release validation pass.

### Current completion state

- **Complete:** service-owned worker execution, service data proxy, typed
  output leases, Arrow table output for new workers, service workflow/result
  inspection, pipeline-mode publication, and parallel independent-project
  validation.
- **Complete for the active runtime:** all exposed MCP project/runtime commands
  use service sessions; new table result leases reject JSONL and use Arrow IPC.
- **Remaining migration work:** convert or quarantine existing JSONL manifests,
  then remove the bounded JSONL reader and its service dispatch branch after
  project conversion and release validation.

