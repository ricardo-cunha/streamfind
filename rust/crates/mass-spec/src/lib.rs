#![allow(non_snake_case)]

use std::path::Path;

use serde_json::{json, Value};
use streamfind_rust_core::{
    Error, ErrorCode, Method, MethodRegistry, MethodValidator, Operation, OperationRegistry,
    ParameterDefinition, ParameterSchema, ParameterType, Project, ProjectTableStore, Result,
    TypeDescriptor,
};

pub mod nta;
pub mod nta_alignment;
pub mod nta_annotation;
pub mod nta_blank_subtraction;
pub mod nta_componentization;
pub mod nta_correction_algorithms;
pub mod nta_filters;
pub mod nta_gap_filling;
pub mod nta_metfrag;
pub mod nta_suspect_screening;
pub mod nta_transformation_products;
pub mod nta_utils;
pub mod processing_methods_chromatograms;
pub mod processing_methods_nta;
pub mod reader;
pub mod reader_agilent;
pub mod reader_agilent_chemstation;
pub mod reader_agilent_ims;
pub mod reader_bruker;
pub mod reader_sciex;
pub mod reader_thermo;

const ANALYSES_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_ANALYSES (analysis VARCHAR NOT NULL, analysis_index INTEGER NOT NULL DEFAULT 0, source_analysis_number INTEGER, analysis_count INTEGER NOT NULL DEFAULT 1, replicate VARCHAR, blank VARCHAR, file_name VARCHAR, file_path VARCHAR NOT NULL, file_dir VARCHAR, file_extension VARCHAR, format VARCHAR, type VARCHAR, time_stamp VARCHAR, number_spectra INTEGER, number_chromatograms INTEGER, number_spectra_binary_arrays INTEGER, min_mz DOUBLE, max_mz DOUBLE, start_rt DOUBLE, end_rt DOUBLE, has_ion_mobility BOOLEAN, concentration DOUBLE, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis))";
const SPECTRA_HEADERS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_SPECTRA_HEADERS (analysis VARCHAR NOT NULL, index INTEGER NOT NULL, scan INTEGER, array_length INTEGER, level INTEGER, mode INTEGER, polarity INTEGER, configuration INTEGER, lowmz DOUBLE, highmz DOUBLE, bpmz DOUBLE, bpint DOUBLE, tic DOUBLE, rt DOUBLE, mobility DOUBLE, window_mz DOUBLE, window_mzlow DOUBLE, window_mzhigh DOUBLE, precursor_mz DOUBLE, precursor_intensity DOUBLE, precursor_charge INTEGER, activation_ce DOUBLE, PRIMARY KEY(analysis, index))";
const CHROMATOGRAMS_HEADERS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_CHROMATOGRAMS_HEADERS (analysis VARCHAR NOT NULL, index INTEGER NOT NULL, chromatogram_id VARCHAR, array_length INTEGER, polarity INTEGER, precursor_mz DOUBLE, activation_ce DOUBLE, product_mz DOUBLE, signal_type VARCHAR, chromatogram_type VARCHAR, detector VARCHAR, channel VARCHAR, units VARCHAR, wavelength_nm DOUBLE, interval_ms DOUBLE, start_time DOUBLE, end_time DOUBLE, intensity_multiplier DOUBLE, PRIMARY KEY(analysis, index))";
pub(crate) const CHROMATOGRAMS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS MASS_SPEC_CHROMATOGRAMS (analysis TEXT NOT NULL, index INTEGER NOT NULL DEFAULT 0, chromatogram_id TEXT NOT NULL, polarity INTEGER, precursor_mz DOUBLE, activation_ce DOUBLE, product_mz DOUBLE, wavelength_nm DOUBLE NOT NULL DEFAULT 0, rt DOUBLE NOT NULL, raw_intensity DOUBLE NOT NULL, baseline DOUBLE NOT NULL DEFAULT 0, intensity DOUBLE NOT NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(analysis, chromatogram_id, rt))";

const CHROMATOGRAMS_SCHEMA_ALTERS: [&str; 6] = [
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS index INTEGER DEFAULT 0",
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS polarity INTEGER",
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS precursor_mz DOUBLE",
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS activation_ce DOUBLE",
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS product_mz DOUBLE",
    "ALTER TABLE MASS_SPEC_CHROMATOGRAMS ADD COLUMN IF NOT EXISTS wavelength_nm DOUBLE DEFAULT 0",
];

pub(crate) fn ensure_chromatograms_schema(project: &Project) -> Result<()> {
    let tables = ProjectTableStore::new(project);
    tables.ensure_table(CHROMATOGRAMS_SCHEMA)?;
    for alter in CHROMATOGRAMS_SCHEMA_ALTERS {
        tables.execute(alter)?;
    }
    Ok(())
}

fn sql(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

fn ensure_schema(project: &Project) -> Result<()> {
    let tables = ProjectTableStore::new(project);
    tables.ensure_table(ANALYSES_SCHEMA)?;
    tables.execute(
        "ALTER TABLE MASS_SPEC_ANALYSES ADD COLUMN IF NOT EXISTS analysis_index INTEGER DEFAULT 0",
    )?;
    tables.execute(
        "ALTER TABLE MASS_SPEC_ANALYSES ADD COLUMN IF NOT EXISTS source_analysis_number INTEGER",
    )?;
    tables.execute(
        "ALTER TABLE MASS_SPEC_ANALYSES ADD COLUMN IF NOT EXISTS analysis_count INTEGER DEFAULT 1",
    )?;
    tables.execute("ALTER TABLE MASS_SPEC_ANALYSES ADD COLUMN IF NOT EXISTS replicate VARCHAR")?;
    tables.ensure_table(SPECTRA_HEADERS_SCHEMA)?;
    tables.ensure_table(CHROMATOGRAMS_HEADERS_SCHEMA)?;
    Ok(())
}

fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::InvalidArgument, message)
}

fn format_name(format: reader::Format) -> &'static str {
    match format {
        reader::Format::MzMl => "mzML",
        reader::Format::MzXml => "mzXML",
        reader::Format::Asc => "ASC",
        reader::Format::ShimadzuLcd => "ShimadzuLCD",
        reader::Format::SciexWiff => "SciexWIFF",
        reader::Format::AgilentMassHunterD => "AgilentMassHunterD",
        reader::Format::AgilentChemStationD => "AgilentChemStationD",
        reader::Format::BrukerTsf => "BrukerTSF",
        reader::Format::BrukerBaf => "BrukerBAF",
        reader::Format::ThermoRaw => "ThermoRAW",
    }
}

fn add_analyses(project: &mut Project, parameters: &Value) -> Result<Value> {
    ensure_schema(project)?;
    let analyses = parameters
        .get("analyses")
        .and_then(Value::as_array)
        .ok_or_else(|| invalid("analyses must be an array"))?;
    let mut added = Vec::new();
    for item in analyses {
        let path_string = item
            .get("path")
            .and_then(Value::as_str)
            .ok_or_else(|| invalid("analysis.path is required"))?;
        let path = Path::new(path_string);
        let mut reader = reader::Reader::open(path)
            .map_err(|error| Error::new(ErrorCode::InvalidArgument, error.to_string()))?;
        let base_analysis = path
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        if base_analysis.is_empty() {
            return Err(invalid("analysis path has no file stem"));
        }
        let replicate = item
            .get("replicate_name")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let blank = item
            .get("blank_name")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let file_name = path
            .file_name()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        let file_dir = path
            .parent()
            .map(|value| value.to_string_lossy())
            .unwrap_or_default();
        let extension = path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        let catalog = reader.analysis_catalog().to_vec();
        for descriptor in catalog {
            reader
                .select_analysis(descriptor.analysis_index)
                .map_err(|error| invalid(error.to_string()))?;
            let summary = reader.summary();
            let analysis = if reader.format() == crate::reader::Format::SciexWiff {
                format!("{base_analysis}::{}", descriptor.name)
            } else {
                base_analysis.to_owned()
            };
            let existing = project.query_json(&format!(
                "SELECT analysis FROM MASS_SPEC_ANALYSES WHERE analysis = {}",
                sql(&analysis)
            ))?;
            if existing.as_array().is_some_and(|rows| !rows.is_empty()) {
                return Err(invalid(format!(
                    "analysis already exists in project: {analysis}"
                )));
            }
            let source_number = descriptor
                .source_analysis_number
                .map_or("NULL".to_owned(), |value| value.to_string());
            let query = format!("INSERT INTO MASS_SPEC_ANALYSES (analysis, analysis_index, source_analysis_number, analysis_count, replicate, blank, file_name, file_path, file_dir, file_extension, format, type, time_stamp, number_spectra, number_chromatograms, number_spectra_binary_arrays, min_mz, max_mz, start_rt, end_rt, has_ion_mobility, concentration) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, NULL)", sql(&analysis), descriptor.analysis_index, source_number, descriptor.analysis_count, sql(replicate), sql(blank), sql(file_name), sql(path_string), sql(file_dir.as_ref()), sql(extension), sql(format_name(summary.format)), sql("MS"), sql(&summary.time_stamp), summary.number_spectra, summary.number_chromatograms, summary.number_spectra_binary_arrays, summary.min_mz, summary.max_mz, summary.start_rt, summary.end_rt, summary.has_ion_mobility);
            project.execute_sql(&query)?;
            added.push(json!({"analysis": analysis, "file_path": path_string, "analysis_index": descriptor.analysis_index, "source_analysis_number": descriptor.source_analysis_number, "analysis_count": descriptor.analysis_count, "replicate": replicate, "blank": blank}));
        }
    }
    Ok(Value::Array(added))
}

fn remove_analyses(project: &mut Project, parameters: &Value) -> Result<Value> {
    ensure_schema(project)?;
    let names = parameters
        .get("analysis_names")
        .and_then(Value::as_array)
        .ok_or_else(|| invalid("analysis_names must be an array"))?;
    let mut removed = Vec::new();
    for name in names {
        let name = name
            .as_str()
            .ok_or_else(|| invalid("analysis_names must contain strings"))?;
        project.execute_sql(&format!(
            "DELETE FROM MASS_SPEC_ANALYSES WHERE analysis = {}",
            sql(name)
        ))?;
        removed.push(name);
    }
    Ok(json!(removed))
}

