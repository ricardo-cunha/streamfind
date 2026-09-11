//! Transformation-product assignment for non-target analysis projects.
//!
//! Ported operation-faithfully from
//! `bindings/r/src/core/nta/nta_assign_transformation_products.cpp`
//! (`nta::assign_transformation_products::assign_transformation_products_impl`).
//! Scoring formulas, tolerances, normalization rules, ranking and filters are
//! kept identical; only the plumbing (Rust types, parameter parsing,
//! persistence via the SUSPECTS table) is adapted. The executor works on the
//! CURRENT suspect buffer (from a prior suspect_screening run) and appends the
//! combination-scored rows.

use std::collections::HashMap;

use serde_json::Value;
use streamfind_rust_core::{Error, ErrorCode, Project, Result};

use crate::nta::{NtaSuspectRow, ProjectNonTargetAnalysis};
use crate::nta_processing_methods::{
    finished, load_analysis_features, load_suspects, persist_suspects,
    persist_transformation_products,
};
use crate::nta_utils::decode_floats_base64;

/// Mirrors `nta::api::NTA_TRANSFORMATION_PRODUCT_ROW`.
#[derive(Debug, Clone, Default)]
pub struct TransformationProductRow {
    pub name: String,
    pub formula: String,
    pub mass: f64,
    pub SMILES: String,
    pub InChI: String,
    pub InChIKey: String,
    pub xLogP: f64,
    pub transformation: String,
    pub precursor_name: String,
    pub precursor_formula: String,
    pub precursor_mass: f64,
    pub precursor_SMILES: String,
    pub precursor_InChI: String,
    pub precursor_InChIKey: String,
    pub precursor_xLogP: f64,
    pub main_precursor_name: String,
    pub main_precursor_formula: String,
    pub main_precursor_mass: f64,
    pub main_precursor_SMILES: String,
    pub main_precursor_InChI: String,
    pub main_precursor_InChIKey: String,
    pub main_precursor_xLogP: f64,
    pub feature_group: String,
    pub precursor_feature_group: String,
    pub main_precursor_feature_group: String,
    pub cosine_similarity: f64,
    pub main_precursor_cosine_similarity: f64,
    pub rt_plausibility: f64,
    pub main_precursor_rt_plausibility: f64,
    pub product_structure_key: String,
    pub precursor_structure_key: String,
    pub main_precursor_structure_key: String,
    pub resolved_direct_parent_feature_group: String,
    pub resolved_main_parent_feature_group: String,
    pub assignment_status: String,
    pub is_direct_assignment: bool,
    pub is_main_parent_consistent: bool,
    pub transformation_valid: bool,
    pub assignment_rank: i32,
    pub network_level: i32,
    pub assignment_score: f64,
    pub transformation_mass_delta_expected: f64,
    pub transformation_mass_delta_observed: f64,
    pub transformation_mass_delta_error: f64,
}

/// `nta::assign_transformation_products::trim_copy`.
fn trim_copy(value: &str) -> String {
    value.trim().to_string()
}

/// `nta::assign_transformation_products::upper_copy`.
fn upper_copy(value: &str) -> String {
    value.to_uppercase()
}

/// `nta::assign_transformation_products::collapse_spaces` — collapses runs of
/// whitespace into single spaces and trims the result.
fn collapse_spaces(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut prev_space = false;
    for ch in value.chars() {
        let is_space = ch.is_whitespace();
        if is_space {
            if !prev_space {
                out.push(' ');
            }
        } else {
            out.push(ch);
        }
        prev_space = is_space;
    }
    trim_copy(&out)
}

/// `nta::assign_transformation_products::normalize_structure_key` —
/// InChIKey (upper) > InChI > SMILES (space-collapsed).
fn normalize_structure_key(inchikey: &str, inchi: &str, smiles: &str) -> String {
    let ik = upper_copy(&trim_copy(inchikey));
    if !ik.is_empty() {
        return ik;
    }
    let ic = trim_copy(inchi);
    if !ic.is_empty() {
        return ic;
    }
    let smi = collapse_spaces(&trim_copy(smiles));
    if !smi.is_empty() {
        return smi;
    }
    String::new()
}

