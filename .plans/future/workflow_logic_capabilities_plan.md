# Generic workflow logic capabilities

## Purpose

Expand the core workflow model with domain-neutral logic and control-flow
capabilities so users can express acquisition checks, branching, persistence,
analysis, and failure handling in the workflow itself.

This plan is independent of the service scheduler. A scheduler only decides
when to invoke a saved workflow. It must not interpret or modify the workflow
graph. These capabilities must work for sensors, mass spectrometry, ML, and
future domains without sensors-specific execution paths.

## Design boundary

The current workflow is a directed acyclic graph of operations whose
dependencies determine execution order. Preserve that model for ordinary
one-shot operations and add control-flow semantics in stages:

1. Add value-producing logic operations that fit the existing DAG execution
   contract.
2. Add true conditional branch execution only when an unselected branch must
   not execute.
3. Add bounded loop bodies only when repeated execution of a subgraph is
   required.

A conditional-value node can select between already-produced values without
changing the executor. A conditional branch must prevent an expensive,
stateful, or side-effecting subgraph from running and therefore requires
executor support. A loop that repeats connected operations is not an ordinary
one-shot node and must have explicit bounds and cancellation.

## Initial domain-neutral operation families

| Family | Initial operations | Contract |
| --- | --- | --- |
| Boolean | `core.and`, `core.or`, `core.not` | Typed Boolean inputs and output |
| Comparison | `core.equal`, `core.greater_than`, `core.less_than` | Explicit coercion/null rules |
| Selection | `core.if`, `core.switch` | Select values; branch execution is later |
| Data utilities | `core.filter`, `core.select`, `core.merge` | Generic tables/objects with declared schemas |
| Validation | `core.assert`, `core.fail` | Deterministic failure and diagnostics |
| Iteration | `core.for`, `core.for_each`, `core.while` | Bounded subgraph execution, later stage |

Names and exact ports must be confirmed against the core semantic catalogue
before implementation. Operations should expose typed ports, explicit null and
missing-value behaviour, and deterministic diagnostics. They must not embed
ML, OPC UA, or other protocol-specific assumptions.

## Type, null, and error semantics

The implementation must define before adding broad operation coverage:

- Boolean truth rules for missing, null, empty, and invalid values;
- numeric comparison rules across integer, real, and decimal representations;
- string comparison and case/locale behaviour;
- equality for arrays, objects, tables, and structured values;
- whether invalid conversion is a failed operation or a false condition;
- scalar versus table/object selection behaviour;
- error propagation through selected and unselected values; and
- diagnostics that identify the operation and input without exposing secrets.

Do not silently coerce arbitrary JSON or tables into unrelated types. Use
declared input contracts and explicit conversion operations where necessary.

## Conditional execution

### Stage A: value selection

Implement `core.if` and `core.switch` as ordinary operations that select among
values already available to the executor. This is compatible with the current
DAG and is useful for choosing thresholds, labels, paths, and downstream
parameters.

### Stage B: branch subgraphs

Extend the workflow representation and executor only when a branch must prevent
the other branch from executing. The design must specify:

- how branch entry and exit ports are represented;
- how skipped operations are reported;
- how branch-local artifacts are scoped and published;
- whether both branches must satisfy static validation;
- how side effects and cacheability are handled;
- how cancellation propagates into an active branch; and
- how nested branches are bounded and diagnosed.

An unselected branch must not invoke plugins, publish side effects, or create
misleading success results. Static validation can still validate the complete
graph before execution.

## Bounded iteration

Loops are a later executor extension, not a scheduler feature. The first loop
contract must require an explicit maximum iteration count or an equivalent
resource budget. It must also support cancellation, progress, per-iteration
diagnostics, and a deterministic terminal result.

Possible first forms are:

- `core.for_each` over a bounded list/table input;
- `core.for` with an integer range; and
- `core.while` only with an explicit maximum iteration count.

Loop bodies should be represented as validated subgraphs or another explicit
workflow structure, not as hidden callbacks or operation-specific loops. The
executor must prevent runaway execution, uncontrolled artifact publication,
and accidental concurrent side effects.

## Data and persistence operations

Generic logic should enable user-defined persistence workflows, for example:

```text
sensors.capture_opcua_timeseries
        |
    core.if / core.assert
        |
  sensors.normalize_sensor_table
        |
  user-selected persistence operation
```

Persistence is the responsibility of the operation that owns the external
resource. A future `core.append_table_to_parquet` is illustrative only; it
must define locking, retries, file replacement/segment strategy, idempotence,
and duplicate handling itself. The workflow engine and scheduler must not
silently impose those semantics.

