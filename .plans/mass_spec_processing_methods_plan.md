Yes. I would make this a dedicated development plan for the `mass_spec` plugin, with **Phase 1 explicitly repairing the current chromatogram contract before any quantification functionality is introduced**.

The plan below is designed to be copied directly into something like `.plans/mass_spec_chromatogram_quantification_plan.md`.

# StreamFind MassSpec Chromatogram Processing and Targeted Quantification Plan

**Target branch:** `dev_refactoring`
**Primary component:** `cpp/plugins/mass_spec`
**Scope:** Generic chromatogram processing, chromatographic peak detection/integration, MRM-targeted quantification, calibration, internal standards, QC, and detector-independent extension to DAD/UV chromatograms.

---

# 1. Purpose

This plan extends the current StreamFind `mass_spec` plugin with a generic chromatographic processing and targeted quantification framework.

The initial use case is an MRM workflow comparable in analytical structure to quantitative workflows available in software such as SCIEX MultiQuant and equivalent vendor tools:

```text
MRM chromatograms
        ↓
chromatographic peak integration
        ↓
target / transition assignment
        ↓
internal-standard normalization
        ↓
calibration curve fitting
        ↓
unknown-sample quantification
        ↓
quality-control evaluation
```

The architecture must, however, **not be MRM-specific**.

The same processing framework must support other chromatographic detector signals such as:

```text
DAD
UV
ADC
TIC
EIC/XIC
future chromatographic detector types
```

The central architectural principle is:

> A chromatographic peak is independent of the detector that generated the signal. MRM transition metadata, DAD wavelength metadata, detector identity, and similar attributes describe the chromatographic channel rather than the peak-integration algorithm.

This allows StreamFind to implement one chromatographic processing framework and specialize only the signal/channel matching layer.

---

# 2. Existing architecture and rationale

The current MassSpec plugin already contains separate domain modules including:

```text
baseModule
chromatogramsModule
ntaModule
```

and exposes Methods and Operations through the MassSpec plugin capability registry.

The reader abstraction is already sufficiently generic for this extension.

`MASS_SPEC_CHROMATOGRAMS_HEADERS` currently exposes information including:

```text
index
chromatogram_id
array_length

polarity
precursor_mz
activation_ce
product_mz

signal_type
chromatogram_type
detector
channel
units
wavelength_nm

interval_ms
start_time
end_time
intensity_multiplier
```

This means MRM and DAD/UV channels can already be described through the same reader interface.

The current persistence and processing layer does not yet preserve all of this information consistently, which must be repaired first.

---

# 3. Target MassSpec plugin organization

The MassSpec plugin should evolve conceptually toward:

```text
MassSpec plugin
│
├── baseModule
│
├── chromatogramsModule
│   ├── chromatogram loading
│   ├── chromatogram preprocessing
│   ├── baseline correction
│   ├── smoothing
│   └── chromatographic peak detection/integration
│
├── quantificationModule
│   ├── targets
│   ├── analytical channels
│   ├── internal standards
│   ├── calibration
│   ├── sample quantification
│   └── quantitative QC
│
└── ntaModule
    └── existing non-target workflows
```

The new functionality remains within the existing `mass_spec` plugin.

Do **not** create a separate MRM plugin.

The new semantic module should be:

```text
sfms:quantificationModule
```

with a label similar to:

```text
MassSpec targeted quantification module
```

---

# 4. Architectural separation

The implementation should maintain four separate concepts.

## 4.1 Raw chromatographic signal

Stored as points:

```text
retention time → intensity
```

with channel identity obtained from the chromatogram header.

---

## 4.2 Chromatographic peak

A detector-independent analytical feature containing:

```text
apex
integration boundaries
area
height
width
signal/noise
shape metrics
```

A chromatographic peak does not inherently know whether it originated from:

```text
MRM
DAD
UV
EIC
```

---

## 4.3 Quantitative target

Describes what compound is expected and how it should be quantified.

Examples:

```text
Caffeine
Diclofenac
Caffeine-d9 internal standard
```

---

## 4.4 Quantitative channel

Describes which analytical signal identifies or quantifies a target.

Examples:

```text
MRM:
195.1 → 138.1

DAD:
254 nm
```

This separation is critical for keeping the implementation generic.

---

# PHASE 1 — Correct and normalize the chromatogram persistence contract

## Goal

Repair the current mismatch between the chromatogram reader/header model and the persisted `MASS_SPEC_CHROMATOGRAMS` representation before introducing any new chromatographic processing methods.

This phase is a prerequisite for every later phase.

---

## 1.1 Current mismatch

The reader header model contains detector-independent metadata such as:

```text
signal_type
chromatogram_type
detector
channel
units
wavelength_nm
interval_ms
start_time
end_time
intensity_multiplier
```

as well as MRM-specific metadata:

```text
polarity
precursor_mz
activation_ce
product_mz
```

The semantic contract for `MASS_SPEC_CHROMATOGRAMS_HEADERS` already represents these fields.

However, the current `MASS_SPEC_CHROMATOGRAMS` processing method only persists:

```text
analysis
index
chromatogram_id
polarity
precursor_mz
activation_ce
product_mz
rt
raw_intensity
baseline
intensity
```

Meanwhile `mass_spec.get_chromatograms` already attempts to expose `wavelength_nm`, creating a mismatch between reader metadata, semantic table definitions, persistence, and query behavior.

---

## 1.2 Make chromatogram headers authoritative

Use:

```text
MASS_SPEC_CHROMATOGRAMS_HEADERS
```

as the canonical description of a chromatographic signal.

The table should contain one row per chromatographic channel:

