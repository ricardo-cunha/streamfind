//! NTA filtering.
//!
//! Ported from `core/domains/mass_spec/src/nta_filters.cpp`
//! (`nta::filter_features::filter_features_impl`,
//! `nta::filter_suspects::filter_suspects_impl`,
//! `nta::filter_internal_standards::filter_internal_standards_impl` and
//! `nta::filter_features_ms2::filter_features_ms2_impl`). Keep the filtering
//! semantics, the set/order of `filter` reason strings and the row
//! removal/copying behaviour identical to the C++ operations.
//!
//! Threshold arguments mirror the C++ `double` signature; NaN disables a
//! filter. Feature columns are `f32`/`i32` exactly like the C++ `float`/`int`
//! columns, so threshold values are narrowed to `f32` before comparison, as
//! the C++ `static_cast<float>` does.

use std::collections::{HashMap, HashSet};

use streamfind_rust_core::Result;

use crate::nta::{NtaFeatures, NtaInternalStandards, NtaSuspects, ProjectNonTargetAnalysis};
use crate::nta_utils::{decode_floats_base64, encode_floats_base64};

/// `nta::filter_features::FilterParams`.
struct FilterParams {
    min_sn: Option<f32>,
    min_intensity: Option<f32>,
    min_area: Option<f32>,
    min_width: Option<f32>,
    max_width: Option<f32>,
    max_ppm: Option<f32>,
    min_fwhm_rt: Option<f32>,
    max_fwhm_rt: Option<f32>,
    min_fwhm_mz: Option<f32>,
    max_fwhm_mz: Option<f32>,
    min_gaussian_a: Option<f32>,
    min_gaussian_mu: Option<f32>,
    max_gaussian_mu: Option<f32>,
    min_gaussian_sigma: Option<f32>,
    max_gaussian_sigma: Option<f32>,
    min_gaussian_r2: Option<f32>,
    max_jaggedness: Option<f32>,
    min_sharpness: Option<f32>,
    min_asymmetry: Option<f32>,
    max_asymmetry: Option<f32>,
    max_modality: Option<i32>,
    min_plates: Option<f32>,
    remove_filled: bool,
    min_size_eic: Option<i32>,
    min_size_ms1: Option<i32>,
    min_size_ms2: Option<i32>,
    min_rel_presence_replicate: Option<f32>,
    remove_isotopes: bool,
    remove_adducts: bool,
    remove_losses: bool,
}

impl Default for FilterParams {
    fn default() -> Self {
        Self {
            min_sn: None,
            min_intensity: None,
            min_area: None,
            min_width: None,
            max_width: None,
            max_ppm: None,
            min_fwhm_rt: None,
            max_fwhm_rt: None,
            min_fwhm_mz: None,
            max_fwhm_mz: None,
            min_gaussian_a: None,
            min_gaussian_mu: None,
            max_gaussian_mu: None,
            min_gaussian_sigma: None,
            max_gaussian_sigma: None,
            min_gaussian_r2: None,
            max_jaggedness: None,
            min_sharpness: None,
            min_asymmetry: None,
            max_asymmetry: None,
            max_modality: None,
            min_plates: None,
            remove_filled: false,
            min_size_eic: None,
            min_size_ms1: None,
            min_size_ms2: None,
            min_rel_presence_replicate: None,
            remove_isotopes: false,
            remove_adducts: false,
            remove_losses: false,
        }
    }
}

/// `nta::filter_features::append_filter` — append a reason name to a feature's
/// `filter` string, space separated when already set.
fn append_filter(filter: &mut String, name: &str) {
    if filter.is_empty() {
        *filter = name.to_owned();
    } else {
        filter.push(' ');
        filter.push_str(name);
    }
}

/// `nta::filter_features::filter_features_impl`'s `apply_filter` loop: mark
/// every not-yet-filtered feature satisfying `predicate` as filtered and
/// append `name` to its `filter` string.
fn apply_filter(
    feature_buffers: &mut [NtaFeatures],
    replicate_names: &[String],
    name: &str,
    predicate: impl Fn(&NtaFeatures, usize, &str) -> bool,
) {
    for (a, fts) in feature_buffers.iter_mut().enumerate() {
        let replicate = replicate_names[a].as_str();
        for i in 0..fts.size() {
            if fts.filtered[i] {
                continue;
            }
            if predicate(fts, i, replicate) {
                fts.filtered[i] = true;
                append_filter(&mut fts.filter[i], name);
            }
        }
    }
}