fn get_analyses_info(project: &Project) -> Result<Value> {
    ensure_schema(project)?;
    project.query_json("SELECT analysis, analysis_index, source_analysis_number, analysis_count, replicate, blank, file_path, format, number_spectra, number_chromatograms FROM MASS_SPEC_ANALYSES ORDER BY analysis")
}

fn text(value: &Value) -> String {
    value.as_str().unwrap_or_default().into()
}
fn number(value: &Value) -> f32 {
    value.as_f64().unwrap_or(0.0) as f32
}
fn string_list(parameters: &Value, key: &str) -> Vec<String> {
    parameters
        .get(key)
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .map(text)
                .filter(|value| !value.is_empty())
                .collect()
        })
        .unwrap_or_default()
}
fn int_list(parameters: &Value, key: &str) -> Vec<i32> {
    parameters
        .get(key)
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_i64)
                .map(|value| value as i32)
                .collect()
        })
        .unwrap_or_default()
}
fn selected(names: &[String], analysis: &str) -> bool {
    names.is_empty() || names.iter().any(|name| name == analysis)
}
fn in_range(value: f32, parameters: &Value, low: &str, high: &str) -> bool {
    parameters
        .get(low)
        .map_or(true, |bound| value >= number(bound))
        && parameters
            .get(high)
            .map_or(true, |bound| value <= number(bound))
}

#[derive(Debug, Clone)]
pub struct TargetRange {
    pub id: String,
    pub analyses: Vec<String>,
    pub polarities: Vec<i32>,
    pub levels: Vec<i32>,
    pub mz_min: f32,
    pub mz_max: f32,
    pub rt_min: f32,
    pub rt_max: f32,
}

#[derive(Debug, Clone)]
pub struct TargetQuery {
    pub targets: Vec<TargetRange>,
    pub ppm: f64,
    pub rt_tolerance: f64,
    pub charge: i32,
}

fn target_number(value: Option<&Value>) -> Option<f64> {
    value.and_then(Value::as_f64)
}

fn normalize_targets(parameters: &Value) -> TargetQuery {
    const PROTON: f64 = 1.007276;
    let ppm = target_number(parameters.get("ppm")).unwrap_or(20.0);
    let rt_tolerance = target_number(parameters.get("rt_tolerance")).unwrap_or(60.0);
    let charge = target_number(parameters.get("charge"))
        .unwrap_or(1.0)
        .abs()
        .max(1.0) as i32;
    let source = parameters
        .get("targets")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_else(|| vec![json!({})]);
    let mut targets = Vec::new();
    for (source_index, source) in source.iter().enumerate() {
        let polarities = source
            .get("polarity")
            .or_else(|| parameters.get("polarity"))
            .map(|value| {
                if let Some(values) = value.as_array() {
                    values
                        .iter()
                        .filter_map(Value::as_i64)
                        .map(|v| v as i32)
                        .collect()
                } else {
                    vec![value.as_i64().unwrap_or(0) as i32]
                }
            })
            .unwrap_or_default();
        let polarities = if polarities.is_empty() {
            vec![0]
        } else {
            polarities
        };
        let analyses = source
            .get("analyses")
            .map(|value| {
                if let Some(values) = value.as_array() {
                    values
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_owned)
                        .collect()
                } else {
                    vec![text(value)]
                }
            })
            .unwrap_or_else(|| string_list(parameters, "analysis_names"));
        let levels = source
            .get("levels")
            .map(|value| {
                if let Some(values) = value.as_array() {
                    values
                        .iter()
                        .filter_map(Value::as_i64)
                        .map(|v| v as i32)
                        .collect()
                } else {
                    vec![value.as_i64().unwrap_or(0) as i32]
                }
            })
            .unwrap_or_else(|| int_list(parameters, "levels"));
        let mut mass = target_number(source.get("mass"));
        let mut mass_min = target_number(source.get("mass_min"));
        let mut mass_max = target_number(source.get("mass_max"));
        let req_mz_min =
            target_number(source.get("mz_min")).or_else(|| target_number(parameters.get("mz_min")));
        let req_mz_max =
            target_number(source.get("mz_max")).or_else(|| target_number(parameters.get("mz_max")));
        let req_exact_mz = target_number(source.get("mz"));
        let mut chemical_mass: f64 = 0.0;
        if mass.is_none()
            && mass_min.is_none()
            && mass_max.is_none()
            && req_mz_min.is_none()
            && req_mz_max.is_none()
            && req_exact_mz.is_none()
            && (source.get("SMILES").is_some() || source.get("InChI").is_some())
        {
            let smiles = source.get("SMILES").and_then(Value::as_str).unwrap_or("");
            let inchi = source.get("InChI").and_then(Value::as_str).unwrap_or("");
            let normalized = crate::nta_suspect_screening::normalize_structure(smiles, inchi);
            if normalized.ok && normalized.exact_mass > 0.0 {
                chemical_mass = normalized.exact_mass;
                mass = Some(chemical_mass);
                mass_min = Some(chemical_mass);
                mass_max = Some(chemical_mass);
            }
        }
        let mass_based = mass.is_some() || mass_min.is_some() || mass_max.is_some();
        let front_sign = if polarities.first().copied().unwrap_or(0) < 0 {
            -1
        } else {
            1
        };
        let sign_list: Vec<(i32, Vec<i32>)> = if chemical_mass > 0.0 && polarities == vec![0] {
            // query both [M-H]- and [M+H]+ so the analysis polarity selects the hit
            vec![(-1, vec![-1]), (1, vec![1])]
        } else {
            vec![(front_sign, polarities.clone())]
        };
        for (polarity, range_polarities) in sign_list {
            let sign = if polarity < 0 { -1.0 } else { 1.0 };
            let mut mz_min = req_mz_min;
            let mut mz_max = req_mz_max;
            let exact_mz = req_exact_mz;
            if mz_min.is_none() && mz_max.is_none() {
                if let Some(mz) = exact_mz {
                    mz_min = Some(mz);
                    mz_max = Some(mz);
                }
            }
            if mz_min.is_none() && mz_max.is_none() && mass_based {
                mz_min = mass_min
                    .or(mass)
                    .map(|value| value + sign * PROTON / charge as f64);
                mz_max = mass_max
                    .or(mass)
                    .map(|value| value + sign * PROTON / charge as f64);
            }
            if let Some(mz) = mz_min.or(mz_max) {
                let delta = mz * ppm / 1e6;
                if (mass_based || exact_mz.is_some()) && mz_min == mz_max {
                    mz_min = Some(mz - delta);
                    mz_max = Some(mz + delta);
                } else {
                    mz_min = Some(mz_min.unwrap_or(mz - delta));
                    mz_max = Some(mz_max.unwrap_or(mz + delta));
                }
            }
            let isolation_window = target_number(parameters.get("isolation_window")).unwrap_or(0.0);
            if isolation_window > 0.0 {
                mz_min = Some(mz_min.unwrap_or(f64::NEG_INFINITY) - isolation_window / 2.0);
                mz_max = Some(mz_max.unwrap_or(f64::INFINITY) + isolation_window / 2.0);
            }
            let rt = target_number(source.get("rt"));
            let rt_min = target_number(source.get("rt_min"))
                .or_else(|| target_number(parameters.get("rt_min")))
                .or_else(|| rt.map(|v| v - rt_tolerance));
            let rt_max = target_number(source.get("rt_max"))
                .or_else(|| target_number(parameters.get("rt_max")))
                .or_else(|| rt.map(|v| v + rt_tolerance));
            targets.push(TargetRange {
                id: source
                    .get("id")
                    .and_then(Value::as_str)
                    .map(str::to_owned)
                    .unwrap_or_else(|| format!("target{source_index}")),
                analyses: analyses.clone(),
                polarities: range_polarities,
                levels: levels.clone(),
                mz_min: mz_min.unwrap_or(f32::NEG_INFINITY as f64) as f32,
                mz_max: mz_max.unwrap_or(f32::INFINITY as f64) as f32,
                rt_min: rt_min.unwrap_or(f32::NEG_INFINITY as f64) as f32,
                rt_max: rt_max.unwrap_or(f32::INFINITY as f64) as f32,
            });
        }
    }
    TargetQuery {
        targets,
        ppm,
        rt_tolerance,
        charge,
    }
}

fn target_matches(
    target: &TargetRange,
    analysis: &str,
    polarity: i32,
    level: i32,
    rt: f32,
    mz: f32,
) -> bool {
    (target.analyses.is_empty() || target.analyses.iter().any(|value| value == analysis))
        && (target.polarities.contains(&0) || target.polarities.contains(&polarity))
        && (target.levels.is_empty() || target.levels.contains(&level))
        && mz >= target.mz_min
        && mz <= target.mz_max
        && rt >= target.rt_min
        && rt <= target.rt_max
}

fn analysis_rows(project: &Project) -> Result<Value> {
    project.query_json("SELECT analysis, file_path, COALESCE(replicate, '') AS replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis")
}

fn update_values(
    project: &mut Project,
    parameters: &Value,
    key: &str,
    column: &str,
    numeric: bool,
) -> Result<Value> {
    ensure_schema(project)?;
    let rows = analysis_rows(project)?;
    let values = parameters
        .get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| invalid(format!("{key} must be an array")))?;
    if values.len() != rows.as_array().map_or(0, Vec::len) {
        return Err(invalid(format!("{key} length must match analyses")));
    }
    for (row, value) in rows.as_array().unwrap().iter().zip(values) {
        let expression = if numeric {
            number(value).to_string()
        } else {
            sql(&text(value))
        };
        project.execute_sql(&format!(
            "UPDATE MASS_SPEC_ANALYSES SET {column} = {expression} WHERE analysis = {}",
            sql(&text(&row["analysis"]))
        ))?;
    }
    Ok(json!({"updated": values.len()}))
}

fn get_analysis_column(project: &Project, column: &str) -> Result<Value> {
    ensure_schema(project)?;
    let rows = project.query_json(&format!(
        "SELECT {column} FROM MASS_SPEC_ANALYSES ORDER BY analysis"
    ))?;
    Ok(Value::Array(
        rows.as_array()
            .map(|rows| rows.iter().map(|row| row[column].clone()).collect())
            .unwrap_or_default(),
    ))
}

