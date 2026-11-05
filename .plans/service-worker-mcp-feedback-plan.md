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
- Use service-issued OS file handles for all worker payload I/O. The service allocates, validates, publishes, and removes result-store entries; the worker runtime reads/writes their bytes directly through restricted handles. JSON pipes carry control, metadata, progress, and status only.
- Publish every operation output as an immutable result file, including all workflow intermediates. Downstream operations receive result IDs and fresh read-only handle grants. Remove the transient workflow payload store and persistence-mode branches; standalone and workflow runs use one publication path.
- Distinguish liveness, readiness, load, and individual run state. A busy service is still alive.
- Give every asynchronous client request a bounded wait policy and a clear resumable execution ID; do not make an MCP call wait indefinitely for a workflow.
- Preserve a single execution path and the current service/worker architecture. Do not add legacy fallbacks or a second execution mode to hide a broken path.
- Separate infrastructure faults from MassSpec algorithm/product improvements. Do not attribute a scientific-data issue to the service without evidence.

## Phase 0 — Establish the current baseline and reproduce reports

1. Record the checked-out commit, current version, package layout, Windows/WSL environment, and relevant service configuration. Do not assume the 0.7.1 reports still reproduce on the current branch.
2. Create a report ledger mapping every `SF-xx` to current code, existing tests, reproduction status, owner boundary, and evidence needed. Include the non-ID issues in `errors.md` and documentation findings.
3. Define a small reproducible fixture for each infrastructure symptom. Request the reporter's workflow JSON, sanitized log, or sample only where needed. The example files referenced by the feedback are available under `E:\example_files\ms_merck` and may be used to reproduce applicable MassSpec/Bruker issues. Keep these data external to the repository; do not copy them into committed fixtures. Other large Bruker data may come from the sibling `streamfind.data` repository or reporter-provided files.
4. Capture service/worker PID, run and operation IDs, memory high-water mark, bytes and rows emitted, result stage/publish timings, HTTP status/latency, MCP request duration, and terminal state. Redact paths or sample names in shared diagnostics where necessary.
5. Verify whether suspected causes are true before implementing them, especially antivirus/file-lock behavior (SF-49), Windows Job Object inheritance (SF-60), and row-size causes (SF-57).

**Exit criteria:** the ledger has an explicit disposition and reproduction path for each report; highest-priority service/MCP reports have a current-source reproducer or are explicitly blocked on reporter data.

### Phase 0 execution record — 2026-11-04

#### Baseline captured

| Item | Observed value |
|---|---|
| Checkout | `dev_refactoring`, commit `8d80e8d7` (`Update service-worker-mcp-feedback-plan.md`), clean before this record |
| Product version | `0.7.1` (`VERSION`) |
| Host | Windows x64, PowerShell 7.6.5 in the current development shell |
| Build toolchain | `sfdev env show --json`: CMake 4.3.3, GCC/G++ 16.1, Ninja 1.13.2; repository `.venv` Python 3.11.16; npm 11.19.0 / Node 26.7.0; no reported environment issues |
| Canonical native build | `tmp/build/windows/core` |
| Packaged build | `tmp/packaged_build` exists; package binaries were last written 2026-10-25 and therefore predate this checkout; treat it as stale for feedback validation |
| Packaged service config | `bin/streamfind.json`: port 8787; tray enabled and configured to start with app/service; plugin roots package-relative; MassSpec, Raman, Sensors, and Statistics enabled |
| Runtime state at inspection | No `streamfind*` process and no listener on ports 8787 or 8790; no project was attached to a running service |
| External example data | `E:\example_files\ms_merck` exists. It contains `ACC1_27166_find_features_debug.duckdb`, `ACC1_27166_smoothing_comparison.duckdb`, `ACC1_27803.duckdb`, `ACC1_27989.duckdb`, and `merck_test_short.duckdb`, with sidecars/workflow JSONs for several. Keep these user data external and do not mutate canonical projects during triage. |

The feedback names report files such as `20766_1`, `14151_2`, `14327_1`, `14329_3`, `23111_1`, `20878_1`, and `21072_1`. Those exact source runs were not located by name in the inspected Merck directory. The ACC1 projects are suitable for general NTA/MassSpec regression and some Bruker/result-store checks, but they do not reproduce those specific reports without the promised source `.d`/mzML inputs and workflow JSON. The feedback-provided `bruker_calibration.py` is reference material only; it uses its own temporary-file behavior and was not executed.

#### Report ledger

“Source-confirmed” below means the relevant code path is present in the current checkout; it does not mean the reporter’s complete scenario has been reproduced. “Needs data/run” means the plan must retain the report as open until a controlled current-source run provides evidence. Existing focused native coverage is noted where located; no large-data workflow was run in Phase 0.

