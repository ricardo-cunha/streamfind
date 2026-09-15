use regex::Regex;
use serde_json::Value;
use streamfind_rust_core::{Error, ErrorCode, Project, Result};

use crate::reader;

#[derive(Debug, Clone, Default)]
pub struct LoadChromatogramsRequest {
    pub analyses: Vec<String>,
    pub chromatogram_id_regex: Vec<String>,
    pub ignore_case: bool,
    pub invert: bool,
}

#[derive(Debug, Clone, Default)]
pub struct FilterChromatogramsRetentionTimeRequest {
    pub analyses: Vec<String>,
    pub rtmin: f64,
    pub rtmax: f64,
}

fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::InvalidArgument, message)
}

fn sql(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

fn option(value: Option<f32>) -> String {
    value.map_or_else(|| "NULL".to_string(), |v| format!("{}", v as f64))
}

fn analyses(project: &Project, wanted: &[String]) -> Result<Vec<(String, String, i64)>> {
    let rows = project.query_json(&format!(
        "SELECT analysis, file_path, analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis"
    ))?;
    let rows = rows.as_array().cloned().unwrap_or_default();
    let selected = rows
        .into_iter()
        .filter_map(|row| {
            let name = row["analysis"].as_str()?.to_owned();
            (wanted.is_empty() || wanted.iter().any(|value| value == &name)).then(|| {
                (
                    name,
                    row["file_path"].as_str().unwrap_or_default().to_owned(),
                    row["analysis_index"].as_i64().unwrap_or(0),
                )
            })
        })
        .collect::<Vec<_>>();
    if selected.is_empty() {
        return Err(invalid("No analyses available for loading chromatograms."));
    }
    Ok(selected)
}

fn matches(id: &str, patterns: &[Regex]) -> bool {
    patterns.iter().any(|pattern| pattern.is_match(id))
}

pub fn load_chromatograms(
    project: &mut Project,
    request: &LoadChromatogramsRequest,
) -> Result<bool> {
    crate::ensure_chromatograms_schema(project)?;
    let patterns = request
        .chromatogram_id_regex
        .iter()
        .filter_map(|pattern| {
            let pattern = if request.ignore_case {
                format!("(?i){pattern}")
            } else {
                pattern.clone()
            };
            Regex::new(&pattern).ok()
        })
        .collect::<Vec<_>>();
    for (analysis, path, analysis_index) in analyses(project, &request.analyses)? {
        let mut file = reader::Reader::open(path).map_err(|error| invalid(error.to_string()))?;
        file.select_analysis(analysis_index as usize)
            .map_err(|error| invalid(error.to_string()))?;
        for (chromatogram_index, chromatogram) in file
            .chromatograms()
            .iter()
            .enumerate()
            .filter(|(_, chromatogram)| matches(&chromatogram.id, &patterns) ^ request.invert)
        {
            let count = chromatogram.time.len().min(chromatogram.intensity.len());
            if count == 0 {
                continue;
            }
            let polarity = chromatogram.polarity;
            let precursor_mz = option(chromatogram.precursor_mz);
            let activation_ce = option(chromatogram.activation_ce);
            let product_mz = option(chromatogram.product_mz);
            let mut statements = format!(
                "DELETE FROM MASS_SPEC_CHROMATOGRAMS WHERE analysis = {} AND chromatogram_id = {}",
                sql(&analysis),
                sql(&chromatogram.id)
            );
            statements.push(';');
            for (rt, intensity) in chromatogram
                .time
                .iter()
                .zip(&chromatogram.intensity)
                .take(count)
            {
                statements.push_str(&format!(
                    "INSERT INTO MASS_SPEC_CHROMATOGRAMS (analysis, index, chromatogram_id, polarity, precursor_mz, activation_ce, product_mz, wavelength_nm, rt, raw_intensity, baseline, intensity) VALUES ({},{},{},{},{},{},{},{},{},{},0,{})",
                    sql(&analysis), chromatogram_index, sql(&chromatogram.id), polarity, precursor_mz, activation_ce, product_mz, chromatogram.wavelength_nm, *rt as f64, *intensity as f64, *intensity as f64
                ));
                statements.push(';');
            }
            project.execute_sql(&statements)?;
        }
    }
    Ok(true)
}

pub fn filter_chromatograms_retention_time(
    project: &mut Project,
    request: &FilterChromatogramsRetentionTimeRequest,
) -> Result<bool> {
    if request.rtmin >= request.rtmax {
        return Err(invalid("rtmin must be less than rtmax."));
    }
    crate::ensure_chromatograms_schema(project)?;
    let analysis_filter = if request.analyses.is_empty() {
        String::new()
    } else {
        format!(
            " AND analysis IN ({})",
            request
                .analyses
                .iter()
                .map(|value| sql(value))
                .collect::<Vec<_>>()
                .join(",")
        )
    };
    let rows = project.query_json(&format!(
        "SELECT analysis, chromatogram_id, index, polarity, precursor_mz, activation_ce, product_mz, wavelength_nm, rt, raw_intensity, baseline, intensity FROM MASS_SPEC_CHROMATOGRAMS WHERE {} AND rt >= {} AND rt <= {} ORDER BY chromatogram_id, rt",
        analysis_filter, request.rtmin, request.rtmax
    ))?;
    let mut grouped = std::collections::BTreeMap::<(String, String), Vec<&Value>>::new();
    for row in rows.as_array().into_iter().flatten() {
        grouped
            .entry((
                row["analysis"].as_str().unwrap_or_default().into(),
                row["chromatogram_id"].as_str().unwrap_or_default().into(),
            ))
            .or_default()
            .push(row);
    }
    for ((analysis, id), selected) in grouped {
        let mut statements = format!(
            "DELETE FROM MASS_SPEC_CHROMATOGRAMS WHERE analysis = {} AND chromatogram_id = {};",
            sql(&analysis),
            sql(&id)
        );
        for row in selected {
            statements.push_str(&format!("INSERT INTO MASS_SPEC_CHROMATOGRAMS (analysis, index, chromatogram_id, polarity, precursor_mz, activation_ce, product_mz, wavelength_nm, rt, raw_intensity, baseline, intensity) VALUES ({},{},{},{},{},{},{},{},{},{},{},{}) ;", sql(&analysis), row["index"], sql(&id), row["polarity"], row["precursor_mz"], row["activation_ce"], row["product_mz"], row["wavelength_nm"], row["rt"], row["raw_intensity"], row["baseline"], row["intensity"]));
        }
        project.execute_sql(&statements)?;
    }
    Ok(true)
}
