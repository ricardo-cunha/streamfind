# Quantification Framework: Current Design and Expansion Plan

Status: active

Scope: targeted mass-spectrometry quantification in the
`mass_spec_expansion` worktree, including the native operation boundary,
typed result contracts, the quantification viewer, and the development-stage
SCIEX validation workflow.

This plan was reactivated from `.plans/stale/` after an implementation audit.
It describes the system that exists now and the work required to make the
calibration and QC implementation scientifically complete. It is deliberately
aligned with the current `.sf` project-container architecture.

## 1. Current implementation baseline

The quantification workflow is now intentionally exposed through two native
operations:

```text
mass_spec.set_quantification_plan
    -> quantificationMethodTable
    -> quantificationAnalysesTable

mass_spec.quantify
    -> quantificationTable
    -> quantificationCalibrationModelsTable
```

The second operation is the single execution boundary for response extraction,
internal-standard normalization, calibration fitting, sample quantification,
qualifier evaluation, and QC status publication. There is no separate runtime
operation for extracting responses, fitting calibration, or quantifying
samples in the current catalogue.

### 1.1 Configuration operation

`mass_spec.set_quantification_plan` accepts two array parameters:

- `quantification_method`: one horizontal row per analyte or internal
  standard;
- `quant_sample_assignments`: one row per `analysis × target_id` assignment.

The method row currently contains:

- target identity and role (`analyte` or `internal_standard`);
- assigned internal standard and normalization mode;
- expected RT, RT tolerance, response metric (`area` or `height`), and
  concentration unit;
- calibration declarations (`regression_model`, `weighting`, and
  `intercept_mode`);
- one required quantifier transition and one optional qualifier transition,
  each with signal/chromatogram type, polarity, m/z or wavelength, detector,
  and tolerance fields;
- expected qualifier ratio, ratio tolerance, and enabled state.

The operation validates target uniqueness, roles, internal-standard references,
transition completeness, valid calibration option values, RT settings, and
normalization choices. Assignment validation covers the supported sample
types (`blank`, `zero`, `calibrator`, `qc`, `unknown`), required calibrator/QC
nominal concentrations, positive dilution factors, exclusion flags, and the
`analysis × target_id` uniqueness key. Assignment analyses are checked against
`analysesTable`.

The operation publishes typed method and assignment tables. These tables are
the persisted, inspectable plan and are the only configuration source consumed
by `mass_spec.quantify`.

### 1.2 Response extraction and peak assignment

`mass_spec.quantify` consumes:

- `quantificationMethodTable`;
- `quantificationAnalysesTable`;
- `chromatogramPeaksTable`.

It reads chromatogram metadata and peak rows, matches configured transitions
against persisted channel metadata, and selects an eligible peak inside the
configured RT window. The current deterministic selection policy prefers the
strongest eligible apex, then area, then RT/tie-break fields. It does not run a
second peak detector and does not access reader internals.

For every assigned target it records explicit response-extraction status,
candidate count, chromatogram index, peak ID, RT, area, height, and response.
It also carries the quantifier EIC arrays into the unified result table. When a
qualifier is configured and matched, the table carries its EIC arrays,
response, qualifier/quantifier ratio, and qualifier status.

For analytes using internal-standard normalization, the assigned internal
standard is resolved within the same analysis. The result stores the internal
standard response and `response_ratio = response / internal_standard_response`.
Missing, unmatched, zero, and otherwise unusable internal-standard responses
produce explicit statuses rather than a plausible concentration.

### 1.3 Calibration and result calculation

The current fitter executes the declared linear, quadratic, and Wagner model
families:

- calibration observations are analyte rows with `sample_type = calibrator`;
- excluded rows, missing nominal concentrations, failed response extraction,
  missing internal standards, and invalid model-domain responses are retained
  as diagnostics but excluded from the fit;
- linear and quadratic fits support `none`, `1/x`, and `1/x2` weighting plus
  free or forced-zero intercepts;
- Wagner fits an unweighted quadratic in `ln(response)` against `ln(concentration)`
  and its square, requiring strictly positive concentration and response;
- models store coefficients, fit statistics, point count, range, domain status,
  monotonicity status, and fit status;
- included calibration rows receive back-calculated concentration, residual,
  relative residual, and exclusion information;