| Feedback ID | Current-source finding / disposition | Owner and Phase 0 reproduction path | Existing focused coverage found |
|---|---|---|---|
| SF-01 | **Source-confirmed defect:** MCP `request_result` and the parallel API result path iterate `row.at(index)` while the service table response exposes rows as objects. | Core MCP/API. Request one bounded table page with `include_data=true`; compare service rows to MCP output. | No direct contract test found. |
| SF-02 | **Source-confirmed risk:** demo discovery uses `current_path()/plugins` and its parent, plus optional environment configuration; parse errors are swallowed. | MCP packaging. Invoke `list_workflow_demos` from a directory outside the package and capture diagnostics. | `mcp_discovery_contract.cpp` exists but no CWD/package-resource case identified. |
| SF-03 | **Partially source-confirmed:** add/connect schemas still describe operation IDs as strings, and whole-workflow validation is deferred; verify actual workflow ID representation and incremental graph behavior before calling the report reproduced. | MCP schema and workflow mutation. Run add-node, connect-input, validate in order against a disposable project. | `cpp/core/tests/fixtures/mcp/execution_contract.json`; no incremental graph case identified. |
| SF-04 | **Needs app/service event check:** project callbacks persist operation events, but no confirmed workflow-definition-change broadcast was found in this pass. | Service WebSocket + frontend. Mutate via MCP while app is open; compare revision, nodes, and event cursor without reload. | No focused live-refresh test found. |
| SF-05 | **Needs runtime check:** plugin metadata pins fragmenter v0.2.0; existing-install version comparison/upgrade behavior has not been verified. | MassSpec dependency installer. Install a v0.1.0 fixture under isolated `.streamfind`, request v0.2.0, inspect selected JAR/version. | No installer test found. |
| SF-06 | **Needs proxy reproduction:** reported Windows Schannel revocation failure not reproduced. | Dependency download. Reproduce behind the reporter’s proxy or sanitized proxy harness; capture curl exit/error and security settings. | No proxy-specific test found. |
| SF-07 | Roadmap references the large-component scenario but has no separately scoped bug reproduction in the reports inspected; track with SF-24/SF-59 until clarified. | NTA feature/component analysis. Needs exact issue details or map to the reported 15,919-feature component. | No dedicated test found. |
| SF-08, SF-09, SF-10, SF-26 | Feedback labels these as possibly fixed; **not retested** on this checkout. SF-10 requires checking that `notifications/initialized` produces no JSON-RPC response. | MCP protocol and related operations. Run the original report steps once their exact cases are extracted from feedback. | MCP contract tests exist; these specific regression cases were not confirmed. |
| SF-20, SF-21, SF-23 | Product proposals: RT-gap splitting, noise estimation, and feature fragmentation; **not defects reproduced in Phase 0**. | MassSpec NTA. Compare the supplied baseline workflow and representative raw data after agreeing scientific acceptance metrics. | No data-backed acceptance test identified. |
| SF-22 | **Source path confirmed, behavior unverified:** `base_quantile` feeds adaptive quantile calculation and is clamped against a dynamic upper bound. Need compare outputs at reported values 0.1 and 0.01 on the same input. | `find_features`/deconvolution. Run controlled parameter sweep with same source and all other parameters fixed. | No specific regression test found. |
| SF-24 | Proposal for component RT-span/size bounds; linked to SF-58/SF-59, but acceptance policy is not yet validated. | `create_components`; use reported 15,919-feature component when reporter data are available. | No large-component fixture found. |
| SF-25 | Product proposal for ion type and formula element bounds; no failure report reproduced. | MassSpec annotation semantics and operation. Define expected formula/ion cases first. | No dedicated test found. |
| SF-27 | **Needs output-contract check:** inspect whether blank subtraction preserves pre-existing filter reasons and how already-filtered blank features participate. | `subtract_blank`. Run a tiny synthetic sample/blank table with existing filter values and assert combined reasons. | No focused operation test found. |
| SF-40 | Documentation/release-note quality item; **not audited for completeness** in Phase 0. | Release docs. Compare 0.6.0/0.7.0/0.7.1 notes for breaking changes, renamed parameters, MCP tools, and evidence. | Documentation review, not a runtime test. |
| SF-41 | Unnumbered `errors.md` confirms tool-name mismatch (`get_result_registry` vs `get_result_inventory`); current docs need a direct search/audit. | MCP quickstart. Search user-facing docs and verify names against the current tool list. | No docs test identified. |
| SF-42 | **Partly addressed in source:** `run_operation` detaches execution and returns an accepted/queued response with polling guidance. Full `run_workflow` timeout/disconnect behavior still needs MCP-client reproduction and docs check. | MCP async lifecycle. Invoke a long operation; verify immediate response, polling, terminal failure, and client disconnect behavior. | `execution_contract.json`; no long-running client timeout case identified. |
| SF-43 | Column naming/`ppm` semantics report; **not validated** against current ontology, UI labels, and `filter_features` parameter descriptions. | MassSpec semantic catalogue/docs. Trace `ppm` and `max_ppm` from ontology to renderer and operation code. | Ontology catalogue generation exists; no semantic regression identified. |
| SF-45 | Same-contract table merge is a roadmap proposal, not a reproduced defect. | Workflow/result contract design; require provenance and duplicate-row expectations. | No implementation acceptance test yet. |
| SF-46 | **Needs reporter input:** large-molecule depth-2 timeout/zero-row scenario and exact HTM-526 structure are absent from local fixtures. | Fragmenter operation. Reproduce with exact structure; capture child exit, timeout, stderr, row count at depths 1 and 2. | No large-molecule fixture found. |
| SF-47 | **Needs reporter data:** Bruker reader exists; exact 2020 Impact BAF files were not found locally, so transformator/root failures are unverified here. | Bruker reader. Use `14151_2`, `14327_1`, `14329_3` source `.d` files and compare native reader with vendor-converted mzML. | No reporter-data C++ fixture found. |
| SF-48 | **Needs reporter data:** acquisition calibration reader exists; DataAnalysis recalibration behavior and reported ppm comparisons cannot be confirmed without the named TSF/BAF runs. | Bruker calibration. Re-run named calibrant masses; compare stored calibration states, SDK output, and native output. | No reporter-data calibration fixture found. |
| SF-49 | **Suspected cause unverified:** publication has staging/promotion paths; no evidence yet that antivirus or an open producer handle caused the permission error. | Result store/service. Capture OS error, source/destination, result/run IDs, producer-close time, and publication timings under parallel projects. | Result-store Arrow contract test exists; no transient-lock test found. |
| SF-50 | UV/MS time-offset capability proposal; no run reproduced. | DAD/MS analysis. Needs paired channels and a declared offset acceptance measure. | No test found. |
| SF-51 | DAD spectrum/storage proposal; no run reproduced. | MassSpec reader and result schema. Needs representative DAD dataset and memory/latency baseline. | No test found. |
| SF-52 | **Needs chemistry fixture:** current expansion output must be sanitized against the reported 1,018 structures; hypervalence is not confirmed from current code alone. | Fragment-tree expansion. Re-run reporter tree and compare valence/sanitization per output row. | No chemistry regression fixture found. |
| SF-53 | **Needs chemistry fixture:** bracket-H/radical serialization needs strict parser/sanitizer checks on the reported suspect list. | Suspect conversion/serialization. Re-run the 1,018-row set and record valid count and canonical structures. | No reporter-data regression fixture found. |
| SF-54 | Closed-shell/synthesis-aware impurity generation proposal; not a reproduced defect. | In-silico impurity generation, after SF-52/53 validation. | No agreed acceptance data. |
| SF-55 | **Validate by representative load simulation; exact reporter file is not required.** The report describes a workflow whose later stages process data derived from locally available Merck examples, run on a 16 GB machine. The development host has 256 GB, so an unconstrained run would not exercise comparable pressure. | Use disposable copies of ACC1/other available Merck workflows and their late-stage operations; scale or replay schema-faithful result traffic and run concurrent project workers. Measure worker and service RSS, bytes/rows, stage timings, result integrity, health latency, and cleanup. Where practical, enforce a documented aggregate 16 GiB test envelope; do not interpret it as a 16 GiB per-worker recommendation. | Synthetic worker memory-limit contract exists; service-level representative-load simulation is pending. |
| SF-56 | **Source-confirmed health-classification risk:** CLI health probe applies 500 ms send/receive socket timeouts and maps any request error to `unreachable`; this can conflate slow/busy and dead. Hangs and duplicate service starts still need reproduction. | Service health + MCP client. Exercise idle, busy, long result page, dead server, stale record, and duplicate start; record latency/state. | MCP execution contract exists; no busy-vs-dead health test identified. |
| SF-57 | **Boundary behavior source-confirmed; sample cause unverified:** table output halves a batch to one row, then throws when its Arrow frame remains above 1 MiB. The reported large MS2 row needs the named sample to confirm size and field. | MS2 output + result sink. Re-run `load_features_ms2`; record largest row/field and exact encoded frame size. | Result-store Arrow contract exists; no oversized-row contract test located. |
| SF-58 | **Needs reporter run:** missing published `create_components` result may be a worker failure, output serialization limit, or publication failure; no single cause established. | Workflow scheduler/result publication. Reproduce the 14151_2 workflow; correlate worker exit, operation error, output lease, and registry commit. | No large-component end-to-end fixture found. |
| SF-59 | **Partial source support, performance unverified:** `annotate_components` reports progress at component-related stages, but the reported >60-minute behavior and progress visibility need current app/MCP event testing. | Annotation operation + progress delivery. Record component index/size and event timestamps for reporter’s large components. | No large-component performance test found. |
| SF-60 | **Needs Windows Scheduled Task reproduction:** Job Object inheritance/launch behavior remains a hypothesis; no current launch test confirms it. | Service process launch. Start from the same interactive scheduled-task configuration; capture parent/child/job state and service PID after launcher exit. | No scheduled-task test found. |
| SF-61 | Per-step timing/overhead instrumentation proposal; current phase defines required fields but does not yet establish an end-to-end timing baseline. | Scheduler and result store. Time read/compute/emit/publish separately on the supplied baseline workflow. | No timing benchmark found. |
| SF-62a–f | Whole-table isotope/adduct grouping, regions, ranking, replicate support, history matching, and formula-class flags are roadmap proposals, not confirmed regressions. | MassSpec product scope. Define one scientific acceptance dataset and transparent ranking/grouping criteria before implementation. | No accepted evaluation set identified. |
| SF-63 | BPC/background visualization proposal; no report reproduced. | Visualization and optional preprocessing. Needs representative data and plot comparison. | No test found. |
| SF-64 | **Progress path exists, performance unresolved:** MetFrag runner emits progress messages in current source, but the 3-hour/no-result run, candidate selection, timeout behavior, and progress reachability need a reporter workflow. | MetFrag operation + worker event transport. Run selected reporter feature/candidates; correlate child process, progress sequence, elapsed time, and publication. | No long-running MetFrag integration fixture found. |
| SF-65 | **Needs large-result benchmark:** current result paging exists, but the 75 MB table/576k-point latency report has not been repeated on this source/package. | Result API and renderers. Benchmark page/selected columns/trace endpoints, cold and warm, with transfer bytes and server/client timings. | `result_store_arrow_contract.cpp`; no scale benchmark found. |
| SF-66 | DAD trace reading proposal; not a reproduced failure. | mzML reader and DAD result contract. Needs a representative file and expected trace. | No test found. |
| SF-67 | MS1 scan-gap QC proposal; not a reproduced failure. | MassSpec quality-control operation. Needs expected gap detection and report format. | No test found. |

