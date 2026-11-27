# Generic workflow scheduler

## Purpose

Add service-owned scheduling for saved workflows so bounded acquisition and
processing workflows can run repeatedly without embedding continuous-runtime
logic in the sensors plugin or in workflow nodes.

The scheduler decides when a workflow starts. The workflow decides which
operations execute, which data they use, and how results are persisted. The
scheduler must remain unaware of node types, artifacts, protocols, algorithms,
and file formats.

## Scope and non-goals

### In scope

- schedule persistence separate from workflow definitions;
- manual, interval, and after-completion triggers;
- enable/disable, start/stop, cancel, and status lifecycle;
- invocation through the existing workflow-start/runtime interface;
- no-overlap protection for a workflow;
- bounded retry for failed after-completion schedules;
- service restart recovery for persisted schedule state;
- service/MCP/API inspection of schedules and recent runs; and
- a project configuration layer for scheduler settings and defaults.

### Deferred

- cron/calendar expressions;
- multi-host distributed scheduling;
- durable acquisition checkpoints and stream offsets;
- months-long persistent subscriptions;
- scheduler-specific plugin ABI callbacks; and
- scheduler-owned persistence, deduplication, or append semantics.

## Architectural contract

Project configuration, workflow metadata, and schedule state have separate
owners and purposes:

- **Project configuration** is deterministic, validated configuration for a
  project. It holds that project's scheduler enablement and policy defaults,
  plus its schedule definitions: target workflow, trigger, enabled state, and
  any per-schedule policy overrides. Reading it returns the effective
  configuration, including documented defaults, even when no overrides have
  been saved.
- **Workflow metadata** is descriptive metadata on the saved workflow, such as
  its name and purpose. It describes the workflow for its application; it does
  not configure scheduling or execution policy.
- **Schedule runtime state** is service-owned execution state keyed by project
  and schedule identity. It tracks lifecycle, due times, active runs, retry
  progress, and recent runs separately for each project. The project
  configuration remains the source of truth for schedule definitions;
  runtime state does not duplicate or replace those definitions.

In the target model introduced by this future work, the current arbitrary
project `metadata` field will be deprecated and workflow metadata will be the
sole metadata object in the project/workflow model. Until that migration is
implemented, current project metadata APIs and values remain supported. The
scheduler and project configuration must not use project metadata for
configuration, scheduling, or workflow description. Before removing the
persisted project metadata field, S0 must inventory its remaining consumers
and specify a direct schema migration or removal; do not silently reinterpret
existing arbitrary values as configuration.

Project configuration should use a versioned, typed schema with explicit
defaults. Its first version should contain project scheduler enablement, a
validated default retry policy, and per-schedule definitions. Exact fields
and constraints are finalized in S0. Schedule configuration changes can
change future trigger behavior, but must not rewrite prior run history. A
resolved configuration has this shape:

```json
{
  "configuration_schema_version": 1,
  "configuration_revision": 4,
  "scheduler": {
    "enabled": true,
    "default_retry_policy": {
      "max_retries": 3,
      "retry_delay_seconds": 30
    },
    "schedules": []
  }
}
```

Expose separate
`get_project_configuration` and `set_project_configuration` operations; the
getter returns resolved defaults and the current configuration revision, and
the setter validates the supplied complete configuration and rejects stale
revisions. Project configuration is never exposed through a metadata tool.

Schedule definitions live in the project's `scheduler.schedules` array, not
workflow graph metadata. For example:

```json
{
  "schedule_id": "acquisition-schedule",
  "workflow_id": "workflow-1",
  "enabled": true,
  "trigger": {
    "type": "interval",
    "seconds": 300
  },
  "retry_policy": {"max_retries": 5, "retry_delay_seconds": 60}
}
```

An after-completion trigger is also stored in the project's schedule
definition:

```json
{
  "type": "after_completion",
  "delay_seconds": 0
}
```

The service invokes the existing `ProjectRuntimeManager::start_workflow()`
boundary. It must not inspect or modify workflow nodes to schedule a run.

The service scheduler operates on projects currently loaded by that service.
For each loaded project, it reads policy and schedule definitions from that
project's configuration and reads or writes only that project's runtime state
in the service-owned store. It must not infer scheduler settings from project
metadata or workflow metadata, and it must not run schedules for projects
that are not loaded by the service.

## Lifecycle and execution rules

1. A schedule is created disabled or explicitly enabled by the caller.
2. An enabled interval schedule calculates its next due time from persisted
   schedule state rather than from process uptime alone.
3. If the target workflow is already active, an interval tick is recorded as
   skipped; it does not start an overlapping run.
4. An after-completion schedule is evaluated only after the previous run has a
   terminal result. Failed runs use bounded retry and then enter a visible
   failed/waiting state instead of restarting rapidly.
5. Manual execution uses the same invocation path and no special operation
   semantics.
6. Stopping a schedule prevents future starts but does not silently terminate
   a running workflow. Cancellation is an explicit action.
7. Service restart reloads schedules and reconciles stale in-flight state
   without duplicating a run whose start was already durably recorded.
8. Every scheduled run has a correlation identifier, trigger source, start
   time, terminal state, and error summary that excludes secrets.

The first implementation should use a single scheduler loop or equivalent
service-owned coordinator with bounded waiting. It must not create one
unmanaged thread per schedule.

## Persistence and project boundaries

