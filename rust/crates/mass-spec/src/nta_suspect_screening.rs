//! Suspect screening and internal-standard detection for non-target analysis
//! projects.
//!
//! Ported from `core/domains/mass_spec/src/nta_suspect_screening.cpp`
//! (`nta::suspect_screening::suspect_screening_impl` /
//! `find_internal_standards_impl`). Keep the algorithm identical; only the
//! plumbing (Rust types, structure normalization) is adapted. Structure
//! normalization uses the Open Babel CLI tools (`obabel`/`obprop`) instead of
//! the C-API adapter (`sf::obabel::normalize_structure`); when the tools are
//! unavailable suspects pass through unchanged, exactly like the C++ fallback.

use std::collections::HashSet;

use crate::nta::{
    NtaInternalStandardRow, NtaInternalStandards, NtaSuspectRow, NtaSuspects,
    ProjectNonTargetAnalysis,
};
use crate::nta_utils::{decode_floats_base64, encode_floats_base64};

/// One suspect query (mirrors `nta::suspect_screening::SuspectQuery`).
#[derive(Debug, Clone, Default)]
pub struct SuspectQuery {
    pub name: String,
    pub has_mass: bool,
    pub mass: f64,
    pub rt: f64,
    pub formula: String,
    pub SMILES: String,
    pub InChI: String,
    pub InChIKey: String,
    pub score: f64,
    pub has_xLogP: bool,
    pub xLogP: f64,
    pub database_id: String,
    pub fragments_mz_pos: Vec<f64>,
    pub fragments_intensity_pos: Vec<f64>,
    pub fragments_mz_neg: Vec<f64>,
    pub fragments_intensity_neg: Vec<f64>,
}

/// Normalized structure record (mirrors `sf::obabel::NormalizedStructure`).
#[derive(Debug, Clone, Default)]
pub(crate) struct NormalizedStructure {
    pub(crate) ok: bool,
    pub(crate) canonical_smiles: String,
    pub(crate) formula: String,
    pub(crate) inchi: String,
    pub(crate) inchikey: String,
    pub(crate) exact_mass: f64,
    pub(crate) xlogp: f64,
    pub(crate) has_xlogp: bool,
}

/// `nta::suspect_screening::ppm_tol`.
fn ppm_tol(value: f64, ppm: f64) -> f64 {
    value.abs() * ppm / 1e6
}

/// `nta::suspect_screening::within_ppm`.
fn within_ppm(value: f64, target: f64, ppm: f64) -> bool {
    let tol = ppm_tol(target, ppm);
    (value - target).abs() <= tol
}

/// `nta::suspect_screening::within_sec`.
fn within_sec(value: f64, target: f64, sec: f64) -> bool {
    (value - target).abs() <= sec
}

/// `nta::suspect_screening::encode_floats` — doubles narrowed to f32,
/// little-endian, base64. Empty input encodes to the empty string.
fn encode_floats(input: &[f64]) -> String {
    if input.is_empty() {
        return String::new();
    }
    let tmp: Vec<f32> = input.iter().map(|&v| v as f32).collect();
    encode_floats_base64(&tmp)
}

/// `nta::suspect_screening::get_or_default` — defensive read of one column
/// element with a fallback when the column is shorter than `idx`.
fn get_or_default<T: Clone>(vec: &[T], idx: usize, def: T) -> T {
    vec.get(idx).cloned().unwrap_or(def)
}

/// Locate an executable on `PATH` (checks both the bare name and `.exe`).
fn find_on_path(name: &str) -> Option<std::path::PathBuf> {
    let path_var = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path_var) {
        for candidate in [name, &format!("{name}.exe")] {
            let path = dir.join(candidate);
            if path.is_file() {
                return Some(path);
            }
        }
    }
    None
}

/// Capture the stdout of one command; `None` on spawn error, non-zero exit,
/// or empty output.
fn capture_stdout(program: &std::path::Path, tmp: &std::path::Path) -> Option<String> {
    let tmp_str = tmp.to_str()?;
    let output = std::process::Command::new(program)
        .arg(tmp_str)
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    if stdout.trim().is_empty() {
        return None;
    }
    Some(stdout.into_owned())
}

/// Open Babel `obprop` output for the molecule in `tmp`: try `obprop` on
/// `PATH`, then `obprop`/`obprop.exe` next to `obabel`.
fn obprop_stdout(tmp: &std::path::Path, obabel_dir: Option<&std::path::Path>) -> Option<String> {
    if let Some(stdout) = capture_stdout(std::path::Path::new("obprop"), tmp) {
        return Some(stdout);
    }
    if let Some(dir) = obabel_dir {
        for candidate in [dir.join("obprop"), dir.join("obprop.exe")] {
            if let Some(stdout) = capture_stdout(&candidate, tmp) {
                return Some(stdout);
            }
        }
    }
    None
}