#### Reproduction fixtures and evidence to capture

- Use disposable copies under `tmp/projects/` for workflow edits/runs. Do not delete or rewrite the canonical databases under `E:\example_files\ms_merck`.
- Start Phase 1 infrastructure checks from clean source-built and packaged binaries; the existing packaged build is stale and is not evidence for current source behavior.
- General data-backed workflow candidates available locally are the ACC1 27166 smoothing/debug projects and ACC1 27803/27989/`merck_test_short` projects with their sidecars/workflow JSONs. First inventory their result-store state, source paths, and workflow revisions; then copy to `tmp/projects/` before any destructive rerun.
- Ask for exact reporter `.d`/mzML inputs and workflow JSON only for cases whose named assets are not present: especially SF-47/48, SF-57–59, SF-64, and the exact large-molecule SF-46/52/53 chemistry input. For SF-55, use a documented service/worker load simulation based on the available Merck examples; reproducing the exact 3.3 GB source file is not a requirement.
- For every reproduction record: source commit/version, binary path and timestamp, OS/build details, service instance/PID/endpoint, project session ID, run/node/operation/result IDs, worker PID/Job Object or process group, operation parameters, row/byte counts, largest row and field, per-stage elapsed time, process RSS high-water, HTTP/MCP latency/status, event sequence, OS error, and terminal state. Redact user paths/sample identifiers in any shared report.

#### Phase 0 disposition

Phase 0 is **complete as a static baseline and triage ledger**. Highest-priority issues SF-01, SF-02, SF-56, and the SF-57 chunk boundary have concrete current-source observations; none of the external-data-dependent end-to-end reports are claimed reproduced. The next work is to refresh the source-built/package baseline, add focused contract reproductions for the confirmed MCP/result/health cases, and obtain or safely stage the missing reporter datasets before attributing causes to memory pressure, Bruker calibration, file locks, or chemistry.

## Phase 1 — Keep the service healthy when workers fail or are overloaded

### 1.1 Worker memory and crash isolation (SF-55, SF-56)