Persist versioned project configuration, including schedule definitions, in
the `.sf` project store apart from workflow JSON. Persist scheduler runtime
state and run history in the service-owned scheduler store, partitioned by
project and keyed by schedule IDs. Each loaded project's scheduler state and
history must be independently inspectable and recoverable; one project's
configuration, runs, retries, or history must not affect another project's
scheduler. The project configuration API resolves schema defaults
deterministically and exposes a configuration revision for optimistic
concurrency. Workflow metadata remains descriptive and does not affect
configuration revisions or schedule state.

The initial implementation may support one selected workflow definition per
project, matching the current project model. Supporting multiple named
workflow definitions in one project is a separate project-model extension.

Schedule definitions are owned by their project and reference workflow
identity rather than copying workflow definitions, operation parameters,
credentials, or result data. The service-owned runtime store records state
against those project/schedule identities only while the project is loaded.
Unloaded projects have no active scheduling; their persisted project
configuration remains in `.sf`, and their prior per-project runtime history
remains associated with that project for inspection or recovery when loaded
again. A missing project or workflow produces an inspectable schedule error
and does not cause a retry storm.

## API and observability requirements

Provide generic service/MCP operations or endpoints to:

- read and replace project configuration with revision checking;
- create, update, enable, disable, and delete schedule definitions in that
  project's configuration;
- validate project configuration and schedule definitions;
- list a project's schedule definitions together with that project's runtime
  state;
- request a manual run or cancel an active run;
- inspect next due time, last trigger, active run, retry count, and last error;
- delete a schedule only when no active run is attached.

Progress and logs should identify schedule and run IDs, but must not include
passwords, tokens, secret references, or full sensitive operation parameters.

## Cache and side-effect contract

Scheduling must not make side-effecting operations unsafe through accidental
result-cache reuse. Add generic operation metadata for cacheability and, where
needed, idempotence or run identity. Acquisition and file-write operations may
declare that each invocation must execute even when parameters are unchanged.

The scheduler does not implement file locking, retries, append, deduplication,
or transaction semantics for persistence operations. Those behaviours belong
to the operation that owns the external resource.

## Implementation stages

### S0 — Runtime inventory and contract tests

- Locate the existing workflow-start, worker, cancellation, service lifecycle,
  and project persistence boundaries.
- Inventory every reader and writer of project `metadata`, mark it deprecated,
  and decide direct schema migration or removal before deleting the field.
- Specify the versioned project configuration schema, deterministic defaults,
  revision behavior, validation, and persistence/API boundary.
- Define schedule state transitions and run correlation identifiers.
- Add deterministic tests for interval calculation, disabled schedules,
  overlap handling, and after-completion retry decisions.

**Exit:** lifecycle and state transitions are specified without a scheduler
implementation coupled to sensors.

### S1 — In-memory scheduler coordinator

- Implement interval and after-completion triggers.
- Invoke the existing workflow runtime interface.
- Enforce no overlap and bounded retry.
- Support manual trigger, enable/disable, stop, and status.

**Exit:** a disposable service can run a bounded workflow repeatedly with
correct lifecycle and no duplicate concurrent runs.

### S2 — Persisted schedules and restart recovery

- Persist each project's validated scheduler configuration and schedule
  definitions in that project's `.sf` store, separately from workflow JSON.
- Persist runtime state and run history separately in the service-owned
  scheduler store partitioned by project; reconcile each loaded project's
  state with its configuration.
- Load schedule definitions only for projects opened by the service and resume
  each project's runtime state independently.
- Reconcile stale active runs and prevent duplicate starts after restart.
- Keep workflow metadata descriptive; do not store scheduler settings or
  schedule state in workflow metadata.

**Exit:** an enabled schedule survives service restart and resumes according
to its persisted state without changing the workflow graph.

### S3 — Service/MCP exposure and packaged validation

- Add schedule management and inspection through the supported service/MCP
  surface.
- Add redacted observability and recent-run inspection.
- Validate with a packaged worktree service, disposable `.sf` project, and a
  bounded OPC UA acquisition workflow.

**Exit:** users can configure and inspect a schedule through the generic host;
the same workflow remains runnable manually.

### S4 — Optional calendar triggers

Only after interval and after-completion behaviour is stable, evaluate cron or
calendar expressions, timezone/DST semantics, misfire policy, and calendar
validation. Do not add this complexity to the first scheduler.

## Validation matrix

| Area | Required checks |
| --- | --- |
| Scheduling | Interval calculation, after-completion, manual trigger, disable/stop |
| Concurrency | No overlapping runs, skipped tick visibility, bounded retries |
| Recovery | Service restart, stale run reconciliation, duplicate-start prevention |
| Runtime | Existing workflow execution, cancellation, terminal-state handling |
| Security | Secret-free schedule records, logs, errors, and status payloads |
| Project isolation | Worktree packaged service and disposable `.sf` project |
| Integration | OPC UA bounded workflow triggered repeatedly and inspected through MCP |

## Definition of done

The scheduler is complete when a generic saved workflow can be manually
started, run on an interval, or run after completion through the packaged
service; resolved project scheduler configuration and project-partitioned
schedule state survive restart; only loaded projects are scheduled;
configuration is validated and revisioned; overlapping runs are prevented;
project metadata is no longer used by the supported model; and the scheduler
has no knowledge of workflow nodes, protocols, persistence formats, or
sensor-specific logic.
