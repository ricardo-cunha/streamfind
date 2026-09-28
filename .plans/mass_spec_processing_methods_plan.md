# MassSpec Processing and Quantification Future Plan

**Owner:** `cpp/plugins/mass_spec`
**Scope:** chromatogram processing, feature operations, targeted quantification, calibration, internal standards, and QC.

## Operation model

Every processing step is a typed `sf:Operation` with semantic parameters, explicit input/output ports, validation, execution, provenance, and immutable output artifacts. Processing must not mutate an existing result in place or depend on an implicit current table.

## Planned operation families

### Chromatograms

- load chromatograms and headers;
- baseline correction;
- smoothing;
- peak detection and integration;
- retention-time filtering;
- TIC/EIC/XIC and detector-independent channel processing.

### Targeted quantification

- target and transition definition;
- channel matching for MRM, DAD, UV, ADC, TIC, and EIC/XIC;
- internal-standard assignment and normalization;
- calibration-curve fitting;
- unknown-sample quantification;
- quantitative QC and acceptance diagnostics.

### Non-target analysis

- feature detection and loading;
- componentization, annotation, blank subtraction, correction, and filtering;
- suspect/internal-standard screening;
- transformation-product assignment with an explicit typed result contract.

## Implementation order

1. Confirm canonical chromatogram headers, point columns, units, and optional-value behavior in semantic sources.
2. Define typed table/result contracts and operation ports for each family.
3. Implement one vertical load → preprocess → detect → publish flow with atomic artifact publication.
4. Add branching, filtering, query operations, and visualization-spec publication.
5. Add targeted quantification and calibration only after peak contracts are stable.
6. Add cross-operation persistence/reopen, cancellation, cache, and failure rollback tests.

## Acceptance

- Parameters and defaults are declared once in Turtle and projected to the runtime catalogue.
- Operations consume exact artifact bindings, including secondary dependencies.
- Results use canonical names such as `precursor_mz`, `activation_ce`, and `product_mz`.
- Processing remains detector-independent; channel metadata selects the signal, not a separate algorithm implementation.
- Numerical fixtures, persistence tests, and packaged MCP smoke tests cover each released operation family.
