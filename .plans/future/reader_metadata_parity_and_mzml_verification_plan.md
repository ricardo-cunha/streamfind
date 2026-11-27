# Reader metadata parity and mzML verification plan

Status: future
Owner: streamfind mass-spec readers

## Objective

Build a reader-parity framework that verifies every field exposed by the generic
mass-spectrometry reader interface against the underlying format-specific
parsers. Use ProteoWizard MSConvert-generated mzML as the comparison reference
for each supported vendor/raw format, while preserving format-specific
semantics where mzML cannot represent them exactly.

The immediate motivating case is SCIEX WIFF TOF DDA data: native isolation
window metadata is currently not exposed by the SCIEX reader, while mzML
isolation-window CV parameters are parsed by the generic mzML reader. The
resulting framework must make this discrepancy measurable and prevent silent
loss of metadata.

## Scope

In scope:

- The generic spectrum, chromatogram, analysis-catalogue, and binary-array
  reader contracts.
- All currently supported specific readers under
  `cpp/plugins/mass_spec/src/readers/`.
- SCIEX WIFF TOF DDA, SCIEX WIFF MRM, mzML, and every other active reader
  registered in the reader factory.
- Native-to-mzML comparison using corresponding files converted with
  ProteoWizard MSConvert.
- Correct parsing and propagation of precursor m/z, isolation-window center,
  lower and upper bounds, MS level, polarity, retention time, collision energy,
  scan/index identifiers, array lengths, and chromatogram metadata.
- Contract tests and disposable data-backed verification scripts.

Out of scope:

- Changes to third-party code under `cpp/vendor/`.
- Treating mzML as authoritative when it drops vendor-specific information.
- Replacing profile data with a weighted-average centroiding algorithm.
- Changing workflow semantics or MetFrag matching rules except where a reader
  contract bug directly causes incorrect input data.

## Design decisions to preserve

- Keep one generic reader API and one owning implementation path per format;
  do not add compatibility shims or duplicate readers.
- Keep native raw/profile data available to callers that request it.
- Apply centroiding only at the explicit extraction boundary used by MS1/MS2
  feature loading, using apex/top-trace selection rather than weighted
  averaging.
- Represent isolation windows as explicit metadata with center, lower bound,
  upper bound, and provenance. Do not infer a symmetric window when native
  bounds are available.
- Distinguish the instrument isolation window from an operation’s target
  matching tolerance. A workflow parameter such as `isolation_window` must be
  documented as either full width or half-width and must not silently replace
  native scan metadata.

## Phase 1 — Inventory the generic contract

1. Enumerate the reader interface and all fields returned by:
   - file summary and analysis catalogue;
   - spectrum headers;
   - spectrum binary arrays;
   - chromatogram headers and arrays;
   - precursor and activation metadata;
   - analysis selection and source identifiers.
2. Build a field matrix with, for every field:
   - generic field name and type;
   - units and valid missing-value representation;
   - required/optional status;
   - expected cardinality and row alignment;
   - whether it is native, derived, or unavailable;
   - parser implementations currently supplying it.
3. Record all format factory branches and identify fields that currently return
   empty vectors, zeroes, or placeholder values.
4. Add a machine-readable contract description used by the verification tools,
   rather than duplicating field lists in each test.

Deliverable: a reviewed reader-field matrix and a contract schema covering
every generic field.

## Phase 2 — Define the comparison model

1. Define stable correspondence keys between native data and converted mzML:
   - source file and analysis number/name;
   - acquisition mode and polarity;
   - scan number where preserved;
   - retention time with a documented tolerance;
   - MS level;
   - precursor m/z and isolation-window identity.
2. Define field-specific comparison tolerances:
   - exact equality for categorical fields and analysis selection;
   - bounded tolerance for retention time and m/z;
   - relative/intensity tolerance for TIC, BPC, and profile arrays;
   - set or sequence comparison for chromatogram identifiers;
   - explicit handling of vendor fields absent from mzML.