- Simulate the reported large-data workflow using disposable copies of the available Merck projects, focusing on later feature/component/annotation and result-reading stages where the report says the data grew. Do not require or claim reproduction of the exact 3.3 GB BAF-derived mzML. Scale the workload with schema-faithful rows/batches or repeated analyses and exercise one and multiple concurrent projects.
- Profile worker-tree and service RSS high-water, emitted bytes/rows, stage timings, result-store I/O, health/status latency, and result integrity. The reporter had 16 GB total RAM while the development host has 256 GB: where safe and available, run the simulation within a documented aggregate 16 GiB environment limit; otherwise increase concurrent/scaled load and explicitly report that the host was not hard-limited. Do not set a 16 GiB per-worker cap or infer a production memory budget from this simulation.
- Stream or process input per analysis and release intermediate buffers as soon as downstream consumers no longer need them. Use bounded decoding/writing buffers over service-issued file handles, with disk-backed published results between operations. File backing reduces service payload buffering but does not itself bound operation allocations, Arrow decoding, or the OS file cache; measure these separately.
- Avoid full-result copies across the operation API, worker IPC, and result writer. Measure peak resident memory and bytes copied at each boundary.
- Keep explicit per-worker memory budgets optional and unlimited by default. Operating systems already arbitrate system memory; a fixed application cap can reject a valid large dataset. If a user configures a cap, enforce it per worker tree and return a clear operation-level failure. Under Linux system OOM pressure, bias reclamation toward workers; use a service-wide cgroup budget only where strict containment is required and the host grants cgroup control.
- Ensure unexpected worker exit is detected and finalizes the run/operation as failed or interrupted with exit code/signal and last progress timestamp. The service process must remain available.
- Test parallel projects and multiple workers under the simulated memory envelope; verify the service remains responsive, published results are complete, and a worker failure or configured per-worker limit does not terminate the service. Make concurrency limits observable and configurable only if measurements show a need.

### 1.2 Service health, single-instance behavior, and lifecycle (SF-56, SF-60)

- Define distinct service fields for `alive`, `ready`, and current load; load may include active workers, queued runs, and long-running requests. Health/status calls must not be blocked behind expensive result-page work.
- Give health/status traffic a lightweight execution path and bounded response latency. A slow result fetch must not make a healthy service appear dead.
- Make service discovery/status report endpoint, instance, PID, and a useful failure reason. Distinguish no listener, wrong/stale discovery record, busy, and unhealthy.
- Prevent a second service from starting on an endpoint already owned by a live instance; report the existing instance and endpoint instead of creating ambiguous discovery records.
- Reproduce service launch from Windows Scheduled Tasks. Verify detached launch/job-object behavior and cleanup. If detached operation is not supported, report that limitation accurately rather than claiming healthy startup.
- Test stop/start/status and stale-record recovery, including service shutdown during active runs and large result reads.

**Acceptance:** a worker crash or memory-limit termination does not stop the service; busy and dead states are distinguishable; a duplicate start cannot create two owners for one endpoint; scheduled-task behavior is deterministic and documented; representative scaled/concurrent project load preserves result integrity and service responsiveness. The simulation documents its data source, scaling method, aggregate memory envelope (or lack of a hard envelope), and limits; it is not reported as an exact reproduction of SF-55's 3.3 GB input.

### Phase 1 implementation record — 2026-11-04 to 2026-11-05 (partial)

- Added a lightweight `GET /health` response with `alive`, `ready`, `state`, active worker/workflow counts, attached-project count, and service instance metadata. CLI service status now queries this route rather than `/session`, which constructs the full project inventory. CLI output includes the active worker/workflow counts. This addresses the 500 ms `/session` health-probe risk from SF-56 at source level; live busy/dead latency behavior still needs a service lifecycle run.
- Added `worker.memory_limit_mb` to `streamfind.json`, defaulting to `0` (unlimited), and documented it. Windows applies a Job Object job-memory ceiling and monitors the job high-water mark; Linux samples aggregate resident memory for the worker process group every 100 ms and kills that group on limit breach. The failure is returned as a worker/operation failure with the configured budget, leaving the service process outside the worker tree.
- **Memory policy:** operating systems do apply system-wide memory safeguards, but they do not guarantee the service remains responsive or is spared from OOM termination. Windows may page heavily; Linux selects OOM victims by score, not by StreamFind's worker/service relationship. Keep `worker.memory_limit_mb` opt-in and unlimited by default because large valid datasets need memory. Linux workers now set `oom_score_adj=500` before `exec`, biasing OOM reclamation toward the worker and inherited descendants; this is best-effort and not a hard cap. The optional Linux configured cap is still sampled process-group RSS, not cgroup enforcement. For strict aggregate containment, require an explicit deployment-level cgroup budget/admission design rather than guessing a cap from dataset size.
- Added a native CTest contract for default, valid, and invalid worker-memory configuration. Build of `streamfind_service`, `streamfind_cli`, `streamfind_worker`, and the contract target succeeded with the required MinGW UCRT64 toolchain; `streamfind_service_configuration_contract` passed.
- Fixed worker-exit propagation: a worker that exits non-zero without an IPC error now returns a specific failure message instead of looking like an empty successful result. Added a subprocess contract covering success, abrupt non-zero exit, a 64 MiB memory-limited worker, and a later successful worker in the same supervisor process. On this Windows host, both new focused contracts pass.
- Reconfigured a clean Linux build tree in WSL after finding the earlier configure attempt had retained a Windows MSYS2 `CURL_DIR`. The service-configuration and worker-supervisor contracts both build and pass on Linux; this exercises process-group cleanup, memory-limit handling, abnormal exit reporting, and continued supervisor use after a failed worker.
- Linux worker startup now best-effort raises `oom_score_adj` for the worker process before `exec`, so Linux OOM pressure is more likely to reclaim a worker tree before the service. Added a Linux-only contract check that the worker receives this score; the contract still verifies a worker failure does not prevent the supervisor from running another worker.
- Worker lifecycle metadata now includes sampled `peak_memory_bytes` plus `peak_memory_kind` (`job_commit` on Windows, `process_group_rss` on Linux). Sampling is active with the worker supervisor, independent of whether an optional cap is configured, so real operation runs can guide any future budget. The memory-cap contract asserts that usage telemetry was captured before the worker was terminated.
- Worker lifecycle metadata now includes `last_progress_at` in UTC, refreshed when the supervisor receives a worker progress/log event. The worker contract checks that progress is reflected in the final worker record, improving diagnostics for abnormal exits and stalled work.
- Extended the worker-supervisor contract with a blocking operation cancelled from a second thread. The contract verifies cancellation completes and the same supervisor can continue to execute workers; it passes on Windows and Linux.
- Added a finite, configurable inactivity timeout to the C++ service client (`STREAMFIND_SERVICE_REQUEST_TIMEOUT_SECONDS`, default 300 seconds). Socket receive/send waits now terminate with a specific timeout error; this prevents MCP-side status/data calls from hanging indefinitely when the service accepts a connection but never responds. Added a native contract with a deliberately unresponsive local endpoint.
- Verified the service-client timeout contract with the service-configuration and worker-supervisor contracts on Windows MinGW UCRT64 and WSL Linux. All three passed on both platforms. WSL initially reused Windows MSYS2 Protobuf/re2/CURL paths from old CMake caches; clearing those package-directory hints and using a Linux-only `PATH` produced a clean Linux reconfigure and successful contract build. This validates the focused client/supervisor code, not a full Linux service package.
- Follow-up build-environment isolation (2026-11-05): Linux shell builds remove Windows-mounted executable paths and Windows/MSYS2 CMake/pkg-config hints; the Linux preset supplies a native Linux tool path, clears inherited package-search variables, and disables package registries. Linux wrappers and `sfdev configure` identify contaminated managed CMake caches and recreate them under `tmp/build`, while refusing to remove a build tree outside that managed directory. Verified the sanitizer/cache reset with a synthetic contaminated cache, then configured from WSL while the parent environment deliberately contained MSYS2 `PATH`, `CMAKE_PREFIX_PATH`, and `PKG_CONFIG_PATH`; the fresh cache contained no Windows/MSYS2/UCRT prefixes. Rebuilt and reran the service-configuration, worker-supervisor, and service-client-timeout CTest contracts on that clean Linux tree; all three passed.
- Attempted to build the complete Linux service/CLI for a live `/health` check. The build reached plugin linking but failed in the existing SDK plugin-validator invocation (`free(): double free detected in tcache 2`); this is recorded as an unresolved Linux build blocker, not as a Phase 1 service-health result.
- Refreshed the unpacked Windows package from current source at `tmp/packaged_build` and exercised its service on isolated loopback ports. During a worker load, `/health` reported both `ready` and `busy`, remained responsive across 64 samples (median 1.6 ms, p95 3.9 ms, maximum 31.1 ms), and 64 concurrent reads of a previously published result page succeeded with zero errors. The operation completed successfully. The initial fixture attempt used long generated identifiers that exceeded Windows staging-path limits; shortening the disposable identifiers fixed the fixture, and the failed diagnostic project was removed.
- Repeated duplicate service start against the same endpoint: both invocations resolved to the same service instance and endpoint; no second owner was created. Stopped the service and confirmed it left no healthy discovery record.
- Reproduced the interactive Windows Scheduled Task launch path using a temporary uniquely named task and isolated port. The task completed successfully (`LastTaskResult=0`), its detached packaged service remained healthy after the task launcher exited, and the service stopped cleanly. Removed the temporary task after the check.
- Remaining Phase 1 work: execute the representative SF-55 service/worker load simulation using disposable copies of the available Merck workflows, scaled late-stage result traffic, and concurrent project runs. Run the final simulation after the Phase 2 file-handle redesign so it measures the target payload path. The reporter's exact 3.3 GB BAF-derived mzML is not required. The dev host has 256 GB versus the reporter's 16 GB; attempt a documented aggregate 16 GiB test envelope where safe, and otherwise report the simulated workload and memory limit honestly. Do not prescribe a per-worker cap from this test. Phase 1 is **in progress**, not complete.

