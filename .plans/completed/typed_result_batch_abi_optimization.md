# Plan: typed result ABI and format-aware result store

## Objective

Refactor plugin result I/O so result tables stay columnar and typed from the
plugin operation, across the worker/service boundary, into the project result
store, and back into downstream operations. Do not materialize JSON rows while
writing or reading a table result. The C++ service owns result projection,
filtering, ordering, paging, and extraction; only the selected query response
is serialized as JSON for the browser, MCP, or another JSON client.

Use one generic result-store and plugin-access framework for every domain. The
framework must support several table schemas and result payload kinds without
mass-spec-specific assumptions. For table batches, use the Apache Arrow C Data
Interface (`ArrowArray` + `ArrowSchema`) as the ABI's extensible typed memory
contract instead of maintaining a parallel, hand-written scalar/list type
system. Keep media and opaque binary artifacts on a separate byte-oriented
contract.

## Design decisions

### Separate logical result kind from physical format

Every published result declares its semantic kind and physical encoding.
Physical encoding is selected by the kind and access pattern, not by a global
assumption that Arrow fits every payload.

| Result kind | Storage representation | Typical access |
| --- | --- | --- |
| Table | Arrow IPC file with an explicit schema and record batches | Column projection, predicate/filter, ordering, offset/limit, typed batch scan |
| JSON document | UTF-8 JSON file | Whole document or bounded JSON-path/query access |
| Binary/media | Opaque file bytes plus media type, byte length, checksum, and optional dimensions | Range/full-byte retrieval or a service-generated URL/reference |
| Future structured domain payload | A registered codec/format selected by semantic kind and query needs | Codec-owned typed query interface |

Images, spectra images, and other opaque media must not be forced into Arrow
tables. Tabular metadata about an image may be a table; the image itself is a
binary/media artifact. Arrow IPC remains the default table format because it
preserves types and nulls, supports record-batch streaming, and allows the
backend to project columns without first rebuilding row objects. Parquet is
not the default for operation handoff because the current workflow path needs
low-latency append/write and sequential batch consumption; reconsider it only
with benchmarks showing a better fit for persisted analytical querying.

### One domain-neutral ownership path

All backend operations, bundled or external, use the same SDK `PluginProjectAccess`
for reading result inputs and the same runtime-owned `OperationOutputSink` for
publishing result outputs. Domain plugins declare schemas and compute values;
they do not select project paths, write result files, maintain indexes, or
invent domain-specific store adapters. Runtime code owns allocation,
leases, validation, publication, lineage, cleanup, and query access.

Parameters, workflow definitions, small control metadata, and explicitly JSON
result artifacts may continue to use JSON. Table result rows and columnar
payloads must not use JSON as an intermediate transport or persistence format.

## Current implementation findings

- The plugin ABI is now version 3 and exchanges table batches through Arrow C
  Data Interface `ArrowArray`/`ArrowSchema`. Plugin reads expose an Arrow
  `RecordBatch`; output batches reach `DataProxyOutputSink` and its Arrow IPC
  writer without the former dynamic-plugin row-JSON conversion.
- The SDK's JSON table read/write methods and unused JSON/SQL-string mutation
  callbacks have been removed. Bundled mass-spec, Raman, statistics, and sensor
  operation paths have been migrated to the shared typed batch API, with
  additional NTA, chromatogram, fragmentation, and quantification input/output
  migrations recorded below.
- `ArrowFileBatchReader` exposes typed record batches; its JSON row reader is
  removed. Distinct, scalar filters, text search, and sorting scan stored Arrow
  arrays natively. Page and row-key requests seek directly to selected ranges;
  only selected response rows are serialized for the client.
- Core `read_csv`, `bind_table_rows`, and `bind_table_columns` now send their
  SELECT statements to the service, which streams DuckDB chunks as Arrow
  batches directly into the result lease. They no longer make JSON row arrays.
- Quantification response, calibration point/model, and final result rows now
  use native typed cells, including typed numeric lists for EIC data. Its JSON
  inputs are limited to inline parameters and small method/assignment control
  tables.
- Generic `OperationExecutionContext::query(sql)` still returns JSON rows for
  control queries. Audit remaining callers and keep it only where the result is
  bounded control metadata; large/table-result paths must use typed batches.
- Result query selection is native for projection, scalar filters, text search,
  sorting, paging, distinct values, and stable row-key lookup. Numeric range
  sidecars prune eligible batches. The service no longer builds or consumes a
  JSON row navigation index.