3. Define expected transformations made by MSConvert, including centroiding,
   scan numbering, spectrum ordering, compression, and possible loss of vendor
   metadata.
4. Require the report to distinguish:
   - parser disagreement;
   - MSConvert transformation;
   - information unavailable in one representation;
   - unsupported or ambiguous native metadata.

Deliverable: comparison specification and tolerance configuration.

## Phase 3 — Implement native metadata extraction

1. SCIEX WIFF TOF:
   - locate the native method/experiment records containing precursor target,
     isolation lower/upper offsets, collision energy, polarity, and acquisition
     mode;
   - populate the generic spectrum header fields for each MS2 scan;
   - preserve asymmetric windows;
   - distinguish DDA, SWATH/DIA, and MRM records;
   - keep MRM transition windows in chromatogram metadata rather than treating
     them as ordinary DDA spectra.
2. SCIEX WIFF MRM:
   - verify transition precursor/product values, period/experiment/transition
     identity, start/end times, and collision energy;
   - ensure TIC/BPC and other chromatograms retain their signal type and units.
3. mzML:
   - verify target, lower-offset, and upper-offset CV parsing;
   - preserve selected-ion precursor m/z separately from isolation center;
   - handle missing or asymmetric offsets without manufacturing incorrect
     bounds.
4. Audit all remaining readers for the same fields and implement the intended
   behavior at their owning parser boundary.
5. Add provenance/availability indicators where a generic field cannot be
   obtained natively, so missing metadata is visible rather than represented as
   an indistinguishable zero.

Deliverable: complete, format-specific population of the generic metadata
contract with unit tests for native parsing helpers.

## Phase 4 — Separate isolation filtering from target matching

1. Update the shared target-extraction logic so that a candidate MS2 scan must
   satisfy:
   - MS level, polarity, retention-time, and precursor matching rules; and
   - the native isolation-window constraint when valid bounds are available.
2. Keep the feature-target precursor tolerance as a separate, explicit
   parameter used only when native isolation metadata is unavailable or when
   the operation intentionally requests a broader search.
3. Replace ambiguous parameter naming and descriptions. In particular, make
   clear whether `isolation_window` is a total width or a half-width, and avoid
   using `feature_mz ± 0.65` accidentally when a 0.65 Da total window is
   intended.
4. Add diagnostics to the operation result/log:
   - candidate scans before native-window filtering;
   - scans rejected by precursor matching;
   - scans rejected by native isolation bounds;
   - scans accepted through fallback matching because native bounds were absent.
5. Verify that product-ion filtering remains independent of precursor isolation:
   product m/z must not be used as a substitute for precursor-window matching.

Deliverable: deterministic MS2 selection with auditable native-window and
fallback behavior.

## Phase 5 — Build the reader-parity verification framework

Implement a development-stage checker under `scripts/dev/` using the repository
`.venv`. It should:

1. Accept a native file and its MSConvert mzML counterpart.
2. Enumerate every analysis and compare summaries, headers, arrays, and
   chromatograms using the contract schema.
3. Produce machine-readable JSON/CSV results and a human-readable HTML report.
4. Show per-field coverage, mismatch counts, tolerances, and representative
   examples.
5. Flag all zero/empty native fields when the mzML counterpart contains valid
   values, with special emphasis on isolation metadata.
6. Compare profile and centroid representations separately; do not classify
   expected profile-vs-centroid differences as parser failures.
7. Support focused filters for a file, analysis, scan range, MS level, polarity,
   precursor range, and retention-time range.
8. Verify ordering and row alignment independently from value comparison.
9. Exit nonzero for contract violations, missing required fields, or mismatches
   beyond configured tolerances.

The checker must resolve the shared example-data location according to the
repository instructions and keep disposable reports under the checkout `tmp/`.

Deliverable: reusable parity checker, schema-driven reports, and developer
documentation.

## Phase 6 — Test data and ProteoWizard workflow

