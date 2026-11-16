# Quantification Framework Design

## Overview

This document defines the data model, input parameters, and workflow for targeted quantitative analysis in StreamFind. The framework supports MRM (MS), DAD/UV, and any future detector type through a common channel/target model.

## Revision: implementation audit and next-task baseline

This revision separates the original design from the work that is already
present in the `mass_spec_expansion` worktree. The chromatogram-processing
foundation and the configuration/assignment contract slice are implemented;
peak-to-target response extraction and calibration remain future work.

### Already implemented

- `mass_spec.load_chromatograms` reads native chromatogram headers and points
  into `chromatogramsHeadersTable` and `chromatogramsTable`.
- `mass_spec.correct_chromatogram_baseline` supports `none`, `rolling_min`,
  `moving_average`, and `als`, preserving `raw_intensity`.
- `mass_spec.smooth_chromatograms` supports `none`, `moving_average`, and
  `savitzky_golay`.
- `mass_spec.find_chromatogram_peaks` detects and integrates peaks from the
  processed `intensity` signal and publishes `chromatogramPeaksTable`.
  The current peak contract already exposes RT bounds, height, area, raw area,
  baseline area, width, FWHM, SNR, and integration metadata.
- `mass_spec.get_chromatograms` and `mass_spec.get_chromatogram_peaks` expose
  persisted data, with chromatogram header fields joined to peak rows.
- The ontology and operation catalogue describe these tables and operations.
- `mass_spec.set_quantification_method` accepts one horizontal row per
  compound, including explicit internal-standard assignment and prefixed
  quantifier/qualifier MRM fields, and emits `quantificationMethodTable`.
- `mass_spec.set_quantification_analyses` consumes that method table plus
  `analysesTable` and emits the project-specific
  `quantificationAnalysesTable`.
- Validation rejects incomplete transitions, duplicate compound IDs, unknown
  internal-standard references, and analysis/target assignment errors.
- `scripts/dev/cpp/test-quantification-sciex-fixtures.ps1` provides a
  development-only harness for the supplied SCIEX WIFF fixtures. It checks
  `.wiff.scan` companions, discovers real analysis names, and exercises the
  horizontal method and assignment workflow in an isolated runtime when no
  service is already running. It deliberately refuses to attach to an
  existing service.
- The shared Arrow output path now preserves semantic `timestamp` columns for
  plugin-emitted table rows; this is required by the chromatogram, method,
  assignment, and response contracts.
- The horizontal method contract uses `rt` for expected retention time, and
  the assignment operation validates against the complete method projection
  before emitting project-specific assignments.

### Not implemented yet

Calibration fitting, unknown calculation, QC evaluation, and the final
`mass_spec.quantify_samples` orchestration operation are not implemented yet.
The method and assignment tables plus response-extraction output are
configuration/observation artifacts, not calibrated concentration results.

The implemented `mass_spec.extract_quantification_responses` operation matches
configured quantifier and qualifier transitions to chromatogram headers,
selects peaks inside the configured RT window, extracts area or height,
resolves the assigned internal-standard response for the same analysis, and
publishes explicit statuses and qualifier ratios in
`quantificationTable` (the unified per-analysis table containing response,
calibration-point diagnostics, and final quantification fields).

### Contract constraints discovered during the audit

- Point rows use `analysis` plus `index`; `chromatogram_index` is the peak-table
  column and must not be introduced as a competing point-table key.
- Peak IDs are generated per `find_chromatogram_peaks` output, so a future
  quantification result must identify the analysis, chromatogram index, and
  peak ID together.
- Header metadata is the current source of channel identity (`signal_type`,
  `chromatogram_type`, polarity, precursor/product m/z, detector, channel,
  and wavelength). Quantification channel matching should reuse these fields
  rather than duplicate reader-specific metadata.
- The existing processing operations mutate/publish the chromatogram artifact;
  quantification should consume the persisted peak/header artifacts and must
  not create a second peak-detection path.

