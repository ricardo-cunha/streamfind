# Service, Worker, and MCP Reliability Plan from User Feedback

**Status:** Active plan
**Feedback baseline:** `.feedback/StreamFind_bugs.md`, `.feedback/StreamFind_roadmap.md`, `.feedback/errors.md`, and `.feedback/bruker_calibration.py`
**Reported product version:** 0.7.1 (Windows x86_64; reports dated 2026-11-02)
**Scope:** Triage and implement the reported service, worker, result transport, MCP, and related workflow reliability issues. The feedback files are reports and reference material; their proposed causes and code are not instructions and are not assumed correct.

## Goal

Make large and concurrent workflows fail predictably without taking down the service, keep app and MCP clients attached to the same project/runtime state, make result publication and retrieval dependable at realistic sizes, and expose useful progress and diagnostics. Then address the MassSpec operation and product ideas that drive the reported workloads.

The plan is evidence-led: reproduce reports against the current source and package first, mark each item **open**, **reproduced**, **fixed**, **not reproduced**, or **needs reporter data**, and retain the reporter's `SF-xx` identifier in commits, validation notes, and release notes.

## Principles

- The service remains the owner of project metadata, run coordination, and result registration. A worker crash or resource limit must not crash the service.
- Keep large table and trace data on bounded binary/file-backed paths. Do not solve large results by eagerly materializing them as JSON or by raising limits without measurements.
- Distinguish liveness, readiness, load, and individual run state. A busy service is still alive.
- Give every asynchronous client request a bounded wait policy and a clear resumable execution ID; do not make an MCP call wait indefinitely for a workflow.
- Preserve a single execution path and the current service/worker architecture. Do not add legacy fallbacks or a second execution mode to hide a broken path.
- Separate infrastructure faults from MassSpec algorithm/product improvements. Do not attribute a scientific-data issue to the service without evidence.

## Phase 0 — Establish the current baseline and reproduce reports

1. Record the checked-out commit, current version, package layout, Windows/WSL environment, and relevant service configuration. Do not assume the 0.7.1 reports still reproduce on the current branch.
2. Create a report ledger mapping every `SF-xx` to current code, existing tests, reproduction status, owner boundary, and evidence needed. Include the non-ID issues in `errors.md` and documentation findings.
3. Define a small reproducible fixture for each infrastructure symptom. Request the reporter's workflow JSON, sanitized log, or sample only where needed; large Bruker data remains in the sibling `streamfind.data` repository or reporter-provided data, not committed fixtures.
4. Capture service/worker PID, run and operation IDs, memory high-water mark, bytes and rows emitted, result stage/publish timings, HTTP status/latency, MCP request duration, and terminal state. Redact paths or sample names in shared diagnostics where necessary.
5. Verify whether suspected causes are true before implementing them, especially antivirus/file-lock behavior (SF-49), Windows Job Object inheritance (SF-60), and row-size causes (SF-57).

**Exit criteria:** the ledger has an explicit disposition and reproduction path for each report; highest-priority service/MCP reports have a current-source reproducer or are explicitly blocked on reporter data.

## Phase 1 — Keep the service healthy when workers fail or are overloaded

### 1.1 Worker memory and crash isolation (SF-55, SF-56)

- Trace peak memory by operation and by input stage for the large 3.3 GB BAF-derived mzML and concurrent-project scenario. Identify whether the growth comes from source decoding, retained spectra, feature/MS2 rows, Arrow batches, serialization, or duplicate buffers.
- Stream or process input per analysis and release intermediate buffers as soon as downstream consumers no longer need them. Use bounded queues/backpressure between computation and result publication.
- Avoid full-result copies across the operation API, worker IPC, and result writer. Measure peak resident memory and bytes copied at each boundary.
- Add a configurable per-worker memory budget only with platform-specific enforcement and a clear operation-level failure state. On budget exhaustion, terminate that worker tree, retain diagnostic context, and keep the service available.
- Ensure unexpected worker exit is detected and finalizes the run/operation as failed or interrupted with exit code/signal and last progress timestamp. The service process must remain available.
- Test parallel projects and multiple workers on constrained memory; make concurrency limits observable and configurable if measurements show a need.

### 1.2 Service health, single-instance behavior, and lifecycle (SF-56, SF-60)