fn get_spectra_headers_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    ensure_schema(project)?;
    let wanted = string_list(parameters, "analysis_names");
    let mut out = Vec::new();
    for row in analysis_rows(project)?
        .as_array()
        .cloned()
        .unwrap_or_default()
    {
        let analysis = text(&row["analysis"]);
        if !selected(&wanted, &analysis) {
            continue;
        }
        let mut reader =
            reader::Reader::open(text(&row["file_path"])).map_err(|e| invalid(e.to_string()))?;
        reader
            .select_analysis(row["analysis_index"].as_i64().unwrap_or(0) as usize)
            .map_err(|e| invalid(e.to_string()))?;
        for index in 0..reader.spectra().len() {
            let spectrum = if matches!(
                reader.format(),
                reader::Format::MzMl
                    | reader::Format::BrukerBaf
                    | reader::Format::BrukerTsf
                    | reader::Format::SciexWiff
                    | reader::Format::AgilentMassHunterD
                    | reader::Format::ThermoRaw
            ) {
                reader
                    .spectrum(index)
                    .cloned()
                    .ok_or_else(|| invalid(format!("spectrum index is out of range: {index}")))?
            } else {
                reader
                    .spectrum_data(index)
                    .map_err(|error| invalid(error.to_string()))?
            };
            out.push(json!({
                "analysis": analysis,
                "index": spectrum.index,
                "scan": spectrum.scan,
                "array_length": spectrum.array_length,
                "level": spectrum.level,
                "mode": spectrum.mode,
                "polarity": spectrum.polarity,
                "configuration": spectrum.configuration,
                "lowmz": spectrum.low_mz,
                "highmz": spectrum.high_mz,
                "bpmz": spectrum.base_peak_mz,
                "bpint": spectrum.base_peak_intensity,
                "tic": spectrum.tic,
                "rt": spectrum.retention_time,
                "mobility": spectrum.mobility,
                "window_mz": spectrum.window_mz,
                "window_mzlow": spectrum.window_mzlow,
                "window_mzhigh": spectrum.window_mzhigh,
                "precursor_mz": spectrum.precursor_mz,
                "precursor_intensity": spectrum.precursor_intensity,
                "precursor_charge": spectrum.precursor_charge,
                "activation_ce": spectrum.collision_energy
            }));
        }
    }
    Ok(Value::Array(out))
}

fn get_chromatograms_headers_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    ensure_schema(project)?;
    let wanted = string_list(parameters, "analysis_names");
    let mut out = Vec::new();
    for row in analysis_rows(project)?
        .as_array()
        .cloned()
        .unwrap_or_default()
    {
        let analysis = text(&row["analysis"]);
        if !selected(&wanted, &analysis) {
            continue;
        }
        let mut reader =
            reader::Reader::open(text(&row["file_path"])).map_err(|e| invalid(e.to_string()))?;
        reader
            .select_analysis(row["analysis_index"].as_i64().unwrap_or(0) as usize)
            .map_err(|e| invalid(e.to_string()))?;
        let chromatograms = reader.chromatograms().to_vec();
        for (index, chromatogram) in chromatograms.iter().enumerate() {
            let start_time = chromatogram.time.first().copied();
            let end_time = chromatogram.time.last().copied();
            let interval_ms = (chromatogram.time.len() > 1).then_some(chromatogram.interval_ms);
            out.push(json!({"analysis": analysis, "index": index, "chromatogram_id": chromatogram.id, "array_length": chromatogram.array_length, "polarity": chromatogram.polarity, "precursor_mz": chromatogram.precursor_mz.unwrap_or(0.0), "activation_ce": chromatogram.activation_ce.unwrap_or(0.0), "product_mz": chromatogram.product_mz.unwrap_or(0.0), "signal_type": chromatogram.signal_type, "chromatogram_type": chromatogram.chromatogram_type, "detector": chromatogram.detector, "channel": chromatogram.channel, "units": chromatogram.units, "wavelength_nm": chromatogram.wavelength_nm, "interval_ms": interval_ms, "start_time": start_time, "end_time": end_time, "intensity_multiplier": 1.0}));
        }
    }
    Ok(Value::Array(out))
}

fn get_spectra_tic_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    let levels = int_list(parameters, "levels");
    let replicates = analysis_rows(project)?
        .as_array()
        .into_iter()
        .flatten()
        .map(|row| (text(&row["analysis"]), text(&row["replicate"])))
        .collect::<std::collections::BTreeMap<_, _>>();
    Ok(Value::Array(
        get_spectra_headers_impl(project, parameters)?
            .as_array()
            .unwrap()
            .iter()
            .filter(|row| {
                (levels.is_empty() || levels.contains(&(row["level"].as_i64().unwrap_or(0) as i32)))
                    && in_range(number(&row["rt"]), parameters, "rt_min", "rt_max")
            })
            .map(|row| json!({"analysis": row["analysis"], "replicate": replicates.get(&text(&row["analysis"])).cloned().unwrap_or_default(), "polarity": row["polarity"], "level": row["level"], "rt": row["rt"], "mobility": row["mobility"], "tic": row["tic"], "bpmz": row["bpmz"], "bpint": row["bpint"]}))
            .collect(),
    ))
}

fn get_raw_spectra_impl(
    project: &mut Project,
    parameters: &Value,
    forced_level: Option<i32>,
) -> Result<Value> {
    ensure_schema(project)?;
    let query = normalize_targets(parameters);
    let mut out = Vec::new();
    for row in analysis_rows(project)?
        .as_array()
        .cloned()
        .unwrap_or_default()
    {
        let analysis = text(&row["analysis"]);
        if !query.targets.is_empty()
            && query
                .targets
                .iter()
                .all(|target| !target.analyses.is_empty())
            && query
                .targets
                .iter()
                .all(|target| !target.analyses.iter().any(|value| value == &analysis))
        {
            continue;
        }
        let mut reader =
            reader::Reader::open(text(&row["file_path"])).map_err(|e| invalid(e.to_string()))?;
        reader
            .select_analysis(row["analysis_index"].as_i64().unwrap_or(0) as usize)
            .map_err(|e| invalid(e.to_string()))?;
        let requested_indices = parameters
            .get("indices")
            .and_then(Value::as_array)
            .map(|values| {
                values
                    .iter()
                    .map(|value| {
                        value
                            .as_i64()
                            .ok_or_else(|| invalid("indices must contain integers"))
                            .and_then(|index| {
                                usize::try_from(index)
                                    .map_err(|_| invalid("indices must be non-negative"))
                            })
                    })
                    .collect::<Result<Vec<_>>>()
            })
            .transpose()?
            .unwrap_or_default();
        let indexed = !requested_indices.is_empty();
        let selected_indices = if indexed {
            for index in &requested_indices {
                if *index >= reader.spectra().len() {
                    return Err(invalid(format!("spectrum index is out of range: {index}")));
                }
            }
            requested_indices
        } else {
            (0..reader.spectra().len()).collect()
        };
        for index in selected_indices {
            let spectrum = reader.spectrum_data(index).map_err(|error| {
                invalid(format!(
                    "analysis={analysis} spectrum_index={index}: {error}"
                ))
            })?;
            if forced_level.is_some() && forced_level != Some(spectrum.level) {
                continue;
            }
            for (mz, intensity) in spectrum.mz.iter().zip(&spectrum.intensity) {
                let minimum_intensity = if spectrum.level == 1 {
                    target_number(parameters.get("min_intensity_ms1")).unwrap_or(0.0)
                } else {
                    target_number(parameters.get("min_intensity_ms2")).unwrap_or(0.0)
                };
                if (*intensity as f64) < minimum_intensity {
                    continue;
                }
                if indexed {
                    out.push(json!({"analysis": analysis, "replicate": row["replicate"], "target_id": format!("spectrum:{}", spectrum.index), "id": format!("{}:{}", analysis, spectrum.index), "polarity": spectrum.polarity, "level": spectrum.level, "pre_mz": spectrum.precursor_mz, "pre_mzlow": spectrum.window_mzlow, "pre_mzhigh": spectrum.window_mzhigh, "pre_ce": spectrum.collision_energy, "rt": spectrum.retention_time, "mobility": spectrum.mobility, "mz": mz, "intensity": intensity}));
                    continue;
                }
                let matches = query
                    .targets
                    .iter()
                    .filter(|target| {
                        target_matches(
                            target,
                            &analysis,
                            spectrum.polarity,
                            forced_level.unwrap_or(spectrum.level),
                            spectrum.retention_time,
                            *mz,
                        )
                    })
                    .collect::<Vec<_>>();
                for target in matches {
                    out.push(json!({"analysis": analysis, "replicate": row["replicate"], "target_id": target.id, "id": format!("{}:{}", analysis, spectrum.index), "polarity": spectrum.polarity, "level": spectrum.level, "pre_mz": spectrum.precursor_mz, "pre_mzlow": spectrum.window_mzlow, "pre_mzhigh": spectrum.window_mzhigh, "pre_ce": spectrum.collision_energy, "rt": spectrum.retention_time, "mobility": spectrum.mobility, "mz": mz, "intensity": intensity}));
                }
            }
        }
    }
    Ok(Value::Array(out))
}

fn get_raw_spectra_eic_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    let rows = get_raw_spectra_impl(project, parameters, Some(1))?;
    let mut summaries: std::collections::BTreeMap<
        (String, i32, String, String, u32),
        (Value, f64, usize, f64),
    > = std::collections::BTreeMap::new();
    for row in rows.as_array().cloned().unwrap_or_default() {
        let key = (
            text(&row["analysis"]),
            row["polarity"].as_i64().unwrap_or(0) as i32,
            text(&row["target_id"]),
            text(&row["id"]),
            number(&row["rt"]).to_bits(),
        );
        let entry = summaries
            .entry(key)
            .or_insert_with(|| (row.clone(), 0.0, 0, 0.0));
        entry.1 += number(&row["mz"]) as f64;
        entry.2 += 1;
        entry.3 += number(&row["mobility"]) as f64;
        if number(&row["intensity"]) > number(&entry.0["intensity"]) {
            entry.0["intensity"] = row["intensity"].clone();
        }
    }
    Ok(Value::Array(
        summaries
            .into_values()
            .map(|(mut row, mz_sum, count, mobility_sum)| {
                row["level"] = json!(1);
                row["mz"] = json!(mz_sum / count as f64);
                row["mobility"] = json!(mobility_sum / count as f64);
                row
            })
            .collect(),
    ))
}