### SOP-informed requirements and product request

The supplied German SOPs are reference documents, not executable instructions
for the implementation. Their rendered method tables reinforce these domain
requirements:

- A compound is identified by one or more explicit MRM transitions, with the
  precursor/product m/z pair retained as method data rather than inferred from
  a free-text compound name.
- Internal standards are explicit method compounds and must be assigned to the
  analyte compounds they normalize. One internal standard may serve multiple
  analytes, but an analyte must not silently inherit an arbitrary standard.
- Quantifier and qualifier transitions need distinct roles, so the method can
  calculate a response from the quantifier and check a qualifier/quantifier
  ratio when available.
- Method configuration includes the concentration/calibration context needed
  to interpret the measured response; the assignment must remain auditable.

The product requirement is to let the user provide this mapping in a clear
node-parameter UI. Therefore the quantification configuration node must expose
structured repeatable rows for compounds, internal standards, and MRM
transitions, with references selected from the same edited assay rather than
entered as unrelated strings.

The original model and examples below remain the desired product direction,
with the execution sequence and acceptance criteria replaced by the plan at
the end of this document.

### Implementation checkpoint — 2026-11-10

The first implementation slice is now present in the worktree:

- native validation helpers cover target, channel, and sample-assignment
  invariants;
- `mass_spec.set_quantification_method` and
  `mass_spec.set_quantification_analyses` are registered native operations;
- the horizontal method and sample-assignment table contracts and parameters
  are in the MassSpec ontology;
- a focused CTest contract covers valid analyte/internal-standard MRM setup and
  representative invalid m/z and sample-concentration cases.

This slice is intentionally not the completed quantification workflow. The
operations currently validate and publish configuration rows; they do not yet
perform peak assignment, calibration, or quantification, and the frontend
mapping editor still needs to be built against these table parameters.

## Core Concepts

### Analytical Workflow

```text
1. Define targets (compounds to quantify)
2. Define channels (which chromatographic signal identifies each target)
3. Assign samples (calibrators, QCs, unknowns with concentrations)
4. Load chromatograms
5. Process chromatograms (baseline, smoothing)
6. Detect peaks
7. Assign peaks to targets (match channel + RT)
8. Calculate responses (area or height)
9. Normalize to internal standards (if applicable)
10. Fit calibration models
11. Calculate unknown concentrations
12. Evaluate QC criteria
```

## Data Model

### 1. Target Definition

**Table: `MASS_SPEC_QUANT_TARGETS`**

A target represents an analyte or internal standard to be quantified.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `target_id` | string | yes | Unique identifier (e.g. `caffeine`, `IS_diclofenac_d4`) |
| `name` | string | no | Human-readable name |
| `role` | string | yes | `analyte` or `internal_standard` |
| `internal_standard_id` | string | no | Reference to IS target (for analytes only) |
| `rt` | real | no | Expected retention time in seconds |
| `rt_tolerance` | real | no | RT matching window in seconds |
| `response_metric` | string | yes | `area` or `height` |
| `normalization` | string | yes | `none` or `internal_standard` |
| `concentration_unit` | string | no | e.g. `ug/L`, `nmol/L` |
| `regression_model` | string | no | `linear`, `quadratic` |
| `weighting` | string | no | `none`, `1/x`, `1/x2` |
| `intercept_mode` | string | no | `free` or `forced_zero` |
| `enabled` | boolean | yes | Whether to include in quantification |
| `created_at` | timestamp | yes | Creation time |

**Key rules:**
- Internal standards have `role = 'internal_standard'` and no `internal_standard_id`
- Analytes reference their IS via `internal_standard_id`
- Multiple analytes can share the same IS
- `rt` and `rt_tolerance` are used for peak assignment

### 2. Channel Definition

**Table: `MASS_SPEC_QUANT_CHANNELS`**