/// `sf::obabel::normalize_structure` — CLI replacement. Returns `ok=false`
/// (and an otherwise default record) whenever the Open Babel tools are
/// unavailable, the molecule cannot be read, or any command fails; the caller
/// then leaves the suspect unchanged, matching the C++ fallback.
pub(crate) fn normalize_structure(smiles: &str, inchi: &str) -> NormalizedStructure {
    let mut out = NormalizedStructure::default();
    if smiles.is_empty() && inchi.is_empty() {
        return out;
    }

    // SMILES is the preferred input format; InChI is the fallback, matching
    // `sf_ob_normalize_structure` in the C API.
    let (fmt, input, ext) = if !smiles.is_empty() {
        ("smi", smiles, "smi")
    } else {
        ("inchi", inchi, "inchi")
    };

    let tmp = std::env::temp_dir().join(format!("sf_nta_{}.{}", std::process::id(), ext));
    if std::fs::write(&tmp, format!("{input}\n")).is_err() {
        return out;
    }

    // InChIKey via `obabel -i<fmt> <tmp> -oinchikey`.
    let inchikey = {
        let mut cmd = std::process::Command::new("obabel");
        cmd.arg(format!("-i{fmt}")).arg(&tmp).arg("-oinchikey");
        match cmd.output() {
            Ok(output) if output.status.success() => {
                let stdout = String::from_utf8_lossy(&output.stdout);
                stdout.split_whitespace().next().map(str::to_owned)
            }
            _ => None,
        }
    };
    let inchikey = match inchikey {
        Some(key) => key,
        None => {
            let _ = std::fs::remove_file(&tmp);
            return out;
        }
    };

    // Molecular properties via `obprop <tmp>`.
    let obabel_dir = find_on_path("obabel").and_then(|p| p.parent().map(|d| d.to_path_buf()));
    let obprop_stdout = match obprop_stdout(&tmp, obabel_dir.as_deref()) {
        Some(stdout) => stdout,
        None => {
            let _ = std::fs::remove_file(&tmp);
            return out;
        }
    };

    // Parse the property lines: "<key> <value>".
    let mut formula: Option<String> = None;
    let mut canonical_smiles: Option<String> = None;
    let mut inchi_out: Option<String> = None;
    let mut exact_mass: Option<f64> = None;
    let mut xlogp: Option<f64> = None;
    for line in obprop_stdout.lines() {
        let (key, value) = match line.split_once(char::is_whitespace) {
            Some(kv) => kv,
            None => continue,
        };
        let value = value.trim();
        match key {
            "formula" => formula = Some(value.to_string()),
            "exact_mass" => exact_mass = value.parse::<f64>().ok(),
            "canonical_SMILES" => canonical_smiles = Some(value.to_string()),
            "InChI" => inchi_out = Some(value.to_string()),
            "logP" => xlogp = value.parse::<f64>().ok(),
            _ => {}
        }
    }

    let _ = std::fs::remove_file(&tmp);

    match (formula, canonical_smiles, exact_mass) {
        (Some(formula), Some(canonical_smiles), Some(exact_mass)) => {
            out.ok = true;
            out.canonical_smiles = canonical_smiles;
            out.formula = formula;
            out.inchi = inchi_out.unwrap_or_default();
            out.inchikey = inchikey;
            out.exact_mass = exact_mass;
            if let Some(logp) = xlogp {
                out.xlogp = logp;
                out.has_xlogp = true;
            }
            out
        }
        _ => out,
    }
}

/// `nta::suspect_screening::normalize_suspects` — when normalization succeeds
/// the structure-derived fields are replaced by the normalized Open Babel
/// values so mass, formula, identifiers, and logP stay in sync. Failed
/// normalization (or missing Open Babel) leaves the suspect untouched.
fn normalize_suspects(suspects: &[SuspectQuery]) -> Vec<SuspectQuery> {
    let mut normalized: Vec<SuspectQuery> = suspects.to_vec();
    for sus in normalized.iter_mut() {
        if sus.SMILES.is_empty() && sus.InChI.is_empty() {
            continue;
        }
        let structure = normalize_structure(&sus.SMILES, &sus.InChI);
        if !structure.ok {
            continue;
        }
        sus.SMILES = structure.canonical_smiles;
        sus.formula = structure.formula;
        sus.InChI = structure.inchi;
        sus.InChIKey = structure.inchikey;
        sus.mass = structure.exact_mass;
        sus.has_mass = true;
        sus.xLogP = structure.xlogp;
        sus.has_xLogP = structure.has_xlogp;
    }
    normalized
}