- sample concentration is obtained by a model-specific safe inverse and the
  dilution factor; range and inverse-root failures are explicit statuses;
- QC accuracy is compared with the configured maximum relative error;
- qualifier failures can be made result-blocking with `require_qualifier`.

Wagner is deliberately explicit and is not selected automatically when a
linear fit is poor. Its diagnostics and residuals are in log-response space,
while reported concentration accuracy remains on the original scale.

The current output intentionally uses one unified `quantificationTable` for
per-analysis observations, calibration diagnostics, and final results. A
separate calibration-points artifact is not emitted. The calibration viewer
derives its raw calibration-point view from calibrator rows in this table.
`quantificationCalibrationModelsTable` remains a separate artifact because it
has one row per fitted target rather than one row per analysis/target
observation.

### 1.4 Current viewer

`QuantifySamplesViewer` currently provides:

- a calibration tab restricted to calibrant analyte rows;
- model curves with included and excluded points;
- a residual plot beside the model plot;
- target filtering and merged target legends;
- click-to-select synchronization between plot points and table rows;
- a results tab restricted to QC, unknown, and blank rows;
- selectable final concentration, response-ratio, or response plots;
- target-specific lower and upper calibration-limit traces;
- QC diamonds, filled unknown circles, and open blank circles;
- quantifier and qualifier EIC sparklines in the raw calibration and result
  tables;
- row highlighting and scrolling when a plotted point is clicked.

The viewer loads sibling result artifacts from the same producer instance and
uses the result-query API rather than relying on a second backend query path.

### 1.5 Project and runtime boundary

All development and validation in this plan uses the current project layout:

```text
<project>.sf/
    project.duckdb
    data/
```

The workflow must be opened and executed through the packaged service in the
worktree under test. Disposable projects, result stores, logs, and build trees
remain under that worktree's `tmp/` directory. Legacy direct `.duckdb` project
paths are not a supported quantification input.

## 2. Current contract inventory

### Inputs

| Contract | Role |
| --- | --- |
| `quantificationMethodTable` | persisted target, transition, normalization, and calibration declarations |
| `quantificationAnalysesTable` | analysis/sample type/concentration/dilution/exclusion context |
| `chromatogramPeaksTable` | persisted peak metrics, channel metadata, and EIC arrays |

### Outputs

| Contract | Role |
| --- | --- |
| `quantificationTable` | unified analyte and internal-standard observations, quantifier/qualifier responses, EIC arrays, calibration diagnostics, concentrations, and statuses |
| `quantificationCalibrationModelsTable` | one fitted model and diagnostic summary per analyte target |
| `operationSuccessSignal` | operation completion signal |
| `quantificationDebugResult` | optional debug log for a selected analysis or the complete run |

Important current columns in `quantificationTable` include:

- identity and assignment: `analysis`, `target_id`, `role`, `sample_type`,
  `calibration_level`, `nominal_concentration`, `dilution_factor`,
  `exclude`;
- selected quantifier peak: `chromatogram_index`, `peak_id`, `peak_rt`,
  `peak_area`, `peak_height`, `eic_size`, `eic_rt`, `eic_intensity`;
- selected qualifier peak data: `qualifier_eic_size`, `qualifier_eic_rt`,
  `qualifier_eic_intensity`, `qualifier_response`, `qualifier_ratio`,
  `qualifier_status`;
- normalization and status: `internal_standard_response`, `response_ratio`,
  `quantifier_status`, `internal_standard_status`, `candidate_count`,
  `status`, `flags`;
- calibration and result values: `included`,
  `back_calculated_concentration`, `residual`, `relative_residual`,
  `exclusion_reason`, `calculated_concentration`, `final_concentration`,
  `accuracy_percent`, and `rt_error`.

The table currently does not expose qualifier peak identity and metrics as
fully as it exposes the quantifier peak. In particular, qualifier peak ID,
chromatogram index, RT, area, and height should be considered a contract gap
even though qualifier EIC arrays are already available.

## 3. What is still missing

### 3.1 Numerical calibration support

The declared calibration choices need either implementation or removal from
the editable method schema. The preferred direction is implementation, in the
following order:

1. weighted linear regression with a free intercept (`none`, `1/x`, `1/x2`);
2. forced-zero linear regression;
3. quadratic regression, with explicit weighting and intercept semantics;
4. Wagner regression for positive-response, positive-concentration curves that
   are better described by a quadratic in log-space;