fn merge_raw_spectra_rows(rows: Value, mz_clust: f64, presence: f64) -> Value {
    let mut groups: std::collections::BTreeMap<(String, String, i32), Vec<Value>> =
        std::collections::BTreeMap::new();
    for row in rows.as_array().cloned().unwrap_or_default() {
        let key = (
            text(&row["analysis"]),
            text(&row["id"]),
            row["polarity"].as_i64().unwrap_or(0) as i32,
        );
        groups.entry(key).or_default().push(row);
    }
    let tolerance = mz_clust.max(0.0);
    let threshold = presence.clamp(0.0, 1.0);
    let mut output = Vec::new();
    for (_, mut values) in groups {
        values.sort_by(|left, right| {
            (number(&left["mz"]) as f64).total_cmp(&(number(&right["mz"]) as f64))
        });
        let all_rt = values
            .iter()
            .map(|row| number(&row["rt"]).to_bits())
            .collect::<std::collections::BTreeSet<_>>();
        let mut start = 0;
        while start < values.len() {
            let mut end = start + 1;
            while end < values.len()
                && (number(&values[end]["mz"]) as f64) - (number(&values[end - 1]["mz"]) as f64)
                    <= tolerance
            {
                end += 1;
            }
            let cluster_rt = values[start..end]
                .iter()
                .map(|row| number(&row["rt"]).to_bits())
                .collect::<std::collections::BTreeSet<_>>();
            if threshold > 0.0
                && !all_rt.is_empty()
                && (cluster_rt.len() as f64) < threshold * (all_rt.len() as f64)
            {
                start = end;
                continue;
            }
            let mut row = values[start].clone();
            let mut intensity_sum = 0.0;
            let mut weighted_mz = 0.0;
            let mut rt_sum = 0.0;
            let mut mobility_sum = 0.0;
            let mut max_intensity: f64 = 0.0;
            for value in &values[start..end] {
                let intensity = number(&value["intensity"]) as f64;
                intensity_sum += intensity;
                weighted_mz += number(&value["mz"]) as f64 * intensity;
                rt_sum += number(&value["rt"]) as f64;
                mobility_sum += number(&value["mobility"]) as f64;
                max_intensity = max_intensity.max(intensity);
            }
            if intensity_sum > 0.0 {
                row["mz"] = json!(weighted_mz / intensity_sum);
                row["intensity"] = json!(max_intensity);
                row["rt"] = json!(rt_sum / (end - start) as f64);
                row["mobility"] = json!(mobility_sum / (end - start) as f64);
                output.push(row);
            }
            start = end;
        }
    }
    output.sort_by(|left, right| {
        text(&left["analysis"])
            .cmp(&text(&right["analysis"]))
            .then_with(|| text(&left["id"]).cmp(&text(&right["id"])))
            .then_with(|| (number(&left["mz"]) as f64).total_cmp(&(number(&right["mz"]) as f64)))
    });
    Value::Array(output)
}

fn get_chromatograms_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    crate::ensure_chromatograms_schema(project)?;
    let wanted = parameters
        .get("analysis_names")
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let filter = if wanted.is_empty() {
        String::new()
    } else {
        format!(
            " AND c.analysis IN ({})",
            wanted
                .iter()
                .map(|value| sql(value))
                .collect::<Vec<_>>()
                .join(",")
        )
    };
    project.query_json(&format!(
        "SELECT c.analysis, COALESCE(a.replicate, '') AS replicate, c.index, c.chromatogram_id, c.polarity, c.precursor_mz, c.activation_ce, c.product_mz, c.wavelength_nm, c.rt, c.raw_intensity, c.baseline, c.intensity FROM MASS_SPEC_CHROMATOGRAMS c JOIN MASS_SPEC_ANALYSES a ON a.analysis = c.analysis WHERE 1=1{} ORDER BY c.analysis, c.chromatogram_id, c.rt",
        filter
    ))
}

fn get_raw_chromatograms_impl(project: &mut Project, parameters: &Value) -> Result<Value> {
    ensure_schema(project)?;
    let wanted = parameters
        .get("analysis_names")
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let indices = parameters
        .get("indices")
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_u64)
                .map(|value| value as usize)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let rows = project.query_json(&format!(
                "SELECT analysis, file_path, analysis_index, COALESCE(replicate, '') AS replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"
            ))?;
    let mut output = Vec::new();
    for row in rows.as_array().into_iter().flatten() {
        let analysis = row["analysis"].as_str().unwrap_or_default();
        if !wanted.is_empty() && !wanted.iter().any(|value| value == analysis) {
            continue;
        }
        let replicate = row["replicate"].clone();
        let mut file = reader::Reader::open(row["file_path"].as_str().unwrap_or_default())
            .map_err(|error| Error::new(ErrorCode::InvalidArgument, error.to_string()))?;
        file.select_analysis(row["analysis_index"].as_i64().unwrap_or(0) as usize)
            .map_err(|error| Error::new(ErrorCode::InvalidArgument, error.to_string()))?;
        let selected = if indices.is_empty() {
            (0..file.chromatograms().len()).collect::<Vec<_>>()
        } else {
            indices
                .iter()
                .copied()
                .filter(|index| *index < file.chromatograms().len())
                .collect()
        };
        let chromatograms = file
            .chromatograms_data(&selected)
            .map_err(|error| Error::new(ErrorCode::InvalidArgument, error.to_string()))?;
        for (index, chromatogram) in selected.iter().zip(chromatograms.iter()) {
            for (rt, intensity) in chromatogram.time.iter().zip(&chromatogram.intensity) {
                output.push(json!({
                    "analysis": analysis,
                    "replicate": replicate,
                    "index": index,
                    "chromatogram_id": chromatogram.id,
                    "polarity": chromatogram.polarity,
                    "precursor_mz": chromatogram.precursor_mz.unwrap_or(0.0),
                    "activation_ce": chromatogram.activation_ce.unwrap_or(0.0),
                    "product_mz": chromatogram.product_mz.unwrap_or(0.0),
                    "wavelength_nm": chromatogram.wavelength_nm,
                    "rt": *rt as f64,
                    "raw_intensity": *intensity as f64,
                    "baseline": 0.0,
                    "intensity": *intensity as f64,
                }));
            }
        }
    }
    Ok(Value::Array(output))
}

fn get_features_impl(project: &mut Project, p: &Value) -> Result<Value> {
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(20.0);
    let rt_tolerance = p
        .get("rt_tolerance")
        .and_then(Value::as_f64)
        .unwrap_or(60.0);
    if ppm < 0.0 || rt_tolerance < 0.0 {
        return Err(invalid("ppm and rt_tolerance must be non-negative"));
    }
    let list = |value: &Value| {
        value
            .as_array()
            .map(|v| {
                v.iter()
                    .filter_map(Value::as_i64)
                    .map(|x| x as i32)
                    .collect()
            })
            .unwrap_or_else(|| vec![value.as_i64().unwrap_or(0) as i32])
    };
    let analyses = string_list(p, "analysis_names");
    let mut targets = p
        .get("targets")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_else(|| vec![json!({})]);
    if targets.is_empty() {
        targets.push(json!({}));
    }
    let mut target_filters = Vec::new();
    for target in targets {
        let mut matchers = Vec::new();
        let target_analyses = target
            .get("analyses")
            .map(|v| {
                if v.is_array() {
                    v.as_array()
                        .unwrap()
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_owned)
                        .collect()
                } else {
                    vec![text(v)]
                }
            })
            .unwrap_or_else(|| analyses.clone());
        if !target_analyses.is_empty() {
            matchers.push(format!(
                "analysis IN ({})",
                target_analyses
                    .iter()
                    .map(|v| sql(v))
                    .collect::<Vec<_>>()
                    .join(",")
            ));
        }
        let polarities = target
            .get("polarity")
            .or_else(|| p.get("polarity"))
            .map(list)
            .unwrap_or_default();
        if !polarities.is_empty() {
            matchers.push(format!(
                "polarity IN ({})",
                polarities
                    .iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join(",")
            ));
        }
        for (column, exact, minimum, maximum) in [
            ("mass", "mass", "mass_min", "mass_max"),
            ("mz", "mz", "mz_min", "mz_max"),
        ] {
            if let Some(center) = target.get(exact).and_then(Value::as_f64) {
                let delta = center.abs() * ppm / 1e6;
                matchers.push(format!(
                    "{column} BETWEEN {} AND {}",
                    center - delta,
                    center + delta
                ));
            } else if target.get(minimum).is_some() || target.get(maximum).is_some() {
                let lo = target
                    .get(minimum)
                    .and_then(Value::as_f64)
                    .map_or("-1e300".into(), |v| v.to_string());
                let hi = target
                    .get(maximum)
                    .and_then(Value::as_f64)
                    .map_or("1e300".into(), |v| v.to_string());
                matchers.push(format!("{column} BETWEEN {lo} AND {hi}"));
            }
        }
        let has_mass = target.get("mass").is_some()
            || target.get("mass_min").is_some()
            || target.get("mass_max").is_some();
        let has_mz = target.get("mz").is_some()
            || target.get("mz_min").is_some()
            || target.get("mz_max").is_some();
        if !has_mass && !has_mz && (target.get("SMILES").is_some() || target.get("InChI").is_some())
        {
            let normalized = crate::nta_suspect_screening::normalize_structure(
                target.get("SMILES").and_then(Value::as_str).unwrap_or(""),
                target.get("InChI").and_then(Value::as_str).unwrap_or(""),
            );
            if normalized.ok && normalized.exact_mass > 0.0 {
                let delta = normalized.exact_mass * ppm / 1e6;
                matchers.push(format!(
                    "mass BETWEEN {} AND {}",
                    normalized.exact_mass - delta,
                    normalized.exact_mass + delta
                ));
            }
        }
        if let Some(center) = target.get("rt").and_then(Value::as_f64) {
            matchers.push(format!(
                "rt BETWEEN {} AND {}",
                center - rt_tolerance,
                center + rt_tolerance
            ));
        } else if target.get("rt_min").is_some() || target.get("rt_max").is_some() {
            let lo = target
                .get("rt_min")
                .and_then(Value::as_f64)
                .map_or("-1e300".into(), |v| v.to_string());
            let hi = target
                .get("rt_max")
                .and_then(Value::as_f64)
                .map_or("1e300".into(), |v| v.to_string());
            matchers.push(format!("rt BETWEEN {lo} AND {hi}"));
        }
        if !matchers.is_empty() {
            target_filters.push(format!("({})", matchers.join(" AND ")));
        }
    }
    let include_filtered = p.get("filtered").and_then(Value::as_bool).unwrap_or(false);
    let mut query = "SELECT * FROM MASS_SPEC_NTA_FEATURES".to_owned();
    if !include_filtered {
        query.push_str(" WHERE filtered = FALSE");
    }
    if !target_filters.is_empty() {
        query.push_str(if include_filtered { " WHERE " } else { " AND " });
        query.push_str(&format!("({})", target_filters.join(" OR ")));
    }
    query.push_str(" ORDER BY analysis, rt, feature");
    project.query_json(&query)
}

