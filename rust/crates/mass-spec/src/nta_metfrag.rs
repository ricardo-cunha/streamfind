//! MetFragCL subprocess screening for non-target analysis projects.
//!
//! Ported operation-faithfully from
//! `bindings/r/src/core/nta/nta_metfrag_runner.cpp` (`metfrag_screening_impl`).
//! CSV constants, score normalization, ranking, filters, input generation and
//! the internal fixed thresholds are copied verbatim; only the plumbing is
//! adapted (Rust types, tool resolution via
//! `streamfind_external::tools::resolve_metfrag()`, persistence through the
//! existing SUSPECTS table path).

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use serde_json::Value;
use streamfind_rust_core::{Error, ErrorCode, Project, Result};

use crate::nta::{NtaSuspectRow, ProjectNonTargetAnalysis};
use crate::nta_processing_methods::{
    finished, load_analysis_features, persist_features, persist_suspects,
};
use crate::nta_suspect_screening::{normalize_structure, NormalizedStructure};
use crate::nta_utils::encode_floats_base64;

const SUPPORTED_METFRAG_DATABASE_TYPES: [&str; 7] = [
    "KEGG",
    "PubChem",
    "ExtendedPubChem",
    "ChemSpiderRest",
    "LocalSDF",
    "LocalPSV",
    "LocalCSV",
];

/// Candidate writers used for streamfind result parsing (`CSV` for standard
/// columns, `FragmentSmilesPSV` for fragment SMILES) — the runner's default;
/// not exposed as a wire parameter.
const CANDIDATE_WRITERS: [&str; 2] = ["CSV", "FragmentSmilesPSV"];

/// Runner configuration (mirrors `nta::metfrag_runner::MetFragParams`; the
/// `debug` flag is accepted for surface parity but unused, exactly like the
/// C++ runner).
#[derive(Debug, Clone)]
pub struct MetFragParams {
    pub database_type: String,
    pub database_path: String,
    pub ppm: f64,
    pub sec: f64,
    pub ppm_ms2: f64,
    pub mzr_ms2: f64,
    pub top_n: i32,
    pub score_types: Vec<String>,
    pub score_weights: Vec<f64>,
    pub pre_processing_candidate_filter: Vec<String>,
    pub post_processing_candidate_filter: Vec<String>,
    pub maximum_tree_depth: i32,
    pub number_threads: i32,
    pub use_smiles: bool,
    pub filtered: bool,
    pub java_path: PathBuf,
    pub metfrag_path: PathBuf,
    pub run_dir: PathBuf,
}

#[derive(Debug, Clone, Default)]
struct MetFragRow {
    name: String,
    formula: String,
    SMILES: String,
    InChI: String,
    InChIKey: String,
    database_id: String,
    score: f64,
    xLogP: f64,
    neutral_mass: f64,
    expl_peaks: String,
    expl_formulas: String,
    expl_smiles: String,
    expl_aromatic_smiles: String,
}

// ── Internal helpers ──────────────────────────────────────────────────────────

/// base64 little-endian float32 -> Vec<f64> (mirrors `decode_encoded`).
fn decode_encoded(encoded: &str) -> Vec<f64> {
    crate::nta_utils::decode_floats_base64(encoded)
        .into_iter()
        .map(|f| f as f64)
        .collect()
}

/// Trim leading/trailing whitespace (mirrors `trim_ws`).
fn trim_ws(s: &str) -> String {
    s.trim().to_string()
}

/// Strip leading "mass:" prefixes from a "mass:value;mass:value" string
/// (mirrors `strip_mass_prefixes` — tokens without a ':' are dropped).
fn strip_mass_prefixes(input: &str) -> String {
    if input.is_empty() {
        return String::new();
    }
    let mut parts = Vec::new();
    for token in input.split(';') {
        if token.is_empty() {
            continue;
        }
        if let Some(colon) = token.find(':') {
            let val = &token[colon + 1..];
            if !val.is_empty() {
                parts.push(val.to_string());
            }
        }
    }
    parts.join(";")
}

fn to_lower_ascii(s: &str) -> String {
    s.to_lowercase()
}

#[allow(dead_code)]
fn is_local_database_type(database_type: &str) -> bool {
    database_type == "LocalSDF" || database_type == "LocalPSV" || database_type == "LocalCSV"
}

fn join_strings(values: &[String], separator: &str) -> String {
    values.join(separator)
}

/// Fixed 6-decimal join (mirrors `join_doubles` with `setprecision(6)`).
fn join_doubles(values: &[f64], separator: &str) -> String {
    values
        .iter()
        .map(|v| format!("{v:.6}"))
        .collect::<Vec<_>>()
        .join(separator)
}

/// `%Y%m%d_%H%M%S` (UTC; the C++ runner used local time — plumbing-only
/// difference in the run directory name).
fn make_run_timestamp() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let (h, m, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    // civil_from_days (Howard Hinnant)
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mth = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if mth <= 2 { y + 1 } else { y };
    format!("{year:04}{mth:02}{d:02}_{h:02}{m:02}{s:02}")
}

/// Run directory for this screening batch (under `std::env::temp_dir()`;
/// the C++ runner defaulted to `./log/metfrag/run_<timestamp>`).
fn default_metfrag_run_dir() -> PathBuf {
    std::env::temp_dir()
        .join("streamfind_metfrag")
        .join(format!("run_{}", make_run_timestamp()))
}

fn default_empty_peak_list_path(run_dir: &Path) -> PathBuf {
    run_dir.join("_metfrag_empty_peaklist.txt")
}

fn ensure_empty_peak_list_file(path: &Path) {
    if !path.exists() {
        let _ = std::fs::File::create(path);
    }
}

/// Build a safe filename component from analysis + feature names
/// (mirrors `safe_id`).
fn safe_id(a: &str, b: &str) -> String {
    let combined = format!("{a}_{b}");
    let mut out = String::with_capacity(combined.len());
    for c in combined.chars() {
        if c.is_alphanumeric() || c == '-' || c == '.' {
            out.push(c);
        } else {
            out.push('_');
        }
    }
    out
}