/// `nta::assign_transformation_products::normalize_transformation_tag` —
/// collapses spaces, strips a leading "tag:" prefix and a trailing
/// " Transformation" suffix (case-insensitive).
fn normalize_transformation_tag(value: &str) -> String {
    let mut tag = collapse_spaces(&trim_copy(value));
    if tag.is_empty() {
        return String::new();
    }
    if let Some(pos) = tag.find(':') {
        tag = trim_copy(&tag[pos + 1..]);
    }
    const SUFFIX: &str = " transformation";
    if tag.len() > SUFFIX.len() {
        let lower = tag.to_uppercase();
        let lower_suffix = SUFFIX.to_uppercase();
        if lower.ends_with(&lower_suffix) {
            tag = trim_copy(&tag[..tag.len() - SUFFIX.len()]);
        }
    }
    tag
}

/// base64 little-endian float32 -> Vec<f64> (mirrors `decode_encoded`).
fn decode_encoded(encoded: &str) -> Vec<f64> {
    decode_floats_base64(encoded)
        .into_iter()
        .map(|f| f as f64)
        .collect()
}

/// `nta::assign_transformation_products::cosine_similarity` — product MS2 vs
/// precursor MS2; for each peak of the SECOND spectrum the best intensity of
/// the first spectrum within `tol` is used (`mz2`-aligned vectors).
fn cosine_similarity(mz1: &[f64], int1: &[f64], mz2: &[f64], int2: &[f64], tol: f64) -> f64 {
    if mz1.is_empty() || mz2.is_empty() {
        return f64::NAN;
    }
    let mut i1 = Vec::with_capacity(mz2.len());
    let mut i2 = Vec::with_capacity(mz2.len());
    for j in 0..mz2.len() {
        let mut best = 0.0;
        for k in 0..mz1.len() {
            if (mz1[k] - mz2[j]).abs() <= tol && int1[k] > best {
                best = int1[k];
            }
        }
        i1.push(best);
        i2.push(int2[j]);
    }
    let mut dot = 0.0;
    let mut mag1 = 0.0;
    let mut mag2 = 0.0;
    for j in 0..i1.len() {
        dot += i1[j] * i2[j];
        mag1 += i1[j] * i1[j];
        mag2 += i2[j] * i2[j];
    }
    if mag1 <= 0.0 || mag2 <= 0.0 {
        return 0.0;
    }
    dot / (mag1.sqrt() * mag2.sqrt())
}

/// `nta::assign_transformation_products::mean_rt` — NaN-aware mean.
fn mean_rt(v: &[f64]) -> f64 {
    let mut sum = 0.0;
    let mut n = 0;
    for value in v {
        if !value.is_nan() {
            sum += value;
            n += 1;
        }
    }
    if n > 0 {
        sum / n as f64
    } else {
        f64::NAN
    }
}

/// `nta::assign_transformation_products::rt_plausibility` — sign agreement of
/// logP and RT deltas, sign-flipped for hilic phase.
fn rt_plausibility(prod_logp: f64, prec_logp: f64, prod_rt: f64, prec_rt: f64, phase: &str) -> f64 {
    if prod_logp.is_nan() || prec_logp.is_nan() || prod_rt.is_nan() || prec_rt.is_nan() {
        return f64::NAN;
    }
    let dl = prod_logp - prec_logp;
    let dr = prod_rt - prec_rt;
    let sl = if dl > 0.0 {
        1.0
    } else if dl < 0.0 {
        -1.0
    } else {
        0.0
    };
    let sr = if dr > 0.0 {
        1.0
    } else if dr < 0.0 {
        -1.0
    } else {
        0.0
    };
    match phase {
        "reverse_phase" => sl * sr,
        "hilic" => -sl * sr,
        _ => f64::NAN,
    }
}