```text
analysis
index

chromatogram_id
array_length

signal_type
chromatogram_type

polarity
precursor_mz
product_mz
activation_ce

detector
channel
units
wavelength_nm

interval_ms
start_time
end_time
intensity_multiplier
```

The logical chromatogram key should be:

```text
analysis + index
```

rather than `chromatogram_id` alone.

`chromatogram_id` remains an externally useful identifier but should not be assumed to be globally unique.

---

## 1.3 Normalize the chromatogram point table

Reduce `MASS_SPEC_CHROMATOGRAMS` to data that belongs to each actual signal point.

Recommended logical contract:

```text
MASS_SPEC_CHROMATOGRAMS

analysis
index
rt
raw_intensity
baseline
intensity
created_at
```

Optional:

```text
point_index
```

if deterministic point ordering needs to be preserved independently from RT.

Detector/channel metadata should generally **not** be repeated for every point.

Queries requiring metadata should join:

```text
MASS_SPEC_CHROMATOGRAMS
        +
MASS_SPEC_CHROMATOGRAMS_HEADERS
```

through:

```text
analysis
index
```

---

## 1.4 Update chromatogram loading

Refactor:

```text
mass_spec.load_chromatograms
```

so it:

1. resolves the selected chromatogram headers;
2. loads corresponding arrays;
3. persists signal points only;
4. uses `analysis + index` as channel identity;
5. preserves raw intensity;
6. initializes:

```text
baseline = 0
intensity = raw_intensity
```

unless preprocessing is applied.

---

## 1.5 Update existing chromatogram queries

Update:

```text
mass_spec.get_chromatograms
```

to join the point and header tables.

The operation can still return a flattened frontend-friendly result:

```text
analysis
index
chromatogram_id
signal_type
chromatogram_type

polarity
precursor_mz
product_mz
activation_ce

detector
channel
wavelength_nm
units

rt
raw_intensity
baseline
intensity
```

without duplicating those metadata physically in DuckDB.

---

## 1.6 Update retention-time filtering

The current:

```text
mass_spec.filter_chromatograms_retention_time
```

may remain a Method but should operate only on point data.

It should not rewrite or modify chromatogram header records.

---

## 1.7 Semantic changes

Update:

```text
cpp/plugins/mass_spec/semantic/tables.ttl
cpp/plugins/mass_spec/semantic/columns.ttl
cpp/plugins/mass_spec/semantic/methods.ttl
cpp/plugins/mass_spec/semantic/operations.ttl
```

so the semantic contract accurately reflects the physical schema.

Turtle remains authoritative.

---

## 1.8 Tests

Test at minimum:

### MRM

Header:

```text
signal_type = MS
chromatogram_type = MRM
precursor_mz != NULL
product_mz != NULL
```

### DAD

Header:

```text
signal_type = UV
detector = DAD
wavelength_nm != NULL
```

### Generic

Ensure:

```text
header metadata
+
point arrays
```

round-trip correctly through DuckDB.

---

## Phase 1 acceptance criteria

* Reader, semantic catalogue, DuckDB schema and Operations expose the same chromatogram model.
* `analysis + index` identifies a chromatographic channel.
* DAD metadata survives import and persistence.
* MRM metadata survives import and persistence.
* `MASS_SPEC_CHROMATOGRAMS` no longer unnecessarily duplicates channel metadata.
* `get_chromatograms` reconstructs the complete signal through joins.
* Existing chromatogram tests continue to pass.

---

# PHASE 2 — Generic chromatographic peak table

## Goal

Introduce detector-independent chromatographic peaks as first-class persistent StreamFind results.

---

## 2.1 New table

Create:

```text
MASS_SPEC_CHROMATOGRAM_PEAKS
```

This table must not be called:

```text
MRM_PEAKS
```

because it must also support DAD/UV and other chromatographic signal types.

---

## 2.2 Recommended schema

```text
analysis
chromatogram_index
peak_id

rt
rt_start
rt_end

height
raw_height

area
raw_area
baseline_area

width
fwhm

snr
asymmetry
sharpness
plates

number_points

integration_algorithm
integration_status
manual_override

created_at
```

Optional later metrics:

```text
tailing_factor
peak_capacity
gaussian_r2
noise
modality
```

---

## 2.3 Peak key

Recommended logical identity:

```text
analysis
chromatogram_index
peak_id
```

A generated globally unique peak identifier may additionally be used if useful.

---

## 2.4 Relationship

```text
MASS_SPEC_CHROMATOGRAMS_HEADERS
           │
           │ 1
           │
           │ N
           ▼
MASS_SPEC_CHROMATOGRAM_PEAKS
```

via:

```text
analysis
chromatogram_index = index
```

---

## Phase 2 acceptance criteria

* A persistent detector-independent chromatographic peak table exists.
* Peaks can be joined back to their signal metadata.
* MRM and DAD peaks use exactly the same schema.
* No quantification-specific fields are added to the peak table.

---

# PHASE 3 — Native chromatographic peak detection and integration

## Goal

Create a proper MassSpec Method for detecting and integrating chromatographic peaks.

The legacy StreamFind implementation already included an `IntegrateChromatograms` algorithm with configurable minimum height, peak distance, peak width, S/N, and merging behavior.

Useful algorithms should be ported/refactored into the new C++ plugin implementation rather than recreated blindly.

---

## 3.1 Method

Introduce:

```text
mass_spec.find_chromatogram_peaks
```

Provided by:

```text
sfms:chromatogramsModule
```

Reads:

```text
MASS_SPEC_CHROMATOGRAMS
MASS_SPEC_CHROMATOGRAMS_HEADERS
```