- Keep the one shared result-store path for all domains. Do not add a temporary
  JSON-to-Arrow bridge, compatibility API, or alternate domain-specific store
  while completing the remaining work.

## Target contracts

### Plugin ABI

1. Adopt Arrow's C Data Interface (`ArrowArray` and `ArrowSchema`) for typed
   table batches. It already defines primitive, temporal, decimal, binary,
   list, struct, map, dictionary, and extension-type layouts and can be
   extended without a new StreamFind enum for every Arrow data type.
2. Define callback ownership precisely: exported arrays/schemas are borrowed
   for the callback duration unless ownership is explicitly transferred via
   the C Data Interface release callbacks. Import/export must invoke release
   exactly once; plugins may not retain borrowed pointers.
3. Specify schema/type checking, null handling, empty batches, cancellation,
   progress, exceptions/status mapping, maximum batch rows/bytes, and ABI
   version negotiation. Reject unsupported types explicitly; never reinterpret
   an unknown type as UTF-8 or JSON text.
4. Keep JSON request/response ABI only for operation configuration and JSON
   document artifacts; table data uses the typed batch ABI.

### SDK result access

1. Replace JSON-batch table reads with a typed, callback-scoped batch view.
   The SDK must preserve names, type, nested offsets/children, validity, and
   order without copying whole rows into `nlohmann::json`.
2. Replace string-encoded row writes with typed append/update/delete operations
   using the same column model. Operations that need values may materialize
   domain structs, but must not stringify or parse table rows as JSON.
3. Keep a query API for small control/catalogue metadata only. It must not be
   the result-table scan API.
4. Keep one SDK surface across mass-spec, Raman, statistics, sensors, and
   future domains; table semantics are supplied by schema descriptors, not
   special-case domain code.

### Worker and result-store transport

1. For table output, pass the typed ABI batch directly to the worker-owned
   Arrow writer. Construct Arrow arrays from the typed ABI values and write
   record batches straight to the leased result file handle.
2. Do not construct `Json` row arrays in `dynamic_plugin_manager`, serialize
   JSON through the data proxy, or infer table types from row contents.
3. For table input, read projected Arrow columns from the leased result file
   and expose them directly as callback-scoped typed ABI batches. Do not decode
   result rows to JSON and re-encode them.
4. Keep transport bounded by both row and byte budgets. Large single rows,
   nested arrays, cancellation, partial reads, and empty results must have
   explicit behavior.
5. Make typed access apply to downstream operations from every domain. Avoid
   separate read/write paths for particular plugins.

### Query and client boundary

1. The service performs field projection, filters, text search, ordering,
   pagination, stable row-key lookup, and numeric range selection against the
   stored Arrow format before constructing JSON.
2. The browser/MCP receives JSON only for the selected output window, plus
   schema, total/filtered row counts, and stable navigation metadata needed by
   the UI. Large binary/media data is returned as a reference or requested
   byte range instead of inline JSON/base64 by default.
3. Keep query semantics generic across table schemas and domain-specific field
   names. Domain renderers may request domain fields but do not own storage
   decoding.
4. For image/media artifacts, expose media type, dimensions when known, size,
   digest, and a local authenticated/authorized retrieval route or binary
   stream. Do not serialize image bytes as JSON arrays.

## Implementation phases

### Phase 0 — inventory and baseline

1. Audit every `PluginProjectAccess` read/write call in all plugin domains and
   classify it as control metadata, table input, table output, mutation, JSON
   artifact, or binary/media artifact.
2. Trace table bytes end-to-end through operation invocation, worker ABI,
   data-proxy handle transfer, result-store publication, cache reuse, query
   API, frontend, and MCP.
3. Record current table schemas and Arrow-supported scalar/list types, plus
   nested/binary types the ABI/store do not yet preserve.
4. Capture representative baseline measurements for throughput, peak memory,
   and query latency, using small fixtures and available workflow projects.
5. Keep this phase diagnostic: no compatibility or migration layer is added.

### Phase 1 — ABI and SDK typed batches

1. Finalize the Arrow C Data Interface contract and deliberate ABI version
   bump for this pre-release refactor. No old ABI fallback is retained.
2. Add generic typed read, write, update, and delete batch callbacks that
   import/export Arrow arrays and schemas directly, including nested validity,
   binary values, extension types, and callback lifetime rules.