fn clamp01(value: f64) -> f64 {
    if value.is_nan() {
        return 0.0;
    }
    value.clamp(0.0, 1.0)
}

fn bool_score(value: bool) -> f64 {
    if value {
        1.0
    } else {
        0.0
    }
}

/// `nta::assign_transformation_products::formula_mass` — monoisotopic mass of
/// an elemental formula; NaN for any unknown element or malformed symbol.
fn formula_mass(formula: &str) -> f64 {
    let masses: [(&str, f64); 10] = [
        ("H", 1.00782503223),
        ("C", 12.0),
        ("N", 14.00307400443),
        ("O", 15.99491461957),
        ("F", 18.99840316273),
        ("P", 30.97376199842),
        ("S", 31.9720711744),
        ("CL", 34.968852682),
        ("BR", 78.9183376),
        ("I", 126.9044719),
    ];
    let mut total = 0.0;
    let chars: Vec<char> = formula.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if !chars[i].is_alphabetic() {
            return f64::NAN;
        }
        let mut symbol = chars[i].to_uppercase().collect::<String>();
        i += 1;
        if i < chars.len() && chars[i].is_lowercase() {
            // C++ keeps the lowercase second char and looks up the uppercased
            // symbol; replicate exactly.
            symbol.push(chars[i]);
            i += 1;
        }
        let upper = symbol.to_uppercase();
        let mass = masses
            .iter()
            .find(|(key, _)| **key == upper)
            .map(|(_, m)| *m);
        let Some(mass) = mass else {
            return f64::NAN;
        };
        let mut count = 0;
        while i < chars.len() && chars[i].is_ascii_digit() {
            count = count * 10 + chars[i].to_digit(10).unwrap() as i64;
            i += 1;
        }
        total += mass * if count == 0 { 1.0 } else { count as f64 };
    }
    total
}

/// `nta::assign_transformation_products::expected_transformation_delta` —
/// monoisotopic mass delta encoded by the transformation tag (known table or
/// a `+/-<formula>` expression); `MAIN_PRECURSOR` and empty tags map to 0.
fn expected_transformation_delta(tag: &str) -> f64 {
    let key = upper_copy(&trim_copy(tag));
    if key.is_empty() || key == "MAIN_PRECURSOR" {
        return 0.0;
    }
    const KNOWN: [(&str, &str); 15] = [
        ("HYDROXYLATION", "+O"),
        ("DIHYDROXYLATION", "+O2"),
        ("OXIDATION", "+O"),
        ("REDUCTION", "+H2"),
        ("METHYLATION", "+CH2"),
        ("DEMETHYLATION", "-CH2"),
        ("ETHYLATION", "+C2H4"),
        ("DEETHYLATION", "-C2H4"),
        ("SULFATION", "+SO3"),
        ("GLUCURONIDATION", "+C6H8O6"),
        ("ACETYLATION", "+C2H2O"),
        ("DEAMINATION", "-NH"),
        ("DEHYDRATION", "-H2O"),
        ("DECHLORINATION", "-Cl"),
        ("DEBROMINATION", "-Br"),
    ];
    let mut expr = key.clone();
    if let Some((_, replacement)) = KNOWN.iter().find(|(k, _)| *k == key) {
        expr = upper_copy(replacement);
    }
    let mut total = 0.0;
    let chars: Vec<char> = expr.chars().collect();
    let mut pos = 0;
    let mut matched = false;
    while pos < chars.len() {
        while pos < chars.len() && (chars[pos] == ' ' || chars[pos] == ',' || chars[pos] == ';') {
            pos += 1;
        }
        if pos >= chars.len() {
            break;
        }
        let sign = chars[pos];
        if sign != '+' && sign != '-' {
            return f64::NAN;
        }
        pos += 1;
        let start = pos;
        while pos < chars.len() && chars[pos].is_alphanumeric() {
            pos += 1;
        }
        if start == pos {
            return f64::NAN;
        }
        let mass = formula_mass(&expr[start..pos]);
        if mass.is_nan() {
            return f64::NAN;
        }
        total += if sign == '+' { mass } else { -mass };
        matched = true;
    }
    if matched {
        total
    } else {
        f64::NAN
    }
}