Writes:

```text
MASS_SPEC_CHROMATOGRAM_PEAKS
```

---

## 3.2 Initial parameters

```text
analysis_names

chromatogram_ids / indices
signal_type
chromatogram_type

rt_min
rt_max

min_peak_height
min_peak_distance

min_peak_width
max_peak_width

min_snr

merge
merge_distance
```

Later:

```text
integration_algorithm
baseline_algorithm
peak_shape_algorithm
```

---

## 3.3 Algorithm architecture

Separate reusable algorithm code from the workflow Method.

Suggested structure:

```text
src/utils/chromatogram_peaks.hpp
src/utils/chromatogram_peaks.cpp
```

with pure analytical functions such as:

```text
find_peak_candidates()
determine_peak_boundaries()
integrate_peak()
calculate_peak_metrics()
merge_peaks()
```

The workflow Method should mainly:

```text
read tables
→ group chromatograms
→ call algorithms
→ persist peaks
```

---

## 3.4 Chromatogram grouping

Input point rows should be grouped by:

```text
analysis
index
```

before processing.

---

## 3.5 Determinism

Given identical:

```text
input table
parameters
algorithm version
```

the Method must produce identical peak records.

---

## 3.6 Provenance

Store algorithm identity such as:

```text
streamfind.integration.v1
```

rather than only:

```text
native
```

This will make future integration algorithms distinguishable.

---

## Phase 3 acceptance criteria

* Real chromatograms can be processed.
* Peaks are persisted.
* MRM and DAD signals use the same integration algorithm.
* Algorithm code is reusable independently from Project access.
* Peak output is deterministic.
* Existing legacy integration behavior can be benchmarked against the new implementation.

---

# PHASE 4 — Chromatogram preprocessing Methods

## Goal

Support the preprocessing needed for robust quantitation without coupling preprocessing to quantification.

---

## 4.1 Baseline correction

Introduce:

```text
mass_spec.correct_chromatogram_baseline
```

Reads/writes:

```text
MASS_SPEC_CHROMATOGRAMS
```

Output semantics:

```text
raw_intensity = original data
baseline = calculated baseline
intensity = raw_intensity - baseline
```

---

## 4.2 Initial algorithms

Support at least:

```text
none
rolling minimum / moving window
ALS
```

if the existing algorithm infrastructure allows it.

---

## 4.3 Smoothing

Introduce:

```text
mass_spec.smooth_chromatograms
```

Possible algorithms:

```text
moving average
Savitzky-Golay
```

The exact persistence strategy must be decided explicitly.

Preferred initial approach:

```text
raw_intensity
baseline
intensity
```

where `intensity` represents the current processed signal used downstream.

Do not create an uncontrolled number of transformed signal columns.

---

## 4.4 Ordering

Typical workflow:

```text
load
↓
filter RT
↓
baseline correction
↓
smoothing
↓
peak integration
```

Semantic read/write dependencies should enforce this through table contracts rather than hard-coded UI ordering.

---

## Phase 4 acceptance criteria

* Baseline correction is independently runnable.
* Smoothing is independently runnable.
* Peak detection consumes processed intensity.
* Raw intensity remains preserved.
* Preprocessing works independently of target quantification.

## Phase 4 completion status

**Completed.**

### Implemented Methods

| Method | Algorithms | Key Parameters |
|--------|-----------|----------------|
|  | none, rolling_min, als, moving_average | window_size, lambda, asymmetry_penalty, max_iterations |
|  | none, moving_average, savitzky_golay | window_size, poly_order |

### Data model



 reads from  (processed signal), not .

### Algorithms implemented

* **rolling_min**: sliding-window minimum. Fast, robust for well-separated peaks. Window = half-width in data points.
* **als**: asymmetric least-squares (Boels & Eilers 2005). First-difference D'D penalty with Thomas solver. Lambda auto-scaled by signal range². Better for drifting baselines.
* **moving_average**: sliding-window arithmetic mean. Follows broad trends well. Best for smooth gradient elution profiles.
* **savitzky_golay**: polynomial local regression. Preserves peak shape/height. Configurable window and polynomial order.
* **moving_average (smoothing)**: simple filter but broadens peaks.

### Bugs fixed during Phase 4

1. **ALS D'D matrix**: incorrect second-difference coefficients  replaced with correct first-difference penalty .
2. **ALS lambda scaling**: auto-scaled by signal range² so user-supplied value works across different signal magnitudes.
3. **RT float precision**: string-based RT matching failed due to  precision artifacts ( vs ). Fixed with RT-tolerance matching.
4. **Parameter name mismatch**: TTL parameter names (, , ) must match C++  calls.
5. **Smoothing double-subtraction**: smoothing method was re-subtracting baseline from already-corrected intensity. Fixed to smooth the current intensity directly.
6. **find_chromatogram_peaks read raw_intensity**: changed to read  so peak detection uses processed signal.

### Semantic registration

* 7 new parameters in  (baselineAlgorithm, smoothingAlgorithm, windowSize, polyOrder, lambda, asymmetryPenalty, maxIterations)
* 2 new methods in  with rich descriptions
* All parameters optional with sensible defaults
* C++ entry point registered in 

### Verified with

* Agilent ChemStation .D (DAD): 108 channels, 22,680 points, baseline correction + smoothing + peak detection pipeline
* All 11 C++ unit tests pass

---

# PHASE 5 — Add the Targeted Quantification domain module

## Goal

Introduce the semantic and implementation boundary for quantitative assays.

---

## 5.1 Domain module

Add:

```text
sfms:quantificationModule
```

Definition:

```text
Targeted chromatographic quantification using analytical targets,
detector channels, calibration standards and internal standards.
```

---

## 5.2 C++ implementation structure

Add:

```text
cpp/plugins/mass_spec/src/methods/
    quantification_processing_methods.hpp
    quantification_processing_methods.cpp

cpp/plugins/mass_spec/src/operations/
    quantification.cpp

cpp/plugins/mass_spec/src/utils/
    calibration.hpp
    calibration.cpp

    quantification.hpp
    quantification.cpp
```

---

## 5.3 Plugin registration

Register new capabilities in:

```text
plugin_entrypoint.cpp
```

using the existing `CapabilityRegistry` pattern.

No special dispatch path should be introduced for quantification.

---

## Phase 5 acceptance criteria

* Quantification appears as a normal MassSpec module.
* Capabilities are discoverable through semantic catalogue + executable registry.
* No special frontend or MCP method inventory is required.

---

# PHASE 6 — Quantitative target and channel model

## Goal

Represent analytes, internal standards and detector channels independently from raw chromatograms.

---

# 6.1 Table: `MASS_SPEC_QUANT_TARGETS`

Recommended fields:

```text
target_id
name

role
internal_standard_id

expected_rt
rt_tolerance

response_metric
normalization

concentration_unit

regression_model
weighting
intercept_mode

enabled

created_at
```

`role`:

```text
analyte
internal_standard
```

`response_metric`:

```text
area
height
```

`normalization`:

```text
none
internal_standard
```

---

# 6.2 Table: `MASS_SPEC_QUANT_CHANNELS`

Recommended fields:

```text
channel_id
target_id

channel_role

signal_type
chromatogram_type

polarity
precursor_mz
product_mz
activation_ce

detector
detector_channel
wavelength_nm

expected_ratio
ratio_tolerance

enabled
created_at
```

`channel_role`:

```text
quantifier
qualifier
```

---

## 6.3 MRM representation

Example:

```text
target_id              caffeine
channel_role           quantifier

signal_type            MS
chromatogram_type      MRM

precursor_mz           195.1
product_mz             138.1
polarity               positive
```

---

## 6.4 DAD representation

Example:

```text
target_id              caffeine
channel_role           quantifier

signal_type            UV
chromatogram_type      DAD

detector               DAD1
wavelength_nm          254
```

No quantification algorithm changes are required.

---

## 6.5 Internal standards

Internal standards are normal targets with:

```text
role = internal_standard
```

Analyte targets reference:

```text
internal_standard_id
```

Do not introduce an unrelated second internal-standard model for targeted quantification.

---

## Phase 6 acceptance criteria

* Analytes can be defined independently from channels.
* Multiple channels can reference one analyte.
* Internal standards use the same target model.
* MRM and DAD channels are supported through the same table.
* Qualifier transitions can be represented.

---

# PHASE 7 — Quantification assay management Operations

## Goal

Provide the React frontend, MCP clients and AI agents with safe interactive management of the quantitative assay configuration.

---

## Operations

Add:

```text
mass_spec.get_quant_targets
mass_spec.set_quant_targets

mass_spec.get_quant_channels
mass_spec.set_quant_channels
```

Potentially:

```text
mass_spec.remove_quant_targets
mass_spec.remove_quant_channels
```

depending on the generic mutation style established elsewhere.

---

## Validation

Backend validates:

```text
target IDs unique
channel IDs unique
internal standard references valid
quantifier exists
expected RT valid
MRM channel has precursor/product m/z
DAD channel has appropriate wavelength metadata
```

React must not implement these rules independently.

---

## Phase 7 acceptance criteria

* Full assay configuration can be created without direct DuckDB access.
* Operations expose normalized JSON contracts.
* Invalid target/channel configurations are rejected by the backend.

---

# PHASE 8 — Sample and calibration assignments

## Goal

Represent calibrators, QCs, blanks and unknowns correctly.

The existing single `concentration` column in `MASS_SPEC_ANALYSES` is insufficient because one calibration sample may contain different concentrations for many analytes.

---

# 8.1 New table

Create:

```text
MASS_SPEC_QUANT_SAMPLE_ASSIGNMENTS
```

Recommended schema:

```text
analysis
target_id

sample_type
calibration_level

nominal_concentration
internal_standard_concentration

dilution_factor

exclude

created_at
```

---

## 8.2 Sample types

Initially support:

```text
blank
zero
calibrator
qc
unknown
```

Later:

```text
double_blank
matrix_blank
control
```

can be introduced if needed.

---

## 8.3 Important design choice

Assignments are:

```text
analysis × target
```

not merely:

```text
analysis
```

because nominal concentration differs between compounds.

---

## 8.4 Operations

Add:

```text
mass_spec.get_quant_sample_assignments
mass_spec.set_quant_sample_assignments
```

---

## Phase 8 acceptance criteria

* Different compounds in one calibration sample can have different nominal concentrations.
* Unknowns require no nominal concentration.
* Dilution factors are supported.
* Sample role is explicit and queryable.

---

# PHASE 9 — Assign chromatographic peaks to quantitative targets

## Goal

Connect raw analytical peaks with assay targets.

---

## 9.1 Method

Introduce:

```text
mass_spec.assign_quant_targets
```

Reads:

```text
MASS_SPEC_CHROMATOGRAMS_HEADERS
MASS_SPEC_CHROMATOGRAM_PEAKS

MASS_SPEC_QUANT_TARGETS
MASS_SPEC_QUANT_CHANNELS
```

Produces target assignments or records sufficient information in the quantitative result pipeline.