## Phase 2 — Make result publication and retrieval bounded and diagnosable

### Redesign decision — 2026-11-05

Adopt the attached file-handle payload I/O implementation brief as the target architecture. Replace worker payload transfers through base64 JSON and service-memory buffers with direct access to service-managed files through OS handles. Retain the service-orchestrated, one-worker-per-operation scheduler and the existing operation-facing `OperationExecutionContext`/output-sink boundaries. Operations consume inputs, parameters, and output sinks; storage authority stays in the service. This redesign is planned, not yet implemented or validated by the earlier Phase 1 checks.

The implementation order is handle broker and lifecycle, direct input/output I/O, unified workflow publication, removal of the old payload paths, native/platform validation, then the Phase 1 representative load simulation. Existing control/events and public app/MCP result APIs retain their contracts; internal worker protocols change as required. No compatibility shim, parallel payload route, or old/new execution flag is retained.

Primary owners: `cpp/service/src/worker_supervisor.cpp`, `project_runtime_manager.cpp`, `result_store.cpp`, `streamfind-worker.cpp`; `cpp/core/src/data_proxy_output_sink.cpp`, `cpp/core/include/streamfind/operation_execution_context.hpp`; and `cpp/sdk/src/plugin_data_service.cpp`. Confirm the owning paths and current contracts before editing.

### Storage and query decision — standard Arrow IPC files

Use one **standard Arrow IPC file** per table result, written with the Arrow file writer and opened with the file reader. Replace the existing `arrow-ipc-batches` container of length-prefixed independent streams. Use its footer and record-batch metadata for random access. Arrow remains the working columnar representation; Parquet is not an alternative runtime storage branch. A future explicit Parquet export may be considered separately after measurements; do not publish duplicate Arrow/Parquet copies of every result.

This is an atomic development refactor. Existing generated results and caches may be discarded and regenerated by rerunning workflows. Do not implement old-format readers, automatic conversion, migration commands, compatibility aliases, format fallbacks, old/new switches, or a second writer. Do not silently delete user projects/results on open: report an unsupported result format with an instruction to clear/regenerate results using the ordinary explicit deletion controls. Workflow definitions and user source data are not disposable generated results.

### 2.0 Implementation sequence and removal gates

1. Inventory all payload writers/readers, worker/SDK/ABI entry points, manifests, result/cache descriptors, query routes, frontend clients, semantics, tests, examples, and docs. Record the owning files in this plan. Locate every `arrow-ipc-batches`, JSONL table reader, base64 worker chunk route, transient payload map, and legacy frame cache/index before changing contracts.
2. Define the single typed table schema, standard IPC manifest, handle ownership, and persistent navigation-index contracts below. Update semantics, SDK interfaces, and fixtures together; allow the development build to be temporarily broken while the owning implementation is replaced.
3. Implement the handle broker and standard file writer/reader. Replace operation/plugin input/output plumbing across every domain through the SDK boundary. Preserve scientific/processing logic; remove materialization/conversion in the shared path rather than duplicating fixes in operations.
4. Implement bounded publication/index construction, unified workflow persistence, and the native columnar query/trace path. Switch frontend and MCP consumers to the resulting shared service contracts.
5. Delete superseded implementations and branches, then build the full core/plugins, run focused native contracts and required frontend checks, and regenerate the native packaged build from source. Regenerate disposable project results through real operations; do not patch package files or copy old payloads into the new store.
6. Execute the Phase 1 load simulation and Phase 2 benchmarks against that package. Record cold/warm results, source revision, package provenance, and remaining limitations. Do not call the refactor complete while any runtime reader/writer still depends on the removed format.