A channel maps a target to a specific chromatographic signal.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `channel_id` | string | yes | Unique channel identifier |
| `target_id` | string | yes | Reference to target |
| `channel_role` | string | yes | `quantifier` or `qualifier` |
| `signal_type` | string | yes | `MS` or `UV` |
| `chromatogram_type` | string | no | `MRM`, `SRM`, `DAD`, `EIC` |
| `polarity` | integer | no | `1` (positive) or `-1` (negative) |
| `precursor_mz` | real | no | Precursor m/z (MRM only) |
| `product_mz` | real | no | Product m/z (MRM only) |
| `activation_ce` | real | no | Collision energy (MRM only) |
| `detector` | string | no | Detector name (DAD: `DAD1`) |
| `wavelength_nm` | real | no | Wavelength (DAD/UV only) |
| `mz_tolerance_ppm` | real | no | m/z matching tolerance |
| `expected_ratio` | real | no | Expected qualifier/quantifier ratio |
| `ratio_tolerance` | real | no | Acceptable ratio deviation |
| `enabled` | boolean | yes | Whether to include |
| `created_at` | timestamp | yes | Creation time |

**Key rules:**
- Each target must have exactly one enabled quantifier channel
- Zero or more qualifier channels per target
- MRM channels require `precursor_mz` and `product_mz`
- DAD channels require `wavelength_nm`
- The same channel table works for all detector types

### 3. Sample Assignment

**Table: `MASS_SPEC_QUANT_SAMPLE_ASSIGNMENTS`**

Assigns concentration and role to each analysis × target combination.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `analysis` | string | yes | Analysis name |
| `target_id` | string | yes | Reference to target |
| `sample_type` | string | yes | `blank`, `zero`, `calibrator`, `qc`, `unknown` |
| `calibration_level` | integer | no | Level number (calibrators only) |
| `nominal_concentration` | real | no | Known concentration (calibrators/QCs) |
| `dilution_factor` | real | no | Dilution factor (default 1.0) |
| `exclude` | boolean | yes | Whether to exclude from calibration |
| `created_at` | timestamp | yes | Creation time |

**Key rules:**
- Key is `analysis × target_id` (not just `analysis`)
- Unknowns need no `nominal_concentration`
- `dilution_factor` scales the final concentration
- `exclude` allows removing outliers from calibration

### 4. Calibration Results

**Table: `MASS_SPEC_QUANT_CALIBRATION_MODELS`**

Persisted calibration model per target.

| Field | Type | Description |
|-------|------|-------------|
| `target_id` | string | Reference to target |
| `model` | string | `linear` or `quadratic` |
| `weighting` | string | `none`, `1/x`, `1/x2` |
| `intercept_mode` | string | `free` or `forced_zero` |
| `coefficient_0` | real | Intercept |
| `coefficient_1` | real | Slope |
| `coefficient_2` | real | Quadratic term |
| `r_squared` | real | R² |
| `adjusted_r_squared` | real | Adjusted R² |
| `rmse` | real | Root mean square error |
| `n_points` | integer | Number of calibration points |
| `range_min` | real | Minimum concentration in calibration |
| `range_max` | real | Maximum concentration in calibration |
| `fit_status` | string | `ok`, `failed`, `insufficient_data` |
| `created_at` | timestamp | Creation time |

### 5. Calibration Points

**Table: `MASS_SPEC_QUANT_CALIBRATION_POINTS`**

Individual observations used for calibration.

| Field | Type | Description |
|-------|------|-------------|
| `target_id` | string | Reference to target |
| `analysis` | string | Analysis name |
| `peak_id` | integer | Reference to peak |
| `calibration_level` | integer | Level number |
| `nominal_concentration` | real | Known concentration |
| `response` | real | Peak area or height |
| `is_response` | real | IS response (if IS-normalized) |
| `response_ratio` | real | response / IS response (if IS-normalized) |
| `included` | boolean | Whether used in calibration fit |
| `back_calculated_concentration` | real | Concentration from calibration model |
| `residual` | real | Difference from nominal |
| `relative_residual` | real | Residual as % of nominal |
| `exclusion_reason` | string | Why excluded (if applicable) |
| `created_at` | timestamp | Creation time |