/// `nta::filter_features::filter_features_impl`.
///
/// Flags features (filtered=true + filter reason) in place; rows are never
/// removed. Filter order and reason strings match the C++ exactly.
#[allow(clippy::too_many_arguments)]
pub fn filter_features_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    min_sn: f64,
    min_intensity: f64,
    min_area: f64,
    min_width: f64,
    max_width: f64,
    max_ppm: f64,
    min_fwhm_rt: f64,
    max_fwhm_rt: f64,
    min_fwhm_mz: f64,
    max_fwhm_mz: f64,
    min_gaussian_a: f64,
    min_gaussian_mu: f64,
    max_gaussian_mu: f64,
    min_gaussian_sigma: f64,
    max_gaussian_sigma: f64,
    min_gaussian_r2: f64,
    max_jaggedness: f64,
    min_sharpness: f64,
    min_asymmetry: f64,
    max_asymmetry: f64,
    max_modality: i32,
    has_max_modality: bool,
    min_plates: f64,
    remove_filled: bool,
    min_size_eic: i32,
    has_min_size_eic: bool,
    min_size_ms1: i32,
    has_min_size_ms1: bool,
    min_size_ms2: i32,
    has_min_size_ms2: bool,
    min_rel_presence_replicate: f64,
    remove_isotopes: bool,
    remove_adducts: bool,
    remove_losses: bool,
) -> Result<()> {
    let analysis_names: Vec<String> = nta_data.analysis_names().to_vec();
    let replicate_names: Vec<String> = nta_data.replicate_names().to_vec();
    let feature_buffers = &mut nta_data.feature_buffers;

    if analysis_names.is_empty() {
        return Ok(());
    }

    let mut params = FilterParams::default();
    if !min_sn.is_nan() {
        params.min_sn = Some(min_sn as f32);
    }
    if !min_intensity.is_nan() {
        params.min_intensity = Some(min_intensity as f32);
    }
    if !min_area.is_nan() {
        params.min_area = Some(min_area as f32);
    }
    if !min_width.is_nan() {
        params.min_width = Some(min_width as f32);
    }
    if !max_width.is_nan() {
        params.max_width = Some(max_width as f32);
    }
    if !max_ppm.is_nan() {
        params.max_ppm = Some(max_ppm as f32);
    }
    if !min_fwhm_rt.is_nan() {
        params.min_fwhm_rt = Some(min_fwhm_rt as f32);
    }
    if !max_fwhm_rt.is_nan() {
        params.max_fwhm_rt = Some(max_fwhm_rt as f32);
    }
    if !min_fwhm_mz.is_nan() {
        params.min_fwhm_mz = Some(min_fwhm_mz as f32);
    }
    if !max_fwhm_mz.is_nan() {
        params.max_fwhm_mz = Some(max_fwhm_mz as f32);
    }
    if !min_gaussian_a.is_nan() {
        params.min_gaussian_a = Some(min_gaussian_a as f32);
    }
    if !min_gaussian_mu.is_nan() {
        params.min_gaussian_mu = Some(min_gaussian_mu as f32);
    }
    if !max_gaussian_mu.is_nan() {
        params.max_gaussian_mu = Some(max_gaussian_mu as f32);
    }
    if !min_gaussian_sigma.is_nan() {
        params.min_gaussian_sigma = Some(min_gaussian_sigma as f32);
    }
    if !max_gaussian_sigma.is_nan() {
        params.max_gaussian_sigma = Some(max_gaussian_sigma as f32);
    }
    if !min_gaussian_r2.is_nan() {
        params.min_gaussian_r2 = Some(min_gaussian_r2 as f32);
    }
    if !max_jaggedness.is_nan() {
        params.max_jaggedness = Some(max_jaggedness as f32);
    }
    if !min_sharpness.is_nan() {
        params.min_sharpness = Some(min_sharpness as f32);
    }
    if !min_asymmetry.is_nan() {
        params.min_asymmetry = Some(min_asymmetry as f32);
    }
    if !max_asymmetry.is_nan() {
        params.max_asymmetry = Some(max_asymmetry as f32);
    }
    if has_max_modality {
        params.max_modality = Some(max_modality);
    }
    if !min_plates.is_nan() {
        params.min_plates = Some(min_plates as f32);
    }
    params.remove_filled = remove_filled;
    if has_min_size_eic {
        params.min_size_eic = Some(min_size_eic);
    }
    if has_min_size_ms1 {
        params.min_size_ms1 = Some(min_size_ms1);
    }
    if has_min_size_ms2 {
        params.min_size_ms2 = Some(min_size_ms2);
    }
    if !min_rel_presence_replicate.is_nan() {
        params.min_rel_presence_replicate = Some(min_rel_presence_replicate as f32);
    }
    params.remove_isotopes = remove_isotopes;
    params.remove_adducts = remove_adducts;
    params.remove_losses = remove_losses;

    // Precompute replicate mapping (kept for parity with the C++).
    let _analysis_to_replicate: HashMap<String, String> = analysis_names
        .iter()
        .zip(replicate_names.iter())
        .map(|(analysis, replicate)| (analysis.clone(), replicate.clone()))
        .collect();

    // Precompute replicate counts.
    let mut replicate_counts: HashMap<String, i32> = HashMap::new();
    for rep in &replicate_names {
        *replicate_counts.entry(rep.clone()).or_insert(0) += 1;
    }

    // Precompute minRelPresenceReplicate keys if needed.
    let mut low_presence_keys: HashSet<String> = HashSet::new();
    if let Some(min_rel_presence) = params.min_rel_presence_replicate {
        let mut grp_analyses: HashMap<String, HashSet<String>> = HashMap::new();
        for a in 0..analysis_names.len() {
            let analysis = &analysis_names[a];
            let replicate = &replicate_names[a];
            let fts = &feature_buffers[a];
            for i in 0..fts.size() {
                if fts.filtered[i] {
                    continue;
                }
                let fg = &fts.feature_group[i];
                if fg.is_empty() {
                    continue;
                }
                grp_analyses
                    .entry(format!("{}|{}", replicate, fg))
                    .or_default()
                    .insert(analysis.clone());
            }
        }
        for (key, analyses) in &grp_analyses {
            let replicate = match key.find('|') {
                Some(sep) => &key[..sep],
                None => key.as_str(),
            };
            let n_analyses = replicate_counts.get(replicate).copied().unwrap_or(0);
            let n_features = analyses.len() as i32;
            let mut rel_presence = 0.0f32;
            if n_analyses > 0 && n_features > 0 {
                rel_presence = n_features as f32 / n_analyses as f32;
            }
            if rel_presence < min_rel_presence {
                low_presence_keys.insert(key.clone());
            }
        }
    }

    if let Some(v) = params.min_sn {
        apply_filter(feature_buffers, &replicate_names, "minSN", |fts, i, _| {
            !fts.sn[i].is_nan() && fts.sn[i] < v
        });
    }

    if let Some(v) = params.min_intensity {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minIntensity",
            |fts, i, _| !fts.intensity[i].is_nan() && fts.intensity[i] < v,
        );
    }

    if let Some(v) = params.min_area {
        apply_filter(feature_buffers, &replicate_names, "minArea", |fts, i, _| {
            !fts.area[i].is_nan() && fts.area[i] < v
        });
    }

    if let Some(v) = params.min_width {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minWidth",
            |fts, i, _| !fts.width[i].is_nan() && fts.width[i] < v,
        );
    }

    if let Some(v) = params.max_width {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxWidth",
            |fts, i, _| !fts.width[i].is_nan() && fts.width[i] > v,
        );
    }

    if let Some(v) = params.max_ppm {
        apply_filter(feature_buffers, &replicate_names, "maxPPM", |fts, i, _| {
            !fts.ppm[i].is_nan() && fts.ppm[i] > v
        });
    }

    if let Some(v) = params.min_fwhm_rt {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minFwhmRT",
            |fts, i, _| !fts.fwhm_rt[i].is_nan() && fts.fwhm_rt[i] < v,
        );
    }

    if let Some(v) = params.max_fwhm_rt {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxFwhmRT",
            |fts, i, _| !fts.fwhm_rt[i].is_nan() && fts.fwhm_rt[i] > v,
        );
    }

    if let Some(v) = params.min_fwhm_mz {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minFwhmMZ",
            |fts, i, _| !fts.fwhm_mz[i].is_nan() && fts.fwhm_mz[i] < v,
        );
    }

    if let Some(v) = params.max_fwhm_mz {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxFwhmMZ",
            |fts, i, _| !fts.fwhm_mz[i].is_nan() && fts.fwhm_mz[i] > v,
        );
    }

    if let Some(v) = params.min_gaussian_a {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minGaussianA",
            |fts, i, _| !fts.gaussian_A[i].is_nan() && fts.gaussian_A[i] < v,
        );
    }

    if let Some(v) = params.min_gaussian_mu {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minGaussianMu",
            |fts, i, _| !fts.gaussian_mu[i].is_nan() && fts.gaussian_mu[i] < v,
        );
    }

    if let Some(v) = params.max_gaussian_mu {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxGaussianMu",
            |fts, i, _| !fts.gaussian_mu[i].is_nan() && fts.gaussian_mu[i] > v,
        );
    }

    if let Some(v) = params.min_gaussian_sigma {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minGaussianSigma",
            |fts, i, _| !fts.gaussian_sigma[i].is_nan() && fts.gaussian_sigma[i] < v,
        );
    }

    if let Some(v) = params.max_gaussian_sigma {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxGaussianSigma",
            |fts, i, _| !fts.gaussian_sigma[i].is_nan() && fts.gaussian_sigma[i] > v,
        );
    }

    if let Some(v) = params.min_gaussian_r2 {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minGaussianR2",
            |fts, i, _| !fts.gaussian_r2[i].is_nan() && fts.gaussian_r2[i] < v,
        );
    }

    if let Some(v) = params.max_jaggedness {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxJaggedness",
            |fts, i, _| !fts.jaggedness[i].is_nan() && fts.jaggedness[i] > v,
        );
    }

    if let Some(v) = params.min_sharpness {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minSharpness",
            |fts, i, _| !fts.sharpness[i].is_nan() && fts.sharpness[i] < v,
        );
    }

    if let Some(v) = params.min_asymmetry {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minAsymmetry",
            |fts, i, _| !fts.asymmetry[i].is_nan() && fts.asymmetry[i] < v,
        );
    }

    if let Some(v) = params.max_asymmetry {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxAsymmetry",
            |fts, i, _| !fts.asymmetry[i].is_nan() && fts.asymmetry[i] > v,
        );
    }

    if let Some(v) = params.max_modality {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "maxModality",
            |fts, i, _| fts.modality[i] > v,
        );
    }

    if let Some(v) = params.min_plates {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minPlates",
            |fts, i, _| !fts.plates[i].is_nan() && fts.plates[i] < v,
        );
    }


    if params.remove_filled {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "removeFilled",
            |fts, i, _| fts.filled[i],
        );
    }

    if let Some(v) = params.min_size_eic {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minSizeEIC",
            |fts, i, _| fts.eic_size[i] < v,
        );
    }

    if let Some(v) = params.min_size_ms1 {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minSizeMS1",
            |fts, i, _| fts.ms1_size[i] < v,
        );
    }

    if let Some(v) = params.min_size_ms2 {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "minSizeMS2",
            |fts, i, _| fts.ms2_size[i] < v,
        );
    }

    if let Some(v) = params.min_rel_presence_replicate {
        if v > 0.0 {
            apply_filter(
                feature_buffers,
                &replicate_names,
                "minRelPresenceReplicate",
                |fts, i, replicate| {
                    let fg = &fts.feature_group[i];
                    if fg.is_empty() {
                        return false;
                    }
                    let key = format!("{}|{}", replicate, fg);
                    low_presence_keys.contains(&key)
                },
            );
        }
    }

    if params.remove_isotopes {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "removeIsotopes",
            |fts, i, _| fts.annotation_category[i] == "isotope",
        );
    }

    if params.remove_adducts {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "removeAdducts",
            |fts, i, _| fts.annotation_category[i] == "adduct",
        );
    }

    if params.remove_losses {
        apply_filter(
            feature_buffers,
            &replicate_names,
            "removeLosses",
            |fts, i, _| fts.annotation_category[i] == "loss",
        );
    }

    Ok(())
}

