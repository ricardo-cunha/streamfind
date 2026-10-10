# Unified Suspect and Internal-Standard Identification

**Owner:** `cpp/plugins/mass_spec`
**Operations:** `mass_spec.suspect_screening`, `mass_spec.find_internal_standards`
**Status:** Complete — native/catalogue/frontend/regression validation and disposable DB workflow validation passed.

## Progress track

- [x] Share the screening engine and expose `isotope_ppm` for internal standards.
- [x] Make filtered-feature selection consistent for the two screening operations
      using the repository-wide `filtered` contract (`filtered=false` excludes
      filtered rows; `filtered=true` includes them).
- [x] Make target selection deterministic instead of allowing the last compatible
      suspect to overwrite the assignment.
- [x] Publish the shared identification evidence through the independent
      `internalStandardsTable` contract without emitting a suspect-table side
      effect.
- [x] Preserve isotope evidence and feature component/adduct context through
      reload and filtering.
- [x] Update ontology, operation guidance, and table contracts.
- [x] Keep `internalStandardsTable` as an independent contract while documenting
      its shared suspect-identification vocabulary and reusable explorer view.
- [x] Make `find_internal_standards` consume only `featuresTable`, process all
      analyses, and emit only `internalStandardsTable` plus success.
- [x] Add `enrich_internal_standards` to fill feature group/component/adduct
      context after grouping or component creation.
- [x] Allow the suspects explorer viewer to accept `internalStandardsTable`.
- [x] Pass MinGW plugin build, catalogue generation, plugin contract tests, and
      Python regression tests.
- [x] Run a dedicated `find_internal_standards` workflow on disposable copies of
      `tmp/projects/metfrag_verify.duckdb` and `tmp/projects/merck_workflow.duckdb`.

## Objective

Make suspect screening and internal-standard finding use one identification
engine and one canonical result model. `internalStandardsTable` remains the
authoritative output for alignment and correction, but gains the identification
evidence currently available in `suspectsTable`.

The two tables share the same matched-feature evidence vocabulary and screening
engine. The internal-standard table remains an independent schema, specialized
by its target role and correction consumers, rather than being a physical alias
of the suspect table or a separate matching algorithm.

## Current implementation finding

`find_internal_standards` now calls the shared `screening_impl` used by
`suspect_screening`. Its `filtered` parameter is passed through with the same
semantics and its default is `true`, so the execution path is equivalent to:

```text
shared screening engine(..., filtered=true, write_internal_standards=true)
```

The operation now accepts the same isotope tolerance, uses deterministic
candidate selection, emits only the dedicated internal-standard projection,
and preserves the evidence/context columns when the results are reloaded for
filtering or correction.

## Target data model

Define one canonical matched-feature result containing:

- analysis and feature identity;
- feature group, component, and adduct context;
- candidate rank and target name;
- database/experimental mass, RT, and errors;
- intensity and area;
- identification level and score;
- isotope evidence: theoretical peaks, matched peaks, similarity, and match;
- MS2 evidence: shared fragments, cosine similarity, database and experimental
  spectra;
- formula, SMILES, InChI, InChIKey, and xLogP;
- EIC and MS1/MS2 feature context where available.

Extend `suspectsTable` with every field currently unique to
`internalStandardsTable`, specifically:

- `feature_component`;
- `adduct`.

The independent `internalStandardsTable` retains the identification evidence
needed for QC and the reusable explorer. It remains an independent schema
with its own lifecycle: feature groups/components may be absent at creation
time, and later enrichment joins it to `featuresTable`. The semantic relation
to `suspectsTable` is hierarchical vocabulary reuse, not physical-table
identity.

## Execution design

1. Refactor screening to produce canonical matched-feature rows once.
2. Use the same mass, RT, isotope, MS2, score, candidate-rank, and
   identification-level logic for both operations.
3. Expose `isotope_ppm` on `find_internal_standards` with the same default and
   validation as `suspect_screening`.
4. Keep `filtered` as the feature-input selection control, defaulting to
   `true` for both operations.
5. Project canonical rows into the independent `internalStandardsTable`
   without emitting a `suspectsTable` side effect from
   `find_internal_standards`.
6. Keep correction and alignment consumers reading the existing internal
   standard columns; additional columns enrich the table without changing
   their algorithms.
7. Make candidate selection deterministic when multiple target rows match the
   same feature. Do not let the last mass-compatible target silently win.

## Ontology changes

- Extend the suspect-result column catalogue with the fields needed by the
  canonical result model.
- Add `feature_component` and `adduct` to `suspectsTable`, because they are
  currently internal-standard-only fields.
- Retain the identification columns already present in `internalStandardsTable`
  while preserving its correction-specific columns and lifecycle.
- Document `internalStandardsTable` as the internal-standard specialization of
  the shared suspect-result vocabulary, not as a physical copy of the suspect
  table.
- Add `isotope_ppm` to the Find Internal Standards operation parameters.
- Keep `featuresTable` as the output used by feature-domain operations such as
  grouping; do not make suspect rows a substitute for feature rows.
- Keep `internalStandardsTable` as the input to RT alignment, filtering, and
  matrix/suppression correction.
- Keep `internalStandardsTable` physically independent from `suspectsTable`.
  Reuse the shared identification vocabulary and explorer view through the
  semantic hierarchy rather than requiring identical columns or lifecycle.
- Add `enrich_internal_standards` as a mutation-style projection operation. It
  joins rows to `featuresTable` within each analysis by feature identity and
  fills feature group/component/adduct fields when those become available.

## Validation

- Compare suspect screening and internal-standard finding on the same target
  and feature inputs; mass, RT, isotope, MS2, score, rank, and identification
  level must agree.
- Verify the repository-wide `filtered` contract is applied consistently in both
  operations.
- Verify changing `isotope_ppm` changes isotope evidence consistently.
- Verify internal-standard correction and RT alignment produce the same result
  with the enriched table as with the current required columns.
- Verify all new table columns serialize, reload, filter, and persist through
  DuckDB workflows.
- Verify `find_internal_standards` can run directly after `find_features` with
  empty feature groups/components, and that enrichment fills them later.
- Verify the suspects explorer accepts both table contracts and handles missing
  optional group/component fields.
- Validate with disposable copies of `tmp/projects/metfrag_verify.duckdb`
  and `tmp/projects/merck_workflow.duckdb`.
- Run the MinGW UCRT64 plugin build, catalogue generation, plugin tests, and
  the NTA workflow regression.

## Non-goals

- Do not remove or rename `internalStandardsTable`.
- Do not change the correction or alignment mathematical models in this phase.
- Do not create a second identification engine for internal standards.
- Do not make `suspectsTable` replace `featuresTable` for grouping operations.