### 6. Quantification Results

**Table: `MASS_SPEC_QUANT_RESULTS`**

Final quantified results per analysis × target.

| Field | Type | Description |
|-------|------|-------------|
| `analysis` | string | Analysis name |
| `target_id` | string | Reference to target |
| `sample_type` | string | `blank`, `calibrator`, `qc`, `unknown` |
| `peak_id` | integer | Reference to peak |
| `rt` | real | Observed retention time |
| `rt_error` | real | RT deviation from expected |
| `area` | real | Peak area |
| `height` | real | Peak height |
| `response` | real | Used response (area or height) |
| `is_response` | real | IS response (if IS-normalized) |
| `response_ratio` | real | response / IS response |
| `calculated_concentration` | real | From calibration model |
| `dilution_factor` | real | Applied dilution factor |
| `final_concentration` | real | After dilution correction |
| `concentration_unit` | string | Unit string |
| `qualifier_ratio` | real | Observed qualifier/quantifier ratio |
| `qualifier_ratio_pass` | boolean | Within tolerance |
| `accuracy_percent` | real | For calibrators/QCs |
| `status` | string | `ok`, `peak_not_found`, `below_calibration_range`, `above_calibration_range` |
| `flags` | string | Comma-separated QC flags |
| `created_at` | timestamp | Creation time |

## Input Parameters for Unknown Quantification

### Required Inputs

To quantify unknowns, the user must provide:

#### 1. Target Definition
```json
{
  "target_id": "caffeine",
  "name": "Caffeine",
  "role": "analyte",
  "internal_standard_id": "IS_caffeine_d9",
  "rt": 1020.0,
  "rt_tolerance": 30.0,
  "response_metric": "area",
  "normalization": "internal_standard",
  "concentration_unit": "ug/L"
}
```

#### 2. Channel Definition (at least one quantifier per target)
```json
{
  "channel_id": "caffeine_quant",
  "target_id": "caffeine",
  "channel_role": "quantifier",
  "signal_type": "MS",
  "chromatogram_type": "MRM",
  "precursor_mz": 195.1,
  "product_mz": 138.1,
  "polarity": 1
}
```

#### 3. Sample Assignments
```json
{
  "analysis": "cal_5ugL",
  "target_id": "caffeine",
  "sample_type": "calibrator",
  "calibration_level": 5,
  "nominal_concentration": 5.0,
  "dilution_factor": 1.0
}
```

#### 4. Blank/Calibrator/Unknown Designations
```json
{"analysis": "blank_1", "target_id": "caffeine", "sample_type": "blank"}
{"analysis": "unknown_1", "target_id": "caffeine", "sample_type": "unknown"}
```

### Optional Inputs

| Parameter | Default | Description |
|-----------|---------|-------------|
| `regression_model` | `linear` | Calibration curve type |
| `weighting` | `none` | Regression weighting (`1/x`, `1/x2`) |
| `intercept_mode` | `free` | Force through origin? |
| `mz_tolerance_ppm` | `20` | m/z matching tolerance |
| `expected_ratio` | null | Qualifier/quantifier ratio |
| `ratio_tolerance` | null | Acceptable ratio deviation |

## Workflow for Unknown Quantification

### Step 1: Define Assay
```text
User defines:
  - Targets (analytes + internal standards)
  - Channels (MRM transitions or DAD wavelengths)
  - Expected RTs and tolerances
```

### Step 2: Import and Process Data
```text
mass_spec.add_analyses          → load raw data
mass_spec.load_chromatograms    → persist signal points
mass_spec.correct_chromatogram_baseline → remove baseline
mass_spec.smooth_chromatograms  → reduce noise
mass_spec.find_chromatogram_peaks → detect peaks
```