/// `nta::suspect_screening::suspect_to_internal_standard` — copies every field
/// and leaves the internal-standard-only columns (`feature_component`,
/// `adduct`) empty.
fn suspect_to_internal_standard(suspect: &NtaSuspectRow) -> NtaInternalStandardRow {
    let mut row = NtaInternalStandardRow::default();
    row.created_at = suspect.created_at.clone();
    row.analysis = suspect.analysis.clone();
    row.feature = suspect.feature.clone();
    row.candidate_rank = suspect.candidate_rank;
    row.name = suspect.name.clone();
    row.polarity = suspect.polarity;
    row.db_mass = suspect.db_mass;
    row.exp_mass = suspect.exp_mass;
    row.error_mass = suspect.error_mass;
    row.db_rt = suspect.db_rt;
    row.exp_rt = suspect.exp_rt;
    row.error_rt = suspect.error_rt;
    row.intensity = suspect.intensity;
    row.area = suspect.area;
    row.id_level = suspect.id_level;
    row.score = suspect.score;
    row.shared_fragments = suspect.shared_fragments;
    row.cosine_similarity = suspect.cosine_similarity;
    row.formula = suspect.formula.clone();
    row.SMILES = suspect.SMILES.clone();
    row.InChI = suspect.InChI.clone();
    row.InChIKey = suspect.InChIKey.clone();
    row.xLogP = suspect.xLogP;
    row.database_id = suspect.database_id.clone();
    row.db_ms2_size = suspect.db_ms2_size;
    row.db_ms2_mz = suspect.db_ms2_mz.clone();
    row.db_ms2_intensity = suspect.db_ms2_intensity.clone();
    row.db_ms2_formula = suspect.db_ms2_formula.clone();
    row.db_ms2_smiles = suspect.db_ms2_smiles.clone();
    row.exp_ms2_size = suspect.exp_ms2_size;
    row.exp_ms2_mz = suspect.exp_ms2_mz.clone();
    row.exp_ms2_intensity = suspect.exp_ms2_intensity.clone();
    row
}

