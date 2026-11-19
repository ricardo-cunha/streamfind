# Sensors framework plan

**Owner:** `cpp/plugins/sensors`
**Scope:** protocol-independent sensor acquisition and finite-batch time-series
processing using the existing project, workflow, artifact, plugin, and service
contracts.

## Plan status

This plan was activated from the stale plan in the `mass-spec-expansion`
worktree on 2026-11-19 and expanded from the attached sensors-framework
concept. OPC UA is the first implemented acquisition possibility and the
reference adapter for the framework that follows.

Completion: The planned sensors framework scope is complete as of 2026-11-19.
OPC UA and MQTT provide acquisition adapters over the canonical observation
contract, and the finite-batch time-series processing milestones are
implemented, tested, and packaged.

The target architecture is:

```text
OPC UA / CSV / MQTT / future sources
                 |
       protocol-specific adapters
                 |
      canonical sensor observations
                 |
       typed Arrow result batches
                 |
  validation, resampling, alignment, analysis
```

The sensors plugin owns acquisition adapters and sensor-specific processing.
The core owns workflow execution, cancellation, typed data movement, result
publication, project isolation, and runtime lifecycle. Adding an acquisition
protocol should not require a core or time-series algorithm change.

The plugin does not own cross-run accumulation. One operation invocation
produces one finite result. If a user wants to append that result to a larger
history, retain it for a later workflow, or combine results from repeated
workflow runs, that composition must be explicit in the user's workflow using
generic persistence and data-flow operations.

### Implementation progress

- [x] Added the `SENSOR_TIMESERIES_V1` semantic table contract and explicit
  typed observation columns.
- [x] Added `sensors.normalize_sensor_table` for explicit input-column mapping,
  quality/provenance normalization, and bounded Arrow batch publication.
- [x] Corrected SDK timestamp conversion to preserve microseconds and normalize
  ISO-8601 UTC offsets.
- [x] Added a deterministic native contract test for fractional timestamps,
  UTC offsets, typed values, missing values, and quality.
- [x] Add the shared internal observation/adapter boundary and bounded OPC UA
  time-series capture operation with duration, interval, sample-count, buffer,
  and cancellation limits.
- [x] Validate the packaged OPC UA capture against the LADS pH-meter server,
  including typed pH values and missing rows for non-Variable nodes.
- [x] Add the first time-series processing operation, a non-mutating validator
  for the canonical table contract.
- [x] Implement the CSV-to-`SENSOR_TIMESERIES` workflow through generic
  `streamfind.read_csv` and explicit long/wide normalization mappings.
- [x] Add protocol-independent inclusive-start/exclusive-end time filtering
  over `SENSOR_TIMESERIES`, preserving typed values, quality, and provenance.
- [x] Add fixed-window UTC resampling with explicit `mean`/`last`
  aggregation and quality propagation for partial and all-missing windows.
- [x] Add nearest-time alignment to an explicit reference signal with a
  bounded tolerance, match/missing status, and signed time deltas.
- [x] Add an explicit aligned-table projection back to `SENSOR_TIMESERIES_V1`
  with reference and observed time-axis policies.
- [x] Add rolling mean, minimum, maximum, and standard-deviation operations
  that emit derived observations through `sensorTimeseriesTable`.

## Current baseline: OPC UA reference acquisition

The sensors plugin currently provides three plugin-owned operations:

- `sensors.explore_opcua_server` connects to an endpoint and emits bounded
  endpoint, namespace, and hierarchical node metadata.
- `sensors.inspect_opcua_node` queries selected-node definition, attributes,
  type information, properties, and an on-demand value snapshot.
- `sensors.collect_opcua_data` accepts known node IDs directly, classifies each
  node by OPC UA `NodeClass`, reads values only from value-bearing Variable
  nodes, and emits a typed table with status and value metadata.

The reference slice establishes these decisions:

- Each operation owns its endpoint, authentication parameters, and timeout.
  There is no shared connection node or persisted live client session.
- Connections are created for an operation invocation and released when that
  invocation completes. This keeps workflows simple and avoids connection
  lifecycle, cancellation, and credential propagation problems.