/// `nta::assign_transformation_products::combine_score`.
#[allow(clippy::too_many_arguments)]
fn combine_score(
    cos_sim: f64,
    main_cos_sim: f64,
    rt_plaus: f64,
    main_rt_plaus: f64,
    direct_present: bool,
    main_present: bool,
    main_consistent: bool,
    transformation_valid: bool,
    delta_error: f64,
) -> f64 {
    let delta_score = if delta_error.is_nan() {
        0.0
    } else {
        (1.0 - (delta_error / 0.02)).max(0.0)
    };
    5.0 * clamp01(cos_sim)
        + 2.5 * clamp01(main_cos_sim)
        + 1.0 * rt_plaus.max(0.0)
        + 0.5 * main_rt_plaus.max(0.0)
        + 0.75 * bool_score(direct_present)
        + 0.5 * bool_score(main_present)
        + 1.25 * bool_score(main_consistent)
        + 0.5 * bool_score(transformation_valid)
        + 1.5 * delta_score
}

/// `nta::assign_transformation_products::normalize_row` (NORMALIZED_ROW).
fn normalize_row(input: &TransformationProductRow) -> TransformationProductRow {
    let mut out = input.clone();
    out.name = trim_copy(&input.name);
    out.formula = trim_copy(&input.formula);
    out.SMILES = collapse_spaces(&trim_copy(&input.SMILES));
    out.InChI = trim_copy(&input.InChI);
    out.InChIKey = upper_copy(&trim_copy(&input.InChIKey));
    out.transformation = normalize_transformation_tag(&input.transformation);
    out.precursor_name = trim_copy(&input.precursor_name);
    out.precursor_formula = trim_copy(&input.precursor_formula);
    out.precursor_SMILES = collapse_spaces(&trim_copy(&input.precursor_SMILES));
    out.precursor_InChI = trim_copy(&input.precursor_InChI);
    out.precursor_InChIKey = upper_copy(&trim_copy(&input.precursor_InChIKey));
    out.main_precursor_name = trim_copy(&input.main_precursor_name);
    out.main_precursor_formula = trim_copy(&input.main_precursor_formula);
    out.main_precursor_SMILES = collapse_spaces(&trim_copy(&input.main_precursor_SMILES));
    out.main_precursor_InChI = trim_copy(&input.main_precursor_InChI);
    out.main_precursor_InChIKey = upper_copy(&trim_copy(&input.main_precursor_InChIKey));
    // Structure keys are derived internally from the normalized InChIKey/InChI/SMILES.
    out.product_structure_key = normalize_structure_key(&out.InChIKey, &out.InChI, &out.SMILES);
    out.precursor_structure_key = normalize_structure_key(
        &out.precursor_InChIKey,
        &out.precursor_InChI,
        &out.precursor_SMILES,
    );
    out.main_precursor_structure_key = normalize_structure_key(
        &out.main_precursor_InChIKey,
        &out.main_precursor_InChI,
        &out.main_precursor_SMILES,
    );
    out
}