/// `nta::suspect_screening::screening_impl` — shared screening loop for both
/// entry points; `write_internal_standards` selects the output buffer.
fn screening_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    analyses: &[String],
    suspects: &[SuspectQuery],
    ppm: f64,
    sec: f64,
    ppm_ms2: f64,
    mzr_ms2: f64,
    min_cosine_similarity: f64,
    min_shared_fragments: i32,
    filtered: bool,
    write_internal_standards: bool,
) -> streamfind_rust_core::Result<()> {
    let normalized_suspects = normalize_suspects(suspects);
    let analysis_names: Vec<String> = nta_data.analysis_names().to_vec();

    // Clear the output buffers of every analysis.
    for i in 0..analysis_names.len() {
        nta_data.suspect_buffers[i] = NtaSuspects::default();
        nta_data.internal_standard_buffers[i] = NtaInternalStandards::default();
    }

    if normalized_suspects.is_empty() || analysis_names.is_empty() {
        return Ok(());
    }

    // When `analyses` is non-empty, restrict screening to those analyses.
    let analyses_set: HashSet<&str> = analyses.iter().map(String::as_str).collect();

    let use_mass = normalized_suspects.iter().any(|sus| sus.has_mass);

    struct FeatureRef {
        analysis_idx: usize,
        feature_idx: usize,
        assigned_name: String,
    }

    // Match features to suspects by exact mass (mz = mass +/- proton).
    let mut matched: Vec<FeatureRef> = Vec::new();
    for (a, analysis) in analysis_names.iter().enumerate() {
        if !analyses_set.is_empty() && !analyses_set.contains(analysis.as_str()) {
            continue;
        }
        let fts = &nta_data.feature_buffers[a];
        for i in 0..fts.size() {
            if !filtered && fts.filtered[i] {
                continue;
            }
            let mut assigned = String::new();
            for sus in &normalized_suspects {
                if use_mass {
                    if !sus.has_mass {
                        continue;
                    }
                    let expected_mz = sus.mass + (fts.polarity[i] as f64) * 1.007276;
                    if !within_ppm(fts.mz[i] as f64, expected_mz, ppm) {
                        continue;
                    }
                }
                assigned = sus.name.clone();
            }
            if !assigned.is_empty() {
                matched.push(FeatureRef {
                    analysis_idx: a,
                    feature_idx: i,
                    assigned_name: assigned,
                });
            }
        }
    }

    if matched.is_empty() {
        return Ok(());
    }

    // First suspect whose name is a substring of the assigned feature name.
    let find_suspect = |feature_name: &str| -> Option<&SuspectQuery> {
        normalized_suspects
            .iter()
            .find(|sus| feature_name.contains(sus.name.as_str()))
    };

    for ref_ in &matched {
        let sus = match find_suspect(&ref_.assigned_name) {
            Some(sus) => sus,
            None => continue,
        };

        let fts = &nta_data.feature_buffers[ref_.analysis_idx];
        let idx = ref_.feature_idx;

        let mut row = NtaSuspectRow::default();
        row.analysis = analysis_names[ref_.analysis_idx].clone();
        row.feature = get_or_default(&fts.feature, idx, String::new());
        row.candidate_rank = 1;
        row.name = ref_.assigned_name.clone();
        row.polarity = get_or_default(&fts.polarity, idx, 0);
        row.exp_mass = get_or_default(&fts.mass, idx, 0.0f32) as f64;
        row.exp_rt = get_or_default(&fts.rt, idx, 0.0f32) as f64;
        row.intensity = get_or_default(&fts.intensity, idx, 0.0f32) as f64;
        row.area = get_or_default(&fts.area, idx, 0.0f32) as f64;
        row.id_level = 4;
        row.shared_fragments = 0;
        row.cosine_similarity = 0.0;
        row.score = sus.score;
        row.formula = sus.formula.clone();
        row.SMILES = sus.SMILES.clone();
        row.InChI = sus.InChI.clone();
        row.InChIKey = sus.InChIKey.clone();
        row.xLogP = if sus.has_xLogP { sus.xLogP } else { f64::NAN };
        row.database_id = sus.database_id.clone();

        row.db_mass = f64::NAN;
        if use_mass && sus.has_mass {
            row.db_mass = sus.mass;
        }

        row.error_mass = f64::NAN;
        if row.db_mass.is_finite() && row.exp_mass.is_finite() && row.exp_mass != 0.0 {
            let err = ((row.exp_mass - row.db_mass) / row.exp_mass) * 1e6;
            row.error_mass = (err * 10.0).round() / 10.0;
        }

        row.db_rt = sus.rt;
        row.error_rt = f64::NAN;
        let mut rt_matched = false;
        if row.db_rt > 0.0 && row.exp_rt.is_finite() {
            let err_rt = row.exp_rt - row.db_rt;
            row.error_rt = (err_rt * 10.0).round() / 10.0;
            rt_matched = within_sec(row.exp_rt, row.db_rt, sec);
        }

        row.db_ms2_size = 0;
        row.db_ms2_mz = String::new();
        row.db_ms2_intensity = String::new();
        row.db_ms2_formula = String::new();
        row.db_ms2_smiles = String::new();
        row.exp_ms2_size = get_or_default(&fts.ms2_size, idx, 0);
        row.exp_ms2_mz = get_or_default(&fts.ms2_mz, idx, String::new());
        row.exp_ms2_intensity = get_or_default(&fts.ms2_intensity, idx, String::new());

        let (sus_mz, sus_int): (Option<&Vec<f64>>, Option<&Vec<f64>>) = if row.polarity > 0 {
            (
                Some(&sus.fragments_mz_pos),
                Some(&sus.fragments_intensity_pos),
            )
        } else if row.polarity < 0 {
            (
                Some(&sus.fragments_mz_neg),
                Some(&sus.fragments_intensity_neg),
            )
        } else {
            (None, None)
        };

        let can_check_ms2 = sus_mz.is_some()
            && !sus_mz.as_ref().unwrap().is_empty()
            && !row.exp_ms2_mz.is_empty()
            && !row.exp_ms2_intensity.is_empty();

        let mut ms2_matched = false;
        if can_check_ms2 {
            let sus_mz = sus_mz.unwrap();
            let sus_int = sus_int.unwrap();
            row.db_ms2_size = sus_mz.len() as i32;
            row.db_ms2_mz = encode_floats(sus_mz);
            row.db_ms2_intensity = encode_floats(sus_int);
            row.db_ms2_formula = String::new();
            row.db_ms2_smiles = String::new();

            let exp_mz = decode_floats_base64(&row.exp_ms2_mz);
            let exp_int = decode_floats_base64(&row.exp_ms2_intensity);
            if !exp_mz.is_empty() && exp_mz.len() == exp_int.len() {
                // Nearest experimental fragment within tolerance of each
                // database fragment.
                let mut exp_idx: Vec<i32> = vec![-1; sus_mz.len()];
                for (z, &mz) in sus_mz.iter().enumerate() {
                    let mut tol = mz * ppm_ms2 / 1e6;
                    if tol < mzr_ms2 {
                        tol = mzr_ms2;
                    }
                    let mzmin = mz - tol;
                    let mzmax = mz + tol;
                    let mut best_idx: i32 = -1;
                    let mut best_err: f64 = 0.0;
                    for (k, &emz) in exp_mz.iter().enumerate() {
                        if (emz as f64) < mzmin || (emz as f64) > mzmax {
                            continue;
                        }
                        let err = ((emz as f64) - mz).abs();
                        if best_idx == -1 || err < best_err {
                            best_idx = k as i32;
                            best_err = err;
                        }
                    }
                    exp_idx[z] = best_idx;
                }

                let shared = exp_idx.iter().filter(|&&m| m >= 0).count() as i32;
                row.shared_fragments = shared;

                // Max-normalized cosine similarity over shared fragments.
                let mut cosine = 0.0;
                if shared > 0 {
                    let mut intensity_db: Vec<f64> = Vec::with_capacity(shared as usize);
                    let mut intensity_exp: Vec<f64> = Vec::with_capacity(shared as usize);
                    for (z, &idx_match) in exp_idx.iter().enumerate() {
                        if idx_match < 0 {
                            continue;
                        }
                        intensity_db.push(get_or_default(sus_int, z, 0.0));
                        intensity_exp.push(exp_int[idx_match as usize] as f64);
                    }
                    let max_db = intensity_db
                        .iter()
                        .copied()
                        .fold(f64::NEG_INFINITY, f64::max);
                    let max_exp = intensity_exp
                        .iter()
                        .copied()
                        .fold(f64::NEG_INFINITY, f64::max);
                    if max_db > 0.0 && max_exp > 0.0 {
                        let mut dot = 0.0;
                        let mut mag_db = 0.0;
                        let mut mag_exp = 0.0;
                        for k in 0..intensity_db.len() {
                            let dbi = intensity_db[k] / max_db;
                            let exi = intensity_exp[k] / max_exp;
                            dot += dbi * exi;
                            mag_db += dbi * dbi;
                            mag_exp += exi * exi;
                        }
                        if mag_db > 0.0 && mag_exp > 0.0 {
                            cosine = dot / (mag_db.sqrt() * mag_exp.sqrt());
                        }
                    }
                }

                row.cosine_similarity = (cosine * 10000.0).round() / 10000.0;
                if row.shared_fragments >= min_shared_fragments
                    || row.cosine_similarity >= min_cosine_similarity
                {
                    ms2_matched = true;
                }
            }
        }

        if rt_matched && ms2_matched {
            row.id_level = 1;
        } else if ms2_matched {
            row.id_level = 2;
        } else if rt_matched {
            row.id_level = 3;
        } else {
            row.id_level = 4;
        }

        if write_internal_standards {
            nta_data.internal_standard_buffers[ref_.analysis_idx]
                .append(&suspect_to_internal_standard(&row));
        } else {
            nta_data.suspect_buffers[ref_.analysis_idx].append(&row);
        }
    }

    Ok(())
}