- Exploration is bounded and lightweight. Values and detailed definitions are
  queried on demand instead of being included in the initial graph payload.
- Collection supports direct known Node IDs; exploration is optional rather
  than a required predecessor.
- Credentials remain operation parameters and must not be copied to logs,
  result artifacts, plans, or workflow metadata.
- The native implementation and its semantic catalogue remain in the sensors
  plugin; core and project management stay domain-neutral.
- Projects and result stores use the container layout
  `<project-name>.sf/project.duckdb` and `<project-name>.sf/data/`.

The OPC UA reference slice has been exercised against the pH-meter server in
the local LADS server collection. The packaged service successfully created,
opened, executed, closed, and reopened `.sf` projects; discovery returned the
server namespaces and node categories, and collection returned a Good value
for the pH Variable while correctly reporting non-value Object nodes.

## Proposed canonical observation model

The existing OPC UA collector is a point-in-time node snapshot, not yet a
general time-series acquisition operation. The next abstraction is a logical
long-format table contract:

`SENSOR_TIMESERIES`, version 1.

It should contain one row per signal observation. A long format allows an
arbitrary number of sensors without dynamically changing table columns.

### Observation columns

| Column | Logical type | Meaning |
| --- | --- | --- |
| `source_id` | `VARCHAR` | Stable acquisition source identity |
| `signal_id` | `VARCHAR` | Stable sensor/channel identity |
| `event_time` | `TIMESTAMP` | Measurement time normalized to UTC |
| `received_at` | `TIMESTAMP` | Time received by streamfind |
| `time_origin` | `VARCHAR` | `source`, `server`, `broker`, or `receiver` |
| `value_type` | `VARCHAR` | `real`, `integer`, `boolean`, `string`, or `json` |
| `value_double` | `DOUBLE` | Numeric observation when applicable |
| `value_int` | `BIGINT` | Integer observation when applicable |
| `value_bool` | `BOOLEAN` | Boolean observation when applicable |
| `value_text` | `VARCHAR` | Text observation when applicable |
| `value_json` | `VARCHAR` | Structured or otherwise serialized value |
| `unit` | `VARCHAR` | Unit of measurement, when known |
| `quality` | `VARCHAR` | `good`, `uncertain`, `bad`, or `missing` |
| `status_code` | `VARCHAR` | Protocol-native status code |
| `capture_id` | `VARCHAR` | Acquisition execution identity |
| `event_id` | `VARCHAR` | Observation identity for deduplication |

Normally exactly one typed value column contains a value. Invalid or
unavailable readings retain quality and status information without fabricating
a numeric value.

The timestamp contract must define microsecond precision, UTC normalization,
out-of-order rows, repeated timestamps, and the fact that a timestamp alone is
not a unique observation key. Resampling windows are left-closed and
right-open, for example `[09:00, 09:01)`, with explicit aggregation,
time-zone, missing-data, and quality-propagation policies.

Two related metadata contracts are also proposed:

- `SENSOR_SOURCE` for protocol, endpoint/topic identity, acquisition settings,
  and provenance;
- `SENSOR_SIGNAL` for signal mapping, unit, calibration, and source-specific
  identifiers.

Credentials are never part of either contract. The logical contract is
separate from its physical storage: initially it will be published as Arrow
IPC artifacts with DuckDB project/result metadata, following the existing
result architecture. Plugins must not receive direct DuckDB access. If an
appendable SQL representation is later needed, it must be introduced through
a service-owned transaction/data-proxy API.

## OPC UA path from snapshot to time series

The existing operations remain useful and should not be replaced:

1. Explore the server to discover bounded metadata.
2. Inspect selected nodes on demand.
3. Collect known nodes as a point-in-time classified snapshot.

The next OPC UA acquisition operation is a separate bounded operation,
tentatively named `sensors.capture_opcua_timeseries`. It should reuse the
open62541 connection and decoding code while adding:

- polling or monitored-item capture modes;
- duration, maximum samples, and maximum buffered bytes;
- source/server timestamps and `time_origin` preservation;
- cancellation and progress reporting;
- typed observation batches and quality propagation;
- reconnect and partial-result rules; and
- one canonical result identified by `capture_id`.

Continuous, months-long ingestion remains out of scope for the sensors
operation. Each invocation performs a finite read or bounded subscription and
publishes one immutable result. Persistent streaming, cross-run accumulation,
and long-lived connections do not belong in the adapter.

## Internal acquisition boundary

All protocols should converge on a plugin-internal abstraction. It must not
become a new public ABI until a protocol-independent capability is proven to be
missing from the current interface.

Conceptually, the boundary contains:

```text
SensorObservation
  source_id, signal_id, event_time_us, received_at_us
  typed value, quality, unit, status_code, event_id

CaptureRequest
  duration, max_samples, max_buffer_bytes

AcquisitionAdapter
  capture(request, observation_sink, cancellation)
```

The common runner owns observation validation, batch publication, progress,
resource limits, and cancellation. Protocol adapters own connection
management, decoding, source-specific timestamps, and protocol diagnostics.
`SensorObservation`, value/quality types, the sink, and the adapter remain
inside the sensors plugin and use the existing SDK batch facilities.

## Acquisition possibilities

| Source | Initial acquisition approach | Main concerns |
| --- | --- | --- |
| OPC UA | Snapshot, bounded polling, or monitored-item capture | Node status, source/server timestamps, reconnect |
| CSV | Finite import and normalization | Timestamp formats, wide/long mapping, units, bad rows |
| MQTT | Bounded topic subscription | QoS, retained messages, duplicates, payload mapping |
| Modbus/serial | Future bounded polling adapter | Registers, scaling, device timestamps |

CSV should reuse the generic `streamfind.read_csv` operation. A sensors-owned
`sensors.normalize_sensor_table` operation can map arbitrary imported columns
into `SENSOR_TIMESERIES`, supporting explicit mappings for both wide and long
inputs without duplicating CSV parsing.

MQTT delivery semantics must be explicit; broker QoS must not be treated as
application-level exactly-once recording. Device-control commands remain
separate operations because their side effects and safety requirements differ
from acquisition.

## Time-series processing operations

Processing operations consume `SENSOR_TIMESERIES`, not protocol-specific
tables:

| Operation ID | Input | Output |
| --- | --- | --- |
| `sensors.validate_timeseries` | Any table or sensor table | Validation report |
| `sensors.filter_timeseries` | `SENSOR_TIMESERIES` | `SENSOR_TIMESERIES` |
| `sensors.resample_timeseries` | `SENSOR_TIMESERIES` | `SENSOR_TIMESERIES` |
| `sensors.align_timeseries` | Multiple sensor series | Aligned measurement table |
| `sensors.rolling_statistics` | `SENSOR_TIMESERIES` | `SENSOR_TIMESERIES` |

The first processing slice should be validation, time filtering, resampling,
and alignment. It must define missing samples, out-of-order events, duplicate
timestamps, bad quality, interpolation, nearest-time tolerance, and quality
propagation. Cross-run accumulation, append semantics, and historical-series
querying are intentionally outside this plan; a user workflow may compose
finite results with generic persistence and data-flow operations.

The first processing operation is `sensors.filter_timeseries`. It accepts a
canonical time-series input and optional normalized UTC `start_time` and
`end_time` bounds. The interval is left-closed and right-open, so a row at
`start_time` is retained while a row at `end_time` is excluded. Filtering
copies the canonical typed columns without changing values or quality and is
safe to compose with the validator before resampling and alignment.

The resampling operation is `sensors.resample_timeseries`. It anchors fixed
windows at the Unix epoch, uses left-closed/right-open intervals, supports
numeric `mean` and latest-observation `last`, and propagates a partial window
with both valid and missing numeric readings as `uncertain`. A window with no
usable value remains `missing`, so the validator can distinguish an absent
reading from a degraded aggregate.