3. Add SDK contracts and contract tests for primitive values, nulls, empty
   values, nested arrays/structs, binary data, offsets, cancellation,
   status/error propagation, release-callback ownership, and borrowed-buffer
   lifetime.
4. Remove row-oriented JSON table methods from the plugin-facing API once all
   owning call sites are moved in the same change series.
5. Update bundled plugin entry points and external SDK example to the new ABI.

### Phase 2 — direct typed result writes

1. Make dynamic plugin output callbacks forward typed columns to
   `OperationOutputSink`; remove `table_batch_rows`.
2. Add a schema-validated Arrow IPC writer that consumes typed columns directly
   and writes bounded record batches to the leased file handle.
3. Preserve schema identity across batches and ensure empty tables still
   produce a valid zero-row Arrow IPC artifact.
4. Move table output operations in every domain from JSON row helpers to typed
   batch construction. Do not leave a JSON-based table writer path in the SDK.
5. Keep JSON artifacts on the explicit JSON artifact path and add a generic
   binary/media output path with size/digest/media metadata.

### Phase 3 — direct typed result reads and mutations

1. Make Arrow file reading return typed projected record batches (or ABI
   borrowed views) rather than JSON rows.
2. Make `plugin_read_batch` pass the typed batches directly to plugin
   consumers; remove JSON reconstruction and string-to-number reparsing.
3. Replace JSON callback reads and full JSON table reads across every bundled
   domain. Keep JSON queries only for small control metadata.
4. Implement typed append/update/delete transport with bounded batch payloads;
   use leased binary/Arrow IPC transport where a service round trip is needed.
5. Cover range seeking, skipped record batches, repeated reads, cancellation,
   and partial/empty results.

### Phase 4 — generic service-side result queries

1. Execute selected-column, filter, order, offset/limit, and navigation queries
   in the C++ service over typed artifacts without constructing the full table
   as JSON.
2. Serialize only the requested page/window to the app/MCP JSON envelope.
3. Keep indexes and navigation/range sidecars inside the owning result folder,
   keyed by the generic schema and query plan rather than a MassSpec-only model.
4. Add generic media metadata and byte-range/reference retrieval for binary
   artifacts.
5. Ensure cache hits reuse the same typed result artifact without decode/re-
   encode or format conversion.

### Phase 5 — all-domain migration and cleanup

1. Migrate operations in mass-spec, Raman, statistics, sensors, and every
   additional bundled domain to the same SDK read/write/result interfaces.
2. Search for and remove row-wise JSON result conversion, JSON array encoding
   in table columns, `table_batch_rows`, `write_rows`, and equivalent plugin
   helpers. Retain JSON only for control/configuration and declared JSON
   document results.
3. Remove duplicate domain-specific result-store helpers and obsolete row/ABI
   adapters. No legacy fallback, schema migration path, dual writer, or old
   execution route remains.
4. Regenerate the SDK package and update plugin developer documentation and
   ABI examples.

### Phase 6 — correctness, performance, and package validation

1. Verify round-trip equivalence for all supported scalar and nested table
   values, nulls, empty tables, binary payloads, and schema errors.
2. Run native ABI/SDK contracts and CTest for every plugin domain.
3. Run deterministic workflow checks for mass-spec, Raman, statistics, and
   sensors where fixtures are available; compare table schemas, row counts,
   values, and lineage.
4. Benchmark representative large inputs, including quantification and large
   scientific result tables. Report bytes copied, allocations/peak memory,
   writer throughput, scan throughput, and app/MCP query latency.
5. Validate packaged app table exploration, paging, filtering, plots, and
   media retrieval. The frontend remains a client of the service query API.
6. Refresh the packaged build and release packages through `sfdev`; record
   results and provenance in this plan.

## Non-goals and constraints

- Do not make Arrow a universal format for images, JSON documents, or arbitrary
  opaque binary results.
- Do not move result parsing or filtering into the frontend.
- Do not introduce a second generic query engine in each domain plugin.
- Do not retain old ABI adapters or data migrations during this refactor.
- Do not modify third-party source under `cpp/vendor/`.
- Preserve worker isolation, service ownership of project state, output
  lineage, result cache behavior, cancellation, and bounded memory use.

## Acceptance criteria

- No table result crosses a plugin/service/store boundary as JSON rows or
  JSON-encoded list values.
- Table output reaches Arrow IPC from typed ABI buffers without per-row JSON
  objects; table input reaches plugin callbacks from Arrow arrays without JSON
  decode/re-encode.