/// `nta::suspect_screening::suspect_screening_impl`.
pub fn suspect_screening_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    analyses: &[String],
    suspects: &[SuspectQuery],
    ppm: f64,
    sec: f64,
    ppm_ms2: f64,
    mzr_ms2: f64,
    min_cosine_similarity: f64,
    min_shared_fragments: i32,
    filtered: bool,
) -> streamfind_rust_core::Result<()> {
    screening_impl(
        nta_data,
        analyses,
        suspects,
        ppm,
        sec,
        ppm_ms2,
        mzr_ms2,
        min_cosine_similarity,
        min_shared_fragments,
        filtered,
        false,
    )
}

/// `nta::suspect_screening::find_internal_standards_impl`.
pub fn find_internal_standards_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    analyses: &[String],
    suspects: &[SuspectQuery],
    ppm: f64,
    sec: f64,
    ppm_ms2: f64,
    mzr_ms2: f64,
    min_cosine_similarity: f64,
    min_shared_fragments: i32,
    filtered: bool,
) -> streamfind_rust_core::Result<()> {
    screening_impl(
        nta_data,
        analyses,
        suspects,
        ppm,
        sec,
        ppm_ms2,
        mzr_ms2,
        min_cosine_similarity,
        min_shared_fragments,
        filtered,
        true,
    )
}