/// Shared executor for the NTA table query operations (suspects, internal
/// standards, transformation products): mirrors `get_features_impl` with the
/// target table's own columns. `mass_columns`/`rt_columns` name the columns
/// the table actually carries (suspects/IS: db_mass/exp_mass, db_rt/exp_rt;
/// transformation products: mass only, no rt column), so matchers are only
/// emitted for columns that exist; `has_polarity` drops the polarity matcher
/// for tables without a polarity column (transformation products).
fn query_nta_table_impl(
    project: &mut Project,
    p: &Value,
    table: &str,
    order_by: &str,
    mass_columns: &[&str],
    rt_columns: &[&str],
    has_polarity: bool,
) -> Result<Value> {
    let ppm = p.get("ppm").and_then(Value::as_f64).unwrap_or(20.0);
    let rt_tolerance = p
        .get("rt_tolerance")
        .and_then(Value::as_f64)
        .unwrap_or(60.0);
    if ppm < 0.0 || rt_tolerance < 0.0 {
        return Err(invalid("ppm and rt_tolerance must be non-negative"));
    }
    let list = |value: &Value| {
        value
            .as_array()
            .map(|v| {
                v.iter()
                    .filter_map(Value::as_i64)
                    .map(|x| x as i32)
                    .collect()
            })
            .unwrap_or_else(|| vec![value.as_i64().unwrap_or(0) as i32])
    };
    let analyses = string_list(p, "analysis_names");
    let mut targets = p
        .get("targets")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_else(|| vec![json!({})]);
    if targets.is_empty() {
        targets.push(json!({}));
    }
    let mut target_filters = Vec::new();
    for target in targets {
        let mut matchers = Vec::new();
        let target_analyses = target
            .get("analyses")
            .map(|v| {
                if v.is_array() {
                    v.as_array()
                        .unwrap()
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_owned)
                        .collect()
                } else {
                    vec![text(v)]
                }
            })
            .unwrap_or_else(|| analyses.clone());
        if !target_analyses.is_empty() {
            matchers.push(format!(
                "analysis IN ({})",
                target_analyses
                    .iter()
                    .map(|v| sql(v))
                    .collect::<Vec<_>>()
                    .join(",")
            ));
        }
        if has_polarity {
            let polarities = target
                .get("polarity")
                .or_else(|| p.get("polarity"))
                .map(list)
                .unwrap_or_default();
            if !polarities.is_empty() {
                matchers.push(format!(
                    "polarity IN ({})",
                    polarities
                        .iter()
                        .map(ToString::to_string)
                        .collect::<Vec<_>>()
                        .join(",")
                ));
            }
        }
        let has_mass = target.get("mass").is_some()
            || target.get("mass_min").is_some()
            || target.get("mass_max").is_some();
        let has_mz = target.get("mz").is_some()
            || target.get("mz_min").is_some()
            || target.get("mz_max").is_some();
        // Mass window (±ppm) and explicit [min,max] bounds on every mass column
        // the target table carries. These tables have no mz column, so an
        // mz-only target matches nothing (mirrors dropping the polarity
        // matcher for columns the table does not have).
        for column in mass_columns {
            if let Some(center) = target.get("mass").and_then(Value::as_f64) {
                let delta = center.abs() * ppm / 1e6;
                matchers.push(format!(
                    "{column} BETWEEN {} AND {}",
                    center - delta,
                    center + delta
                ));
            } else if target.get("mass_min").is_some() || target.get("mass_max").is_some() {
                let lo = target
                    .get("mass_min")
                    .and_then(Value::as_f64)
                    .map_or("-1e300".into(), |v| v.to_string());
                let hi = target
                    .get("mass_max")
                    .and_then(Value::as_f64)
                    .map_or("1e300".into(), |v| v.to_string());
                matchers.push(format!("{column} BETWEEN {lo} AND {hi}"));
            }
        }
        if !has_mass && !has_mz && (target.get("SMILES").is_some() || target.get("InChI").is_some())
        {
            let normalized = crate::nta_suspect_screening::normalize_structure(
                target.get("SMILES").and_then(Value::as_str).unwrap_or(""),
                target.get("InChI").and_then(Value::as_str).unwrap_or(""),
            );
            if normalized.ok && normalized.exact_mass > 0.0 {
                let delta = normalized.exact_mass * ppm / 1e6;
                for column in mass_columns {
                    matchers.push(format!(
                        "{column} BETWEEN {} AND {}",
                        normalized.exact_mass - delta,
                        normalized.exact_mass + delta
                    ));
                }
            }
        }
        // RT window (±rt_tolerance) on every rt column the table carries.
        if let Some(center) = target.get("rt").and_then(Value::as_f64) {
            for column in rt_columns {
                matchers.push(format!(
                    "{column} BETWEEN {} AND {}",
                    center - rt_tolerance,
                    center + rt_tolerance
                ));
            }
        } else if target.get("rt_min").is_some() || target.get("rt_max").is_some() {
            let lo = target
                .get("rt_min")
                .and_then(Value::as_f64)
                .map_or("-1e300".into(), |v| v.to_string());
            let hi = target
                .get("rt_max")
                .and_then(Value::as_f64)
                .map_or("1e300".into(), |v| v.to_string());
            for column in rt_columns {
                matchers.push(format!("{column} BETWEEN {lo} AND {hi}"));
            }
        }
        if !matchers.is_empty() {
            target_filters.push(format!("({})", matchers.join(" AND ")));
        }
    }
    let mut query = format!("SELECT * FROM {table} WHERE ");
    if !target_filters.is_empty() {
        query.push_str(&format!(" AND ({})", target_filters.join(" OR ")));
    }
    query.push_str(" ORDER BY analysis");
    if !order_by.is_empty() {
        query.push_str(&format!(", {order_by}"));
    }
    project.query_json(&query)
}

fn get_suspects_impl(project: &mut Project, p: &Value) -> Result<Value> {
    query_nta_table_impl(
        project,
        p,
        "MASS_SPEC_NTA_SUSPECTS",
        "feature",
        &["db_mass", "exp_mass"],
        &["db_rt", "exp_rt"],
        true,
    )
}

fn get_internal_standards_impl(project: &mut Project, p: &Value) -> Result<Value> {
    query_nta_table_impl(
        project,
        p,
        "MASS_SPEC_NTA_INTERNAL_STANDARDS",
        "feature",
        &["db_mass", "exp_mass"],
        &["db_rt", "exp_rt"],
        true,
    )
}

fn get_transformation_products_impl(project: &mut Project, p: &Value) -> Result<Value> {
    query_nta_table_impl(
        project,
        p,
        "MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS",
        "feature_group",
        &["mass"],
        &[],
        false,
    )
}

#[allow(dead_code)]
fn parameter_example(name: &str) -> Option<Value> {
    match name {
        "analysis_names" => Some(json!(["sample-r001"])),
        "levels" => Some(json!([1, 2])),
        "targets" => {
            Some(json!([{"id": "caffeine", "mass": 194.0804, "rt": 1020.0, "polarity": 1}]))
        }
        "indices" => Some(json!([0, 2])),
        "polarity" => Some(json!(1)),
        "ppm" => Some(json!(20.0)),
        "rt_tolerance" => Some(json!(60.0)),
        "charge" => Some(json!(1)),
        "rt_min" => Some(json!(900.0)),
        "rt_max" => Some(json!(1200.0)),
        "mz_clust" => Some(json!(0.003)),
        "presence" => Some(json!(0.8)),
        "isolation_window" => Some(json!(1.3)),
        "min_intensity_ms1" => Some(json!(1000.0)),
        "min_intensity_ms2" => Some(json!(100.0)),
        "analyses" => Some(json!([{"path": "data/sample.mzML", "replicate_name": "r1"}])),
        "replicate_names" | "blank_names" => Some(json!(["r1"])),
        "concentrations" => Some(json!([1.0])),
        _ => None,
    }
}

#[allow(dead_code)]
fn parameter(
    name: &str,
    description: &str,
    kind: TypeDescriptor,
    default: Option<Value>,
) -> ParameterDefinition {
    ParameterDefinition {
        name: name.into(),
        description: description.into(),
        kind,
        default,
        required: false,
        example: parameter_example(name),
    }
}

fn ontology_entry(id: &str) -> Value {
    streamfind_rust_core::catalogue::entries()
        .iter()
        .find(|entry| entry["canonical_id"] == id)
        .cloned()
        .unwrap_or_else(|| panic!("missing ontology operation: {id}"))
}

fn ontology_parameters(id: &str) -> ParameterSchema {
    let entry = ontology_entry(id);
    ParameterSchema {
        definitions: entry["parameters"]
            .as_array()
            .unwrap()
            .iter()
            .map(|item| ParameterDefinition {
                name: item["name"].as_str().unwrap().into(),
                description: item["description"].as_str().unwrap_or_default().into(),
                kind: TypeDescriptor::from_json(&item["schema"]).unwrap(),
                default: (!item["default"].is_null()).then(|| item["default"].clone()),
                required: item["required"].as_bool().unwrap_or(false),
                example: (!item["example"].is_null()).then(|| item["example"].clone()),
            })
            .collect(),
    }
}

fn ontology_description(id: &str) -> String {
    ontology_entry(id)["definition"]
        .as_str()
        .unwrap_or_default()
        .into()
}