fn write_peak_list(path: &Path, mz: &[f64], intensity: &[f64]) {
    if let Ok(mut f) = std::fs::File::create(path) {
        use std::io::Write;
        for i in 0..mz.len().min(intensity.len()) {
            let _ = writeln!(f, "{} {}", mz[i], intensity[i]);
        }
    }
}

/// Forward slashes for MetFrag (Java) paths on all platforms.
fn forward_slashes(s: &str) -> String {
    s.replace('\\', "/")
}

fn write_params_file(
    path: &Path,
    common_template: &str,
    precursor_mass: f64,
    polarity: i32,
    ms2_path: &Path,
    sample_name: &str,
) {
    use std::io::Write;
    if let Ok(mut f) = std::fs::File::create(path) {
        let _ = write!(f, "{common_template}");
        let _ = writeln!(
            f,
            "PeakListPath = {}",
            forward_slashes(&ms2_path.to_string_lossy())
        );
        // C++ streamed the double with default (6 significant digit)
        // precision; Rust's shortest round-trip representation is a plumbing
        // adaptation that hands MetFrag a more precise precursor mass.
        let _ = writeln!(f, "NeutralPrecursorMass = {precursor_mass}");
        let _ = writeln!(f, "PrecursorIonMode = {polarity}");
        let _ = writeln!(
            f,
            "IsPositiveIonMode = {}",
            if polarity > 0 { "True" } else { "False" }
        );
        let _ = writeln!(f, "SampleName = {sample_name}");
    }
}

/// Quote a path when it contains whitespace or quotes (mirrors `q`).
fn shell_quote(path: &Path) -> String {
    let s = path.to_string_lossy().into_owned();
    if s.contains([' ', '\t', '"']) {
        format!("\"{s}\"")
    } else {
        s
    }
}

/// Run MetFragCL and redirect stdout+stderr to `log_path`; returns the exit
/// status (0 = success). Mirrors `run_metfrag` including the extra outer
/// quote pair on Windows (cmd.exe "rule 2" workaround).
fn run_metfrag(metfrag_path: &Path, java_path: &Path, params_path: &Path, log_path: &Path) -> i32 {
    let is_jar = metfrag_path
        .extension()
        .map(|e| e.to_string_lossy().eq_ignore_ascii_case("jar"))
        .unwrap_or(false);
    let mut cmd = if is_jar {
        format!(
            "{} -jar {} {}",
            shell_quote(java_path),
            shell_quote(metfrag_path),
            shell_quote(params_path)
        )
    } else {
        format!("{} {}", shell_quote(metfrag_path), shell_quote(params_path))
    };
    cmd.push_str(&format!(" > {} 2>&1", shell_quote(log_path)));

    let status = if cfg!(windows) {
        std::process::Command::new("cmd")
            .arg("/C")
            .arg(format!("\"{cmd}\""))
            .status()
    } else {
        std::process::Command::new("sh")
            .arg("-c")
            .arg(&cmd)
            .status()
    };
    status.map(|s| s.code().unwrap_or(-1)).unwrap_or(-1)
}

fn build_common_params_template(
    params: &MetFragParams,
    database_path: &str,
    results_dir: &str,
) -> String {
    let mut out = String::new();
    let mut w = |k: &str, v: &str| out.push_str(&format!("{k} = {v}\n"));
    w("MetFragDatabaseType", &params.database_type);
    w(
        "DatabaseSearchRelativeMassDeviation",
        &params.ppm.to_string(),
    );
    w(
        "FragmentPeakMatchRelativeMassDeviation",
        &params.ppm_ms2.to_string(),
    );
    w(
        "FragmentPeakMatchAbsoluteMassDeviation",
        &params.mzr_ms2.to_string(),
    );
    w("MetFragScoreTypes", &join_strings(&params.score_types, ","));
    w(
        "MetFragScoreWeights",
        &join_doubles(&params.score_weights, ","),
    );
    w(
        "MetFragPreProcessingCandidateFilter",
        &join_strings(&params.pre_processing_candidate_filter, ","),
    );
    w(
        "MetFragPostProcessingCandidateFilter",
        &join_strings(&params.post_processing_candidate_filter, ","),
    );
    w("MetFragCandidateWriter", &CANDIDATE_WRITERS.join(","));
    w("ResultsPath", &forward_slashes(results_dir));
    w("MaximumTreeDepth", &params.maximum_tree_depth.to_string());
    w("NumberThreads", &params.number_threads.to_string());
    w(
        "UseSmiles",
        if params.use_smiles { "True" } else { "False" },
    );
    if !database_path.is_empty() {
        w("LocalDatabasePath", &forward_slashes(database_path));
    }
    out
}

// ── CSV parsing ───────────────────────────────────────────────────────────────

fn split_delimited_line(line: &str, delimiter: char) -> Vec<String> {
    let mut fields = Vec::new();
    let mut field = String::new();
    let mut in_quotes = false;
    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '"' {
            if in_quotes && i + 1 < chars.len() && chars[i + 1] == '"' {
                field.push('"');
                i += 1;
            } else {
                in_quotes = !in_quotes;
            }
        } else if c == delimiter && !in_quotes {
            fields.push(trim_ws(&field));
            field.clear();
        } else {
            field.push(c);
        }
        i += 1;
    }
    fields.push(trim_ws(&field));
    fields
}

fn split_csv_line(line: &str) -> Vec<String> {
    split_delimited_line(line, ',')
}

fn split_psv_line(line: &str) -> Vec<String> {
    split_delimited_line(line, '|')
}

fn csv_escape(value: &str) -> String {
    if !value.contains([',', '"', '\r', '\n']) {
        return value.to_string();
    }
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for c in value.chars() {
        if c == '"' {
            out.push('"');
            out.push('"');
        } else {
            out.push(c);
        }
    }
    out.push('"');
    out
}