5. model diagnostics and safe inverse calculation for each supported model.

The current fitter also needs explicit handling for:

- zero-concentration calibrators under inverse-concentration weighting;
- duplicate calibration levels and replicate averaging versus individual-point
  fitting;
- singular or ill-conditioned design matrices;
- negative or non-monotonic fitted response curves;
- quadratic inverse roots, including roots outside the calibration range and
  two valid roots within the range;
- response and concentration units in diagnostics;
- non-finite values and negative responses;
- model-specific minimum point counts and degrees of freedom.

Wagner regression has additional domain constraints. It fits

```text
ln(y) = a2 * ln(x)^2 + a1 * ln(x) + a0
```

where `x` is known concentration and `y` is the measured response or response
ratio. Therefore both `x` and `y` must be strictly positive. Zero-level
calibrators, zero responses, negative responses, and non-finite values cannot
be transformed and must remain visible as excluded diagnostic rows with an
explicit reason. The implementation must not silently add a pseudocount or
replace zero with a small value.

### 3.2 Calibration acceptance and QC policy

The workflow has a basic QC accuracy threshold but does not yet model a full
validation policy. The next contract should define:

- calibrator back-calculated accuracy limits by level, including LLOQ and ULOQ;
- QC acceptance by level and replicate, not only one global percentage;
- blank and zero handling, including allowed blank response and carryover;
- minimum accepted calibrator levels and minimum replicate counts;
- whether excluded calibrators remain visible and how their exclusion is
  justified;
- dilution integrity and re-dilution rules;
- qualifier ratio limits and whether the limits are symmetric;
- missing peak, missing IS, out-of-window RT, and ambiguous-candidate policy;
- an overall run-level pass/fail summary in addition to row-level flags.

These are policy decisions and should remain separate from the numerical model
implementation.

### 3.3 Peak and transition auditability

The current peak selection is deterministic, but the contract should make the
decision more inspectable:

- persist quantifier and qualifier channel identity for the selected peak;
- persist qualifier peak ID, chromatogram index, RT, area, height, and RT error;
- distinguish “no matching channel”, “no peak in RT window”, “multiple
  candidates”, and “selected strongest candidate”;
- expose candidate peak IDs or a compact candidate diagnostic when ambiguity is
  relevant;
- define whether selection should prefer nearest RT or strongest apex for each
  response policy, rather than leaving the choice implicit;
- preserve activation-energy and other transition metadata when it is needed to
  distinguish otherwise identical transitions.

### 3.4 Internal-standard and qualifier completeness

The current implementation calculates analyte/internal-standard ratios and
qualifier ratios, but the plan should still add:

- explicit validation that every normalized analyte has a usable assignment for
  the relevant analysis set;
- separate quantifier and qualifier peak provenance;
- configurable qualifier acceptance ranges per target, rather than only a
  single optional ratio/tolerance pair when more expressive rules are needed;
- clear behavior when the qualifier is absent but `require_qualifier` is false;
- tests proving that internal-standard rows are never themselves calibrated as
  analytes.

### 3.5 Viewer and result inspection

The viewer is useful, but further work should add:

- model-specific fit equations and inverse formulas in the model table;
- weighted residuals, relative residuals, and back-calculated accuracy plots;
- confidence/prediction bands only when their statistical assumptions are
  explicitly documented;
- a compact per-target QC/run summary;
- a detail view for quantifier and qualifier peak identity and full EIC data;
- explicit labels for normalized response versus raw response;
- robust handling of large result sets without loading every EIC array into the
  browser at once;
- export of the selected model, points, and results with provenance.

### 3.6 Test coverage

Current native quantification coverage focuses on configuration validation and
chromatogram peak contracts. There is no dedicated C++ numerical contract for
the implemented linear fit and inverse calculation. The frontend test covers
viewer loading, filtering, EIC columns, plot traces, and row selection, while
the SCIEX scripts provide development-stage data-backed checks.

The remaining test layers are:

- typed-output contract tests for model, calibration diagnostics, qualifier
  provenance, and statuses;
- an end-to-end `.sf` project run comparing model families and expected
  original-scale result behavior;