/// `nta::assign_transformation_products::assign_transformation_products_impl`
/// — one output row per (transformation product row, product feature group).
pub fn assign_transformation_products_impl(
    suspects: &[NtaSuspectRow],
    tp_rows: &[TransformationProductRow],
    chromatographic_phase: &str,
    mzr_ms2: f64,
) -> Vec<TransformationProductRow> {
    let mut out: Vec<TransformationProductRow> = Vec::new();
    if tp_rows.is_empty() {
        return out;
    }
    let empty_fg = [String::new()];

    // structure key (InChIKey > InChI > SMILES) -> distinct feature groups,
    // and key + '\0' + feature group -> suspect indices.
    let mut fg_map: HashMap<String, Vec<String>> = HashMap::new();
    let mut sf_idx: HashMap<String, Vec<usize>> = HashMap::new();
    for (i, s) in suspects.iter().enumerate() {
        let key = normalize_structure_key(&s.InChIKey, &s.InChI, &s.SMILES);
        if key.is_empty() || s.feature_group.is_empty() {
            continue;
        }
        let groups = fg_map.entry(key.clone()).or_default();
        if !groups.iter().any(|g| *g == s.feature_group) {
            groups.push(s.feature_group.clone());
        }
        sf_idx
            .entry(format!("{key}\0{}", s.feature_group))
            .or_default()
            .push(i);
    }

    for raw_tp in tp_rows {
        let normalized = normalize_row(raw_tp);

        let prod_fgs = match fg_map.get(&normalized.product_structure_key) {
            Some(groups) => groups.clone(),
            None => empty_fg.to_vec(),
        };
        let prec_fgs = match fg_map.get(&normalized.precursor_structure_key) {
            Some(groups) => groups.clone(),
            None => empty_fg.to_vec(),
        };
        let main_fgs = match fg_map.get(&normalized.main_precursor_structure_key) {
            Some(groups) => groups.clone(),
            None => empty_fg.to_vec(),
        };

        for prod_fg in &prod_fgs {
            let mut best: Option<TransformationProductRow> = None;

            for prec_fg in &prec_fgs {
                for main_fg in &main_fgs {
                    let mut candidate = normalized.clone();
                    candidate.feature_group = prod_fg.clone();
                    candidate.precursor_feature_group = prec_fg.clone();
                    candidate.main_precursor_feature_group = main_fg.clone();

                    let mut cos_sim = f64::NAN;
                    let mut main_cos_sim = f64::NAN;
                    let mut rt_plaus = f64::NAN;
                    let mut main_rt_plaus = f64::NAN;

                    if !prod_fg.is_empty() && !prec_fg.is_empty() {
                        let ps_key = format!("{}\0{}", candidate.product_structure_key, prod_fg);
                        let qs_key = format!("{}\0{}", candidate.precursor_structure_key, prec_fg);
                        if let (Some(ps_idx), Some(qs_idx)) =
                            (sf_idx.get(&ps_key), sf_idx.get(&qs_key))
                        {
                            let prod_rts: Vec<f64> =
                                ps_idx.iter().map(|&i| suspects[i].exp_rt).collect();
                            let prec_rts: Vec<f64> =
                                qs_idx.iter().map(|&i| suspects[i].exp_rt).collect();
                            rt_plaus = rt_plausibility(
                                candidate.xLogP,
                                candidate.precursor_xLogP,
                                mean_rt(&prod_rts),
                                mean_rt(&prec_rts),
                                chromatographic_phase,
                            );
                            let mut best_cos = f64::NAN;
                            for &pi in ps_idx {
                                if suspects[pi].exp_ms2_size <= 0 {
                                    continue;
                                }
                                let mz1 = decode_encoded(&suspects[pi].exp_ms2_mz);
                                let in1 = decode_encoded(&suspects[pi].exp_ms2_intensity);
                                for &qi in qs_idx {
                                    if suspects[qi].exp_ms2_size <= 0 {
                                        continue;
                                    }
                                    let mz2 = decode_encoded(&suspects[qi].exp_ms2_mz);
                                    let in2 = decode_encoded(&suspects[qi].exp_ms2_intensity);
                                    let value = cosine_similarity(&mz1, &in1, &mz2, &in2, mzr_ms2);
                                    if !value.is_nan() && (best_cos.is_nan() || value > best_cos) {
                                        best_cos = value;
                                    }
                                }
                            }
                            cos_sim = best_cos;
                        }
                    }

                    if !prod_fg.is_empty()
                        && !main_fg.is_empty()
                        && !candidate.main_precursor_structure_key.is_empty()
                    {
                        let ps_key = format!("{}\0{}", candidate.product_structure_key, prod_fg);
                        let ms_key =
                            format!("{}\0{}", candidate.main_precursor_structure_key, main_fg);
                        if let (Some(ps_idx), Some(ms_idx)) =
                            (sf_idx.get(&ps_key), sf_idx.get(&ms_key))
                        {
                            let prod_rts: Vec<f64> =
                                ps_idx.iter().map(|&i| suspects[i].exp_rt).collect();
                            let main_rts: Vec<f64> =
                                ms_idx.iter().map(|&i| suspects[i].exp_rt).collect();
                            main_rt_plaus = rt_plausibility(
                                candidate.xLogP,
                                candidate.main_precursor_xLogP,
                                mean_rt(&prod_rts),
                                mean_rt(&main_rts),
                                chromatographic_phase,
                            );
                            let mut best_cos = f64::NAN;
                            for &pi in ps_idx {
                                if suspects[pi].exp_ms2_size <= 0 {
                                    continue;
                                }
                                let mz1 = decode_encoded(&suspects[pi].exp_ms2_mz);
                                let in1 = decode_encoded(&suspects[pi].exp_ms2_intensity);
                                for &mi in ms_idx {
                                    if suspects[mi].exp_ms2_size <= 0 {
                                        continue;
                                    }
                                    let mz2 = decode_encoded(&suspects[mi].exp_ms2_mz);
                                    let in2 = decode_encoded(&suspects[mi].exp_ms2_intensity);
                                    let value = cosine_similarity(&mz1, &in1, &mz2, &in2, mzr_ms2);
                                    if !value.is_nan() && (best_cos.is_nan() || value > best_cos) {
                                        best_cos = value;
                                    }
                                }
                            }
                            main_cos_sim = best_cos;
                        }
                    }

                    candidate.cosine_similarity = cos_sim;
                    candidate.main_precursor_cosine_similarity = main_cos_sim;
                    candidate.rt_plausibility = rt_plaus;
                    candidate.main_precursor_rt_plausibility = main_rt_plaus;
                    candidate.transformation_mass_delta_observed =
                        if candidate.mass.is_nan() || candidate.precursor_mass.is_nan() {
                            f64::NAN
                        } else {
                            candidate.mass - candidate.precursor_mass
                        };
                    candidate.transformation_mass_delta_expected =
                        expected_transformation_delta(&candidate.transformation);
                    candidate.transformation_mass_delta_error =
                        if candidate.transformation_mass_delta_expected.is_nan()
                            || candidate.transformation_mass_delta_observed.is_nan()
                        {
                            f64::NAN
                        } else {
                            (candidate.transformation_mass_delta_observed
                                - candidate.transformation_mass_delta_expected)
                                .abs()
                        };
                    candidate.transformation_valid = candidate.transformation == "main_precursor"
                        || (!candidate.transformation_mass_delta_error.is_nan()
                            && candidate.transformation_mass_delta_error <= 0.01);

                    candidate.resolved_direct_parent_feature_group = prec_fg.clone();
                    candidate.resolved_main_parent_feature_group = if main_fg.is_empty() {
                        prec_fg.clone()
                    } else {
                        main_fg.clone()
                    };
                    candidate.is_direct_assignment = !prec_fg.is_empty();
                    candidate.is_main_parent_consistent = candidate.transformation
                        == "main_precursor"
                        || candidate.main_precursor_structure_key.is_empty()
                        || main_fg.is_empty()
                        || candidate.resolved_main_parent_feature_group == *main_fg;

                    candidate.network_level = 0;
                    if candidate.transformation != "main_precursor" {
                        candidate.network_level = if candidate.resolved_main_parent_feature_group
                            == candidate.resolved_direct_parent_feature_group
                        {
                            1
                        } else {
                            2
                        };
                    }

                    candidate.assignment_score = combine_score(
                        candidate.cosine_similarity,
                        candidate.main_precursor_cosine_similarity,
                        candidate.rt_plausibility,
                        candidate.main_precursor_rt_plausibility,
                        candidate.is_direct_assignment,
                        !candidate.resolved_main_parent_feature_group.is_empty(),
                        candidate.is_main_parent_consistent,
                        candidate.transformation_valid,
                        candidate.transformation_mass_delta_error,
                    );

                    let take = match &best {
                        Some(current) => candidate.assignment_score > current.assignment_score,
                        None => true,
                    };
                    if take {
                        best = Some(candidate);
                    }
                }
            }

            // Unreachable fallback branch (the grouping vectors always contain
            // at least the empty-string sentinel); kept for C++ fidelity.
            let Some(mut best_row) = best else {
                let mut fallback = normalized.clone();
                fallback.feature_group = prod_fg.clone();
                fallback.resolved_main_parent_feature_group = prod_fg.clone();
                fallback.assignment_status = "unresolved".to_string();
                fallback.transformation_mass_delta_expected =
                    expected_transformation_delta(&fallback.transformation);
                fallback.transformation_mass_delta_observed = f64::NAN;
                fallback.transformation_mass_delta_error = f64::NAN;
                out.push(fallback);
                continue;
            };

            best_row.assignment_rank = 1;
            best_row.assignment_status = if best_row.is_direct_assignment {
                "assigned".to_string()
            } else {
                "unresolved".to_string()
            };
            if best_row.transformation == "main_precursor" {
                best_row.resolved_direct_parent_feature_group = best_row.feature_group.clone();
                best_row.resolved_main_parent_feature_group = best_row.feature_group.clone();
                best_row.is_direct_assignment = true;
                best_row.is_main_parent_consistent = true;
                best_row.transformation_valid = true;
                best_row.network_level = 0;
            }
            out.push(best_row);
        }
    }

    out
}