fn configure_method(mut method: Method, id: &str) -> Method {
    let entry = ontology_entry(id);
    method.cacheable = entry["cacheable"].as_bool().unwrap_or(false);
    method.writes = entry["effects"]["writes"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect();
    method.required_methods = entry["required_methods"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect();
    method.single_occurrence = entry["single_occurrence"].as_bool().unwrap_or(false);
    if let Some(validator) = nta_validator(id) {
        method = method.with_validator(validator);
    }
    method
}

fn ontology_result_schema(id: &str) -> Value {
    ontology_entry(id)["result"]["schema"].clone()
}

fn table_result(id: &str, result: Result<Value>) -> Result<Value> {
    let rows = result?;
    let schema = ontology_result_schema(id);
    if schema["type"] != "table" {
        return Ok(rows);
    }
    let rows = rows
        .as_array()
        .ok_or_else(|| invalid("table result must be an array of rows"))?;
    let mut columns = serde_json::Map::new();
    if let Some(properties) = schema["properties"].as_object() {
        for name in properties.keys() {
            columns.insert(name.clone(), Value::Array(Vec::with_capacity(rows.len())));
        }
    }
    for row in rows {
        for (name, values) in &mut columns {
            values
                .as_array_mut()
                .unwrap()
                .push(row.get(name).cloned().unwrap_or(Value::Null));
        }
    }
    Ok(json!({"row_count": rows.len(), "columns": columns}))
}

#[allow(dead_code)]
fn data_parameters(operation: &str) -> ParameterSchema {
    return ontology_parameters(operation);
    #[allow(unreachable_code)]
    {
        let mut definitions = vec![parameter(
            "analysis_names",
            "Optional analysis names",
            TypeDescriptor::array(TypeDescriptor::scalar(ParameterType::String)),
            Some(json!([])),
        )];
        if operation.ends_with("get_spectra_tic") {
            definitions.extend([
                parameter(
                    "levels",
                    "Optional MS levels",
                    TypeDescriptor::array(TypeDescriptor::scalar(ParameterType::Integer)),
                    Some(json!([])),
                ),
                parameter(
                    "rt_min",
                    "Minimum retention time",
                    TypeDescriptor::scalar(ParameterType::Real),
                    None,
                ),
                parameter(
                    "rt_max",
                    "Maximum retention time",
                    TypeDescriptor::scalar(ParameterType::Real),
                    None,
                ),
            ]);
        } else if operation.ends_with("get_chromatograms")
            && !operation.ends_with("get_chromatograms_headers")
        {
            definitions.push(parameter(
                "indices",
                "Optional zero-based row indices",
                TypeDescriptor::array(TypeDescriptor::scalar(ParameterType::Integer)),
                Some(json!([])),
            ));
        } else if operation.contains("get_raw_spectra") {
            definitions.extend([
                parameter(
                    "levels",
                    "Optional MS levels",
                    TypeDescriptor::array(TypeDescriptor::scalar(ParameterType::Integer)),
                    Some(json!([])),
                ),
                parameter(
                    "targets",
                    "Independent mass spectrometry target ranges",
                    TypeDescriptor::array(TypeDescriptor::scalar(ParameterType::Object)),
                    None,
                ),
                parameter(
                    "polarity",
                    "Optional ion polarity (-1 or 1)",
                    TypeDescriptor::scalar(ParameterType::Integer),
                    None,
                ),
                parameter(
                    "ppm",
                    "Mass tolerance in parts per million",
                    TypeDescriptor::scalar(ParameterType::Real),
                    None,
                ),
                parameter(
                    "rt_tolerance",
                    "Retention-time tolerance",
                    TypeDescriptor::scalar(ParameterType::Real),
                    None,
                ),
                parameter(
                    "charge",
                    "Ion charge used for mass conversion",
                    TypeDescriptor::scalar(ParameterType::Integer),
                    None,
                ),
            ]);
            if operation.ends_with("get_raw_spectra_ms1") {
                definitions.extend([
                    parameter(
                        "mz_clust",
                        "m/z clustering tolerance",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.003)),
                    ),
                    parameter(
                        "presence",
                        "Minimum trace presence fraction",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.8)),
                    ),
                    parameter(
                        "min_intensity_ms1",
                        "Minimum MS1 peak intensity",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.0)),
                    ),
                ]);
            } else if operation.ends_with("get_raw_spectra_ms2") {
                definitions.extend([
                    parameter(
                        "isolation_window",
                        "Precursor isolation window width",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(1.3)),
                    ),
                    parameter(
                        "mz_clust",
                        "m/z clustering tolerance",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.005)),
                    ),
                    parameter(
                        "presence",
                        "Minimum trace presence fraction",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.0)),
                    ),
                    parameter(
                        "min_intensity_ms2",
                        "Minimum MS2 peak intensity",
                        TypeDescriptor::scalar(ParameterType::Real),
                        Some(json!(0.0)),
                    ),
                ]);
            }
        } else if operation.ends_with("get_spectra_headers")
            || operation.ends_with("get_chromatograms_headers")
        {
            // Headers accept analysis selection only.
        }
        ParameterSchema { definitions }
    }
}

pub fn register_operations(registry: &mut OperationRegistry) -> Result<()> {
    registry.register(Operation::new(
        "mass_spec.add_analyses",
        "mass_spec.add_analyses",
        ontology_description("mass_spec.add_analyses"),
        "mass_spec",
        ontology_parameters("mass_spec.add_analyses"),
        Box::new(|project, parameters| {
            table_result("mass_spec.add_analyses", add_analyses(project, parameters))
        }),
    ))?;
    registry.register(Operation::new(
        "mass_spec.remove_analyses",
        "mass_spec.remove_analyses",
        ontology_description("mass_spec.remove_analyses"),
        "mass_spec",
        ontology_parameters("mass_spec.remove_analyses"),
        Box::new(remove_analyses),
    ))?;
    registry.register(Operation::new(
        "mass_spec.get_analyses_info",
        "mass_spec.get_analyses_info",
        ontology_description("mass_spec.get_analyses_info"),
        "mass_spec",
        ParameterSchema {
            definitions: vec![],
        },
        Box::new(|project, _| {
            table_result("mass_spec.get_analyses_info", get_analyses_info(project))
        }),
    ))?;
    for id in [
        "mass_spec.get_spectra_headers",
        "mass_spec.get_chromatograms_headers",
        "mass_spec.get_spectra_tic",
        "mass_spec.get_raw_spectra",
        "mass_spec.get_raw_spectra_eic",
        "mass_spec.get_raw_spectra_ms1",
        "mass_spec.get_raw_spectra_ms2",
        "mass_spec.get_chromatograms",
        "mass_spec.get_raw_chromatograms",
        "mass_spec.get_features",
        "mass_spec.get_suspects",
        "mass_spec.get_internal_standards",
        "mass_spec.get_transformation_products",
    ] {
        registry.register(Operation::new(
            id,
            id,
            ontology_description(id),
            "mass_spec",
            ontology_parameters(id),
            Box::new(move |project, parameters| {
                table_result(
                    id,
                    match id {
                        "mass_spec.get_spectra_headers" => {
                            get_spectra_headers_impl(project, parameters)
                        }
                        "mass_spec.get_chromatograms_headers" => {
                            get_chromatograms_headers_impl(project, parameters)
                        }
                        "mass_spec.get_spectra_tic" => get_spectra_tic_impl(project, parameters),
                        "mass_spec.get_raw_spectra" => {
                            get_raw_spectra_impl(project, parameters, None)
                        }
                        "mass_spec.get_raw_spectra_eic" => {
                            get_raw_spectra_eic_impl(project, parameters)
                        }
                        "mass_spec.get_raw_spectra_ms1" => Ok(merge_raw_spectra_rows(
                            get_raw_spectra_impl(project, parameters, Some(1))?,
                            target_number(parameters.get("mz_clust")).unwrap_or(0.003),
                            target_number(parameters.get("presence")).unwrap_or(0.8),
                        )),
                        "mass_spec.get_raw_spectra_ms2" => Ok(merge_raw_spectra_rows(
                            get_raw_spectra_impl(project, parameters, Some(2))?,
                            target_number(parameters.get("mz_clust")).unwrap_or(0.005),
                            target_number(parameters.get("presence")).unwrap_or(0.0),
                        )),
                        "mass_spec.get_chromatograms" => {
                            get_chromatograms_impl(project, parameters)
                        }
                        "mass_spec.get_raw_chromatograms" => {
                            get_raw_chromatograms_impl(project, parameters)
                        }
                        "mass_spec.get_features" => get_features_impl(project, parameters),
                        "mass_spec.get_suspects" => get_suspects_impl(project, parameters),
                        "mass_spec.get_internal_standards" => {
                            get_internal_standards_impl(project, parameters)
                        }
                        "mass_spec.get_transformation_products" => {
                            get_transformation_products_impl(project, parameters)
                        }
                        _ => unreachable!(),
                    },
                )
            }),
        ))?;
    }
    for (id, column) in [
        ("mass_spec.get_analysis_names", "analysis"),
        ("mass_spec.get_replicate_names", "replicate"),
        ("mass_spec.get_blank_names", "blank"),
        ("mass_spec.get_concentrations", "concentration"),
    ] {
        registry.register(Operation::new(
            id,
            id,
            ontology_description(id),
            "mass_spec",
            ontology_parameters(id),
            Box::new(move |project, _| get_analysis_column(project, column)),
        ))?;
    }
    for (id, key, column, numeric) in [
        (
            "mass_spec.set_replicate_names",
            "replicate_names",
            "replicate",
            false,
        ),
        ("mass_spec.set_blank_names", "blank_names", "blank", false),
        (
            "mass_spec.set_concentrations",
            "concentrations",
            "concentration",
            true,
        ),
    ] {
        registry.register(Operation::new(
            id,
            id,
            ontology_description(id),
            "mass_spec",
            ontology_parameters(id),
            Box::new(move |project, parameters| {
                update_values(project, parameters, key, column, numeric)
            }),
        ))?;
    }
    Ok(())
}