/// Shared keep-vector filtering for the suspect/internal-standard buffers
/// (`nta::filter_suspects` and `nta::filter_internal_standards`).
struct KeepCriterion<'a> {
    names: &'a [String],
    min_score: f64,
    max_error_rt: f64,
    max_error_mass: f64,
    id_levels: &'a [i32],
    min_shared_fragments: i32,
    min_cosine_similarity: f64,
}

impl KeepCriterion<'_> {
    fn keep(
        &self,
        name: &str,
        score: f64,
        error_rt: f64,
        error_mass: f64,
        id_level: i32,
        shared_fragments: i32,
        cosine_similarity: f64,
    ) -> bool {
        if !self.names.is_empty() {
            let name_found = self.names.iter().any(|wanted| name.contains(wanted));
            if !name_found {
                return false;
            }
        }
        if !self.min_score.is_nan() && !score.is_nan() && score < self.min_score {
            return false;
        }
        if !self.max_error_rt.is_nan() && !error_rt.is_nan() && error_rt.abs() > self.max_error_rt {
            return false;
        }
        if !self.max_error_mass.is_nan()
            && !error_mass.is_nan()
            && error_mass.abs() > self.max_error_mass
        {
            return false;
        }
        if !self.id_levels.is_empty() && !self.id_levels.contains(&id_level) {
            return false;
        }
        if self.min_shared_fragments > 0 && shared_fragments < self.min_shared_fragments {
            return false;
        }
        if !self.min_cosine_similarity.is_nan()
            && !cosine_similarity.is_nan()
            && cosine_similarity < self.min_cosine_similarity
        {
            return false;
        }
        true
    }
}