- Define distinct service fields for `alive`, `ready`, and current load; load may include active workers, queued runs, and long-running requests. Health/status calls must not be blocked behind expensive result-page work.
- Give health/status traffic a lightweight execution path and bounded response latency. A slow result fetch must not make a healthy service appear dead.
- Make service discovery/status report endpoint, instance, PID, and a useful failure reason. Distinguish no listener, wrong/stale discovery record, busy, and unhealthy.
- Prevent a second service from starting on an endpoint already owned by a live instance; report the existing instance and endpoint instead of creating ambiguous discovery records.
- Reproduce service launch from Windows Scheduled Tasks. Verify detached launch/job-object behavior and cleanup. If detached operation is not supported, report that limitation accurately rather than claiming healthy startup.
- Test stop/start/status and stale-record recovery, including service shutdown during active runs and large result reads.

**Acceptance:** a worker crash or memory-limit termination does not stop the service; busy and dead states are distinguishable; a duplicate start cannot create two owners for one endpoint; scheduled-task behavior is deterministic and documented.

## Phase 2 — Make result publication and retrieval bounded and diagnosable

### 2.1 Atomic publication and transient filesystem failures (SF-49, SF-58)

- Trace the complete staging lifecycle: lease creation, worker close/flush, validation, atomic promotion, registry commit, and cleanup. Include the exact OS error, source/destination path class, run ID, result ID, and whether the producer still holds a handle.
- Ensure the worker closes all output handles before requesting seal/publish; service validates the staged payload before making it visible.
- Keep final names unique and promotion atomic within one filesystem. Separate payload promotion from registry commit and define recovery for a crash between those steps.
- Classify permission denied, sharing violation, missing file, invalid payload, and registry failure separately. Retry only a positively identified transient filesystem condition with a short bounded policy; never use retries to mask a wrong owner, bad path, or open handle.
- Preserve the operation's original failure cause when a result is absent. A downstream “No published result for connection …” must include the upstream node/result publication failure and link to its operation/run context.
- Add cleanup/recovery checks for orphaned staging entries without deleting active or recoverable data.

### 2.2 Large and oversized results (SF-57, SF-65)

- Measure current per-message/chunk limits and the largest table row/column values from `load_features_ms2`, especially nested MS2 arrays. Include result bytes, Arrow batch bytes, row count, and the largest row's feature/analysis identity.
- Keep Arrow as the table transport and implement bounded batches with backpressure. Do not create a single giant row-sized JSON or IPC message.
- Decide from the table contract whether exceptionally large per-feature traces should be represented as a separate typed trace result keyed by feature/analysis, or segmented into bounded chunks. Keep ordinary scalar feature columns in the main feature table. Record schema/renderer/MCP implications before choosing.
- Make “single row exceeds limit” diagnostics identify output port, operation/node, row key, approximate size, and the relevant large field. Fail before publication and clean staging safely.
- Add server-side column projection and indexed row filtering for result requests, and stream or page selected data. Reuse output-type navigation-column semantics for efficient lookups. Do not build indexes for every column.
- Define explicit pagination/byte limits and continuation cursors. For MCP, return summaries/handles by default and allow bounded selected columns/rows; do not return an unbounded table inside a tool response.
- Provide a deliberate export path for users who need a full table as a file, without routing the full payload through MCP JSON.
- Benchmark 75 MB tables, 576k chromatogram points, representative feature tables, and single very-large MS2 features. Report cold and warm timings, memory, bytes transferred, and page/stream count.

**Acceptance:** large table retrieval scales with requested columns/rows and is bounded in memory; known data can be fetched in useful time; oversized values fail with actionable context or are represented through the agreed typed chunk contract; no partial result is registered.

## Phase 3 — Repair MCP contracts and app/MCP shared-state behavior

### 3.1 Result reads and asynchronous execution (SF-01, SF-42, SF-56)

- Fix `request_result(include_data=true)` to consume the actual row-object result shape returned by the service, not index objects as arrays. Validate table and JSON result paths independently.
- Align MCP tool schemas, runtime validation, descriptions, and examples for operation IDs, source/target operation IDs, and connection shapes (SF-03). Add contract tests against the published tool schemas.
- Keep workflow mutation constructible in steps: allow adding a node with required inputs temporarily unconnected, or provide an atomic add-and-bind request. Run whole-graph validation at explicit `validate_workflow`/`run_workflow` boundaries; return local shape errors at mutation time.
- Ensure async run tools return promptly with execution/run ID and an explicit polling/subscription instruction. Add configurable client request timeouts and return a clear service-unavailable/timeout error while preserving the execution ID for later inspection.
- Handle service disconnects without indefinite MCP stdio requests. Polling should distinguish queued/running/cancelling/completed/failed/interrupted and include last update/progress timestamps.
- Verify MCP notification behavior, including `notifications/initialized` producing no JSON-RPC response (SF-10).