### Step 3: Assign Samples
```text
User designates each analysis as:
  - blank (no analyte)
  - calibrator (known concentration)
  - qc (quality control)
  - unknown (to be quantified)
```

### Step 4: Link Peaks to Targets
```text
For each analysis × target:
  1. Find the channel(s) matching the target
  2. Find peaks in the chromatogram matching the channel
  3. Select the peak within RT tolerance
  4. Extract response (area or height)
  5. If IS-normalized: find IS peak, calculate ratio
```

### Step 5: Fit Calibration
```text
For each target:
  1. Collect calibration points (calibrator analyses)
  2. Apply weighting (if specified)
  3. Fit linear or quadratic model
  4. Calculate R², RMSE, residuals
  5. Flag outliers (residual > threshold)
```

### Step 6: Quantify Unknowns
```text
For each unknown analysis × target:
  1. Get response (area/height or IS ratio)
  2. Apply calibration model inverse
  3. Apply dilution factor
  4. Check if within calibration range
  5. Store result with status
```

### Step 7: QC Evaluation
```text
For each QC analysis × target:
  1. Calculate concentration
  2. Compare with nominal
  3. Calculate accuracy (%)
  4. Apply QC rules (e.g. ±15% accuracy)
  5. Flag pass/fail
```

## Method Parameters

### `mass_spec.set_quantification_method`
```json
{
  "targets": [
    {
      "target_id": "caffeine",
      "role": "analyte",
      "internal_standard_id": "IS_caffeine_d9",
      "rt": 1020.0,
      "rt_tolerance": 30.0,
      "response_metric": "area",
      "normalization": "internal_standard",
      "quantifier_signal_type": "MS",
      "quantifier_polarity": 1,
      "quantifier_precursor_mz": 195.1,
      "quantifier_product_mz": 138.1,
      "qualifier_signal_type": "MS",
      "qualifier_polarity": 1,
      "qualifier_precursor_mz": 195.1,
      "qualifier_product_mz": 110.1
    }
  ]
}
```

### `mass_spec.set_quantification_analyses`
```json
{
  "assignments": [
    {
      "analysis": "cal_5ugL",
      "target_id": "caffeine",
      "sample_type": "calibrator",
      "calibration_level": 5,
      "nominal_concentration": 5.0
    }
  ]
}
```

### `mass_spec.quantify_samples`
```json
{
  "analysis_names": ["unknown_1", "unknown_2"],
  "target_ids": ["caffeine", "ibuprofen"]
}
```

## Backend Validation Rules

| Rule | Error |
|------|-------|
| Target ID unique | Duplicate target_id |
| Channel ID unique | Duplicate channel_id |
| Each target has ≥1 quantifier | No quantifier channel |
| MRM channel has precursor/product | Missing m/z fields |
| DAD channel has wavelength | Missing wavelength |
| IS reference exists | Unknown internal_standard_id |
| Calibrator has concentration | Missing nominal_concentration |
| Unknown has no concentration | Unexpected nominal_concentration |
| RT > 0 | Invalid rt |
| RT tolerance > 0 | Invalid rt_tolerance |

## Updated execution plan

The first release should prove one complete, deterministic targeted workflow
before adding every field in the original design. The recommended vertical
slice is one analyte plus one internal standard, one quantifier channel each,
linear calibration, area response, and unknown quantification. Qualifier
channels, quadratic/weighted models, DAD, and richer QC policy follow only
after that slice has a stable contract.

### Phase 1 — Freeze the quantification boundary

1. Define the persisted contracts for targets, channels, sample assignments,
   calibration points/models, and results in the mass-spec ontology. Reuse the
   existing `analysis`, `index`/`chromatogram_index`, and peak column names.