- All domains use the same generic SDK and runtime result framework.
- Result queries do projection/filtering/paging in C++ and JSON-serialize only
  the selected response rows for the app/MCP.
- JSON document and binary/media results retain suitable non-Arrow formats;
  media bytes are not embedded into JSON by default.
- No legacy result reader/writer, duplicate ABI, fallback, or data migration
  code remains.
- Schemas, lineage, cache reuse, nulls, nested arrays, cancellation, and
  empty-result behavior pass native contracts and representative workflow
  checks.
- Benchmarks demonstrate lower peak memory and fewer conversions than the
  current path while preserving query and result equivalence.

## Provenance and completion

Implementation and backend validation were completed on 2026-11-16. All bundled
operation domains use the shared Arrow C Data Interface SDK and typed
`TableBatchBuilder`/`BatchView` path for table results. The service stores and
queries Arrow artifacts directly; JSON is generated only for selected app/MCP
responses, operation parameters/status, declared JSON documents, and explicitly
small control/configuration tables. The MassSpec helper for the latter is now
named `read_control_rows` and documents that result tables must use typed batch
scans.

Removed production JSON table writers/readers, JSON-encoded list transport,
quantification result-row JSON models, and full-table JSON result query paths.
The shared result query applies projection, supported filtering, ordering,
paging, and row-key selection before constructing client response JSON. Arrow
IPC remains the table format; JSON documents and binary/media artifacts retain
their explicit formats.

### Validation completed

- `pwsh -NoProfile -File scripts/sfdev.ps1 build backend --jobs 4` passed after
the final control-reader boundary cleanup; the MassSpec SDK plugin validator
passed.
- `pwsh -NoProfile -File scripts/sfdev.ps1 test backend --jobs 4` passed all
40/40 CTest contracts, including the Arrow result store, plugin ABI, domain
contracts, and worker/service contracts.
- Representative NTA, impurity, and quantification workflow projects were
created through MCP and their current schemas validated: respectively 20,
19, and 5 operations.
- The four-operation MassSpec pipeline check passed via the MCP service on an
isolated test port. It retained eight intermediate results, produced 799
feature rows, passed projected-page and native-filter checks, and recovered
EIC/index queries after a service restart. EIC queries measured 85 ms cold
and 1.6 ms average warm for the tested paired-point selection.
- The result-store benchmark passed for 76,800 rows and about 80 MB of typed
payload. A projected 250-row page returned about 5.4 KB of JSON with a
10.6 MB measured working set. Large EIC/MS2 traces were also queried without
materializing full-table JSON; the tested 576k-point selection returned
about 20.3 MB JSON in 0.67 s cold and 0.84 s median warm, while a 50k-point
MS2 trace took 62 ms cold and 67 ms median warm. These are local benchmark
measurements, not cross-machine performance guarantees.
- Static audit found no production `write_rows`, `table_batch_rows`,
`ArrowFileBatchReader::read`, or generic `input_rows` helper. `git diff
--check` passed.

### Packaged validation completed

- The Windows `tmp/packaged_build` was refreshed through
  `pwsh -NoProfile -File scripts/sfdev.ps1 package`. The package build included
  the frontend, native core/plugins, all 40/40 CTest contracts, and package
  payload validation.
- Package payload validation initially rejected the plugin manifests because
  the release helper still assumed ABI major version 1. The helper now reads
  `STREAMFIND_PLUGIN_ABI_MAJOR` from the SDK ABI header and checks manifests
  against that authoritative value. The refreshed package then passed the
  validator with ABI major version 3.
- The packaged MCP smoke passed `tools/list` and mass-spec operation discovery
  against the package service. The packaged browser smoke passed workspace,
  settings, and health-pane checks without browser console errors.
- A retained packaged MassSpec pipeline project completed four operations and
  produced 799 feature rows across eight intermediate results. Service-side
  projected paging, filtering, EIC retrieval, and persisted index recovery
  passed, including after restarting the isolated test service.
- The retained project was opened in the packaged app. The feature output
  rendered as a 90-column typed result with 799 rows; the explorer displayed
  actual feature values and paging (`Showing rows 1-250 of 799`).
- The packaged service remains available on port 8787 with the inspection
  project attached for manual follow-up. The parallel worktree service was
  left untouched.

The migration and its validation are complete. This plan can be moved to
`.plans/completed/`.