Side-effecting operations need generic cacheability and idempotence metadata.
The executor must not reuse a cached result solely because parameters are
unchanged when the operation declares that each invocation has an external
effect or requires a fresh acquisition.

## Workflow model and validation changes

The current operation graph should remain compatible for existing workflows.
Any branch or loop representation must:

- have one canonical owning implementation path;
- preserve ordinary DAG execution unchanged;
- be validated before execution;
- expose typed entry/exit ports and artifact contracts;
- make skipped, failed, and completed states inspectable;
- avoid duplicate execution when a graph is reopened or retried; and
- remain independent of service scheduling.

If multiple named workflows per project become necessary for scheduled
acquisition and analysis, extend project workflow persistence separately. Do
not make control-flow nodes depend on a scheduler or silently select another
workflow definition.

## Implementation stages

### L0 — Catalogue and semantic contracts

- Define operation IDs, typed ports, null/error semantics, and result
  diagnostics for Boolean, comparison, selection, assertion, and failure
  operations.
- Add generic cacheability/idempotence metadata to operation contracts.
- Add validator and catalogue tests before executor changes.

**Exit:** contracts are discoverable and invalid input types fail validation
before a worker starts.

### L1 — Ordinary DAG logic operations

- Implement Boolean, comparison, conditional-value selection, assertions, and
  deterministic failure using the existing execution contract.
- Add table/object utilities only with explicit schemas and bounded inputs.
- Test scalar, structured, missing, null, invalid, and mixed numeric inputs.

**Exit:** existing DAG workflows can make decisions and fail clearly without
changes to plugin ABI or scheduler behaviour.

### L2 — True conditional branches

- Specify branch graph representation and static validation.
- Extend the executor to skip unselected subgraphs.
- Propagate cancellation and terminal states through branches.
- Verify that skipped side-effecting operations do not invoke plugins or emit
  artifacts.

**Exit:** an expensive or side-effecting branch is never run when its condition
is false, while selected outputs and diagnostics remain inspectable.

### L3 — Bounded loop subgraphs

- Add one bounded iteration construct, preferably `core.for_each` or `core.for`.
- Define iteration input/output and artifact scoping.
- Enforce maximum iterations, resource budgets, cancellation, and progress.
- Add deterministic tests for zero iterations, early failure, cancellation,
  repeated values, and side effects.

**Exit:** a validated bounded subgraph can repeat without runaway execution,
implicit concurrency, or duplicate publication.

### L4 — Data utilities and temporal workflows

- Implement filter/select/merge operations for declared table/object contracts.
- Integrate with `SENSOR_TIMESERIES` validation, filtering, resampling, and
  alignment without protocol-specific logic.
- Validate a complete OPC UA acquisition → logic → persistence workflow in a
  packaged `.sf` project.

**Exit:** sensor and non-sensor workflows can reuse the same logic operations
and published artifacts.

## ABI and runtime constraints

Reuse the current plugin ABI wherever possible. Logic operations that are
implemented in core should not require a sensors-plugin ABI change. If a
workflow executor extension requires a new host capability, prefer an
additive, size/version-checked ABI extension only after a concrete use case
and compatibility test exist.

The workflow executor owns branch and loop lifecycle. The service scheduler
only invokes the workflow and observes its terminal state. Neither component
should introduce ML-specific stages, acquisition checkpoints, or protocol
knowledge.

## Validation matrix

| Area | Required checks |
| --- | --- |
| Catalogue | Operation discovery, typed ports, semantic validation |
| Values | Boolean, comparison, null, missing, numeric, string, object, and table cases |
| Branches | Static validation, skipped side effects, selected output, cancellation |
| Loops | Zero/one/many iterations, bounds, progress, failure, cancellation |
| Caching | Fresh execution for non-cacheable acquisition and persistence operations |
| Compatibility | Existing DAG workflows and existing plugin operations |
| Integration | OPC UA acquisition with conditional quality handling in a `.sf` project |
| Scheduler isolation | Manual and scheduled runs produce identical workflow semantics |

## Definition of done

The core logic expansion is complete when generic workflows can express typed
conditions, comparisons, assertions, conditional values, and bounded
conditional subgraphs without domain-specific code; existing DAG workflows
remain compatible; side effects are not incorrectly cached or executed in
skipped branches; and the scheduler can trigger these workflows without
understanding their graph or operation semantics.