2. Make one horizontal method row express the SOP-relevant mapping directly:
   analyte versus internal-standard role, `internal_standard_id`, one enabled
   quantifier transition, and an optional qualifier transition. Prefix
   transition fields with `quantifier_` and `qualifier_` so the MRM identity
   (`polarity`, `precursor_mz`, `product_mz`, and optional collision energy) is
   explicit without requiring channel IDs or a second input table.
3. Decide whether configuration is stored as project tables, operation
   parameters, or both. Prefer project-owned tables with explicit operation
   inputs so configuration survives workflow reruns and is inspectable.
4. Define keys, replacement/idempotency behavior, timestamps, enabled flags,
   units, missing-peak statuses, and provenance fields. Explicitly define
   whether one analysis can have multiple target rows and how duplicate peaks
   are rejected.
5. Add catalogue entries for the tables, columns, operations, parameters, and
   result statuses. Run the aggregate catalogue build before implementation
   proceeds.

**Exit criteria:** a generated catalogue exposes the complete MVP schema, and
the contract states exactly how an analyte resolves to its assigned internal
standard, quantifier/qualifier MRM header rows, and peak rows.

### Phase 2 — Add assay configuration operations and validation

1. Implement `mass_spec.set_quantification_method` and
   `mass_spec.set_quantification_analyses` at the plugin operation boundary.
2. Validate uniqueness, analyte/internal-standard references, one enabled
   quantifier per target, MRM versus DAD required fields, positive RT windows,
   valid response/normalization values, and sample-type concentration rules.
3. Make writes deterministic and idempotent for the same logical key; do not
   add a legacy API or a second configuration store.
4. Add focused C++ contract tests under
   `cpp/plugins/mass_spec/tests/unit/` and register them with CTest. Keep the
   fixtures small and backend-neutral.

5. Define the operation-node parameter schema consumed by the frontend. The
   configuration editor should provide:
   - one horizontal compound table with name/id, role, expected RT, RT
     tolerance, response metric, normalization, concentration unit,
     internal-standard ID, and prefixed quantifier/qualifier signal, polarity,
     precursor m/z, product m/z, collision energy, detector, wavelength, and
     tolerance columns;
   - a sample/calibration table for analysis, sample type, level, nominal
     concentration, IS concentration, dilution, and exclusion;
   - inline validation for duplicate IDs, missing IS references, multiple
     quantifiers, incomplete transitions, and inconsistent sample roles;
   - a read-only summary/preview showing each analyte → IS → quantifier and
     qualifier mapping before the node is saved.
6. Keep the UI parameter payload aligned with the persisted contracts. The
   frontend may make the mapping easier to edit, but it must not maintain a
   separate interpretation or duplicate quantification implementation.

**Exit criteria:** valid assay configuration persists and reloads; every
invalid rule in the design table produces a stable diagnostic; rerunning the
same configuration does not duplicate rows; a user can inspect the complete
analyte → internal-standard → MRM mapping in the node before execution.

### Phase 3 — Implement peak-to-target assignment and response extraction

1. Implemented an internal named helper layer that joins configured channels to
   chromatogram headers and then to persisted peak rows.
2. Implemented channel metadata matching first, then deterministic peak
   selection within the target RT window (nearest RT, with stable
   chromatogram-index and peak-ID tie-breakers). The output retains explicit
   `peak_not_found`, `ambiguous_peak`, and `channel_mismatch` statuses.
3. Implemented area/height extraction from the existing peak contract. For
   internal-standard normalization, the assigned IS target is resolved using
   the same rules and zero or missing IS responses receive explicit statuses.
4. Persisted assignment/response rows so calibration and quantification consume
   the same observations rather than rerunning peak selection.

**Exit criteria:** the response table records deterministic analyte and IS peak
selection or stable failure statuses across calibrators, QCs, and unknowns;
edge cases never silently select an unrelated channel. A development-only
SCIEX fixture harness is available, but it refuses to attach while another
StreamFind service is running.