- viewer tests for all sample shapes, range traces, model options, and large
  table behavior.

## 4. Calibration model expansion

### 4.1 Recommended model policy

For targeted LC-MS/MS, the next useful models are not all equally appropriate:

| Model | Recommendation | Reason |
| --- | --- | --- |
| Unweighted linear | Keep | Current MVP and useful when variance is approximately constant. |
| Weighted linear, `1/x` | Implement next | Commonly improves low-level accuracy when variance grows with concentration. |
| Weighted linear, `1/x2` | Implement next | Common alternative when relative error dominates; must be compared with `1/x`. |
| Forced-zero linear | Implement with safeguards | Appropriate only when zero response is scientifically justified; never use merely to improve R². |
| Quadratic polynomial | Implement after linear options | Useful for validated curvature, but requires stable inverse-root selection and monotonicity checks. |
| Wagner log-quadratic | Implement after linear options, before logistic models | Useful when response/concentration behavior is non-linear over a wide positive range; requires positive data and log-space inverse-root handling. |
| 4-parameter logistic | Defer | More appropriate for sigmoidal immunoassay-style responses than general MRM quantification. |
| 5-parameter logistic | Defer | Adds asymmetry and parameter-identifiability risk; only add for a demonstrated assay need. |
| Spline, power, exponential, or high-order polynomial | Do not add to the generic MVP | Flexible fits can interpolate calibration points while producing unsafe inverses and poor extrapolation. |

The model should remain explicitly selected per target in the plan. Automatic
selection based only on R² is not acceptable. If automatic comparison is added
later, it must use a declared acceptance criterion such as weighted
back-calculated bias across levels, residual structure, monotonicity, and
out-of-sample or leave-one-level-out performance.

### 4.2 Numerical design

Introduce one internal calibration interface with model-independent operations:

```text
fit(points, method_options) -> fitted model + diagnostics
predict(model, concentration) -> response
inverse(model, response) -> concentration + inverse status
diagnostics(model, points) -> residuals, bias, accuracy, range, condition data
```

The implementation should use a stable small-matrix solver rather than direct
normal-equation inversion where practical. Each model result should carry:

- model family, weighting, and intercept mode actually used;
- fitted parameters and coefficient units;
- valid calibration range and monotonic branch;
- point count, distinct-level count, degrees of freedom, and fit status;
- residual, bias, RMSE, and weighted diagnostics;
- inverse status for each quantified row.

For weighted linear regression:

- define weights as a function of nominal concentration;
- define the zero-concentration rule before implementation;
- retain zero-level observations for diagnostics even if they cannot receive an
  inverse-concentration weight;
- report the effective point count and weighting policy.

For forced-zero linear regression:

- fit only the slope;
- reject or warn when the measured zero response contradicts the assumption;
- report that the intercept was constrained, not estimated.

For quadratic regression:

- fit `response = c0 + c1*x + c2*x²` with the selected weighting and intercept
  mode;
- check that the fitted curve is monotonic over the declared calibration
  range;
- invert using all real roots, choose the root on the valid monotonic branch,
  and emit `ambiguous_inverse` or `out_of_range` when no unique safe root
  exists;
- never silently extrapolate beyond the validated range.

For Wagner regression:

- fit the transformed response `ln(y)` against `ln(x)` and
  `ln(x)^2`, retaining the coefficients as `a0`, `a1`, and `a2` with an
  explicit `model = "wagner"` identifier;
- report that residuals and fit statistics are in log-response space, while
  back-calculated accuracy remains on the original concentration scale;
- invert a measured response by solving
  `a2*z² + a1*z + a0 - ln(y) = 0` for `z = ln(x)`;
- choose the unique real root on the validated log-concentration branch;
  otherwise emit `ambiguous_inverse`, `no_real_inverse`, or `out_of_range`;
- treat a near-zero `a2` as a numerically degenerate Wagner fit and either
  reduce it explicitly to a log-linear model or fail with a diagnostic, rather
  than relying on an unstable quadratic formula;
- check monotonicity of the predicted response over the validated positive
  concentration range before allowing inverse quantification;
- do not apply the ordinary `forced_zero` meaning automatically. Forcing
  `a0 = 0` in log-space means `y = 1` at `x = 1` in the chosen units, which is
  not equivalent to forcing the original response through zero. Wagner
  forced-intercept behavior must therefore be rejected until a separate,
  scientifically defined option is added.