Removal gate: remove the custom length-prefix stream framing, full-file decode-to-build-offset-index path, `read_jsonl_batch` and other legacy table readers, JSON/base64 worker payload upload/download endpoints and SDK callers, service table-to-JSON-to-Arrow worker responses, transient workflow byte stores/persistence branches, obsolete protocol/schema fields, and caches specific to decoded custom frames. Remove dead tests, docs, build references, and unused symbols with their owning code. Keep JSON control events, bounded public JSON responses, and legitimate JSON/binary result formats; those are separate contracts, not legacy table transports. Search for each removed symbol/format and review every remaining occurrence; report unresolved references as incomplete work, not compatibility support.

### 2.0.1 Standard file, schema, and publication contract

- Define a single new manifest format identifier for standard IPC files and use it consistently in semantics, SDK, store, cache, service, tests, and docs. Store the schema fingerprint, row/batch counts, byte size, payload digest, and index descriptors. Use 64-bit row offsets/counts and checked arithmetic throughout.
- Write typed `arrow::RecordBatch` values directly through the handle-backed file writer. Keep one schema for the entire file, explicit nullable fields, stable column names/types, and a valid footer. Empty tables still have a schema and valid empty file. Finalize/close the Arrow writer before closing the worker handle and requesting publication; truncated/missing footers fail validation.
- Replace JSON-string serialization of numeric lists/complex trace fields with explicit Arrow List/LargeList and numeric child arrays (or the declared typed binary representation for truly opaque encoded data). Specify list width, numeric precision, null versus empty semantics, units, and paired-axis length validation in the output contract. Update all producers, consumers, semantics, and fixtures together. Do not leave a packed/base64/JSON-string trace fallback in the table path.
- Bound batches by measured bytes as well as rows. Avoid collecting a complete result just to build an Arrow table. Handle a single large nested value through the declared type/storage contract and an explicit bounded read policy; transport-message size is no longer a reason to reject a valid file-backed value.
- Start with uncompressed IPC for the active runtime path to permit mapped buffer reuse. Record disk cost and benchmark before adding compression; compressed buffers require decompression and cannot be described as universally zero-copy. Never load/mmap the complete file into an owning byte vector first. Wrap authorized OS handles with clear Arrow reader/writer ownership and checked close behavior; mapping lifetime must cover all referenced arrays and respect platform handle rights.
- During service validation, open the standard footer, verify schema/counts, and process batches incrementally for validation, digest/index construction. Write the payload, manifest, and immutable index files inside the same staged result directory; promote that result as one publication unit before registry visibility. Define recovery for directory promotion versus registry commit. Failed validation leaves no published result.

### 2.0.2 Persistent indexes declared by output semantics

- Keep navigation columns in the output type semantics as the authoritative declaration. Define lookup policy for equality/membership keys (`feature`, `analysis`, component/group IDs as applicable) and range keys (`rt`, `mz` where declared); do not automatically index every column or hard-code a MassSpec schema into the generic store.
- Assign stable result-local row identities based on immutable row ordinals, preserving the public `row_key` contract. Store batch row ranges at publication so a row ordinal maps to batch and local row without decoding all batches. The IPC footer locates batches; the row-range metadata locates rows within them.
- Persist indexes from navigation values to row ordinals or bounded postings blocks, plus per-batch min/max/null statistics for declared scalar range/filter columns. Specify duplicate values, missing/null values, numeric comparison, string comparison, compound analysis/component identity, and deterministic ordering. A component name alone must not merge different analyses accidentally.
- Build indexes incrementally with bounded buffers and disk spill/merge where necessary. Keep construction scratch in the leased staging directory, remove it before publication, and record index bytes/build time. A high-cardinality key must not require an unbounded service hash map.
- Bind every index to its result ID, schema fingerprint, row count, and payload digest; validate metadata at open. Corrupt/missing required indexes produce a specific integrity error and regeneration guidance; do not hide errors with a full-result scan or introduce an automatic old-format conversion. Ordinary supported queries without a usable index may use an explicitly bounded columnar scan.
- Open/mmap/read index blocks lazily, with bounded metadata caching; never retain every result's full lookup map in memory. Published payload/index/manifest deletion is coordinated with active reader leases/mappings. Release handles before Windows deletion and invalidate result-specific caches on removal.

### 2.0.3 Shared backend columnar query and trace execution

- Preserve `/results/query` and `/results/trace` as the app-facing boundaries, and route MCP result requests through the same service query implementation. The browser/MCP does not open result-store files. Verify request/response schemas, pagination, row identity, trace labels, null handling, and ordering after the internal replacement.
- Implement a query plan: validate scope/request; resolve projected and predicate columns; locate candidate rows/batches through persistent indexes/statistics; read needed columns from those batches; evaluate predicates and select/slice arrays; sample/downsample as required; serialize only the bounded selected response. Use Arrow compute/Acero or equivalent native typed kernels; do not turn complete record batches into JSON rows before filtering.
- Optimize an unfiltered page to direct row-range access without scanning the table for a total count. Use manifest counts for total rows; distinguish filtered counts from unfiltered counts, and compute exact filtered counts only with an appropriate index/scan plan. Define continuation behavior so large responses do not require a full scan before the first useful page.
- For feature/suspect map overviews, read scalar navigation/display columns only. Build deterministic sampled/viewport responses without loading EIC/MS1/MS2 arrays. For one selected feature or component, locate row ordinals first, group reads by batch, and access only requested trace columns; bound the number of selected traces and points and expose truncation/continuation honestly.
- Decode opaque numeric data in C++ only where the output contract requires decoding. Prefer typed Arrow numeric arrays for new EIC/MS1/MS2 output; validate axes/intensity lengths and preserve analysis, feature, name, polarity, and other renderer-required labels. Convert to render-ready JSON only at the final API boundary. Remove browser packed-array decoders once no producer/API needs them.
- Keep session/registry locks short: snapshot an authorized immutable result descriptor and acquire a reader lease, then release the runtime-manager lock before file I/O, Arrow decoding, index lookup, filtering, or JSON serialization. Coordinate deletion/shutdown with reader leases; avoid holding a global cache/index lock while decoding or scanning a file. Concurrent projects and health requests must not wait behind a slow result query.
- Replace full decoded-frame/feature-row caches with byte-budgeted caches for immutable metadata, useful selected batches, and decoded selected traces as justified by measurements. Keys include result identity/digest, selection, columns, trace type, and rendering options. Bound total bytes, individual entries, and in-flight work; entry-count limits alone are insufficient. Release mapped buffers/handles on eviction and deletion.
- Audit every viewer (table, features, suspects, EIC, Raman, statistics, and other registered UI plugins), the API client's request cache, and MCP tools for eager whole-result fetching or duplicate requests. Keep views on projected/sample/page/detail/trace calls; render returned values and cancel/ignore obsolete selections. Retain only bounded client caches. Preserve correct errors and empty states rather than swallowing query/parse failures.