/// Case-insensitive column-index lookup (mirrors `find_col`).
fn find_col(headers: &[String], options: &[&str]) -> i32 {
    for opt in options {
        let o = opt.to_lowercase();
        for (i, h) in headers.iter().enumerate() {
            if h.to_lowercase() == o {
                return i as i32;
            }
        }
    }
    -1
}

/// `sf::obabel::normalize_structure_fields` — CLI-obabel replacement; returns
/// false when Open Babel is unavailable (fields stay unchanged).
fn normalize_structure_fields(
    smiles: &mut String,
    inchi: &mut String,
    inchikey: &mut String,
    formula: &mut String,
    mass: &mut f64,
    xlogp: &mut f64,
) -> bool {
    if smiles.is_empty() && inchi.is_empty() {
        return false;
    }
    let normalized: NormalizedStructure = normalize_structure(smiles, inchi);
    if !normalized.ok {
        return false;
    }
    if !normalized.canonical_smiles.is_empty() {
        *smiles = normalized.canonical_smiles.clone();
    }
    if !normalized.inchi.is_empty() {
        *inchi = normalized.inchi.clone();
    }
    if !normalized.inchikey.is_empty() {
        *inchikey = normalized.inchikey.clone();
    }
    if !normalized.formula.is_empty() {
        *formula = normalized.formula.clone();
    }
    *mass = normalized.exact_mass;
    if normalized.has_xlogp {
        *xlogp = normalized.xlogp;
    }
    true
}

fn resolve_structure_identifier(
    preferred_identifier: &str,
    inchikey: &str,
    inchi: &str,
    smiles: &str,
    name: &str,
    generated_index: usize,
) -> String {
    if !preferred_identifier.is_empty() {
        return preferred_identifier.to_string();
    }
    if !inchikey.is_empty() {
        return inchikey.to_string();
    }
    if !inchi.is_empty() {
        return inchi.to_string();
    }
    if !smiles.is_empty() {
        return smiles.to_string();
    }
    if !name.is_empty() {
        return name.to_string();
    }
    format!("row_{generated_index}")
}

/// Parse a single (CSV or PSV) file into `MetFragRow`s; with
/// `extract_smiles_only` only the fragment-SMILES columns are kept.
fn parse_metfrag_file(path: &Path, extract_smiles_only: bool) -> Vec<MetFragRow> {
    let mut out = Vec::new();
    let Ok(text) = std::fs::read_to_string(path) else {
        return out;
    };
    let mut lines = text.lines();
    let Some(header_line) = lines.next() else {
        return out;
    };
    let is_psv = header_line.contains('|');
    let split_fn = if is_psv {
        split_psv_line
    } else {
        split_csv_line
    };
    let headers = split_fn(header_line);

    let ci_name = find_col(&headers, &["Name", "CompoundName", "compound_name"]);
    let ci_form = find_col(&headers, &["MolecularFormula", "formula"]);
    let ci_smi = find_col(&headers, &["SMILES", "smiles", "CanonicalSMILES"]);
    let ci_inchi = find_col(&headers, &["InChI", "inchi1", "StandardInChI"]);
    let ci_ikey = find_col(&headers, &["InChIKey", "inchi_key"]);
    let ci_id = find_col(
        &headers,
        &["Identifier", "PubChemCID", "database_id", "InChIKey"],
    );
    let ci_score = find_col(
        &headers,
        &["Score", "MetFragScore", "TotalScore", "FinalScore"],
    );
    let ci_xlogp = find_col(&headers, &["XLogP", "XLogP3", "LogP", "XLogP-3"]);
    let ci_mass = find_col(&headers, &["NeutralMass", "MonoisotopicMass", "ExactMass"]);
    let ci_expl = find_col(&headers, &["ExplPeaks", "ExplainedPeaks"]);
    let ci_exform = find_col(&headers, &["FormulasOfExplPeaks", "ExplPeakFormulas"]);
    let ci_exsmi = find_col(&headers, &["SmilesOfExplPeaks"]);
    let ci_exarosmi = find_col(&headers, &["AromaticSmilesOfExplPeaks"]);

    let gf = |row: &[String], idx: i32| -> String {
        if idx < 0 || (idx as usize) >= row.len() {
            String::new()
        } else {
            row[idx as usize].clone()
        }
    };

    for line in lines {
        if trim_ws(line).is_empty() {
            continue;
        }
        let row = split_fn(line);
        let mut r = MetFragRow::default();
        if !extract_smiles_only {
            r.name = gf(&row, ci_name);
            r.formula = gf(&row, ci_form);
            r.SMILES = gf(&row, ci_smi);
            r.InChI = gf(&row, ci_inchi);
            r.InChIKey = gf(&row, ci_ikey);
            r.database_id = gf(&row, ci_id);
            let ss = gf(&row, ci_score);
            if !ss.is_empty() {
                r.score = ss.parse::<f64>().unwrap_or(0.0);
            }
            let xs = gf(&row, ci_xlogp);
            if !xs.is_empty() && xs != "NA" {
                r.xLogP = xs.parse::<f64>().unwrap_or(0.0);
            }
            let ms = gf(&row, ci_mass);
            if !ms.is_empty() {
                r.neutral_mass = ms.parse::<f64>().unwrap_or(0.0);
            }
            r.expl_peaks = gf(&row, ci_expl);
            r.expl_formulas = gf(&row, ci_exform);
            normalize_structure_fields(
                &mut r.SMILES,
                &mut r.InChI,
                &mut r.InChIKey,
                &mut r.formula,
                &mut r.neutral_mass,
                &mut r.xLogP,
            );
            r.database_id = resolve_structure_identifier(
                &r.database_id,
                &r.InChIKey,
                &r.InChI,
                &r.SMILES,
                &r.name,
                out.len() + 1,
            );
        }
        // Always extract SMILES columns (available in both CSV and PSV).
        r.expl_smiles = gf(&row, ci_exsmi);
        r.expl_aromatic_smiles = gf(&row, ci_exarosmi);
        out.push(r);
    }
    out
}

