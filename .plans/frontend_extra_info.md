# Frontend Capability-Driven UX Notes

This document contains future UX guidance for the operation-graph frontend.

## Generic shell

Use one application shell for project lifecycle, workflow editing, results, provenance, and run monitoring. The shell consumes catalogue and manifest metadata instead of embedding domain operation lists.

## Capability-driven surfaces

- Generate operation cards and parameter forms from semantic declarations.
- Use typed input/output ports to constrain valid connections.
- Show table and result schemas before a node is executed.
- Display plugin availability and version diagnostics from the packaged capability set.
- Keep domain-specific visual renderers behind a registry keyed by visualization schema.

## Future workspace areas

1. Project hub for opening and creating DuckDB project files.
2. Graph canvas for operation nodes and typed data connections.
3. Run monitor for durable execution records and progress.
4. Artifact browser for tables, JSON values, lineage, retention, and previews.
5. Visualization workspace for published visualization-spec artifacts.

## Boundaries

The frontend must not execute scientific algorithms, mutate DuckDB directly, or maintain a second workflow schema. All validation, execution, persistence, and artifact publication remain backend responsibilities.