### 2.0.4 Performance and correctness gates

- Add native CTest contracts for standard IPC interoperability, empty/multi-batch files, typed nested arrays, selective columns/rows, batch-boundary row keys, duplicate/null navigation values, range/equality/component selection, missing/corrupt index/footer, read leases during deletion, and bounded cancellation/cleanup. Verify genuine standard IPC interoperability with the official file reader, not merely a round trip through project wrappers.
- Compare cold-service first open, warm query, single-feature EIC/MS1/MS2, component selection, map sampling, sequential pages, filtered pages, MCP selected-data requests, worker downstream input reads, and publication/index build. Use the same data and parameters for each run. Record p50/p95 latency, first-response time, bytes read/decoded/returned, batches touched, service/worker private/RSS peaks, mapped memory, cache bytes, CPU, disk space, and disk throughput.
- Assert that selecting a known row after restart uses footer/persistent indexes without decoding the whole result; scalar overview reads do not touch trace columns; page size bounds response allocation; service caches stay within byte budgets; and concurrent result queries do not serialize behind the runtime mutex. Instrument these properties so a fast small fixture cannot conceal an accidental full scan.
- Keep standard IPC as the single runtime format. A disposable Arrow-versus-Parquet benchmark may inform a separately recorded design decision, but cannot add a permanent format flag, duplicate canonical result, or alternate production path. Any future decision to replace IPC must replace the owning path completely.
- For manual app validation, regenerate results in disposable projects and test the source-generated `tmp/packaged_build`; refresh the existing browser tab without opening extra tabs. Update SDK/plugin documentation and examples to the new typed handle-backed contract. Record removal audit, checks, measurements, and incomplete work in this plan.

### 2.1 Worker-scoped handle broker

- Add a private broker alongside the existing control channel, available throughout worker execution because outputs are allocated dynamically. The service creates/opens files and binds every grant to the worker process identity, project session, result ID, lease, access mode, and lifecycle state. Reject unauthorized results, wrong workers, expired/aborted leases, invalid states, and replayed grants.
- On Windows, use `DuplicateHandle` into the specific supervised worker with the minimum required read or write rights; transfer only the worker-local handle value and small grant metadata over the authenticated worker exchange. On POSIX, pass descriptors over a private Unix-domain socket with `SCM_RIGHTS`, validating the peer and grant scope. Numeric handle values are never cross-process result identities.
- Keep grants opaque to operation code. Do not expose DuckDB handles, project database paths, result-store directories, or arbitrary payload paths. Prevent unintended inheritance into worker descendants and close handles/descriptors on every success/error path.
- Track outstanding grants and lease ownership in the supervisor/store lifecycle. Closing the service's copy does not revoke the worker's duplicated handle. On cancellation/crash cleanup, terminate and reap the worker tree before requiring exclusive validation/deletion on Windows; account for POSIX unlink/open-descriptor behavior. Reject subsequent broker requests immediately when a lease is aborted. Do not promise remote revocation by closing only a local handle.

### 2.2 Direct input reads and leased output writes

- Resolve input result IDs against published manifests and authorize each input before granting read-only handles. Augment the existing input descriptors with opaque runtime grants; grant fresh handles to each downstream worker. The worker data-access layer decodes Arrow or other declared payload formats directly from handles, using bounded batches and column/row selection where supported, without materializing a full intermediate JSON table.
- For every output type (Arrow tables, small JSON, images, binary/encoded data, and model objects), let the service allocate a result ID/lease, create staging, and grant a write-only handle. Keep typed result formats and Arrow IPC file/batch framing contracts explicit. Operations continue using the same SDK output interface.
- Make the sink the sole writer and enforce sequential writes with its own byte/offset accounting, or explicit positional I/O; do not rely on a shared implicit file position across duplicated handles. Handle partial writes, empty outputs, and platform handle-backed Arrow reader/writer ownership correctly.
- `finish` flushes and closes the worker handle before requesting service validation/publication. The service verifies lease ownership/state and payload format, computes the digest incrementally from disk, finalizes counts/schema/manifest, and publishes atomically. `abort` closes the worker handle and requests discard. Reject writes after finish/abort and repeated invalid transitions.
- Remove data-bearing stdout events and base64 JSON payload requests/responses. Completion/control messages carry result IDs, schemas, counts, progress, and status only. Worker input reads must not route through service JSON rows, Arrow re-encoding, or payload-sized service buffers.

### 2.3 Publish all workflow intermediates

- Use the same result-file publication transaction for standalone operations and every workflow node output. Remove transient workflow payload bytes, transient-result persistence policies, and non-terminal special paths; do not introduce a temporary-result execution mode.
- Downstream nodes consume immutable published result references through fresh read-only grants. Preserve dependency ordering, cancellation, progress, cache-key reuse, result identity semantics, and publication visibility. A cache hit resolves an authorized existing published file, never a cached payload blob in service memory.
- Keep intermediate results subject to the ordinary result retention/deletion rules. Record the extra disk-space/I/O cost, and report disk-full/quota errors clearly; writing to disk does not remove storage limits or guarantee low operation memory.

### 2.4 Handle transport contracts and load validation

- Add native C++ CTest contracts under the owning core/service/SDK test areas for authorized input reads, wrong-worker/unauthorized-result grants, expired/aborted leases, descriptor ownership, and close/error paths on Windows and POSIX.
- Verify exact stored bytes, schemas/counts, digests, manifests, and publication state for JSON, Arrow batches, binary payloads, empty outputs, sequential/multiple writes, and payloads larger than the former JSON chunk limit. Exercise bounded buffers rather than assembling the full test payload in memory.
- Cover finish/abort races, worker crash, cancellation, service restart, stale staging recovery, and cleanup while a worker held a handle. No partial file may become a published result; failed leases must not leave reusable grants or orphaned registry rows.
- Run a multi-node workflow where one worker publishes an intermediate and another reads it by result ID/fresh handle. Verify final and intermediate results are published, intermediate bytes are not retained in the service, cache reuse remains valid, and subsequent runs work after failure/cancellation.
- Run the supported Windows MinGW and Linux/WSL build/test workflows. Keep data-backed Merck checks under `scripts/dev/`, use the repository `.venv` for Python, and place disposable projects/logs under `tmp/`. Then execute the Phase 1 single/concurrent-project simulation and record service/worker memory separately, bytes, disk I/O, health latency, publication integrity, and cleanup.

