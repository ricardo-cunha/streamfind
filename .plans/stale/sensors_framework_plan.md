# Sensors Plugin Future Plan

**Owner:** `cpp/plugins/sensors`
**Scope:** sensor acquisition and analysis operations using the same project, workflow, artifact, and plugin contracts as other domains.

## Future capabilities

- Define sensor-specific semantic tables, columns, parameters, operations, and result contracts.
- Add finite-batch acquisition operations with explicit source/configuration parameters.
- Add preprocessing, quality checks, aggregation, and visualization-spec operations.
- Keep continuous OPC UA/MQTT ingestion outside the first persisted operation-graph slice until lifecycle and cancellation contracts are specified.

## Implementation rules

- Sensor capabilities remain plugin-owned; core stays domain-neutral.
- Every operation declares typed ports and publishes immutable artifacts.
- Acquisition must not execute during project creation or open.
- External sources and credentials are represented by explicit non-secret configuration references; secrets are never persisted in plans, logs, or artifacts.
- Add catalogue, validator, packaged-plugin, and operation-graph tests with disposable project files.

## Acceptance

A sensor plugin can be discovered, configured, validated, executed, and inspected through the generic C++ MCP/service host without frontend or core domain-specific changes.