### 4.3 Wagner-specific contract decisions

Wagner should not be represented as an ordinary quadratic with the same
interpretation as `coefficient_0`, `coefficient_1`, and `coefficient_2`. The
coefficients are fitted in transformed coordinates and the output must make
that explicit. At minimum the model output needs:

- `model = "wagner"`;
- a transform declaration such as `x_transform = "ln"` and
  `y_transform = "ln"`;
- coefficient labels or documentation identifying `a0`, `a1`, and `a2`;
- the original concentration and response ranges used for validation;
- a log-domain fit status and inverse status;
- diagnostics that distinguish log-space residuals from original-scale
  concentration bias.

The method schema should expose Wagner only after its domain validation is
implemented. The initial option should be explicit, for example
`regression_model = "wagner"`, rather than silently selecting it when a
linear fit has a low R². Wagner model selection must be based on declared
calibration acceptance criteria, not R² alone.

Weighting also needs a deliberate definition. The first implementation should
use unweighted least squares in log-space. If `1/x` or `1/x2` is later allowed,
the plan must state whether weights are calculated from original concentration
and applied to log-response residuals, and must test that choice independently.

### 4.4 Contract changes required for model expansion

The existing coefficient columns are sufficient for linear and quadratic
models. Before adding logistic models, decide whether to extend the typed model
table with more coefficient columns or a typed parameter vector. Do not hide
model parameters in an opaque JSON string while the rest of the catalogue is
typed.

Add model-specific fields only when they have a defined interpretation. Likely
near-term additions are:

- `distinct_levels`, `degrees_of_freedom`, `condition_number`;
- `weighted_rmse`, `max_relative_bias`, and `monotonicity_status`;
- `inverse_status` and `inverse_root_count` on result rows;
- `x_transform`, `y_transform`, `inverse_status`, and `inverse_root_count` for
  transformed models such as Wagner;
- explicit `calibration_lower_limit` and `calibration_upper_limit` if the
  limits differ from the observed point range.

## 5. Implementation sequence

### Phase A — Freeze and test the current linear behavior

1. Extract the fitting and inverse logic into a testable native helper within
   the mass-spec plugin's owned source tree.
2. Add C++ numerical contract tests for free-intercept unweighted linear fit,
   insufficient points, singular x values, negative slope, zero response,
   out-of-range results, dilution, and QC accuracy.
3. Add typed-output assertions for the unified quantification table and model
   table, including qualifier EIC/provenance fields.
4. Run the packaged SCIEX fixture and record expected model coefficients and
   selected status counts in a development check.

Exit criteria: the current linear implementation is independently verifiable
without depending on a large instrument fixture.

### Phase B — Implement weighted and forced-zero linear models

1. Implement weighted least squares for `none`, `1/x`, and `1/x2`.
2. Define and test zero-concentration weighting behavior.
3. Implement `forced_zero` and its diagnostics.
4. Preserve the requested model options in the model output and reject any
   unsupported combination explicitly.
5. Add numerical tests for exact synthetic data, noisy data, zero levels,
   duplicate levels, and ill-conditioned inputs.

Exit criteria: every linear option accepted by validation produces the declared
fit, diagnostics, and safe inverse behavior.

### Phase C — Implement quadratic calibration

1. Add a stable quadratic solver and model diagnostics.
2. Implement free and forced-zero intercept semantics only if both are
   scientifically justified and numerically tested.
3. Define monotonicity and inverse-root selection over the fitted range.
4. Add tests for a known quadratic, near-linear quadratic, two-root inverse,
   non-monotonic fit, and out-of-range response.
5. Update the viewer to label the actual model and show the fitted curve using
   the same prediction function as the backend.

Exit criteria: quadratic results cannot silently select an unsafe root or
produce an extrapolated concentration.

### Phase D — Implement Wagner regression

1. Add `wagner` to method validation and catalogue documentation only after
   the transformed-domain contract is agreed.
2. Implement a stable log-quadratic solver for positive concentration and
   response points, with explicit exclusion reasons for invalid inputs.
3. Implement original-scale inverse calculation, root selection, monotonicity
   checks, and the `no_real_inverse`, `ambiguous_inverse`, and `out_of_range`
   statuses.