---

## 9.2 Channel matching

For MRM:

```text
signal_type
chromatogram_type
polarity
precursor_mz
product_mz
```

For DAD:

```text
signal_type
detector
wavelength_nm
```

Matching tolerances must be configurable where numeric comparison is required.

---

## 9.3 Peak matching

Among peaks from the selected channel, select using:

```text
expected_rt
rt_tolerance
```

If multiple peaks are candidates, use deterministic ranking such as:

```text
1. smallest RT error
2. highest response
3. best peak-quality score
```

The ranking rule must be documented and testable.

---

## 9.4 Missing peaks

Do not invent zero-concentration results silently.

Represent:

```text
peak_not_found
```

as an explicit assignment/result status.

---

## Phase 9 acceptance criteria

* MRM target assignments work.
* DAD target assignments use the same method.
* RT windows are respected.
* Ambiguous assignments are deterministic.
* Missing peaks remain explicitly missing.

---

# PHASE 10 — Quantifier and qualifier transitions

## Goal

Support multi-transition targeted assays.

---

## 10.1 Quantifier

Exactly one enabled quantifier channel should normally provide the quantitative response.

---

## 10.2 Qualifiers

A target may have zero or multiple qualifier channels.

Example:

```text
Caffeine

quantifier:
195 → 138

qualifier:
195 → 110
```

---

## 10.3 Ion/transition ratio

Calculate:

```text
qualifier_response / quantifier_response
```

and compare with:

```text
expected_ratio
ratio_tolerance
```

Store:

```text
observed_ratio
ratio_error
ratio_pass
```

---

## 10.4 Genericity

The same concept may be used for another detector if multiple channels provide identity confirmation.

The core calibration engine should not care whether the channels are MRM transitions.

---

## Phase 10 acceptance criteria

* Multiple channels can belong to one target.
* Quantifier is clearly identified.
* Qualifier ratios are computed.
* Ratio failures are flags, not hard workflow crashes.

---

# PHASE 11 — Internal-standard normalization

## Goal

Support analyte/internal-standard response ratios.

---

## 11.1 Response extraction

For every target calculate:

```text
response =
    peak area
or
    peak height
```

according to:

```text
response_metric
```

---

## 11.2 Internal-standard response

Resolve the target referenced by:

```text
internal_standard_id
```

for the same analysis.

---

## 11.3 Response ratio

For internal-standard normalized targets:

```text
response_ratio =
    analyte_response /
    internal_standard_response
```

For targets without IS:

```text
response_ratio = response
```

or keep separate response semantics and use raw response during model fitting.

Preferred: preserve both fields explicitly.

---

## 11.4 Missing internal standard

Store explicit statuses such as:

```text
internal_standard_missing
internal_standard_invalid
```

Do not silently fall back to raw response unless configured.

---

## Phase 11 acceptance criteria

* Internal-standard mapping is target-driven.
* Response ratios are reproducible.
* Missing internal standards are visible.
* Internal standard concentration may be retained for future ratio models if required.

---

# PHASE 12 — Calibration points

## Goal

Persist the exact observations used for calibration.

---

# 12.1 Table

Create:

```text
MASS_SPEC_QUANT_CALIBRATION_POINTS
```

Recommended schema:

```text
target_id
analysis

peak_id
internal_standard_peak_id

calibration_level
nominal_concentration

response
internal_standard_response
response_ratio

included

back_calculated_concentration
residual
relative_residual
accuracy_percent

exclusion_reason

created_at
```

---

## 12.2 Why persist calibration points

This provides:

```text
auditability
reproducibility
frontend calibration plots
point exclusion provenance
calibration diagnostics
```

Do not persist only regression coefficients.

---

## Phase 12 acceptance criteria

* Every calibration observation is inspectable.
* Included/excluded state is explicit.
* Back-calculated concentrations can be persisted.
* Calibration graph can be recreated entirely from project data.

---

# PHASE 13 — Calibration fitting

## Goal

Fit persistent target-specific calibration models.

---

## 13.1 Method

Introduce:

```text
mass_spec.fit_calibration_models
```

Reads:

```text
MASS_SPEC_QUANT_CALIBRATION_POINTS
MASS_SPEC_QUANT_TARGETS
```

Writes:

```text
MASS_SPEC_QUANT_CALIBRATION_MODELS
```

and updates calibration-point diagnostics if necessary.

---

# 13.2 Initial models

Implement:

```text
linear
quadratic
```

Cubic may be deferred until there is a demonstrated use case.

---

## 13.3 Initial weighting

Support:

```text
none
1/x
1/x²
```

---

## 13.4 Intercept

Support:

```text
free
forced_zero
```

if scientifically appropriate for the intended assay.

---

# 13.5 Calibration model table

Create:

```text
MASS_SPEC_QUANT_CALIBRATION_MODELS
```

Recommended fields:

```text
target_id

model
weighting
intercept_mode

coefficient_0
coefficient_1
coefficient_2
coefficient_3

r_squared
adjusted_r_squared
rmse

n_points
range_min
range_max

fit_status

created_at
```

Potential later statistics:

```text
aic
bic
lack_of_fit
```

---

## 13.6 Calibration fitting utility

Implement algorithm code separately:

```text
src/utils/calibration.cpp
```

with functions roughly equivalent to:

```text
fit_linear()
fit_weighted_linear()
fit_quadratic()

predict_response()
inverse_predict_concentration()

calculate_residuals()
calculate_r_squared()
```

The workflow Method should not contain the numerical fitting implementation directly.

---

## Phase 13 acceptance criteria