1. Assemble a representative corpus for each active format and acquisition
   mode, including:
   - SCIEX WIFF TOF DDA with MS1/MS2 profile data;
   - SCIEX WIFF MRM;
   - SWATH/DIA where supported;
   - mzML originals and MSConvert-generated mzML counterparts.
2. For every native file, record the exact MSConvert command, version, options,
   centroid/profile mode, and source checksum.
3. Include the Kooperationslabor controls spanning 2022–2024, especially
   Diclofenac-D4 and other internal standards with diagnostic MS2 ions.
4. Compare native and converted data at both header and peak-list levels.
5. Specifically verify whether m/z 214-related traces are:
   - present in the same precursor-isolated scan in both formats;
   - only present in WIFF profile data;
   - created by centroid/clustering;
   - associated with a different precursor or acquisition mode.

Deliverable: reproducible corpus manifest and baseline comparison reports.

## Phase 7 — Native/plugin and workflow validation

1. Add focused C++ tests under the owning mass-spec reader test area for:
   - isolation-window parsing;
   - asymmetric windows;
   - DDA versus SWATH versus MRM classification;
   - header cardinality and alignment;
   - profile-to-centroid apex selection;
   - target extraction with and without native bounds.
2. Keep data-backed comparisons and workflow runs in `scripts/dev/`, not in the
   official CTest suite.
3. Run the checker against the corpus before and after the implementation.
4. Re-run the relevant Kooperationslabor WIFF and mzML workflows from clean
   result stores, then compare:
   - selected MS2 scan counts;
   - feature MS2 peak counts;
   - precursor/window assignments;
   - internal-standard shared fragments and cosine scores;
   - Diclofenac-D4 diagnostic traces.
5. Confirm that MRM chromatograms remain intact and that SWATH controls are not
   accidentally treated as normal DDA controls.

## Phase 8 — Package and inspection validation

1. Run the narrowest native tests and the parity checker.
2. Rebuild the Windows package through `sfdev` from the current sources; do not
   patch `tmp/packaged_build` directly.
3. Start the packaged service through its package CLI and verify the executable
   and service endpoint belong to the intended package.
4. Load disposable validation projects and inspect the generated header tables,
   feature MS2 tables, and HTML parity reports.
5. Record all commands, build identifiers, package paths, test results, and
   known limitations in the completion notes.

## Acceptance criteria

- Every generic reader field has an explicit status for every active parser:
  parsed, derived, unavailable-with-reason, or not applicable.
- SCIEX WIFF TOF DDA exposes valid precursor isolation center/lower/upper
  bounds whenever the native file contains them.
- Native isolation-window filtering is distinct from feature-target matching
  tolerance and is visible in operation diagnostics.
- Corresponding WIFF and MSConvert mzML files select the same precursor scans
  within documented tolerances, or the report explains every difference.
- Profile-to-centroid differences are quantified without weighted averaging and
  do not create broad artificial clusters.
- The Diclofenac-D4 comparison identifies whether the m/z 214 trace is native,
  co-isolated, or introduced/retained during profile processing.
- MRM, DDA, and SWATH data remain separated and retain their correct output
  types.
- C++ tests, development checks, clean workflow runs, and packaged validation
  all pass before the plan is marked complete.

## Risks and open questions

- SCIEX method metadata may encode isolation windows in different OLE streams
  across instrument/software generations; the parser must report unsupported
  layouts explicitly.
- MSConvert may not preserve all native SCIEX fields, so mzML is a comparison
  reference, not an unquestionable ground truth.
- Some DDA files may have precursor m/z but no explicit isolation bounds. These
  cases require a documented fallback and must not silently appear equivalent to
  a narrow native window.
- Profile data can contain real low-intensity ions as well as sampling noise;
  parity reports must retain raw evidence so filtering decisions are auditable.

## Completion note

This is a future implementation plan only. No reader, workflow, package, or
user project is changed by creating this document.