pub fn register_methods(registry: &mut MethodRegistry) -> Result<()> {
    streamfind_rust_core::catalogue::register_methods_from_catalogue_with(
        registry,
        "mass_spec",
        |entry| ontology_parameters(entry["canonical_id"].as_str().unwrap_or_default()),
        |id| match id {
            "mass_spec.load_chromatograms" => Some(Box::new(|project, parameters| {
                processing_methods_chromatograms::load_chromatograms(
                    project,
                    &processing_methods_chromatograms::LoadChromatogramsRequest {
                        analyses: string_list(parameters, "analysis_names"),
                        chromatogram_id_regex: string_list(parameters, "chromatogram_id_regex"),
                        ignore_case: parameters
                            .get("ignore_case")
                            .and_then(Value::as_bool)
                            .unwrap_or(true),
                        invert: parameters
                            .get("invert")
                            .and_then(Value::as_bool)
                            .unwrap_or(false),
                    },
                )
                .map(|_| json!({"status": "finished", "info": "Chromatograms loaded."}))
            })),
            "mass_spec.filter_chromatograms_retention_time" => {
                Some(Box::new(|project, parameters| {
                    processing_methods_chromatograms::filter_chromatograms_retention_time(
                    project,
                    &processing_methods_chromatograms::FilterChromatogramsRetentionTimeRequest {
                        analyses: string_list(parameters, "analysis_names"),
                        rtmin: number(parameters.get("rt_min").unwrap_or(&Value::Null)) as f64,
                        rtmax: number(parameters.get("rt_max").unwrap_or(&Value::Null)) as f64,
                    },
                )
                .map(|_| json!({"status": "finished", "info": "Chromatograms filtered by retention time."}))
                }))
            }
            "mass_spec.find_features" => Some(Box::new(processing_methods_nta::find_features)),
            "mass_spec.load_features_ms1" => {
                Some(Box::new(processing_methods_nta::load_features_ms1))
            }
            "mass_spec.load_features_ms2" => {
                Some(Box::new(processing_methods_nta::load_features_ms2))
            }
            "mass_spec.subtract_blank" => Some(Box::new(processing_methods_nta::subtract_blank)),
            "mass_spec.filter_features" => Some(Box::new(processing_methods_nta::filter_features)),
            "mass_spec.filter_features_ms2" => {
                Some(Box::new(processing_methods_nta::filter_features_ms2))
            }
            "mass_spec.group_features" => Some(Box::new(processing_methods_nta::group_features)),
            "mass_spec.fill_features" => Some(Box::new(processing_methods_nta::fill_features)),
            "mass_spec.create_components" => {
                Some(Box::new(processing_methods_nta::create_components))
            }
            "mass_spec.annotate_components" => {
                Some(Box::new(processing_methods_nta::annotate_components))
            }
            "mass_spec.suspect_screening" => {
                Some(Box::new(processing_methods_nta::suspect_screening))
            }
            "mass_spec.find_internal_standards" => {
                Some(Box::new(processing_methods_nta::find_internal_standards))
            }
            "mass_spec.filter_suspects" => Some(Box::new(processing_methods_nta::filter_suspects)),
            "mass_spec.filter_internal_standards" => {
                Some(Box::new(processing_methods_nta::filter_internal_standards))
            }
            "mass_spec.correct_matrix_suppression" => {
                Some(Box::new(processing_methods_nta::correct_matrix_suppression))
            }
            "mass_spec.assign_transformation_products" => Some(Box::new(
                nta_transformation_products::assign_transformation_products,
            )),
            "mass_spec.metfrag_screening" => Some(Box::new(nta_metfrag::metfrag_screening)),
            _ => None,
        },
        |method| {
            let id = method.id.clone();
            configure_method(method, &id)
        },
    )
}

fn invalid_message(id: &str, reason: impl std::fmt::Display) -> Error {
    Error::new(
        ErrorCode::WorkflowValidation,
        format!("{id}: invalid parameters: {reason}"),
    )
}

/// Check a numeric parameter when present: `ok` must hold for a valid value.
fn check_number(
    p: &Value,
    id: &str,
    name: &str,
    ok: impl Fn(f64) -> bool,
    expected: &str,
) -> Result<()> {
    if let Some(value) = p.get(name).and_then(Value::as_f64) {
        if !ok(value) {
            return Err(invalid_message(id, format!("{name} must be {expected}")));
        }
    }
    Ok(())
}

fn ge0(p: &Value, id: &str, name: &str) -> Result<()> {
    check_number(p, id, name, |v| v >= 0.0, ">= 0")
}

fn gt0(p: &Value, id: &str, name: &str) -> Result<()> {
    check_number(p, id, name, |v| v > 0.0, "> 0")
}

fn ge1(p: &Value, id: &str, name: &str) -> Result<()> {
    check_number(p, id, name, |v| v >= 1.0, ">= 1")
}

fn in_closed(p: &Value, id: &str, name: &str, lo: f64, hi: f64) -> Result<()> {
    check_number(
        p,
        id,
        name,
        |v| (lo..=hi).contains(&v),
        &format!("in [{lo}, {hi}]"),
    )
}

fn in_unit_interval(p: &Value, id: &str, name: &str) -> Result<()> {
    in_closed(p, id, name, 0.0, 1.0)
}

fn require_pair_array(p: &Value, id: &str, name: &str) -> Result<()> {
    if let Some(array) = p.get(name).and_then(Value::as_array) {
        if array.len() != 2 {
            return Err(invalid_message(
                id,
                format!("{name} must be a two-element array"),
            ));
        }
    }
    Ok(())
}