The alignment operation is `sensors.align_timeseries`. It uses one signal as
the per-source reference timeline, selects the nearest observation from every
other signal within a non-negative tolerance, and emits `SENSOR_ALIGNED_V1`.
The aligned table retains both the reference and matched event times, typed
values and quality metadata, an `alignment_status` of `reference`, `matched`,
or `missing`, and a signed microsecond delta. Unmatched signals are represented
explicitly as missing rows rather than being silently dropped.

The projection operation is `sensors.project_aligned_timeseries`. It is the
explicit bridge from `SENSOR_ALIGNED_V1` to `SENSOR_TIMESERIES_V1`; generic
filtering, resampling, validation, and future analysis nodes consume the
projected canonical table. Its `reference` axis uses the reference timeline,
while its `observed` axis uses matched event times and anchors an unavailable
match to the reference timestamp so the canonical timestamp remains valid.

## Runtime, security, and ABI direction

The current v3.0 plugin interface already provides the primitives needed for a
first bounded slice: batch reads/writes, repeated result emission, progress,
cancellation, and Arrow C Data transport. Do not introduce ABI v4 for the
time-series foundation.

Immediate runtime work:

- preserve microsecond timestamps end to end rather than reducing them to
  whole seconds;
- validate efficient emission of multiple bounded Arrow batches;
- define secret references and redaction tests for OPC UA and MQTT;
- keep secure OPC UA connections, certificate validation, and encryption in
  the production-readiness scope; and
- add optional time-range/projected-column reads only when historical query
  measurements justify them.

If an ABI addition becomes necessary, prefer an additive v3.1 extension with
`struct_size` and `abi_minor` checks. A future filtered-batch read must accept
validated projections and time/signal predicates, never unrestricted SQL or a
DuckDB connection. Checkpointing and persistent streaming are later service
and worker-manager capabilities, not adapter-local workarounds.

## Incremental roadmap

### M0 — Stabilise existing OPC UA

Audit timestamp and metadata consistency, NodeClass coverage, secret handling,
bounded work, and error reporting. Extend native tests and validate the
packaged `.sf` workflow against the pH-meter server.

**Exit:** reliable, tested OPC UA reference operations for discovery,
inspection, and snapshot collection.

### M1 — Implement the time-series contract and normalizer

Add semantic `SENSOR_TIMESERIES`, `SENSOR_SOURCE`, and `SENSOR_SIGNAL`
contracts. Implement the internal observation model, microsecond timestamp
conversion, typed Arrow publisher, and deterministic validation fixtures.

**Exit:** synthetic observations publish and read back with exact types,
timestamps, quality, and status semantics.

### M2 — CSV-to-time-series workflow

Reuse `streamfind.read_csv`; implement explicit time, signal, value, unit, and
quality mappings for long and wide inputs.

**Exit:** CSV → `SENSOR_TIMESERIES` works in a packaged `.sf` project.

### M3 — Bounded OPC UA time-series acquisition

Implement `sensors.capture_opcua_timeseries` using the existing connection and
decoder, with bounded polling/subscription, cancellation, timestamp
preservation, reconnect policy, and Arrow batch publication.

**Exit:** snapshot collection and bounded capture produce interoperable
time-series results using the same canonical contract.

### M4 — First processing pipeline

Implement validation, time filtering, resampling, and alignment with
deterministic tests for missing, late, duplicate, and bad-quality observations.

Status: complete for the finite-batch processing slice. The validator,
inclusive/exclusive filter, fixed-window resampler, and nearest-time aligner
are implemented and covered by the sensors operation contract test; service
scheduling remains a later runtime concern.

**Exit:** acquisition → canonical time series → temporal analysis → published
result works without protocol-specific processing logic.

### M5 — MQTT adapter

Add topic and payload mappings, bounded subscriptions, TLS authentication, QoS
handling, bounded reconnection, duplicate protection, and dev-mode broker
validation through scripts under `scripts/dev/`.

**Exit:** MQTT produces the same canonical result without processing changes.