4. Add synthetic tests for an exact Wagner curve, noisy positive data, a
   near-zero quadratic coefficient, two inverse roots, no real root, invalid
   zero/negative inputs, and a non-monotonic fitted branch.
5. Add a data-backed fixture where ordinary linear fitting fails across a wide
   positive concentration range and compare original-scale back-calculated
   bias against linear, weighted linear, ordinary quadratic, and Wagner fits.
6. Update the calibration viewer to label log-transformed axes or clearly
   state the displayed scale, show the original-scale curve used for
   quantification, and distinguish log-space residuals from concentration
   accuracy.

Exit criteria: Wagner is selected explicitly, cannot consume invalid log-domain
data silently, and produces a unique, validated concentration only when its
inverse is numerically and scientifically safe.

### Phase E — Complete auditability and QC policy

1. Add qualifier peak identity/metrics and explicit channel provenance.
2. Define per-target and per-level calibration/QC acceptance settings.
3. Add blank, zero, carryover, dilution-integrity, and replicate policies.
4. Publish run-level summary diagnostics while retaining row-level flags.
5. Extend the viewer with fit diagnostics, per-target summaries, and a detail
   view for both quantifier and qualifier EIC traces.

Exit criteria: a user can explain every accepted, excluded, or failed result
from the method row, assignment row, selected peak(s), calibration model, and
policy decision.

### Phase F — Evaluate additional model families only when justified

1. Gather an assay-specific requirement for a sigmoidal model before adding
   4PL or 5PL; Wagner should be evaluated first for positive wide-range
   non-linearity.
2. Define parameter constraints, initialization, convergence status, inverse
   behavior, and uncertainty diagnostics before adding the model to the schema.
3. Compare the model against weighted linear/quadratic fits using predefined
   validation criteria, not visual fit or R² alone.
4. Add the model only with synthetic numerical tests and a representative
   data-backed fixture.

## 6. Phase A implementation checkpoint — 2026-11-19

Phase A is now implemented at the native quantification boundary:

- `src/utils/calibration.hpp/.cpp` owns the finite-point filtering, current
  ordinary least-squares linear fit, inverse calculation, range status, and
  invalid-response handling.
- `operations.cpp` uses that helper for both calibration-model fitting and
  sample back-calculation; the existing result fields and status strings remain
  unchanged.

The Phase A contract covered exact fitting, range checks, insufficient and
singular data, negative slopes, invalid responses, and non-finite input points.
The focused calibration, quantification-validation, and chromatogram-peak
contracts passed, and the SCIEX Mix1 development fixture passed through the
isolated packaged-runtime harness on port 8788 with 113 analyses, 226
assignment rows, and 168 quantification rows.

## 7. Phase B implementation checkpoint — 2026-11-19

The first linear model expansion is now implemented in the same native helper:

- weighted least squares supports the declared `none`, `1/x`, and `1/x2`
  policies;
- free-intercept and `forced_zero` fits use the selected weighting policy and
  report weighted fit statistics;
- zero and non-positive concentrations are retained in the diagnostic range,
  but are excluded from inverse-concentration weighted fits with an explicit
  calibration-point exclusion reason;
- unsupported weighting and intercept values return explicit fit statuses;
- unsupported combinations are rejected explicitly instead of silently
  executing a different model;
- numerical contract coverage now includes both inverse-concentration
  weighting policies, forced-zero fitting, zero-level handling, and explicit
  unsupported-option rejection;
- the refreshed packaged build completed the full native/package suite with
  45/45 tests passing, and its isolated service is running on port 8788.

Phase C is the completed next slice. Wagner remains the following slice and
must stay an explicit positive-domain log-quadratic model rather than being
treated as an ordinary quadratic.

## 8. Phase C implementation checkpoint — 2026-11-19

Phase C is now implemented at the native quantification boundary and in the
calibration viewer:

- stable QR-style weighted fitting supports free-intercept and forced-zero
  quadratic models using the declared `none`, `1/x`, and `1/x2` policies;
- quadratic model output publishes the second-order coefficient and an
  explicit monotonicity status;
- fitted quadratic curves use the same prediction helper as sample
  back-calculation, while inverse roots are selected only within the fitted
  calibration range;