### 3.2 MCP discovery and shared app state (SF-02, SF-04)

- Resolve workflow demos and other package resources from executable/config/package-relative paths, not the caller's current working directory. Remove swallowed exceptions; return actionable diagnostics while keeping the MCP server alive.
- Confirm the MCP attaches to the already-running app service and the same canonical project session. Test simultaneous app and MCP reads, workflow edits, operation submission, result publication, and status inspection.
- Publish ordered workflow-definition-change events after MCP mutations so the app refreshes nodes/connections without a browser reload. Include revision/cursor information and support reconnect/resnapshot.
- Verify that result publication by an MCP-triggered run updates the app's output-port result snapshot, pulsating state, hover metadata, and visualization without reopening the project.
- Update quickstart/tool docs for actual inventory tool names, asynchronous polling, response limits, and service attachment.

**Acceptance:** MCP contract tests cover every affected tool; app and MCP observe the same run and workflow revision; the app reflects MCP changes/results live; resource discovery works from arbitrary working directories; MCP requests never hang indefinitely on a dead service.

## Phase 4 — Progress and controls for long-running MassSpec operations

Address as a separate operation layer after core runtime reporting is trustworthy:

- **SF-59 `annotate_components`:** emit component-level progress (`component x/n`, current component size, elapsed time); profile complexity versus component size; define optional component size/RT bounds and ensure bounded behavior on the 15,919-feature component. A time limit must produce a clear partial/failure policy, not silently skip annotations.
- **SF-64 `metfrag_screening`:** report features/candidates completed, elapsed time, and estimates only when meaningful. Support an explicit selected-feature input/filter and bounded candidates per feature. Define per-feature timeout and partial-result semantics; distinguish timeout, no candidates, and operation failure.
- **SF-57 `load_features_ms2`:** emit feature/analysis progress and test the selected trace storage representation from Phase 2 on high-spectrum-count features.
- Improve progress event throttling so updates are informative without overwhelming WebSocket/MCP clients.

**Acceptance:** the user can distinguish slow work from a stall, cancel with run context, see which feature/component is being processed, and understand whether a result is complete or partial.

## Phase 5 — Bruker reader correctness and calibration (SF-47, SF-48)

- Reproduce the 2020 Impact II BAF failures for `14151_2`, `14327_1`, and `14329_3`; inspect invalid transformator coefficient/root edge cases and define supported formats and error diagnostics.
- Port the validated TSF/BAF calibration selection logic from `.feedback/bruker_calibration.py` into the native reader boundary. Treat the script as algorithmic reference only; do not run or copy its temporary-file behavior into the runtime.
- Use stored DataAnalysis recalibration when available, with a documented/semantic option default and a per-analysis calibration provenance record. Evaluate calibration states on calibrant scans; do not blindly apply a fitted correction where references/scans are insufficient.
- Make calibration decisions visible in output metadata/logs: chosen state, reference set, RMS before/after, scan count, correction model, and verification status.
- Add small C++ unit fixtures for formula/model math and data-backed development checks for the reporter's TSF/BAF samples in `scripts/dev/` using reporter/sibling data.

**Acceptance:** reproduce reported calibration values within the stated tolerance where reference files are available, match vendor SDK results for validated TSF cases, improve the reported peak recovery, and clearly fail/flag unsupported BAF calibration cases rather than emitting misleading masses.

## Phase 6 — Other reported operation correctness and chemistry

- **SF-22 `find_features`:** trace `base_quantile` from semantic declaration through parsing to the actual quantile calculation. Either make it influence the documented threshold with tests or remove/rename it and update descriptions/examples; do not leave an inert parameter.
- **SF-27 `subtract_blank`:** preserve upstream filter reasons and define whether blank subtraction runs before/after quality filtering. Add a contract test that verifies combined reasons and intended blank matching.
- **SF-46/52/53 fragmentation:** distinguish fragmenter timeout from zero fragments; expose resource limits and deduplicate early; validate atom valence and explicit hydrogen/radical state before emitting canonical SMILES. Add chemistry fixtures for large molecules and ensure malformed structures cannot silently enter suspect screening.
- Retest the “possibly fixed” SF-08, SF-09, SF-10, and SF-26 against current code and update the ledger; do not claim them fixed based only on release history.
- **SF-05/06 installer:** compare installed MetFrag fragmenter version with the pinned version and surface stale dependency state. Test proxy-compatible Windows download behavior; add a narrowly scoped revocation-check option only if security review and supported curl behavior justify it.