fn row_text(row: &Value, key: &str) -> String {
    row.get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}

fn row_num(row: &Value, key: &str) -> f64 {
    row.get(key).and_then(Value::as_f64).unwrap_or(f64::NAN)
}

fn parse_transformation_product_row(row: &Value) -> TransformationProductRow {
    let mut r = TransformationProductRow::default();
    r.name = row_text(row, "name");
    r.formula = row_text(row, "formula");
    r.mass = row_num(row, "mass");
    r.SMILES = row_text(row, "SMILES");
    r.InChI = row_text(row, "InChI");
    r.InChIKey = row_text(row, "InChIKey");
    r.xLogP = row_num(row, "xLogP");
    r.transformation = row_text(row, "transformation");
    r.precursor_name = row_text(row, "precursor_name");
    r.precursor_formula = row_text(row, "precursor_formula");
    r.precursor_mass = row_num(row, "precursor_mass");
    r.precursor_SMILES = row_text(row, "precursor_SMILES");
    r.precursor_InChI = row_text(row, "precursor_InChI");
    r.precursor_InChIKey = row_text(row, "precursor_InChIKey");
    r.precursor_xLogP = row_num(row, "precursor_xLogP");
    r.main_precursor_name = row_text(row, "main_precursor_name");
    r.main_precursor_formula = row_text(row, "main_precursor_formula");
    r.main_precursor_mass = row_num(row, "main_precursor_mass");
    r.main_precursor_SMILES = row_text(row, "main_precursor_SMILES");
    r.main_precursor_InChI = row_text(row, "main_precursor_InChI");
    r.main_precursor_InChIKey = row_text(row, "main_precursor_InChIKey");
    r.main_precursor_xLogP = row_num(row, "main_precursor_xLogP");
    r
}