* Multiple targets are independently calibrated.
* Weighted regression works.
* Model coefficients are persisted.
* Diagnostics are persisted.
* Calibration can be reconstructed exactly.

---

# PHASE 14 — Quantify unknown samples

## Goal

Calculate concentrations for unknown/QC samples.

---

## 14.1 Method

Introduce:

```text
mass_spec.quantify_samples
```

Reads:

```text
MASS_SPEC_CHROMATOGRAM_PEAKS
MASS_SPEC_QUANT_TARGETS
MASS_SPEC_QUANT_CHANNELS
MASS_SPEC_QUANT_SAMPLE_ASSIGNMENTS
MASS_SPEC_QUANT_CALIBRATION_MODELS
```

and target-assignment/calibration records as appropriate.

Writes:

```text
MASS_SPEC_QUANT_RESULTS
```

---

# 14.2 Result table

Recommended:

```text
MASS_SPEC_QUANT_RESULTS
```

Schema:

```text
analysis
target_id

peak_id
internal_standard_peak_id

rt
rt_error

area
height

internal_standard_area
internal_standard_height

response
internal_standard_response
response_ratio

calculated_concentration

dilution_factor
final_concentration
concentration_unit

qualifier_ratio
qualifier_ratio_error

accuracy_percent

status
flags

created_at
```

---

## 14.3 Concentration calculation

Conceptually:

```text
measured response
        ↓
calibration model inverse
        ↓
calculated concentration
        ↓
× dilution factor
        ↓
final concentration
```

---

## 14.4 Outside calibration range

Do not silently extrapolate without recording that fact.

Possible statuses:

```text
below_calibration_range
above_calibration_range
extrapolated
```

Whether extrapolation is allowed should be configurable.

---

## Phase 14 acceptance criteria

* Unknown concentrations are calculated.
* Dilution factors are applied.
* Units are preserved.
* Out-of-range results are explicitly flagged.
* Result provenance points back to calibration and peak records.

---

# PHASE 15 — Quantitative QC framework

## Goal

Evaluate quantitative results without hard-coding one regulatory guideline.

---

## 15.1 Generic calculated metrics

Support:

```text
retention-time error
qualifier ratio error
peak S/N
calibration residual
back-calculated accuracy
QC accuracy
internal-standard response
```

Potential later metrics:

```text
precision
CV
carry-over
blank response
```

---

## 15.2 Configurable thresholds

Do not hard-code values such as:

```text
80–120 %
±20 %
```

into C++.

Instead define target/method-level parameters.

---

## 15.3 Method

Introduce:

```text
mass_spec.evaluate_quant_qc
```

Reads:

```text
quant results
calibration points
targets
channels
sample assignments
```

Writes updated:

```text
MASS_SPEC_QUANT_RESULTS
```

or a dedicated QC table if later requirements justify it.

Initial preference:

store:

```text
status
flags
```

in the result table and keep detailed metrics as columns.

---

## Phase 15 acceptance criteria

* QC is configurable.
* Failed QC does not delete analytical results.
* Multiple QC flags can coexist.
* Quantitative results remain inspectable even when rejected.

---

# PHASE 16 — Interactive preview Operations

## Goal

Support frontend workflow configuration without modifying persistent project state.

This uses StreamFind's intended distinction between Operations and Methods.

---

## 16.1 Peak-integration preview

Add:

```text
mass_spec.preview_peak_integration
```

Input:

```text
analysis
chromatogram_index
integration parameters
```

Output:

```text
chromatogram
candidate peaks
integration boundaries
areas
metrics
```

No DuckDB mutation.

---

## 16.2 Calibration preview

Add:

```text
mass_spec.preview_calibration_fit
```

Input:

```text
target
model
weighting
included/excluded points
```

Output:

```text
coefficients
predictions
residuals
r_squared
rmse
```

No persistent mutation.

---

## 16.3 React use case

```text
change integration settings
        ↓
preview
        ↓
inspect result
        ↓
accept parameters
        ↓
save workflow
        ↓
run Method
```

This should replace the vendor-software pattern of embedding analytical mutation directly into the GUI.

---

## Phase 16 acceptance criteria

* Previews are fast and non-persistent.
* Preview algorithms use the same numerical routines as persistent Methods.
* React can provide interactive method tuning without creating hidden project state.

---

# PHASE 17 — Read Operations for frontend and MCP clients

## Goal

Expose all quantitative results through stable project Operations.

Add:

```text
mass_spec.get_chromatogram_peaks

mass_spec.get_quant_targets
mass_spec.get_quant_channels
mass_spec.get_quant_sample_assignments

mass_spec.get_calibration_points
mass_spec.get_calibration_models

mass_spec.get_quant_results
```

Operations should support filters such as:

```text
analysis_names
target_ids
sample_type
status
```

Do not expose arbitrary SQL as the normal external interface.

---

# PHASE 18 — React quantitative workflow integration

## Goal

Allow the frontend currently under development to discover and visualize this workflow without MassSpec-specific workflow hard-coding.

---

## 18.1 Workflow canvas

Methods appear automatically:

```text
Load chromatograms
Filter chromatograms
Correct baseline
Smooth chromatograms
Find chromatogram peaks
Assign quantitative targets
Fit calibration models
Quantify samples
Evaluate quantitative QC
```

because they are exposed through the semantic catalogue.

---

## 18.2 Quantification setup page

The frontend may compose reusable forms for:

```text
Targets
Channels
Samples
Calibration
```

but backend Operations remain authoritative.

---

## 18.3 Suggested quantitative results workspace

```text
Quantification
│
├── Overview
├── Compounds
├── Samples
├── Calibration
├── Peaks
└── QC
```