- ambiguous roots, no-real-root responses, non-monotonic fits, and
  out-of-range responses receive explicit statuses rather than silently
  producing extrapolated final concentrations;
- numerical contract coverage includes exact and forced-zero quadratics,
  near-linear behavior, ambiguous roots, non-monotonic fits, and range
  failures;
- the viewer labels the actual model family and the refreshed packaged build
  completed the full native/package suite with 45/45 tests passing.

The isolated packaged service was restarted from this worktree's
`tmp/packaged_build` and is healthy on port 8788. The next implementation
slice is Phase D: Wagner's positive-domain log-quadratic model.

## 9. Phase D implementation checkpoint — 2026-11-19

Phase D is now implemented and verified through the packaged runtime:

- method validation and catalogue documentation expose `wagner` explicitly;
- the native helper fits `ln(y) = a2*ln(x)^2 + a1*ln(x) + a0` on positive
  concentration/response points and reports the transformed-domain status;
- Wagner uses unweighted log-space least squares only; unsupported weighting
  is reported explicitly;
- inverse concentration solves the quadratic in `ln(x)`, checks the validated
  positive range, and reports `ambiguous_inverse`, `no_real_inverse`, or
  range statuses instead of unsafe extrapolation;
- calibration residuals are log-response residuals for Wagner, while sample
  concentrations and accuracy remain on the original concentration scale;
- native synthetic coverage includes exact, forced-intercept, unsupported
  weighting, invalid-domain, ambiguous-root, and no-real-root cases;
- the viewer uses Wagner's prediction function and log-spaced curve grid and
  labels log-response residuals;
- an initial packaged Mix1 run using explicit Wagner methods for both analytes
  produced two `model = wagner`, `fit_status = ok`, `domain_status =
  positive_domain`, increasing calibration models. The resulting coefficients
  were:

  | target | a0 | a1 | a2 | R² | RMSE | points |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: |
  | diclofenac | -0.9302477420 | 0.9988257651 | 0.0102337592 | 0.9952389477 | 0.1459949695 | 37 |
  | sulfamethoxazol | 0.5190621225 | 0.9927003874 | 0.0246244111 | 0.9970603772 | 0.1122126450 | 38 |

During this verification, a packaged-runtime defect was found and fixed:
`mass_spec.quantify`'s method-table projection omitted `regression_model`,
`weighting`, `intercept_mode`, and `concentration_unit`, causing every fit to
fall back to linear defaults even when the persisted method table said Wagner.
The clean rerun after that fix completed successfully.

The current mixed-model Mix1 configuration is now verified through the same
packaged runtime:

- diclofenac uses `wagner`;
- sulfamethoxazol uses `linear`;
- diclofenac-D4 and sulfamethoxazol-D4 are normalization references, not
  calibration targets, so their `regression_model`, `weighting`, and
  `intercept_mode` are canonicalized to null in `quantificationMethodTable`;
- the fresh workflow completed all five operations and published both models
  with `fit_status = ok`:

  | target | model | R² | RMSE | points | domain status |
  | --- | --- | ---: | ---: | ---: | --- |
  | diclofenac | wagner | 0.9952389477 | 0.1459949695 | 37 | positive_domain |
  | sulfamethoxazol | linear | 0.9980049692 | 0.4195074524 | 38 | not_applicable |

Internal-standard responses still participate in analyte normalization. They
do not have their own calibration regression, so displaying `linear` for an
internal-standard row would be misleading; null is the persisted semantic
value and the viewer may render it as “none” or “not applicable”.

## 10. Unified analyte-row and peak-provenance checkpoint — 2026-11-19

The published `quantificationTable` now uses one row per analyte response. The
assigned internal-standard response is carried on that analyte row instead of
being published as a separate internal-standard row:

- `internal_standard_id`, `internal_standard_response`, and
  `internal_standard_status` identify the normalization reference and its
  assignment state;
- `internal_standard_eic_size`, `internal_standard_eic_rt`, and
  `internal_standard_eic_intensity` carry the assigned reference peak trace;
- quantifier and qualifier peak traces remain available through the existing
  `eic_*` and `qualifier_eic_*` columns;
- internal-standard rows are removed from the published quantification result
  after normalization, while the method and assignment artifacts retain the
  configuration/provenance needed for auditability.