/// `nta::filter_suspects::filter_suspects_impl` — replaces every suspect
/// buffer with the rows that pass the criteria, preserving row order.
pub fn filter_suspects_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    names: &[String],
    min_score: f64,
    max_error_rt: f64,
    max_error_mass: f64,
    id_levels: &[i32],
    min_shared_fragments: i32,
    min_cosine_similarity: f64,
) -> Result<()> {
    let criterion = KeepCriterion {
        names,
        min_score,
        max_error_rt,
        max_error_mass,
        id_levels,
        min_shared_fragments,
        min_cosine_similarity,
    };
    for sus in nta_data.suspect_buffers.iter_mut() {
        if sus.size() == 0 {
            continue;
        }
        let keep: Vec<bool> = (0..sus.size())
            .map(|i| {
                let row = sus.get_suspect(i);
                criterion.keep(
                    &row.name,
                    row.score,
                    row.error_rt,
                    row.error_mass,
                    row.id_level,
                    row.shared_fragments,
                    row.cosine_similarity,
                )
            })
            .collect();
        let mut filtered = NtaSuspects::default();
        for i in 0..sus.size() {
            if keep[i] {
                let row = sus.get_suspect(i);
                filtered.append(&row);
            }
        }
        *sus = filtered;
    }
    Ok(())
}

