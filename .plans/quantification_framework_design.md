# Quantification Framework Design

## Overview

This document defines the data model, input parameters, and workflow for targeted quantitative analysis in StreamFind. The framework supports MRM (MS), DAD/UV, and any future detector type through a common channel/target model.

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
| `expected_rt` | real | no | Expected retention time in seconds |
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
- `expected_rt` and `rt_tolerance` are used for peak assignment

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
| `is_concentration` | real | no | IS concentration (if constant across samples) |
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
  "expected_rt": 1020.0,
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

### `mass_spec.set_quant_targets`
```json
{
  "targets": [
    {
      "target_id": "caffeine",
      "role": "analyte",
      "expected_rt": 1020.0,
      "rt_tolerance": 30.0,
      "response_metric": "area",
      "normalization": "internal_standard",
      "internal_standard_id": "IS_caffeine_d9"
    }
  ]
}
```

### `mass_spec.set_quant_channels`
```json
{
  "channels": [
    {
      "channel_id": "caffeine_quant",
      "target_id": "caffeine",
      "channel_role": "quantifier",
      "signal_type": "MS",
      "precursor_mz": 195.1,
      "product_mz": 138.1
    }
  ]
}
```

### `mass_spec.set_quant_sample_assignments`
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
| RT > 0 | Invalid expected_rt |
| RT tolerance > 0 | Invalid rt_tolerance |