The packaged Mix1 rerun on the isolated service completed all five operations.
It published 156 analyte rows, zero internal-standard rows, and 152 rows with
an assigned internal-standard EIC. The viewer requests and renders the
quantifier, qualifier, and internal-standard EIC traces as compact sparklines
in both calibration and QC/unknown/blank tables. The targeted viewer test and
the full native/package suite remain green (1/1 viewer test and 45/45 native
tests).

## 11. Validation and repository workflow

All work remains scoped to this worktree and uses its own `.sf` fixtures and
`tmp/` outputs.

Before implementation changes:

```powershell
pwsh -NoProfile -File scripts/sfdev.ps1 env show
pwsh -NoProfile -File scripts/sfdev.ps1 doctor --workflow native
```

For ontology or contract changes, regenerate the aggregate catalogue using the
repository-supported native build. For native quantification changes, build
the relevant mass-spec targets and run the focused CTest contracts, including:

- `streamfind_mass_spec_quantification_validation_contract`;
- `streamfind_mass_spec_chromatogram_peak_contract`;
- the project-isolation contract when project/runtime boundaries change.

For frontend viewer changes, use the repository frontend checks and the
`QuantifySamplesViewer` test. Validate the UI only through the packaged app;
do not start a Vite development server or attach this worktree to another
checkout's service.

For data-backed validation, use the development SCIEX fixture scripts through
the supported `sfdev` script interface. Start the packaged service on a unique
worktree port when needed, confirm the executable path belongs to this
worktree, and stop only that service after the run.

## 12. Decisions to make before the next implementation slice

The recommended immediate slice is **Phase E**. It completes peak provenance
and the calibration/QC acceptance policy now that the primary model families
and their safe inverse behavior are implemented.

The decisions that should be recorded with that work are:

1. Are calibration replicates fit as individual observations or averaged by
   level, and how are replicate acceptance limits applied?
2. Should zero-level calibrators participate in weighted fits, and what weight
   is assigned when `x = 0`?
3. Should forced-zero be available to users or restricted to validated methods?
4. Are calibration limits the observed fitted-point range, explicit method
   limits, or both?
5. Should a result outside the calibration range be reported with its numeric
   extrapolation for audit purposes, or should final concentration be withheld
   while retaining only a calculated diagnostic value? **Decision:** retain the
   numeric extrapolation as a diagnostic value while marking the row
   `above_calibration_range` or `below_calibration_range`; it remains outside
   the validated reportable range and must not be treated as an in-range result.
6. Is one qualifier sufficient for the target contract, or should the method
   support multiple qualifier transitions with independent ratios?
7. Is a 4PL/5PL model genuinely required for a supported detector/assay, or
   should LC-MS quantification remain limited to linear and quadratic families?
8. Should Wagner initially be unweighted in log-space only, with transformed
   residuals reported separately from original-scale accuracy?
9. Which calibration acceptance limits define a valid Wagner branch, and how
   should a root be selected when both quadratic roots fall within the nominal
   concentration range?

Until these decisions are resolved, do not add automatic model selection,
high-order polynomials, splines, or logistic models to the generic method
editor. Do not treat Wagner as an implicit fallback for a poor linear fit; it
must be an explicit, validated method choice.

## 13. Phase E implementation checkpoint — 2026-11-19

The first auditability slice is now implemented in the quantification contract:

- selected quantifier rows retain `quantifier_chromatogram_id`;
- selected qualifier rows retain chromatogram ID and index, peak ID, retention
  time, area, height, RT error, and the number of eligible candidates;
- qualifier channel selection now reports `channel_mismatch` when no matching
  channel exists, separately from `peak_not_found` when a matching channel has
  no peak inside the configured retention-time window;
- semantic column definitions and the `quantificationTable` artifact list were
  extended for these provenance fields;
- the SCIEX development fixture now checks that the provenance columns are
  present in the published result contract.

The focused native quantification, calibration, and chromatogram-peak
contracts pass after this change. A first packaged rerun exposed and fixed a
semantic/native column-order mismatch at publication; the corrected package
then completed all five Mix1 operations and passed the full 45-test suite.
The newest published Mix1 result contains the selected quantifier and
qualifier chromatogram IDs and qualifier peak provenance for 156 rows. The
next Phase E slice is replicate acceptance, explicit calibration limits, and
dilution/SPE audit summaries.