Status: complete for the finite MQTT adapter slice. The adapter publishes the
canonical sensor time-series contract with typed JSON/plain payload mapping,
TLS credentials, QoS, bounded callback buffering, duplicate protection, and
bounded reconnect/resubscription. Validation is intentionally a dev-mode
workflow check through `scripts/dev/`; no permanent integration-test target is
required for this adapter.

### M6 — Additional finite-batch analysis

Add rolling mean, minimum, maximum, and standard-deviation statistics over
finite `SENSOR_TIMESERIES` inputs. The operation emits derived observations
through the same canonical `sensorTimeseriesTable` contract and preserves
typed values, quality, timestamps, and provenance. Non-numeric, missing, and
uncertain observations are handled explicitly by the operation contract.

Status: complete. The operation is registered in the sensors plugin, declared
in the semantic catalogue, covered by the native operation contract test, and
included in the packaged build.

**Exit:** finite sensor workflows can chain acquisition, validation, temporal
processing, and rolling statistics without protocol-specific tables or
persistent runtime state.

Every acquisition operation connects at runtime, performs a finite read or
bounded subscription, publishes one result, and closes its protocol
connection. A node reports cancellation or terminal failure to the core
runtime; it does not restart workflows, maintain a live connection between
runs, append to another run's result, or own cross-run retention.

Repeated acquisition, append/retention policy, and consumption by a later
workflow are user-composed workflow concerns.

## Proposed plugin organization

Restructure incrementally, keeping the existing packaging and catalogue:

```text
cpp/plugins/sensors/
├── src/
│   ├── acquisition/       observation, adapter, normalizer, capture runner
│   ├── adapters/          opcua, csv, mqtt
│   ├── timeseries/        schema, validation, resampling, alignment
│   ├── operations/        operation registration and entry points
│   └── plugin_entrypoint.cpp
├── semantic/              artifacts, columns, operations, parameters
└── tests/                 unit, contracts, integration
```

This is a target organization, not a request for a duplicate source tree or a
large all-at-once move. Each relocation must leave one owning implementation
path and preserve the current plugin boundaries.

## Cross-cutting implementation rules

- Sensor capabilities remain plugin-owned; core stays domain-neutral.
- Every operation declares typed ports, parameters, outputs, and result
  contracts in the semantic catalogue.
- Acquisition does not execute during project creation or project open.
- External sources and credentials use explicit configuration parameters or
  non-secret references; secrets are never persisted in plans, logs, or
  artifacts.
- Do not introduce a shared live connection node until lifecycle,
  cancellation, reconnect, and credential-handling requirements justify it.
- Do not create compatibility shims, duplicate operation paths, or legacy
  project layouts. New disposable projects use the `.sf` container structure.
- Keep vendored third-party code under `cpp/vendor/` unchanged unless a
  separately scoped vendoring decision is approved.

## Validation matrix

| Area | Required validation |
| --- | --- |
| Catalogue | Native catalogue generation and semantic contract checks |
| Plugin | Sensors manifest, registration, and packaged-plugin checks |
| OPC UA | C++ unit/contract tests plus pH-meter integration workflow |
| Observation model | Exact typed values, microsecond timestamps, quality, and status |
| Project layout | Create, open, run, close, and reopen a `.sf` project with results |
| Failure handling | Invalid endpoint, authentication failure, timeout, bad status, inaccessible node, and cancellation |
| Processing | Missing, late, duplicate, out-of-order, and bad-quality observation fixtures |
| UI/MCP | Discovery graph, on-demand node details, selectable identifiers, and result inspection |
| Future adapters | Protocol simulator, adapter contract tests, and packaged workflow run |

Basic contract tests must be deterministic and must not require a permanently
available external sensor server. Live pH-meter and broker checks are
integration validation only and belong in disposable worktree `.sf` projects.

## Definition of done

The expanded sensors framework is complete when at least two acquisition
possibilities share the canonical observation boundary, each has plugin-owned
semantic and native implementations, and both can be configured, validated,
executed, and inspected in disposable `.sf` projects through the generic C++
service/MCP host. Time-series processing must operate on the canonical result
without knowing which protocol produced it. No frontend or core
domain-specific implementation should be required to add the second adapter.