/// Collect candidate result files for `sample_name` in `results_dir`
/// (mirrors `collect_metfrag_result_files`).
fn collect_metfrag_result_files(results_dir: &Path, sample_name: &str) -> Vec<PathBuf> {
    let mut candidates: Vec<PathBuf> = vec![
        results_dir.join(format!("{sample_name}.csv")),
        results_dir.join(format!("{sample_name}_1.csv")),
        results_dir.join(format!("{sample_name}.psv")),
        results_dir.join(format!("{sample_name}_1.psv")),
    ];
    if let Ok(entries) = std::fs::read_dir(results_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_file() {
                continue;
            }
            let ext = path
                .extension()
                .map(|e| e.to_string_lossy().to_lowercase())
                .unwrap_or_default();
            if ext != "csv" && ext != "psv" {
                continue;
            }
            let stem = path
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default();
            if stem.starts_with(sample_name) {
                candidates.push(path);
            }
        }
    }
    candidates.sort();
    candidates.dedup();
    candidates.into_iter().filter(|p| p.exists()).collect()
}

/// Parse MetFrag output: CSV for all standard columns, PSV to supplement
/// fragment SMILES; sorted descending by score (mirrors `parse_metfrag_output`).
fn parse_metfrag_output(results_dir: &Path, sample_name: &str) -> Vec<MetFragRow> {
    let candidates = collect_metfrag_result_files(results_dir, sample_name);

    let mut csv_path: Option<&PathBuf> = None;
    let mut psv_path: Option<&PathBuf> = None;
    for cand in &candidates {
        let ext = cand
            .extension()
            .map(|e| e.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        if ext == "csv" && csv_path.is_none() {
            csv_path = Some(cand);
        } else if ext == "psv" && psv_path.is_none() {
            psv_path = Some(cand);
        }
    }

    let mut out = match csv_path {
        Some(path) => parse_metfrag_file(path, false),
        None => Vec::new(),
    };

    if let Some(psv_path) = psv_path {
        let psv_rows = parse_metfrag_file(psv_path, true);
        if psv_rows.len() == out.len() {
            for i in 0..out.len() {
                if !psv_rows[i].expl_smiles.is_empty() {
                    out[i].expl_smiles = psv_rows[i].expl_smiles.clone();
                }
                if !psv_rows[i].expl_aromatic_smiles.is_empty() {
                    out[i].expl_aromatic_smiles = psv_rows[i].expl_aromatic_smiles.clone();
                }
            }
        }
    }

    // Stable sort descending by score (mirrors `std::stable_sort`).
    let mut indexed: Vec<(usize, MetFragRow)> = out.into_iter().enumerate().collect();
    indexed.sort_by(|(ia, a), (ib, b)| b.score.partial_cmp(&a.score).unwrap_or(ia.cmp(ib)));
    indexed.into_iter().map(|(_, row)| row).collect()
}

/// Parse MetFrag `ExplPeaks` ("mz_intensity;...") and `FormulasOfExplPeaks`
/// ("mz:formula;...") into parallel vectors (mirrors `parse_expl_peaks`).
fn parse_expl_peaks(
    expl_peaks: &str,
    expl_formulas: &str,
    mz_out: &mut Vec<f64>,
    intensity_out: &mut Vec<f64>,
    formula_out: &mut String,
) {
    mz_out.clear();
    intensity_out.clear();
    formula_out.clear();
    if expl_peaks.is_empty() {
        return;
    }
    for token in expl_peaks.split(';') {
        if token.is_empty() {
            continue;
        }
        let Some(us) = token.find('_') else {
            continue;
        };
        let mz = token[..us].parse::<f64>().unwrap_or(0.0);
        let int = token[us + 1..].parse::<f64>().unwrap_or(0.0);
        mz_out.push(mz);
        intensity_out.push(int);
    }
    if !expl_formulas.is_empty() {
        let mut parts = Vec::new();
        for ft in expl_formulas.split(';') {
            if let Some(colon) = ft.find(':') {
                let form = &ft[colon + 1..];
                if !form.is_empty() {
                    parts.push(form.to_string());
                }
            }
        }
        *formula_out = parts.join(";");
    }
}

/// MetFrag DB-vs-experimental cosine similarity with ppm + absolute tolerance,
/// max normalization and 4-decimal rounding (mirrors the runner's
/// `cosine_similarity`).
#[allow(clippy::too_many_arguments)]
fn cosine_similarity(
    db_mz: &[f64],
    db_int: &[f64],
    exp_mz: &[f64],
    exp_int: &[f64],
    ppm_ms2: f64,
    mzr_ms2: f64,
    shared_out: &mut i32,
) -> f64 {
    *shared_out = 0;
    if db_mz.is_empty() || exp_mz.is_empty() {
        return 0.0;
    }
    let mut i_db = Vec::new();
    let mut i_exp = Vec::new();
    for z in 0..db_mz.len() {
        let tol = (db_mz[z] * ppm_ms2 / 1e6).max(mzr_ms2);
        let lo = db_mz[z] - tol;
        let hi = db_mz[z] + tol;
        let mut best: i32 = -1;
        let mut best_err = f64::MAX;
        for (k, &emz) in exp_mz.iter().enumerate() {
            if emz >= lo && emz <= hi {
                let err = (emz - db_mz[z]).abs();
                if err < best_err {
                    best_err = err;
                    best = k as i32;
                }
            }
        }
        if best >= 0 {
            i_db.push(db_int[z]);
            i_exp.push(exp_int[best as usize]);
            *shared_out += 1;
        }
    }
    if *shared_out == 0 {
        return 0.0;
    }
    let max_db = i_db.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let max_exp = i_exp.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    if max_db <= 0.0 || max_exp <= 0.0 {
        return 0.0;
    }
    let mut dot = 0.0;
    let mut mag_db = 0.0;
    let mut mag_exp = 0.0;
    for k in 0..i_db.len() {
        let di = i_db[k] / max_db;
        let ei = i_exp[k] / max_exp;
        dot += di * ei;
        mag_db += di * di;
        mag_exp += ei * ei;
    }
    if mag_db <= 0.0 || mag_exp <= 0.0 {
        return 0.0;
    }
    let value = dot / (mag_db.sqrt() * mag_exp.sqrt());
    (value * 10000.0).round() / 10000.0
}

/// Rewrite a LocalCSV database with MetFrag's required column names
/// (Identifier, MonoisotopicMass, MolecularFormula, SMILES, InChI, InChIKey,
/// Name, optional XLogP) into `run_dir` (mirrors `normalize_localcsv_database`).
fn normalize_localcsv_database(db_path: &Path, run_dir: &Path) -> PathBuf {
    if db_path.as_os_str().is_empty() {
        return db_path.to_path_buf();
    }
    let Ok(text) = std::fs::read_to_string(db_path) else {
        eprintln!("[metfrag] LocalCSV database not found: {db_path:?}");
        return db_path.to_path_buf();
    };
    let mut lines = text.lines();
    let Some(header_line) = lines.next() else {
        return db_path.to_path_buf();
    };
    let cols = split_csv_line(header_line);
    if cols.is_empty() {
        return db_path.to_path_buf();
    }

    let find_col = |candidates: &[&str]| -> i32 {
        for cand in candidates {
            let lc_cand = cand.to_lowercase();
            for (i, c) in cols.iter().enumerate() {
                if c.to_lowercase() == lc_cand {
                    return i as i32;
                }
            }
        }
        -1
    };

    const RULES: [(&str, &[&str]); 7] = [
        (
            "Identifier",
            &[
                "Identifier",
                "identifier",
                "id",
                "database_id",
                "databaseid",
            ],
        ),
        ("MonoisotopicMass", &["MonoisotopicMass", "mass"]),
        ("MolecularFormula", &["MolecularFormula", "formula"]),
        ("SMILES", &["SMILES", "smiles", "Smiles"]),
        ("InChI", &["InChI", "inchi", "Inchi"]),
        ("InChIKey", &["InChIKey", "inchikey", "Inchikey"]),
        ("Name", &["Name", "name"]),
    ];
    let output_columns: Vec<(&str, i32)> = RULES
        .iter()
        .map(|(target, aliases)| (*target, find_col(aliases)))
        .collect();

    let id_idx = find_col(&[
        "Identifier",
        "identifier",
        "id",
        "database_id",
        "databaseid",
    ]);
    let name_idx = find_col(&["Name", "name"]);
    let ikey_idx = find_col(&["InChIKey", "inchikey", "Inchikey"]);
    let inchi_idx = find_col(&["InChI", "inchi", "Inchi"]);
    let smiles_idx = find_col(&["SMILES", "smiles", "Smiles"]);
    let formula_idx = find_col(&["MolecularFormula", "formula"]);
    let mass_idx = find_col(&["MonoisotopicMass", "mass"]);
    let xlogp_idx = find_col(&["XLogP", "xLogP", "xlogp", "XLogP3", "LogP", "XLogP-3"]);

    let out_path = run_dir.join("metfrag_localcsv_normalized.csv");
    let Ok(mut out) = std::fs::File::create(&out_path) else {
        eprintln!("[metfrag] Cannot write normalised LocalCSV to: {out_path:?}");
        return db_path.to_path_buf();
    };
    use std::io::Write;

    let mut header = String::new();
    for (i, (target, _)) in output_columns.iter().enumerate() {
        if i > 0 {
            header.push(',');
        }
        header.push_str(target);
    }
    if xlogp_idx >= 0 {
        header.push_str(",XLogP");
    }
    header.push('\n');
    let _ = out.write_all(header.as_bytes());

    let mut generated_identifier_index = 0usize;
    for row in lines {
        let fields = split_csv_line(row);
        if fields.is_empty() {
            continue;
        }
        let field_at = |idx: i32| -> String {
            if idx < 0 || (idx as usize) >= fields.len() {
                String::new()
            } else {
                fields[idx as usize].clone()
            }
        };
        generated_identifier_index += 1;

        let name = field_at(name_idx);
        let mut smiles = field_at(smiles_idx);
        let mut inchi = field_at(inchi_idx);
        let mut inchikey = field_at(ikey_idx);
        let mut formula = field_at(formula_idx);
        let identifier = field_at(id_idx);

        let mut mass = f64::NAN;
        let mass_raw = field_at(mass_idx);
        if !mass_raw.is_empty() && mass_raw != "NA" {
            mass = mass_raw.parse::<f64>().unwrap_or(0.0);
        }
        let mut xlogp = f64::NAN;
        let xlogp_raw = field_at(xlogp_idx);
        if !xlogp_raw.is_empty() && xlogp_raw != "NA" {
            xlogp = xlogp_raw.parse::<f64>().unwrap_or(0.0);
        }

        normalize_structure_fields(
            &mut smiles,
            &mut inchi,
            &mut inchikey,
            &mut formula,
            &mut mass,
            &mut xlogp,
        );
        let identifier = resolve_structure_identifier(
            &identifier,
            &inchikey,
            &inchi,
            &smiles,
            &name,
            generated_identifier_index,
        );

        let normalized_value = |target: &str| -> String {
            match target {
                "Identifier" => identifier.clone(),
                "MonoisotopicMass" => {
                    if mass.is_nan() {
                        String::new()
                    } else {
                        format!("{mass:.10}")
                    }
                }
                "MolecularFormula" => formula.clone(),
                "SMILES" => smiles.clone(),
                "InChI" => inchi.clone(),
                "InChIKey" => inchikey.clone(),
                "Name" => name.clone(),
                _ => String::new(),
            }
        };

        let mut line = String::new();
        for (i, (target, _)) in output_columns.iter().enumerate() {
            if i > 0 {
                line.push(',');
            }
            line.push_str(&csv_escape(&normalized_value(target)));
        }
        if xlogp_idx >= 0 {
            line.push(',');
            if !xlogp.is_nan() {
                line.push_str(&csv_escape(&format!("{xlogp:.6}")));
            }
        }
        line.push('\n');
        let _ = out.write_all(line.as_bytes());
    }

    out_path
}

/// Case-insensitive canonicalization over the runner's supported database
/// types (mirrors `canonicalize_database_type`).
fn canonicalize_database_type(database_type: &str) -> Result<String> {
    let needle = to_lower_ascii(&trim_ws(database_type));
    for value in SUPPORTED_METFRAG_DATABASE_TYPES {
        if to_lower_ascii(value) == needle {
            return Ok(value.to_string());
        }
    }
    Err(Error::new(
        ErrorCode::InvalidArgument,
        format!(
            "Unsupported MetFrag database_type '{database_type}'. Supported values are: {}",
            SUPPORTED_METFRAG_DATABASE_TYPES.join(", ")
        ),
    ))
}

// ── Screening loop ────────────────────────────────────────────────────────────

fn feature_suspect(
    analysis: &str,
    f: &crate::nta::NtaFeatureRow,
    row: &MetFragRow,
    rank: i32,
    precursor_mass: f64,
    sec: f64,
    shared: i32,
    cosine: f64,
    db_mz: &[f64],
    db_mz_enc: &str,
    db_int_enc: &str,
    db_form: &str,
) -> NtaSuspectRow {
    let mut s = NtaSuspectRow::default();
    // RT post-filter state: the runner declares db_rt_val = NaN and never
    // assigns it, so the RT filter, error_rt and rt_match are always dead
    // (kept verbatim).
    let db_rt_val = f64::NAN;
    let error_rt = f64::NAN;

    let error_mass = if !row.neutral_mass.is_nan() && precursor_mass > 0.0 {
        (((precursor_mass - row.neutral_mass) / precursor_mass) * 1e6 * 10.0).round() / 10.0
    } else {
        f64::NAN
    };

    let rt_match = !db_rt_val.is_nan() && (f.rt - db_rt_val).abs() <= sec; // dead with NaN db_rt
    let ms2_match = shared > 0;
    let id_level = if rt_match && ms2_match {
        1
    } else if ms2_match {
        2
    } else if rt_match {
        3
    } else {
        4
    };

    s.analysis = analysis.to_string();
    s.feature = f.feature.clone();
    s.candidate_rank = rank;
    s.name = if row.name.is_empty() {
        row.database_id.clone()
    } else {
        row.name.clone()
    };
    s.polarity = f.polarity;
    s.db_mass = row.neutral_mass;
    s.exp_mass = precursor_mass;
    s.error_mass = error_mass;
    s.db_rt = db_rt_val;
    s.exp_rt = f.rt;
    s.error_rt = error_rt;
    s.intensity = f.intensity;
    s.area = f.area;
    s.id_level = id_level;
    s.score = row.score;
    s.shared_fragments = shared;
    s.cosine_similarity = cosine;
    s.formula = row.formula.clone();
    s.SMILES = row.SMILES.clone();
    s.InChI = row.InChI.clone();
    s.InChIKey = row.InChIKey.clone();
    s.xLogP = row.xLogP;
    s.database_id = row.database_id.clone();
    s.db_ms2_size = db_mz.len() as i32;
    s.db_ms2_mz = db_mz_enc.to_string();
    s.db_ms2_intensity = db_int_enc.to_string();
    s.db_ms2_formula = db_form.to_string();
    s.db_ms2_smiles = strip_mass_prefixes(&row.expl_smiles);
    s.exp_ms2_size = f.ms2_size;
    s.exp_ms2_mz = f.ms2_mz.clone();
    s.exp_ms2_intensity = f.ms2_intensity.clone();
    s
}

/// `nta::metfrag_runner::metfrag_screening_impl` — for each selected analysis,
/// query MetFrag per feature and append ranked candidates to the suspect
/// buffers.
fn metfrag_screening_impl(
    data: &mut ProjectNonTargetAnalysis,
    analyses_sel: &[String],
    params: &MetFragParams,
) {
    let analysis_names = data.analysis_names().to_vec();
    let n_ana = analysis_names.len();

    // Ensure run directory exists.
    let run_dir = &params.run_dir;
    if let Err(error) = std::fs::create_dir_all(run_dir) {
        eprintln!("[metfrag_runner] Failed to create run_dir '{run_dir:?}': {error}");
    }
    println!("[metfrag_runner] run_dir: {}", run_dir.to_string_lossy());

    let effective_db_path =
        if !params.database_path.is_empty() && params.database_type == "LocalCSV" {
            normalize_localcsv_database(Path::new(&params.database_path), run_dir)
        } else {
            PathBuf::from(&params.database_path)
        };
    let effective_db_path_str = effective_db_path.to_string_lossy().into_owned();

    let common_params_template =
        build_common_params_template(params, &effective_db_path_str, &run_dir.to_string_lossy());
    let shared_empty_peak_list = default_empty_peak_list_path(run_dir);
    ensure_empty_peak_list_file(&shared_empty_peak_list);

    // Reset suspects for all analyses.
    for buffer in data.suspect_buffers.iter_mut() {
        *buffer = crate::nta::NtaSuspects::default();
    }

    let analyses_sel_set: HashSet<&str> = analyses_sel.iter().map(String::as_str).collect();

    for ai in 0..n_ana {
        let ana = &analysis_names[ai];
        if !analyses_sel_set.is_empty() && !analyses_sel_set.contains(ana.as_str()) {
            continue;
        }
        let feats = &data.feature_buffers[ai];
        let n_feat = feats.size();

        println!(
            "{}/{} MetFrag screening: {ana} ({n_feat} features)",
            ai + 1,
            n_ana
        );
        let mut n_suspects_found = 0;

        for fi in 0..n_feat {
            let f = feats.get_feature(fi);
            // Skip filtered features unless explicitly requested.
            if !params.filtered && f.filtered {
                continue;
            }

            // Decode MS2 peak list.
            let ms2_mz = decode_encoded(&f.ms2_mz);
            let ms2_int = decode_encoded(&f.ms2_intensity);

            // Determine neutral precursor mass.
            let precursor_mass = if f.mass > 0.0 {
                f.mass
            } else if f.mz > 0.0 {
                f.mz - f.polarity as f64 * 1.007276
            } else {
                f64::NAN
            };
            if precursor_mass.is_nan() {
                continue;
            }

            // Build safe file-name stem.
            let sid = safe_id(ana, &f.feature);
            let has_ms2 = !ms2_mz.is_empty();
            let ms2_path = if has_ms2 {
                run_dir.join(format!("ms2_{sid}.txt"))
            } else {
                shared_empty_peak_list.clone()
            };
            let params_path = run_dir.join(format!("metfrag_{sid}.params"));
            let log_path = run_dir.join(format!("metfrag_{sid}.log"));
            let sample_name = format!("metfrag_{sid}");

            if has_ms2 {
                write_peak_list(&ms2_path, &ms2_mz, &ms2_int);
            }

            // Write parameter file.
            write_params_file(
                &params_path,
                &common_params_template,
                precursor_mass,
                f.polarity,
                &ms2_path,
                &sample_name,
            );

            // Invoke MetFragCL.
            let status = run_metfrag(
                &params.metfrag_path,
                &params.java_path,
                &params_path,
                &log_path,
            );

            // Parse output file (PSV from FragmentSmilesPSV, or CSV fallback).
            let rows = parse_metfrag_output(run_dir, &sample_name);
            let csv_paths = collect_metfrag_result_files(run_dir, &sample_name);

            if rows.is_empty() {
                if has_ms2 {
                    let _ = std::fs::remove_file(&ms2_path);
                }
                let _ = std::fs::remove_file(&params_path);
                for csv_path in &csv_paths {
                    let _ = std::fs::remove_file(csv_path);
                }
                continue;
            }

            let mut rank = 1;
            for row in &rows {
                if rank > params.top_n {
                    break;
                }

                // Decode MetFrag's ExplPeaks into parallel mz/intensity vectors.
                let mut db_mz = Vec::new();
                let mut db_int = Vec::new();
                let mut db_form = String::new();
                parse_expl_peaks(
                    &row.expl_peaks,
                    &row.expl_formulas,
                    &mut db_mz,
                    &mut db_int,
                    &mut db_form,
                );

                // Encode explained fragments for SUSPECT storage.
                let db_mzf: Vec<f32> = db_mz.iter().map(|&v| v as f32).collect();
                let db_intf: Vec<f32> = db_int.iter().map(|&v| v as f32).collect();
                let db_ms2_mz_enc = encode_floats_base64(&db_mzf);
                let db_ms2_int_enc = encode_floats_base64(&db_intf);

                // Cosine similarity between explained peaks and experimental MS2.
                let mut shared = 0;
                let mut cosine = 0.0;
                if !db_mz.is_empty() && !ms2_mz.is_empty() {
                    cosine = cosine_similarity(
                        &db_mz,
                        &db_int,
                        &ms2_mz,
                        &ms2_int,
                        params.ppm_ms2,
                        params.mzr_ms2,
                        &mut shared,
                    );
                }

                let suspect = feature_suspect(
                    ana,
                    &f,
                    row,
                    rank,
                    precursor_mass,
                    params.sec,
                    shared,
                    cosine,
                    &db_mz,
                    &db_ms2_mz_enc,
                    &db_ms2_int_enc,
                    &db_form,
                );
                data.suspect_buffers[ai].append(&suspect);
                rank += 1;
                n_suspects_found += 1;
            }

            // Keep the PSV result file for features with candidates
            // (inspectable). Clean up CSV (data already read), MS2 peak list,
            // params, and log.
            for csv_path in &csv_paths {
                let ext = csv_path
                    .extension()
                    .map(|e| e.to_string_lossy().to_lowercase())
                    .unwrap_or_default();
                if ext == "csv" {
                    let _ = std::fs::remove_file(csv_path);
                }
            }
            if has_ms2 {
                let _ = std::fs::remove_file(&ms2_path);
            }
            let _ = std::fs::remove_file(&params_path);
            if status == 0 {
                let _ = std::fs::remove_file(&log_path);
            }
        }

        println!("  Found {n_suspects_found} suspect(s) in {ana}");
    }
}

// ── Executor ──────────────────────────────────────────────────────────────────

fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::InvalidArgument, message)
}