/// `nta::filter_internal_standards::filter_internal_standards_impl` —
/// replaces every internal-standard buffer with the rows that pass the
/// criteria, preserving row order.
pub fn filter_internal_standards_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    names: &[String],
    min_score: f64,
    max_error_rt: f64,
    max_error_mass: f64,
    id_levels: &[i32],
    min_shared_fragments: i32,
    min_cosine_similarity: f64,
) -> Result<()> {
    let criterion = KeepCriterion {
        names,
        min_score,
        max_error_rt,
        max_error_mass,
        id_levels,
        min_shared_fragments,
        min_cosine_similarity,
    };
    for istd in nta_data.internal_standard_buffers.iter_mut() {
        if istd.size() == 0 {
            continue;
        }
        let keep: Vec<bool> = (0..istd.size())
            .map(|i| {
                let row = istd.get_internal_standard(i);
                criterion.keep(
                    &row.name,
                    row.score,
                    row.error_rt,
                    row.error_mass,
                    row.id_level,
                    row.shared_fragments,
                    row.cosine_similarity,
                )
            })
            .collect();
        let mut filtered = NtaInternalStandards::default();
        for i in 0..istd.size() {
            if keep[i] {
                let row = istd.get_internal_standard(i);
                filtered.append(&row);
            }
        }
        *istd = filtered;
    }
    Ok(())
}

// MARK: filter_features_ms2

