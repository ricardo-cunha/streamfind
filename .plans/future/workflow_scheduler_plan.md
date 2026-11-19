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
- service restart recovery for persisted schedule state; and
- service/MCP/API inspection of schedules and recent runs.

### Deferred

- cron/calendar expressions;
- multi-host distributed scheduling;
- durable acquisition checkpoints and stream offsets;
- months-long persistent subscriptions;
- scheduler-specific plugin ABI callbacks; and
- scheduler-owned persistence, deduplication, or append semantics.

## Architectural contract

Schedules are execution metadata, not workflow graph metadata. A minimal
representation is:

```json
{
  "schedule_id": "acquisition-schedule",
  "project_id": "opcua-ph-meter-demo",
  "workflow_id": "workflow-1",
  "enabled": true,
  "trigger": {
    "type": "interval",
    "seconds": 300
  }
}
```

The after-completion form is independent of the workflow definition:

```json
{
  "type": "after_completion",
  "delay_seconds": 0,
  "max_retries": 3,
  "retry_delay_seconds": 30
}
```

The service invokes the existing `ProjectRuntimeManager::start_workflow()`
boundary. It must not inspect or modify workflow nodes to schedule a run.

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

Schedule records should be service-owned and separate from workflow JSON. The
initial implementation may support one selected workflow definition per
project, matching the current project model. Supporting multiple named
workflow definitions in one project is a separate project-model extension.

Schedules must reference project/workflow identities rather than copying
workflow definitions, operation parameters, credentials, or result data. A
missing project or workflow produces an inspectable schedule error and does
not cause a retry storm.

## API and observability requirements

Provide generic service/MCP operations or endpoints to:

- create and validate a schedule;
- list schedules and their current state;
- enable, disable, start, stop, and cancel;
- inspect next due time, last trigger, active run, retry count, and last error;
- request a manual run; and
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

- Persist schedule definitions and execution state in a service-owned store.
- Reload schedules on service startup.
- Reconcile stale active runs and prevent duplicate starts after restart.
- Keep project `.sf` storage and workflow definitions unchanged.

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
service; schedule state survives restart; overlapping runs are prevented; and
the scheduler has no knowledge of workflow nodes, protocols, persistence
formats, or sensor-specific logic.