### Phase 4 — Fit calibration models

1. Implement the MVP linear model with free intercept and no weighting. Store
   coefficients, point count, range, fit status, residuals, and the exact
   included/excluded point decisions.
2. Implement inverse calculation for unknown responses, including dilution
   correction and below/above-range statuses. Reject invalid models and
   non-invertible responses rather than returning a plausible-looking value.
3. Add weighted linear, forced-zero, and quadratic models only after the MVP
   fixture and numerical contracts are stable. Specify weighting at zero
   concentration and quadratic-root selection before coding.
4. Use a small deterministic C++ numerical fixture with hand-checkable values;
   keep large instrument data out of official tests.

**Exit criteria:** calibration coefficients and back-calculated concentrations
match independently calculated expected values within a documented tolerance,
including failure cases for insufficient data and out-of-range responses.

### Phase 5 — Add QC evaluation and end-to-end operation

1. Implement `mass_spec.quantify_samples` as the single orchestration boundary
   for assignment, calibration, unknown calculation, and QC result publication;
   it should consume configured tables plus `chromatogramsHeadersTable` and
   `chromatogramPeaksTable`.
2. Add QC accuracy thresholds and explicit flags, keeping policy parameters
   separate from the numerical fit. Define blank/zero handling and whether
   excluded calibrators remain visible in audit output.
3. Publish calibration points, calibration models, and final results as typed
   artifacts with stable joins back to source analysis, header, and peak IDs.
4. Add one operation-graph contract test covering load → baseline/smoothing →
   peak detection → configuration → quantification, plus a development-stage
   data-backed run under `scripts/dev/cpp/` when a suitable fixture is
   available from `streamfind.data`.

**Exit criteria:** a complete one-analyte/one-IS workflow produces expected
   calibrator, QC, and unknown rows through the supported CMake/Ninja and MCP
   boundaries, with no direct access to reader internals from quantification.

### Phase 6 — Expand detector and method coverage

After the MVP is accepted, add qualifier ratio checks, multiple analytes
sharing an IS, DAD/UV wavelength matching, MRM/SRM metadata variants, weighted
and quadratic regression, dilution series, carryover/blank policy, and
frontend workflow configuration. Each addition needs a contract test and an
ontology/catalogue update. Do not introduce compatibility shims for the
pre-quantification API because none is released or implemented.

## Implementation status and next task

Phase 4 task 1 and the Phase 5 MVP operation boundary are implemented. The
workflow now provides:

- `mass_spec.fit_quantification_calibration`, which fits the unweighted linear
  free-intercept model from calibrant rows in `quantificationTable` and publishes typed
  calibration-model and calibration-point tables;
- `mass_spec.quantify_samples`, which consumes those tables, applies dilution
  correction, reports unknown/QC/calibrator status, evaluates qualifier ratios,
  and publishes a typed final-results table;
- explicit rejection of non-invertible calibration models. A negative slope or
  otherwise failed fit is exposed as `fit_status=failed`, and final
  concentrations are withheld with `calibration_not_available` rather than
  emitting misleading values.

The development SCIEX project was rerun through the packaged service. It
produced 15 included calibration points, one excluded point due to a missing
internal-standard response, and a failed NDMA model (slope approximately
`-0.003125`, `R²` approximately `0.11`). This is now a visible method/raw-peak
quality problem to investigate, not a silent numerical result.

The next task is Phase 4 task 3 / Phase 6 expansion: add a deterministic
calibration-curve inspection view and then implement weighted, forced-zero,
and quadratic models only after the current fixture's analyte/internal-standard
peak assignment and response monotonicity are corrected.

### Current validation note

The native build, catalogue conformance, packaged-service workflow run, and
focused mass-spec contracts pass. The full CTest suite passes after updating
the project-isolation fixture for the three new quantification tables. The
current fixture is intentionally not an end-to-end acceptance result for
unknown concentrations until its failed calibration model is resolved.