/// The analysis owning `feature_group` (first suspect with that group, in
/// analysis order); falls back to the first analysis when no suspect carries
/// the group (mirrors the R/C++ model where assignments are project-scoped by
/// feature group and the SUSPECTS table requires an analysis).
fn analysis_index_of_group(data: &ProjectNonTargetAnalysis, feature_group: &str) -> usize {
    for (i, buffer) in data.suspect_buffers.iter().enumerate() {
        for j in 0..buffer.size() {
            if buffer.get_suspect(j).feature_group == feature_group {
                return i;
            }
        }
    }
    0
}

/// Convert an assignment row to a SUSPECT row for the SUSPECTS table. The
/// SUSPECTS primary key is (analysis, feature); assignment rows
/// have no underlying feature, so the product feature group (when resolved)
/// or the transformation product name is used as the feature identifier —
/// this keeps one row per product assignment while staying unique.
fn transformation_product_to_suspect(
    row: &TransformationProductRow,
    analysis: &str,
) -> NtaSuspectRow {
    let mut s = NtaSuspectRow::default();
    s.analysis = analysis.to_string();
    s.feature = if row.feature_group.is_empty() {
        row.name.clone()
    } else {
        format!("{}/{}", row.feature_group, row.name)
    };
    s.feature_group = row.feature_group.clone();
    s.candidate_rank = row.assignment_rank.max(1);
    s.name = row.name.clone();
    s.polarity = 0;
    s.db_mass = row.mass;
    s.exp_mass = f64::NAN;
    s.error_mass = f64::NAN;
    s.db_rt = f64::NAN;
    s.exp_rt = f64::NAN;
    s.error_rt = f64::NAN;
    s.intensity = f64::NAN;
    s.area = f64::NAN;
    s.id_level = 0;
    s.score = row.assignment_score;
    s.shared_fragments = 0;
    s.cosine_similarity = row.cosine_similarity;
    s.formula = row.formula.clone();
    s.SMILES = row.SMILES.clone();
    s.InChI = row.InChI.clone();
    s.InChIKey = row.InChIKey.clone();
    s.xLogP = row.xLogP;
    s.database_id = String::new();
    s.db_ms2_size = 0;
    s.exp_ms2_size = 0;
    s
}

fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::InvalidArgument, message)
}

/// Executor for `mass_spec.assign_transformation_products`.
///
/// Works on the CURRENT suspect buffer (from a prior suspect_screening run),
/// appends the combination-scored assignment rows, and persists the SUSPECTS
/// table via the existing `persist_suspects` path.
pub fn assign_transformation_products(project: &mut Project, p: &Value) -> Result<Value> {
    let mzr_ms2 = p.get("mzr_ms2").and_then(Value::as_f64).unwrap_or(0.008);
    let chromatographic_phase = p
        .get("chromatographic_phase")
        .and_then(Value::as_str)
        .unwrap_or("reverse_phase")
        .to_string();
    if mzr_ms2.is_nan() || mzr_ms2 < 0.0 {
        return Err(invalid("mzr_ms2 must be >= 0"));
    }
    if chromatographic_phase != "reverse_phase" && chromatographic_phase != "hilic" {
        return Err(invalid(
            "chromatographic_phase must be \"reverse_phase\" or \"hilic\"",
        ));
    }

    let mut tp_rows = Vec::new();
    if let Some(rows) = p.get("transformation_products").and_then(Value::as_array) {
        for row in rows {
            tp_rows.push(parse_transformation_product_row(row));
        }
    }

    let mut data = load_analysis_features(project, p)?;
    load_suspects(project, &mut data)?;

    let mut suspects = Vec::new();
    for buffer in &data.suspect_buffers {
        for i in 0..buffer.size() {
            suspects.push(buffer.get_suspect(i));
        }
    }

    let out =
        assign_transformation_products_impl(&suspects, &tp_rows, &chromatographic_phase, mzr_ms2);

    // Persist real transformation-product rows (one per assignment, with the
    // resolved analysis) alongside the suspects-table append; the analysis
    // resolution mirrors the group -> buffer logic below.
    let mut tp_rows_with_analysis = Vec::new();
    for row in out {
        let index = analysis_index_of_group(&data, &row.feature_group);
        let analysis = data.analysis_names()[index].clone();
        tp_rows_with_analysis.push((analysis.clone(), row.clone()));
        let suspect = transformation_product_to_suspect(&row, &analysis);
        data.suspect_buffers[index].append(&suspect);
    }

    persist_suspects(project, &data)?;
    persist_transformation_products(project, &tp_rows_with_analysis)?;
    Ok(finished("Assigning transformation products completed."))
}
