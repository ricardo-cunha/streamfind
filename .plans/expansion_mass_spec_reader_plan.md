# Native MassSpec Reader Future Plan

**Owner:** `cpp/plugins/mass_spec`
**Scope:** native readers and public operation contracts for Agilent, SCIEX, Bruker, Shimadzu, mzML, and mzXML.

## Contract

Readers remain native, lazy, filename-independent, and processing-level preserving. They expose deterministic zero-based `analysis_index` values, lightweight headers, on-demand arrays, chromatogram metadata, and explicit bounds/errors. No runtime conversion, centroiding, vendor SDK, or external reader fallback is part of this plan.

Persisted operations must bind source files and analysis selections through typed inputs and artifact contracts. Reopening a project must reproduce the selected analysis and artifact schema deterministically.

## Future work

1. Complete SCIEX and Shimadzu inspection coverage, including multi-analysis selection and malformed-container diagnostics.
2. Expand native Bruker, Agilent, mzML, and mzXML corpus validation without changing public column names.
3. Verify lazy indexed decoding, profile/centroid/line representations, calibration metadata, truncation, overflow, and sparse payload handling.
4. Complete chromatogram header/point contracts and persistence/reopen tests.
5. Add operation-level tests for spectra headers, chromatogram headers, TIC/raw arrays, and bounded flattened results.
6. Keep C++ reader implementations and semantic contracts aligned; add Rust only when the separate backend is explicitly reopened.
7. Validate readers through packaged MCP operation graphs, not only direct library tests.

## Acceptance

- Every supported reader preserves native processing level and returns deterministic metadata.
- C++ public operations expose the same canonical column names as semantic tables and results.
- Multi-analysis files never silently read analysis zero for another selected row.
- Unsupported or malformed data fails with a bounded public error and no partial artifact publication.