/// Shared `targets` validation for suspect_screening / find_internal_standards.
fn validate_targets(p: &Value, id: &str) -> Result<()> {
    let Some(targets) = p.get("targets") else {
        return Ok(());
    };
    let Some(targets) = targets.as_array() else {
        return Err(invalid_message(id, "targets must be an array"));
    };
    for (index, target) in targets.iter().enumerate() {
        let Some(target) = target.as_object() else {
            return Err(invalid_message(
                id,
                format!("targets[{index}] must be an object"),
            ));
        };
        let has_id = target.get("id").map_or(false, Value::is_string);
        let has_name = target.get("name").map_or(false, Value::is_string);
        if !has_id && !has_name {
            return Err(invalid_message(
                id,
                format!("targets[{index}] must have a string \"id\" or \"name\""),
            ));
        }
        let has_identity = ["mass", "mz"]
            .iter()
            .any(|key| target.get(*key).map_or(false, Value::is_number))
            || ["formula", "SMILES", "InChI"]
                .iter()
                .any(|key| target.get(*key).map_or(false, Value::is_string));
        if !has_identity {
            return Err(invalid_message(
                id,
                format!(
                    "targets[{index}] must provide at least one of \"mass\", \"mz\", \"formula\", \"SMILES\", \"InChI\""
                ),
            ));
        }
        for (mz_key, intensity_key) in [
            ("fragments_mz_pos", "fragments_intensity_pos"),
            ("fragments_mz_neg", "fragments_intensity_neg"),
        ] {
            if target.contains_key(mz_key) {
                let Some(mz) = target.get(mz_key).and_then(Value::as_array) else {
                    return Err(invalid_message(
                        id,
                        format!("targets[{index}].{mz_key} must be an array"),
                    ));
                };
                let Some(intensity) = target.get(intensity_key).and_then(Value::as_array) else {
                    return Err(invalid_message(
                        id,
                        format!("targets[{index}].{intensity_key} must be an array"),
                    ));
                };
                if mz.len() != intensity.len() {
                    return Err(invalid_message(
                        id,
                        format!(
                            "targets[{index}].{mz_key} and targets[{index}].{intensity_key} must have the same length"
                        ),
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_find_features(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.find_features";
    gt0(p, ID, "ppm_threshold")?;
    ge0(p, ID, "noise_threshold")?;
    gt0(p, ID, "min_snr")?;
    ge1(p, ID, "min_traces")?;
    gt0(p, ID, "baseline_window")?;
    gt0(p, ID, "max_feature_width")?;
    in_unit_interval(p, ID, "base_quantile")?;
    if p.get("rt_windows_min").is_some() || p.get("rt_windows_max").is_some() {
        let (Some(min), Some(max)) = (
            p.get("rt_windows_min").and_then(Value::as_array),
            p.get("rt_windows_max").and_then(Value::as_array),
        ) else {
            return Err(invalid_message(
                ID,
                "rt_windows_min and rt_windows_max must be provided together",
            ));
        };
        if min.len() != max.len() {
            return Err(invalid_message(
                ID,
                "rt_windows_min and rt_windows_max must have the same length",
            ));
        }
        for (min, max) in min.iter().zip(max.iter()) {
            let (Some(min), Some(max)) = (min.as_f64(), max.as_f64()) else {
                return Err(invalid_message(
                    ID,
                    "rt_windows_min and rt_windows_max must contain numbers",
                ));
            };
            if min > max {
                return Err(invalid_message(
                    ID,
                    "rt_windows_min values must not exceed rt_windows_max values",
                ));
            }
        }
    }
    Ok(())
}

fn validate_load_features_ms1(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.load_features_ms1";
    require_pair_array(p, ID, "rt_window")?;
    require_pair_array(p, ID, "mz_window")?;
    ge0(p, ID, "min_traces_intensity")?;
    ge0(p, ID, "mz_clust")?;
    in_unit_interval(p, ID, "presence")?;
    Ok(())
}

fn validate_load_features_ms2(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.load_features_ms2";
    require_pair_array(p, ID, "rt_window")?;
    require_pair_array(p, ID, "mz_window")?;
    ge0(p, ID, "min_traces_intensity")?;
    ge0(p, ID, "mz_clust")?;
    in_unit_interval(p, ID, "presence")?;
    gt0(p, ID, "isolation_window")?;
    Ok(())
}

fn validate_subtract_blank(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.subtract_blank";
    ge0(p, ID, "blank_threshold")?;
    ge0(p, ID, "rt_expand")?;
    ge0(p, ID, "mz_expand")?;
    Ok(())
}

fn validate_filter_features(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.filter_features";
    for name in [
        "min_sn",
        "min_intensity",
        "min_area",
        "min_width",
        "max_width",
        "max_ppm",
        "min_fwhm_rt",
        "max_fwhm_rt",
        "min_fwhm_mz",
        "max_fwhm_mz",
        "min_gaussian_a",
        "min_gaussian_mu",
        "max_gaussian_mu",
        "min_gaussian_sigma",
        "max_gaussian_sigma",
        "min_gaussian_r2",
        "max_jaggedness",
        "min_sharpness",
        "min_asymmetry",
        "max_asymmetry",
        "min_plates",
    ] {
        ge0(p, ID, name)?;
    }
    in_unit_interval(p, ID, "min_rel_presence_replicate")?;
    Ok(())
}

fn validate_filter_features_ms2(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.filter_features_ms2";
    ge0(p, ID, "top")?;
    ge0(p, ID, "min_intensity_ms2")?;
    ge0(p, ID, "rel_min_intensity")?;
    ge0(p, ID, "mz_clust")?;
    in_unit_interval(p, ID, "blank_presence_threshold")?;
    in_unit_interval(p, ID, "global_presence_threshold")?;
    Ok(())
}

fn validate_group_features(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.group_features";
    if let Some(method) = p.get("method").and_then(Value::as_str) {
        if method != "internal_standards" && method != "obi_warp" {
            return Err(invalid_message(
                ID,
                "method must be \"internal_standards\" or \"obi_warp\"",
            ));
        }
    }
    ge0(p, ID, "rt_deviation")?;
    ge0(p, ID, "ppm")?;
    ge1(p, ID, "min_samples")?;
    gt0(p, ID, "bin_size")?;
    Ok(())
}

fn validate_fill_features(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.fill_features";
    ge0(p, ID, "rt_expand")?;
    ge0(p, ID, "mz_expand")?;
    gt0(p, ID, "max_peak_width")?;
    ge0(p, ID, "min_traces_intensity")?;
    ge1(p, ID, "min_number_traces")?;
    ge0(p, ID, "min_intensity_ms1")?;
    ge0(p, ID, "rt_apex_deviation")?;
    ge0(p, ID, "min_signal_to_noise_ratio")?;
    ge0(p, ID, "min_gaussian_fit")?;
    Ok(())
}

fn validate_create_components(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.create_components";
    require_pair_array(p, ID, "rt_window")?;
    in_closed(p, ID, "min_correlation", -1.0, 1.0)?;
    Ok(())
}

fn validate_annotate_components(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.annotate_components";
    ge1(p, ID, "max_isotopes")?;
    ge1(p, ID, "max_charge")?;
    ge0(p, ID, "max_gaps")?;
    ge0(p, ID, "ppm")?;
    Ok(())
}

fn validate_target_method(p: &Value, id: &str) -> Result<()> {
    ge0(p, id, "ppm")?;
    ge0(p, id, "sec")?;
    ge0(p, id, "ppm_ms2")?;
    ge0(p, id, "mzr_ms2")?;
    in_unit_interval(p, id, "min_cosine_similarity")?;
    ge0(p, id, "min_shared_fragments")?;
    validate_targets(p, id)?;
    Ok(())
}

fn validate_correct_matrix_suppression(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.correct_matrix_suppression";
    ge0(p, ID, "mp_rt_window")?;
    Ok(())
}

fn validate_filter_targets(p: &Value, id: &str) -> Result<()> {
    ge0(p, id, "min_score")?;
    ge0(p, id, "max_error_rt")?;
    ge0(p, id, "max_error_mass")?;
    ge0(p, id, "min_shared_fragments")?;
    in_unit_interval(p, id, "min_cosine_similarity")?;
    if let Some(levels) = p.get("id_levels").and_then(Value::as_array) {
        for level in levels {
            if level.as_i64().map_or(true, |value| value < 1) {
                return Err(invalid_message(id, "id_levels must be integers >= 1"));
            }
        }
    }
    if let Some(names) = p.get("names").and_then(Value::as_array) {
        for name in names {
            if !name.is_string() {
                return Err(invalid_message(id, "names must be strings"));
            }
        }
    }
    Ok(())
}

/// `mass_spec.assign_transformation_products` validator (mirrors R checkmate):
/// `mzr_ms2 >= 0`, `chromatographic_phase` in {reverse_phase, hilic}, and the
/// transformation_products row structure (all precursor/main-precursor columns
/// plus at least one product structure column).
fn validate_assign_transformation_products(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.assign_transformation_products";
    ge0(p, ID, "mzr_ms2")?;
    if let Some(phase) = p.get("chromatographic_phase") {
        if !phase.is_null() {
            let phase = phase.as_str().unwrap_or_default();
            if phase != "reverse_phase" && phase != "hilic" {
                return Err(invalid_message(
                    ID,
                    "chromatographic_phase must be \"reverse_phase\" or \"hilic\"",
                ));
            }
        }
    }
    if let Some(rows) = p.get("transformation_products").and_then(Value::as_array) {
        const REQUIRED_COLS: [&str; 16] = [
            "name",
            "transformation",
            "precursor_name",
            "precursor_formula",
            "precursor_mass",
            "precursor_SMILES",
            "precursor_InChI",
            "precursor_InChIKey",
            "precursor_xLogP",
            "main_precursor_name",
            "main_precursor_formula",
            "main_precursor_mass",
            "main_precursor_SMILES",
            "main_precursor_InChI",
            "main_precursor_InChIKey",
            "main_precursor_xLogP",
        ];
        for (index, row) in rows.iter().enumerate() {
            if !row.is_object() {
                return Err(invalid_message(
                    ID,
                    "transformation_products must be an array of objects",
                ));
            }
            for col in REQUIRED_COLS {
                if row.get(col).is_none() {
                    return Err(invalid_message(
                        ID,
                        format!("transformation_products[{index}] must include \"{col}\""),
                    ));
                }
            }
            let has_product_structure = ["SMILES", "InChI", "InChIKey"]
                .iter()
                .any(|key| row.get(*key).is_some());
            if !has_product_structure {
                return Err(invalid_message(
                    ID,
                    format!(
                        "transformation_products[{index}] must include at least one of \"SMILES\", \"InChI\", \"InChIKey\""
                    ),
                ));
            }
        }
    }
    Ok(())
}

/// `mass_spec.metfrag_screening` validator (mirrors R checkmate):
/// ppm/sec/ppm_ms2/mzr_ms2 >= 0; top_n >= 1; maximum_tree_depth >= 1;
/// number_threads >= 1; score_types/score_weights same length; database_type
/// in {KEGG, PubChem, ExtendedPubChem, Local} (case-insensitive); Local
/// requires a non-empty database with the core row columns.
fn validate_metfrag_screening(p: &Value) -> Result<()> {
    const ID: &str = "mass_spec.metfrag_screening";
    ge0(p, ID, "ppm")?;
    ge0(p, ID, "sec")?;
    ge0(p, ID, "ppm_ms2")?;
    ge0(p, ID, "mzr_ms2")?;
    ge1(p, ID, "top_n")?;
    ge1(p, ID, "maximum_tree_depth")?;
    ge1(p, ID, "number_threads")?;
    let len = |key: &str| p.get(key).and_then(Value::as_array).map(Vec::len);
    if let (Some(types), Some(weights)) = (len("score_types"), len("score_weights")) {
        if types != weights {
            return Err(invalid_message(
                ID,
                "score_types and score_weights must have the same length",
            ));
        }
    }
    if let Some(db_type) = p.get("database_type").and_then(Value::as_str) {
        const R_TYPES: [&str; 4] = ["KEGG", "PubChem", "ExtendedPubChem", "Local"];
        if !R_TYPES
            .iter()
            .any(|t| t.eq_ignore_ascii_case(db_type.trim()))
        {
            return Err(invalid_message(
                ID,
                format!("database_type must be one of: {}", R_TYPES.join(", ")),
            ));
        }
        if db_type.trim().eq_ignore_ascii_case("Local") {
            let rows = p.get("database").and_then(Value::as_array);
            let Some(rows) = rows else {
                return Err(invalid_message(
                    ID,
                    "database must contain at least one row for database_type \"Local\"",
                ));
            };
            if rows.is_empty() {
                return Err(invalid_message(
                    ID,
                    "database must contain at least one row for database_type \"Local\"",
                ));
            }
            for (index, row) in rows.iter().enumerate() {
                for col in [
                    "name", "formula", "mass", "SMILES", "InChI", "InChIKey", "xLogP",
                ] {
                    if row.get(col).is_none() {
                        return Err(invalid_message(
                            ID,
                            format!("database[{index}] must include \"{col}\""),
                        ));
                    }
                }
            }
        }
    }
    Ok(())
}

/// Per-method value validators for the mass_spec NTA methods. Runs on the
/// RESOLVED parameter object (defaults applied, types pre-checked). Returns
/// `None` for non-NTA methods. Errors use `WorkflowValidation` with the
/// `<id>: invalid parameters: <reason>` shape.
fn nta_validator(id: &str) -> Option<MethodValidator> {
    match id {
        "mass_spec.find_features" => Some(Box::new(|p: &Value| validate_find_features(p))),
        "mass_spec.load_features_ms1" => Some(Box::new(|p: &Value| validate_load_features_ms1(p))),
        "mass_spec.load_features_ms2" => Some(Box::new(|p: &Value| validate_load_features_ms2(p))),
        "mass_spec.subtract_blank" => Some(Box::new(|p: &Value| validate_subtract_blank(p))),
        "mass_spec.filter_features" => Some(Box::new(|p: &Value| validate_filter_features(p))),
        "mass_spec.filter_features_ms2" => {
            Some(Box::new(|p: &Value| validate_filter_features_ms2(p)))
        }
        "mass_spec.group_features" => Some(Box::new(|p: &Value| validate_group_features(p))),
        "mass_spec.fill_features" => Some(Box::new(|p: &Value| validate_fill_features(p))),
        "mass_spec.create_components" => Some(Box::new(|p: &Value| validate_create_components(p))),
        "mass_spec.annotate_components" => {
            Some(Box::new(|p: &Value| validate_annotate_components(p)))
        }
        "mass_spec.suspect_screening" => Some(Box::new(|p: &Value| {
            validate_target_method(p, "mass_spec.suspect_screening")
        })),
        "mass_spec.find_internal_standards" => Some(Box::new(|p: &Value| {
            validate_target_method(p, "mass_spec.find_internal_standards")
        })),
        "mass_spec.correct_matrix_suppression" => {
            Some(Box::new(|p: &Value| validate_correct_matrix_suppression(p)))
        }
        "mass_spec.filter_suspects" => Some(Box::new(|p: &Value| {
            validate_filter_targets(p, "mass_spec.filter_suspects")
        })),
        "mass_spec.filter_internal_standards" => Some(Box::new(|p: &Value| {
            validate_filter_targets(p, "mass_spec.filter_internal_standards")
        })),
        "mass_spec.assign_transformation_products" => Some(Box::new(|p: &Value| {
            validate_assign_transformation_products(p)
        })),
        "mass_spec.metfrag_screening" => Some(Box::new(|p: &Value| validate_metfrag_screening(p))),
        _ => None,
    }
}