### 2.5 Atomic publication and transient filesystem failures (SF-49, SF-58)

- Trace the complete staging lifecycle: lease creation, worker close/flush, validation, atomic promotion, registry commit, and cleanup. Include the exact OS error, source/destination path class, run ID, result ID, and whether the producer still holds a handle.
- Ensure the worker closes all output handles before requesting seal/publish; service validates the staged payload before making it visible.
- Keep final names unique and promotion atomic within one filesystem. Separate payload promotion from registry commit and define recovery for a crash between those steps.
- Classify permission denied, sharing violation, missing file, invalid payload, and registry failure separately. Retry only a positively identified transient filesystem condition with a short bounded policy; never use retries to mask a wrong owner, bad path, or open handle.
- Preserve the operation's original failure cause when a result is absent. A downstream “No published result for connection …” must include the upstream node/result publication failure and link to its operation/run context.
- Add cleanup/recovery checks for orphaned staging entries without deleting active or recoverable data.

### 2.6 Large and oversized results (SF-57, SF-65)

- Measure current per-message/chunk limits and the largest table row/column values from `load_features_ms2`, especially nested MS2 arrays. Include result bytes, Arrow batch bytes, row count, and the largest row's feature/analysis identity.
- Keep Arrow as the canonical table file format and use bounded batch decoding/writing through handles. Worker payload size is no longer constrained by a base64 JSON control-message limit. Distinguish genuine Arrow/schema/allocation limits from obsolete transport-frame limits; keep public app/MCP reads bounded.
- Decide from the table contract whether exceptionally large per-feature traces should be represented as a separate typed trace result keyed by feature/analysis, or segmented into bounded chunks. Keep ordinary scalar feature columns in the main feature table. Record schema/renderer/MCP implications before choosing.
- Make “single row exceeds limit” diagnostics identify output port, operation/node, row key, approximate size, and the relevant large field. Fail before publication and clean staging safely.
- Add server-side column projection and indexed row filtering for result requests, and stream or page selected data. Reuse output-type navigation-column semantics for efficient lookups. Do not build indexes for every column.
- Define explicit pagination/byte limits and continuation cursors. For MCP, return summaries/handles by default and allow bounded selected columns/rows; do not return an unbounded table inside a tool response.
- Provide a deliberate export path for users who need a full table as a file, without routing the full payload through MCP JSON.
- Benchmark 75 MB tables, 576k chromatogram points, representative feature tables, and single very-large MS2 features. Report cold and warm timings, memory, bytes transferred, and page/stream count.

**Acceptance:** all standalone/workflow worker payloads use service-issued restricted handles; payload bytes do not travel through base64 JSON or payload-sized service-memory buffers; only the service allocates, validates, publishes, and removes results; every workflow intermediate is published and readable by downstream workers through fresh grants; the transient/legacy payload routes are removed. Large table retrieval scales with requested columns/rows and uses bounded buffers; known data can be fetched in useful time; genuine oversized values fail with actionable context or use the agreed typed chunk contract; no partial result is registered.

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
| Worker payload handles | scoped input/output grants; Windows duplication/POSIX descriptor passing; access rights, ownership, offsets, close/error paths; no payload bytes in JSON |
| Workflow persistence | every intermediate/final output published; downstream fresh input grants; cache reuse by result reference; no transient payload store |
| Result store | staged output closure, validation, atomic publish, transient file-lock handling, crash recovery, cleanup, no orphaned registry rows |
| Standard IPC queries | official file-reader interoperability; persistent semantic indexes; selected batch/column reads; native filtering before JSON; typed trace arrays; byte-budgeted caches and short registry locks |
| Large results | bounded Arrow batches, one oversized nested trace, projection/filter/page/stream, export; memory and latency benchmarks |
| MCP | table/JSON `request_result`; tool schema types; incremental graph edits; async run and poll; timeout/disconnect; notifications; package-relative demos |
| Shared app/MCP | same service/project/run, ordered WebSocket change event, reconnect/resnapshot, output-port refresh and result rendering |
| MassSpec | large component progress, MetFrag progress/timeout/selection, Bruker calibration and legacy BAF edge cases, parameter effectiveness, filter reason preservation, valid SMILES |
| Platform/package | Windows MinGW package and Linux/WSL package where available; paths with spaces; arbitrary working directory; scheduled task behavior |

Official native contract tests belong under `cpp/core/tests/` or `cpp/plugins/<domain>/tests/` and run through CTest. Data-backed workflow checks belong under `scripts/dev/` and use the repository `.venv`; large vendor data is not copied into native fixtures.

## Completion criteria

- Every feedback ID has a final status, evidence, and release-note disposition.
- Service remains alive and reports accurate state under worker crashes, memory pressure, concurrent result reads, and cancellation.
- All worker payload reads/writes use restricted service-issued handles; every operation output, including workflow intermediates, is published to disk. Control/events contain metadata only, and the old base64/transient payload paths are removed.
- Standard Arrow IPC files are the sole canonical table format. Persistent semantic navigation indexes survive service restart; queries select rows/columns before JSON conversion and leave scalar overview requests independent of trace decoding. The custom framed-stream format, JSONL table readers, browser packed-trace fallbacks, and their obsolete caches/protocol paths are removed. Existing generated results are regenerated explicitly; no migration or compatibility layer is delivered.
- MCP operations are bounded, typed, resumable, and consistent with the service API; app and MCP observe the same workflow revision and published results.
- Result transfer is bounded for both typical large tables and outlier nested trace rows, with no JSON expansion of bulk table payloads.
- Long operations expose meaningful progress and distinguish timeout/partial/no-result/failure states.
- The fixes and supported limitations are documented, and packaged Windows/Linux validation matches the tested source revision.

## Provenance and status log

Keep implementation progress, decisions, validation evidence, and `SF-xx` dispositions in this plan. Do not create a separate `.plans/provenance/` record. When this plan is fully complete, move it to `.plans/completed/`; if replaced or paused, move it to `.plans/stale/` before opening another root-level active plan.