---

## 18.4 Compound view

Suggested linked layout:

```text
Compound list
     │
     ▼
Chromatogram / integrated peak
     │
     ├── calibration curve
     ├── sample results
     └── QC flags
```

---

## 18.5 Batch overview

Example:

```text
Compound        Calibration     QC      Unknowns     Flags

Caffeine             ✓          ✓          32          1
Diclofenac           ✓          ✕          32          4
Ibuprofen            ✓          ✓          32          0
```

---

# PHASE 19 — DAD/UV architecture validation

## Goal

Prove that the targeted-quantification framework is detector-independent.

This phase is an architectural acceptance test.

---

## 19.1 Test dataset

Use LC-DAD chromatograms with known:

```text
detector
wavelength
retention time
calibration levels
unknown samples
```

---

## 19.2 Expected workflow

```text
Load DAD chromatograms
↓
baseline correction
↓
peak integration
↓
target assignment by wavelength + RT
↓
calibration
↓
quantification
```

No new quantification algorithms should be required.

---

## 19.3 Prohibited architecture

Do not introduce:

```text
MRMQuantificationEngine
DADQuantificationEngine
```

if they perform the same calibration/quantification logic.

Only channel resolution should differ.

---

## Phase 19 acceptance criteria

* DAD uses `MASS_SPEC_CHROMATOGRAM_PEAKS`.
* DAD uses `MASS_SPEC_QUANT_TARGETS`.
* DAD uses `MASS_SPEC_QUANT_CHANNELS`.
* DAD uses the same calibration functions.
* DAD uses the same `MASS_SPEC_QUANT_RESULTS`.
* No detector-specific branch exists in the calibration algorithm.

---

# PHASE 20 — MRM validation and vendor-reader integration

## Goal

Validate the complete targeted workflow using native MRM files.

SCIEX support is particularly appropriate because the current reader work already includes native MRM decoding paths.

---

## Validate

```text
MRM headers
transition identity
precursor/product m/z
RT arrays
signal intensity
multi-experiment files
positive/negative polarity
```

Then run:

```text
import
↓
load chromatograms
↓
process chromatograms
↓
find peaks
↓
assign targets
↓
fit calibration
↓
quantify
```

---

# 21. Methods versus Operations summary

## Methods — persistent workflow steps

```text
mass_spec.load_chromatograms

mass_spec.filter_chromatograms_retention_time

mass_spec.correct_chromatogram_baseline

mass_spec.smooth_chromatograms

mass_spec.find_chromatogram_peaks

mass_spec.assign_quant_targets

mass_spec.fit_calibration_models

mass_spec.quantify_samples

mass_spec.evaluate_quant_qc
```

Methods:

```text
read project tables
perform reproducible processing
write project tables
participate in workflow execution/provenance
```

---

## Operations — interactive/query actions

```text
mass_spec.get_chromatograms

mass_spec.get_chromatogram_peaks

mass_spec.get_quant_targets
mass_spec.set_quant_targets

mass_spec.get_quant_channels
mass_spec.set_quant_channels

mass_spec.get_quant_sample_assignments
mass_spec.set_quant_sample_assignments

mass_spec.get_calibration_points
mass_spec.get_calibration_models
mass_spec.get_quant_results

mass_spec.preview_peak_integration
mass_spec.preview_calibration_fit
```

Operations should not replace workflow Methods for persistent analytical transformations.

---

# 22. Final table architecture

The targeted workflow should eventually use approximately:

```text
MASS_SPEC_ANALYSES

MASS_SPEC_CHROMATOGRAMS_HEADERS
MASS_SPEC_CHROMATOGRAMS
MASS_SPEC_CHROMATOGRAM_PEAKS

MASS_SPEC_QUANT_TARGETS
MASS_SPEC_QUANT_CHANNELS
MASS_SPEC_QUANT_SAMPLE_ASSIGNMENTS

MASS_SPEC_QUANT_CALIBRATION_POINTS
MASS_SPEC_QUANT_CALIBRATION_MODELS

MASS_SPEC_QUANT_RESULTS
```

Relationships:

```text
ANALYSES
   │
   ├─────────────────────────────┐
   │                             │
   ▼                             ▼
CHROMATOGRAM_HEADERS        SAMPLE_ASSIGNMENTS
   │                             │
   ▼                             │
CHROMATOGRAMS                    │
   │                             │
   ▼                             │
CHROMATOGRAM_PEAKS               │
   │                             │
   └──────────────┐              │
                  ▼              │
TARGETS ──── CHANNELS            │
    │             │              │
    └──────┬──────┘              │
           ▼                     │
     TARGET ASSIGNMENT ◄─────────┘
           │
           ▼
    CALIBRATION_POINTS
           │
           ▼
    CALIBRATION_MODELS
           │
           ▼
       QUANT_RESULTS
```

---

# 23. Semantic work

Every new public capability or table must be represented first in the MassSpec semantic catalogue.

Update:

```text
cpp/plugins/mass_spec/semantic/domain.ttl
cpp/plugins/mass_spec/semantic/tables.ttl
cpp/plugins/mass_spec/semantic/columns.ttl
cpp/plugins/mass_spec/semantic/fields.ttl
cpp/plugins/mass_spec/semantic/parameters.ttl
cpp/plugins/mass_spec/semantic/methods.ttl
cpp/plugins/mass_spec/semantic/operations.ttl
cpp/plugins/mass_spec/semantic/results.ttl
```

Follow the existing repository ontology formatting rules.

Do not maintain a second handwritten frontend schema.

---