fn number(p: &Value, key: &str, default: f64) -> f64 {
    p.get(key).and_then(Value::as_f64).unwrap_or(default)
}

fn string_list(p: &Value, key: &str) -> Vec<String> {
    p.get(key)
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn int_param(p: &Value, key: &str, default: i32) -> i32 {
    p.get(key)
        .and_then(Value::as_i64)
        .map(|v| v as i32)
        .unwrap_or(default)
}

fn bool_param(p: &Value, key: &str, default: bool) -> bool {
    p.get(key).and_then(Value::as_bool).unwrap_or(default)
}

/// Case-insensitive normalization over the R-exposed database types
/// (`KEGG`, `PubChem`, `ExtendedPubChem`, `Local`); `Local` maps to
/// `LocalCSV` for the runner (mirrors `.normalize_metfrag_database_type` +
/// the `run()` mapping).
fn normalize_metfrag_database_type(database_type: &str) -> Result<String> {
    const R_TYPES: [&str; 4] = ["KEGG", "PubChem", "ExtendedPubChem", "Local"];
    for value in R_TYPES {
        if value.eq_ignore_ascii_case(database_type.trim()) {
            return Ok(if value == "Local" {
                "LocalCSV".to_string()
            } else {
                value.to_string()
            });
        }
    }
    Err(invalid(format!(
        "`database_type` must be one of: {}.",
        R_TYPES.join(", ")
    )))
}

/// Write the LocalCSV database from the wire `database` rows (mirrors R's
/// `write_local_metfrag_database`: header
/// `name,formula,mass,rt,SMILES,InChI,InChIKey,xLogP`, numbers with 10 fixed
/// decimals, NA -> empty).
fn write_local_metfrag_database(database: &[Value], run_dir: &Path) -> Result<PathBuf> {
    if database.is_empty() {
        return Err(invalid(
            "Local MetFrag database must contain at least one row.",
        ));
    }
    let out_path = run_dir.join("metfrag_local_database.csv");
    let mut out = String::from("name,formula,mass,rt,SMILES,InChI,InChIKey,xLogP\n");
    for row in database {
        let mut fields = Vec::with_capacity(8);
        let text = |key: &str| -> String {
            row.get(key)
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        let num = |key: &str| -> String {
            match row.get(key).and_then(Value::as_f64) {
                Some(v) if !v.is_nan() => format!("{v:.10}"),
                _ => String::new(),
            }
        };
        fields.push(csv_escape(&text("name")));
        fields.push(csv_escape(&text("formula")));
        fields.push(csv_escape(&num("mass")));
        // `rt` is accepted by the R writer; the wire contract has no rt, so
        // the column is emitted empty.
        fields.push(String::new());
        fields.push(csv_escape(&text("SMILES")));
        fields.push(csv_escape(&text("InChI")));
        fields.push(csv_escape(&text("InChIKey")));
        fields.push(csv_escape(&num("xLogP")));
        out.push_str(&fields.join(","));
        out.push('\n');
    }
    if let Err(error) = std::fs::write(&out_path, out) {
        return Err(invalid(format!(
            "Cannot write local MetFrag database to: {} ({error})",
            out_path.to_string_lossy()
        )));
    }
    Ok(out_path)
}

/// Executor for `mass_spec.metfrag_screening`.
///
/// Resolves the Java + MetFragCL jar pair via
/// `streamfind_external::tools::resolve_metfrag()`; when missing, returns a
/// graceful error mirroring R's NA path.
pub fn metfrag_screening(project: &mut Project, p: &Value) -> Result<Value> {
    // R-identical defaults.
    let database_type_raw = p
        .get("database_type")
        .and_then(Value::as_str)
        .unwrap_or("PubChem");
    let database_type = normalize_metfrag_database_type(database_type_raw)?;
    let ppm = number(p, "ppm", 5.0);
    let sec = number(p, "sec", 10.0);
    let ppm_ms2 = number(p, "ppm_ms2", 10.0);
    let mzr_ms2 = number(p, "mzr_ms2", 0.008);
    let top_n = int_param(p, "top_n", 5);
    let score_types = string_list(p, "score_types");
    let score_types = if score_types.is_empty() {
        vec!["FragmenterScore".to_string()]
    } else {
        score_types
    };
    let score_weights = p
        .get("score_weights")
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_f64)
                .collect::<Vec<f64>>()
        })
        .unwrap_or_else(|| vec![1.0]);
    let pre_processing_candidate_filter = string_list(p, "pre_processing_candidate_filter");
    let pre_processing_candidate_filter = if pre_processing_candidate_filter.is_empty() {
        vec![
            "UnconnectedCompoundFilter".to_string(),
            "IsotopeFilter".to_string(),
        ]
    } else {
        pre_processing_candidate_filter
    };
    let post_processing_candidate_filter = string_list(p, "post_processing_candidate_filter");
    let post_processing_candidate_filter = if post_processing_candidate_filter.is_empty() {
        vec!["InChIKeyFilter".to_string()]
    } else {
        post_processing_candidate_filter
    };
    let maximum_tree_depth = int_param(p, "maximum_tree_depth", 3);
    let number_threads = int_param(p, "number_threads", 1);
    let use_smiles = bool_param(p, "use_smiles", true);
    let filtered = bool_param(p, "filtered", false);

    // Defensive mirror of `canonicalize_and_validate_params` (validators run
    // on the resolved parameters; lengths may mismatch when only one of
    // score_types / score_weights is supplied).
    if ppm < 0.0 || sec < 0.0 || ppm_ms2 < 0.0 || mzr_ms2 < 0.0 {
        return Err(invalid("ppm/sec/ppm_ms2/mzr_ms2 must be >= 0"));
    }
    if top_n < 1 {
        return Err(invalid("top_n must be at least 1"));
    }
    if maximum_tree_depth < 1 {
        return Err(invalid("MetFrag maximum_tree_depth must be at least 1"));
    }
    if number_threads < 1 {
        return Err(invalid("MetFrag number_threads must be at least 1"));
    }
    if score_types.is_empty() {
        return Err(invalid("MetFrag score_types must not be empty."));
    }
    if score_types.len() != score_weights.len() {
        return Err(invalid(
            "MetFrag score_types and score_weights must have the same length.",
        ));
    }
    let _ = canonicalize_database_type(&database_type)?;

    // Tool resolution (mirrors R's get_metfrag_path()/get_java_path() NA
    // path, which stops before any screening work).
    let Some((java_path, jar_path)) = streamfind_external::tools::resolve_metfrag() else {
        return Err(Error::new(
            ErrorCode::MethodExecution,
            "MetFrag command line is not installed; run 'streamfind tools install'",
        ));
    };

    let mut data = load_analysis_features(project, p)?;

    let run_dir = default_metfrag_run_dir();
    let _ = std::fs::create_dir_all(&run_dir);

    let mut database_path = String::new();
    if database_type == "LocalCSV" {
        let rows = p
            .get("database")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        if rows.is_empty() {
            return Err(invalid(
                "Local MetFrag screening requires a non-empty database.",
            ));
        }
        database_path = write_local_metfrag_database(&rows, &run_dir)?
            .to_string_lossy()
            .into_owned();
    }

    let params = MetFragParams {
        database_type,
        database_path,
        ppm,
        sec,
        ppm_ms2,
        mzr_ms2,
        top_n,
        score_types,
        score_weights,
        pre_processing_candidate_filter,
        post_processing_candidate_filter,
        maximum_tree_depth,
        number_threads,
        use_smiles,
        filtered,
        java_path,
        metfrag_path: jar_path,
        run_dir,
    };

    let analyses = string_list(p, "analysis_names");

    metfrag_screening_impl(&mut data, &analyses, &params);

    persist_features(project, &data)?;
    persist_suspects(project, &data)?;
    Ok(finished("MetFrag screening completed."))
}