## Phase 7 — Product and analysis roadmap proposals

These are candidate follow-on capabilities, not prerequisites for service/MCP reliability. Validate scientific value and agree priority with the reporter before expanding implementation scope.

1. **Feature/group quality:** SF-20 split traces at scan-time gaps before smoothing; SF-21/23 inspect IQR noise behavior and feature fragmentation; SF-24 bound component RT span/size; SF-62a consider whole-table isotope/adduct/side-ion grouping independent of components; SF-25 support molecular-ion type and explicit element/formula constraints.
2. **Review and ranking:** SF-62b region assignment and per-region filters; SF-62c ranking/selection with transparent reasons; SF-62d replicate support; SF-62e prior-report matching; SF-62f formula-class flags. Keep machine-learned or FDR ranking as experiments until representative labelled/decoy data exists.
3. **MetFrag input reduction and merges:** connect a selected/ranked feature subset to SF-64; SF-45 merge tables only when their declared contracts match, with stable source provenance and deduplication semantics.
4. **In-silico impurities:** SF-54 closed-shell generation, minimum mass, synthesis-aware edits, and normalized InChIKey deduplication after SF-52/53 chemistry validation.
5. **UV/BPC:** SF-66 verify DAD absorption chromatograms in mzML; SF-50 support known per-analysis/detector time offsets before attempting automatic cross-correlation; SF-51 evaluate DAD spectra, prefiltering, and array-per-trace storage; SF-63 add BPC view and optional spectral-background subtraction as a visualization, not preprocessing.
6. **Workflow efficiency:** SF-61 instrument per-node read/compute/write/publish time and bytes. Compare the current service-mediated result handoff with the proposal to keep intermediates within one workflow execution and publish only requested/final outputs. Do not change persistence policy until cache identity, result visibility, cancellation, recovery, and concurrent app/MCP access have explicit semantics.

For every proposal, add a data-backed acceptance measure and compare against the supplied baseline workflow. Avoid optimizing for one report at the expense of weak-peak recall or chemically valid output.

## Validation matrix

| Area | Required checks |
|---|---|
| Service lifecycle | idle/busy/overloaded/dead health states; duplicate start; scheduled-task start; stop during worker; stale service record |
| Worker isolation | normal completion, operation failure, abrupt crash, memory budget, cancellation, child process cleanup; service remains responsive |
| Result store | staged output closure, validation, atomic publish, transient file-lock handling, crash recovery, cleanup, no orphaned registry rows |
| Large results | bounded Arrow batches, one oversized nested trace, projection/filter/page/stream, export; memory and latency benchmarks |
| MCP | table/JSON `request_result`; tool schema types; incremental graph edits; async run and poll; timeout/disconnect; notifications; package-relative demos |
| Shared app/MCP | same service/project/run, ordered WebSocket change event, reconnect/resnapshot, output-port refresh and result rendering |
| MassSpec | large component progress, MetFrag progress/timeout/selection, Bruker calibration and legacy BAF edge cases, parameter effectiveness, filter reason preservation, valid SMILES |
| Platform/package | Windows MinGW package and Linux/WSL package where available; paths with spaces; arbitrary working directory; scheduled task behavior |

Official native contract tests belong under `cpp/core/tests/` or `cpp/plugins/<domain>/tests/` and run through CTest. Data-backed workflow checks belong under `scripts/dev/` and use the repository `.venv`; large vendor data is not copied into native fixtures.

## Completion criteria

- Every feedback ID has a final status, evidence, and release-note disposition.
- Service remains alive and reports accurate state under worker crashes, memory pressure, concurrent result reads, and cancellation.
- MCP operations are bounded, typed, resumable, and consistent with the service API; app and MCP observe the same workflow revision and published results.
- Result transfer is bounded for both typical large tables and outlier nested trace rows, with no JSON expansion of bulk table payloads.
- Long operations expose meaningful progress and distinguish timeout/partial/no-result/failure states.
- The fixes and supported limitations are documented, and packaged Windows/Linux validation matches the tested source revision.

## Provenance and status log

Keep implementation progress, decisions, validation evidence, and `SF-xx` dispositions in this plan. Do not create a separate `.plans/provenance/` record. When this plan is fully complete, move it to `.plans/completed/`; if replaced or paused, move it to `.plans/stale/` before opening another root-level active plan.