# 24. Testing strategy

## Unit tests

Test analytical algorithms independently:

```text
baseline correction
smoothing
peak boundary detection
integration
peak metrics

linear calibration
weighted calibration
quadratic calibration

inverse prediction

internal-standard ratios
qualifier ratios
```

---

## Plugin integration tests

Test:

```text
table registration
Method read/write contracts
Operation schemas
capability registration
semantic/executable parity
```

---

## Chromatogram fixtures

Include controlled fixtures for:

```text
single MRM peak
multiple MRM peaks

no peak
overlapping peaks
low S/N

internal-standard missing

DAD peak
multiple wavelengths
```

---

## Calibration fixtures

Include:

```text
perfect linear curve
linear curve with noise
weighted calibration
excluded standard
zero standard
out-of-range unknown
quadratic calibration
```

Expected coefficients and predictions must be fixed.

---

# 25. Implementation dependency sequence

```text
Phase 1
Normalize chromatogram persistence
        │
        ▼
Phase 2
Chromatogram peak table
        │
        ▼
Phase 3
Peak integration
        │
        ├─────────────┐
        ▼             ▼
Phase 4          Phase 5
Preprocessing    Quant module
                      │
                      ▼
                   Phase 6
                 Targets/channels
                      │
                      ▼
                   Phase 7
                Assay Operations
                      │
                      ▼
                   Phase 8
               Sample assignments
                      │
                      ▼
                   Phase 9
                Target assignment
                      │
                      ▼
                  Phase 10
              Qualifier channels
                      │
                      ▼
                  Phase 11
            Internal-standard normalization
                      │
                      ▼
                  Phase 12
             Calibration points
                      │
                      ▼
                  Phase 13
             Calibration models
                      │
                      ▼
                  Phase 14
               Quantification
                      │
                      ▼
                  Phase 15
                    QC
                      │
              ┌───────┴───────┐
              ▼               ▼
          Phase 16         Phase 17
          Previews         Read APIs
              │               │
              └───────┬───────┘
                      ▼
                  Phase 18
                  React UI
                      │
              ┌───────┴────────┐
              ▼                ▼
          Phase 19          Phase 20
          DAD test          MRM test
```

---

# 26. Recommended milestone grouping

## Milestone Q1 — Reliable chromatograms

Includes:

```text
Phase 1
```

Done when:

```text
reader
semantic contract
DuckDB
Methods
Operations
```

all agree on the chromatogram representation.

---

## Milestone Q2 — Generic chromatographic processing

Includes:

```text
Phases 2–4
```

Done when both MRM and DAD chromatograms can be preprocessed and integrated into `MASS_SPEC_CHROMATOGRAM_PEAKS`.

---

## Milestone Q3 — Quantitative assay model

Includes:

```text
Phases 5–8
```

Done when targets, channels, internal standards, calibration levels and unknown samples can be represented persistently.

---

## Milestone Q4 — Targeted peak identification

Includes:

```text
Phases 9–11
```

Done when StreamFind can reliably identify quantitative peaks and calculate analyte/internal-standard responses.

---

## Milestone Q5 — Calibration and quantification

Includes:

```text
Phases 12–14
```

Done when calibration models and unknown concentrations are reproducible project outputs.

---

## Milestone Q6 — Production-oriented quantitation

Includes:

```text
Phases 15–18
```

Done when QC, previews, Operations and the React UI support practical batch review.

---

## Milestone Q7 — Generic architecture validation

Includes:

```text
Phases 19–20
```

Done only when both MRM and DAD successfully use the same generic quantification architecture.

---

# 27. Definition of done

This MassSpec extension is complete when:

* the current chromatogram persistence mismatch has been removed;
* chromatogram headers are the canonical channel metadata;
* chromatogram point data are detector-independent;
* chromatographic peaks are stored in a generic persistent table;
* preprocessing and peak detection are reusable Methods;
* targeted quantification is a MassSpec domain module rather than a separate plugin;
* analytes and internal standards are represented as targets;
* analytical signals are represented as channels;
* MRM transitions and DAD wavelengths use the same channel contract;
* calibration assignments support different concentrations per target and analysis;
* quantifier and qualifier channels are supported;
* internal-standard normalization is supported;
* calibration points are persisted;
* calibration models are persisted;
* unknown concentrations are persisted;
* quantitative QC is configurable rather than hard-coded;
* preview Operations reuse the same algorithms as workflow Methods;
* the React frontend can discover all Methods and Operations from the semantic catalogue;
* MRM quantification works using native reader data;
* DAD quantification works without introducing a separate quantification engine;
* every quantitative result can be traced back to its peak, chromatogram, target, channel, calibration model, workflow step, parameters and plugin version.

---

# 28. Guiding design rule

The core model should remain:

```text
channel metadata
      │
      ▼
chromatographic signal
      │
      ▼
generic chromatographic peak
      │
      ▼
target/channel assignment
      │
      ▼
response calculation
      │
      ▼
internal-standard normalization
      │
      ▼
calibration
      │
      ▼
quantitative result
```

MRM and DAD differ primarily in **how the analytical channel is identified**.

They should not require separate peak-integration, calibration, or quantification architectures.

This keeps StreamFind extensible toward additional chromatographic detectors without coupling the analytical workflow to any one vendor, detector type, or acquisition mode.

One implementation detail I would treat as especially important is **Phase 1 being completed and tested as its own milestone before adding `MASS_SPEC_CHROMATOGRAM_PEAKS`**. Otherwise the new peak and quantification tables would end up depending on the current ambiguous split between header metadata and point metadata, making that inconsistency much harder to remove later.