/// Cluster a flat list of (uid, mz) pairs from multiple features/analyses.
/// Returns representative mz values (cluster means) that appear in >=
/// `presence_fraction` of the input uids.
fn cluster_ms2_mz(
    uids: &[String],
    all_mz: &[f32],
    mz_clust: f32,
    presence_fraction: f32,
) -> Vec<f32> {
    if all_mz.is_empty() {
        return Vec::new();
    }
    let total_uids = uids.iter().collect::<HashSet<&String>>().len();
    if total_uids == 0 {
        return Vec::new();
    }
    let mut idx: Vec<usize> = (0..all_mz.len()).collect();
    idx.sort_by(|&a, &b| {
        all_mz[a]
            .partial_cmp(&all_mz[b])
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    let mut cluster_mz = Vec::new();
    let mut i = 0usize;
    while i < idx.len() {
        let ref_mz = all_mz[idx[i]];
        let mut cluster_uids: HashSet<&String> = HashSet::new();
        let mut j = i;
        while j < idx.len() && all_mz[idx[j]] <= ref_mz + 2.0 * mz_clust {
            cluster_uids.insert(&uids[idx[j]]);
            j += 1;
        }
        let presence = cluster_uids.len() as f32 / total_uids as f32;
        if presence >= presence_fraction {
            let mut sum = 0.0f32;
            for k in i..j {
                sum += all_mz[idx[k]];
            }
            cluster_mz.push(sum / (j - i) as f32);
        }
        i = j;
    }
    cluster_mz
}

/// True if any value in `background_mz` is within `mz_clust` of `query_mz`.
fn in_background(query_mz: f32, background_mz: &[f32], mz_clust: f32) -> bool {
    for b in background_mz {
        if (query_mz - b).abs() <= mz_clust {
            return true;
        }
    }
    false
}

/// Per-spectrum filters (top-N, minIntensity, relMinIntensity) applied in this
/// order, matching `nta::filter_features_ms2::apply_spectrum_filters`.
fn apply_spectrum_filters(
    mz: &mut Vec<f32>,
    intensity: &mut Vec<f32>,
    top: i32,
    min_intensity: f32,
    rel_min_intensity: f32,
) {
    if mz.is_empty() {
        return;
    }
    if top > 0 && mz.len() as i32 > top {
        let mut ord: Vec<usize> = (0..mz.len()).collect();
        ord.sort_by(|&a, &b| {
            intensity[b]
                .partial_cmp(&intensity[a])
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        ord.truncate(top as usize);
        ord.sort_by(|&a, &b| {
            mz[a]
                .partial_cmp(&mz[b])
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        let new_mz: Vec<f32> = ord.iter().map(|&k| mz[k]).collect();
        let new_int: Vec<f32> = ord.iter().map(|&k| intensity[k]).collect();
        *mz = new_mz;
        *intensity = new_int;
    }
    if !min_intensity.is_nan() {
        let mut new_mz = Vec::new();
        let mut new_int = Vec::new();
        for k in 0..mz.len() {
            if intensity[k] >= min_intensity {
                new_mz.push(mz[k]);
                new_int.push(intensity[k]);
            }
        }
        *mz = new_mz;
        *intensity = new_int;
    }
    if !rel_min_intensity.is_nan() && !mz.is_empty() {
        let max_int = intensity.iter().copied().fold(f32::NEG_INFINITY, f32::max);
        let threshold = max_int * rel_min_intensity;
        let mut new_mz = Vec::new();
        let mut new_int = Vec::new();
        for k in 0..mz.len() {
            if intensity[k] >= threshold {
                new_mz.push(mz[k]);
                new_int.push(intensity[k]);
            }
        }
        *mz = new_mz;
        *intensity = new_int;
    }
}

/// `nta::filter_features_ms2::filter_features_ms2_impl` — collects MS2 peaks
/// from blank analyses, clusters them, removes any peak within `mz_clust` of a
/// background trace and applies per-spectrum filters. Encoded peak lists
/// (`ms2_mz`, `ms2_intensity`, `ms2_size`) are updated in place; no feature
/// rows are removed.
pub fn filter_features_ms2_impl(
    nta_data: &mut ProjectNonTargetAnalysis,
    top: i32,
    min_intensity: f32,
    rel_min_intensity: f32,
    blank_clean: bool,
    mz_clust: f32,
    blank_presence_threshold: f32,
    global_presence_threshold: f32,
) -> Result<()> {
    let analysis_names: Vec<String> = nta_data.analysis_names().to_vec();
    let replicate_names: Vec<String> = nta_data.replicate_names().to_vec();
    let blank_names: Vec<String> = nta_data.blank_names().to_vec();
    let feature_buffers = &mut nta_data.feature_buffers;

    if analysis_names.is_empty() {
        return Ok(());
    }

    // Build blank analysis set: an analysis is blank if its replicate name is
    // listed as a blank.
    let mut blank_analyses: HashSet<String> = HashSet::new();
    for i in 0..analysis_names.len() {
        for b in &blank_names {
            if replicate_names[i] == *b {
                blank_analyses.insert(analysis_names[i].clone());
                break;
            }
        }
    }

    let mut background_mz: Vec<f32> = Vec::new();

    if blank_clean && !blank_analyses.is_empty() {
        // Collect all MS2 mz peaks from blank features, deduped by
        // (analysis, feature) uid.
        let mut blank_uids: Vec<String> = Vec::new();
        let mut blank_all_mz: Vec<f32> = Vec::new();
        for a in 0..analysis_names.len() {
            if !blank_analyses.contains(&analysis_names[a]) {
                continue;
            }
            let fts = &feature_buffers[a];
            for i in 0..fts.size() {
                if fts.ms2_size[i] <= 0 || fts.ms2_mz[i].is_empty() {
                    continue;
                }
                let mz = decode_floats_base64(&fts.ms2_mz[i]);
                let uid = format!("{}_{}", analysis_names[a], fts.feature[i]);
                for m in mz {
                    blank_uids.push(uid.clone());
                    blank_all_mz.push(m);
                }
            }
        }
        if !blank_all_mz.is_empty() {
            let blank_clustered = cluster_ms2_mz(
                &blank_uids,
                &blank_all_mz,
                mz_clust,
                blank_presence_threshold,
            );
            // Optionally intersect with the global background.
            if !blank_clustered.is_empty() {
                let mut global_uids: Vec<String> = Vec::new();
                let mut global_all_mz: Vec<f32> = Vec::new();
                for a in 0..analysis_names.len() {
                    let fts = &feature_buffers[a];
                    for i in 0..fts.size() {
                        if fts.ms2_size[i] <= 0 || fts.ms2_mz[i].is_empty() {
                            continue;
                        }
                        let mz = decode_floats_base64(&fts.ms2_mz[i]);
                        let uid = format!("{}_{}", analysis_names[a], fts.feature[i]);
                        for m in mz {
                            global_uids.push(uid.clone());
                            global_all_mz.push(m);
                        }
                    }
                }
                let global_clustered = cluster_ms2_mz(
                    &global_uids,
                    &global_all_mz,
                    mz_clust,
                    global_presence_threshold,
                );
                // Keep blank peaks that are also present globally.
                for bm in blank_clustered {
                    if in_background(bm, &global_clustered, mz_clust) {
                        background_mz.push(bm);
                    }
                }
            }
        }
    }

    let has_background = !background_mz.is_empty();

    for fts in feature_buffers.iter_mut() {
        for i in 0..fts.size() {
            if fts.ms2_size[i] <= 0 || fts.ms2_mz[i].is_empty() {
                continue;
            }
            let mut mz = decode_floats_base64(&fts.ms2_mz[i]);
            let mut intensity = decode_floats_base64(&fts.ms2_intensity[i]);
            if mz.is_empty() || mz.len() != intensity.len() {
                continue;
            }
            // Remove blank background peaks.
            if has_background {
                let mut new_mz = Vec::new();
                let mut new_int = Vec::new();
                for k in 0..mz.len() {
                    if !in_background(mz[k], &background_mz, mz_clust) {
                        new_mz.push(mz[k]);
                        new_int.push(intensity[k]);
                    }
                }
                mz = new_mz;
                intensity = new_int;
            }
            // Per-spectrum filters.
            apply_spectrum_filters(
                &mut mz,
                &mut intensity,
                top,
                min_intensity,
                rel_min_intensity,
            );
            // Re-encode and update the feature.
            fts.ms2_mz[i] = encode_floats_base64(&mz);
            fts.ms2_intensity[i] = encode_floats_base64(&intensity);
            fts.ms2_size[i] = mz.len() as i32;
        }
    }
    Ok(())
}
