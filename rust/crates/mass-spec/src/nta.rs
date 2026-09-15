//! Columnar NTA data model.
//!
//! Ported from `core/domains/mass_spec/include/streamfind/mass_spec/nta.hpp`
//! (`nta::api::NTA_FEATURE_ROW`, `NTA_FEATURES`, `NTA_SUSPECT_ROW`,
//! `NTA_SUSPECTS`, `NTA_INTERNAL_STANDARD_ROW`, `NTA_INTERNAL_STANDARDS`,
//! `PROJECT_NON_TARGET_ANALYSIS`). Keep the field set and the
//! get/set/append/sort semantics identical; only the plumbing (Rust types,
//! method names) is adapted.

use std::cell::RefCell;
use std::sync::Arc;

use crate::reader::Reader;
use crate::reader::Spectrum;

/// Row view of one feature (f64 precision, mirrors `nta::api::NTA_FEATURE_ROW`).
#[derive(Debug, Clone, Default)]
pub struct NtaFeatureRow {
    pub analysis: String,
    pub feature: String,
    pub feature_component: String,
    pub feature_group: String,
    pub adduct: String,
    pub rt: f64,
    pub mz: f64,
    pub mass: f64,
    pub intensity: f64,
    pub noise: f64,
    pub sn: f64,
    pub area: f64,
    pub rtmin: f64,
    pub rtmax: f64,
    pub width: f64,
    pub mzmin: f64,
    pub mzmax: f64,
    pub ppm: f64,
    pub fwhm_rt: f64,
    pub fwhm_mz: f64,
    pub gaussian_A: f64,
    pub gaussian_mu: f64,
    pub gaussian_sigma: f64,
    pub gaussian_r2: f64,
    pub jaggedness: f64,
    pub sharpness: f64,
    pub asymmetry: f64,
    pub modality: i32,
    pub plates: f64,
    pub polarity: i32,
    pub filtered: bool,
    pub filter: String,
    pub filled: bool,
    pub correction: f64,
    pub eic_size: i32,
    pub eic_rt: String,
    pub eic_mz: String,
    pub eic_intensity: String,
    pub eic_baseline: String,
    pub eic_smoothed: String,
    pub ms1_size: i32,
    pub ms1_mz: String,
    pub ms1_intensity: String,
    pub ms2_size: i32,
    pub ms2_mz: String,
    pub ms2_intensity: String,
    pub annotation_category: String,
    pub annotation_type: String,
    pub annotation_parent_feature: String,
    pub annotation_element: String,
    pub annotation_mass_error_da: f64,
    pub annotation_mass_error_ppm: f64,
    pub annotation_rt_error: f64,
    pub annotation_rel_intensity: f64,
    pub annotation_expected_rel_intensity_min: f64,
    pub annotation_expected_rel_intensity_max: f64,
    pub annotation_score: f64,
    pub component_size: i32,
    pub component_rt_center: f64,
    pub component_rt_spread: f64,
    pub component_density: f64,
    pub component_mean_correlation: f64,
    pub component_best_partner: String,
    pub component_max_correlation: f64,
    pub component_mean_correlation_to_component: f64,
    pub component_membership_score: f64,
    pub component_is_core: bool,
    pub component_bridge_flag: bool,
}

/// Columnar feature buffer for one analysis (structure-of-arrays; mirrors
/// `nta::api::NTA_FEATURES`).
#[derive(Debug, Clone, Default)]
pub struct NtaFeatures {
    pub analysis: String,
    pub feature: Vec<String>,
    pub feature_group: Vec<String>,
    pub feature_component: Vec<String>,
    pub adduct: Vec<String>,
    pub rt: Vec<f32>,
    pub mz: Vec<f32>,
    pub mass: Vec<f32>,
    pub intensity: Vec<f32>,
    pub noise: Vec<f32>,
    pub sn: Vec<f32>,
    pub area: Vec<f32>,
    pub rtmin: Vec<f32>,
    pub rtmax: Vec<f32>,
    pub width: Vec<f32>,
    pub mzmin: Vec<f32>,
    pub mzmax: Vec<f32>,
    pub ppm: Vec<f32>,
    pub fwhm_rt: Vec<f32>,
    pub fwhm_mz: Vec<f32>,
    pub gaussian_A: Vec<f32>,
    pub gaussian_mu: Vec<f32>,
    pub gaussian_sigma: Vec<f32>,
    pub gaussian_r2: Vec<f32>,
    pub jaggedness: Vec<f32>,
    pub sharpness: Vec<f32>,
    pub asymmetry: Vec<f32>,
    pub modality: Vec<i32>,
    pub plates: Vec<f32>,
    pub polarity: Vec<i32>,
    pub filtered: Vec<bool>,
    pub filter: Vec<String>,
    pub filled: Vec<bool>,
    pub correction: Vec<f32>,
    pub eic_size: Vec<i32>,
    pub eic_rt: Vec<String>,
    pub eic_mz: Vec<String>,
    pub eic_intensity: Vec<String>,
    pub eic_baseline: Vec<String>,
    pub eic_smoothed: Vec<String>,
    pub ms1_size: Vec<i32>,
    pub ms1_mz: Vec<String>,
    pub ms1_intensity: Vec<String>,
    pub ms2_size: Vec<i32>,
    pub ms2_mz: Vec<String>,
    pub ms2_intensity: Vec<String>,
    pub annotation_category: Vec<String>,
    pub annotation_type: Vec<String>,
    pub annotation_parent_feature: Vec<String>,
    pub annotation_element: Vec<String>,
    pub annotation_mass_error_da: Vec<f32>,
    pub annotation_mass_error_ppm: Vec<f32>,
    pub annotation_rt_error: Vec<f32>,
    pub annotation_rel_intensity: Vec<f32>,
    pub annotation_expected_rel_intensity_min: Vec<f32>,
    pub annotation_expected_rel_intensity_max: Vec<f32>,
    pub annotation_score: Vec<f32>,
    pub component_size: Vec<i32>,
    pub component_rt_center: Vec<f32>,
    pub component_rt_spread: Vec<f32>,
    pub component_density: Vec<f32>,
    pub component_mean_correlation: Vec<f32>,
    pub component_best_partner: Vec<String>,
    pub component_max_correlation: Vec<f32>,
    pub component_mean_correlation_to_component: Vec<f32>,
    pub component_membership_score: Vec<f32>,
    pub component_is_core: Vec<bool>,
    pub component_bridge_flag: Vec<bool>,
}

impl NtaFeatures {
    pub fn size(&self) -> usize {
        self.feature.len()
    }

    pub fn is_empty(&self) -> bool {
        self.feature.is_empty()
    }

    pub fn get_feature(&self, i: usize) -> NtaFeatureRow {
        let mut r = NtaFeatureRow::default();
        r.analysis = self.analysis.clone();
        r.feature = self.feature[i].clone();
        r.feature_component = self.feature_component[i].clone();
        r.feature_group = self.feature_group[i].clone();
        r.adduct = self.adduct[i].clone();
        r.rt = self.rt[i] as f64;
        r.mz = self.mz[i] as f64;
        r.mass = self.mass[i] as f64;
        r.intensity = self.intensity[i] as f64;
        r.noise = self.noise[i] as f64;
        r.sn = self.sn[i] as f64;
        r.area = self.area[i] as f64;
        r.rtmin = self.rtmin[i] as f64;
        r.rtmax = self.rtmax[i] as f64;
        r.width = self.width[i] as f64;
        r.mzmin = self.mzmin[i] as f64;
        r.mzmax = self.mzmax[i] as f64;
        r.ppm = self.ppm[i] as f64;
        r.fwhm_rt = self.fwhm_rt[i] as f64;
        r.fwhm_mz = self.fwhm_mz[i] as f64;
        r.gaussian_A = self.gaussian_A[i] as f64;
        r.gaussian_mu = self.gaussian_mu[i] as f64;
        r.gaussian_sigma = self.gaussian_sigma[i] as f64;
        r.gaussian_r2 = self.gaussian_r2[i] as f64;
        r.jaggedness = self.jaggedness[i] as f64;
        r.sharpness = self.sharpness[i] as f64;
        r.asymmetry = self.asymmetry[i] as f64;
        r.modality = self.modality[i];
        r.plates = self.plates[i] as f64;
        r.polarity = self.polarity[i];
        r.filtered = self.filtered[i];
        r.filter = self.filter[i].clone();
        r.filled = self.filled[i];
        r.correction = self.correction[i] as f64;
        r.eic_size = self.eic_size[i];
        r.eic_rt = self.eic_rt[i].clone();
        r.eic_mz = self.eic_mz[i].clone();
        r.eic_intensity = self.eic_intensity[i].clone();
        r.eic_baseline = self.eic_baseline[i].clone();
        r.eic_smoothed = self.eic_smoothed[i].clone();
        r.ms1_size = self.ms1_size[i];
        r.ms1_mz = self.ms1_mz[i].clone();
        r.ms1_intensity = self.ms1_intensity[i].clone();
        r.ms2_size = self.ms2_size[i];
        r.ms2_mz = self.ms2_mz[i].clone();
        r.ms2_intensity = self.ms2_intensity[i].clone();
        r.annotation_category = self.annotation_category[i].clone();
        r.annotation_type = self.annotation_type[i].clone();
        r.annotation_parent_feature = self.annotation_parent_feature[i].clone();
        r.annotation_element = self.annotation_element[i].clone();
        r.annotation_mass_error_da = self.annotation_mass_error_da[i] as f64;
        r.annotation_mass_error_ppm = self.annotation_mass_error_ppm[i] as f64;
        r.annotation_rt_error = self.annotation_rt_error[i] as f64;
        r.annotation_rel_intensity = self.annotation_rel_intensity[i] as f64;
        r.annotation_expected_rel_intensity_min =
            self.annotation_expected_rel_intensity_min[i] as f64;
        r.annotation_expected_rel_intensity_max =
            self.annotation_expected_rel_intensity_max[i] as f64;
        r.annotation_score = self.annotation_score[i] as f64;
        r.component_size = self.component_size[i];
        r.component_rt_center = self.component_rt_center[i] as f64;
        r.component_rt_spread = self.component_rt_spread[i] as f64;
        r.component_density = self.component_density[i] as f64;
        r.component_mean_correlation = self.component_mean_correlation[i] as f64;
        r.component_best_partner = self.component_best_partner[i].clone();
        r.component_max_correlation = self.component_max_correlation[i] as f64;
        r.component_mean_correlation_to_component =
            self.component_mean_correlation_to_component[i] as f64;
        r.component_membership_score = self.component_membership_score[i] as f64;
        r.component_is_core = self.component_is_core[i];
        r.component_bridge_flag = self.component_bridge_flag[i];
        r
    }

    pub fn set_feature(&mut self, i: usize, r: &NtaFeatureRow) {
        self.feature[i] = r.feature.clone();
        self.feature_component[i] = r.feature_component.clone();
        self.feature_group[i] = r.feature_group.clone();
        self.adduct[i] = r.adduct.clone();
        self.rt[i] = r.rt as f32;
        self.mz[i] = r.mz as f32;
        self.mass[i] = r.mass as f32;
        self.intensity[i] = r.intensity as f32;
        self.noise[i] = r.noise as f32;
        self.sn[i] = r.sn as f32;
        self.area[i] = r.area as f32;
        self.rtmin[i] = r.rtmin as f32;
        self.rtmax[i] = r.rtmax as f32;
        self.width[i] = r.width as f32;
        self.mzmin[i] = r.mzmin as f32;
        self.mzmax[i] = r.mzmax as f32;
        self.ppm[i] = r.ppm as f32;
        self.fwhm_rt[i] = r.fwhm_rt as f32;
        self.fwhm_mz[i] = r.fwhm_mz as f32;
        self.gaussian_A[i] = r.gaussian_A as f32;
        self.gaussian_mu[i] = r.gaussian_mu as f32;
        self.gaussian_sigma[i] = r.gaussian_sigma as f32;
        self.gaussian_r2[i] = r.gaussian_r2 as f32;
        self.jaggedness[i] = r.jaggedness as f32;
        self.sharpness[i] = r.sharpness as f32;
        self.asymmetry[i] = r.asymmetry as f32;
        self.modality[i] = r.modality;
        self.plates[i] = r.plates as f32;
        self.polarity[i] = r.polarity;
        self.filtered[i] = r.filtered;
        self.filter[i] = r.filter.clone();
        self.filled[i] = r.filled;
        self.correction[i] = r.correction as f32;
        self.eic_size[i] = r.eic_size;
        self.eic_rt[i] = r.eic_rt.clone();
        self.eic_mz[i] = r.eic_mz.clone();
        self.eic_intensity[i] = r.eic_intensity.clone();
        self.eic_baseline[i] = r.eic_baseline.clone();
        self.eic_smoothed[i] = r.eic_smoothed.clone();
        self.ms1_size[i] = r.ms1_size;
        self.ms1_mz[i] = r.ms1_mz.clone();
        self.ms1_intensity[i] = r.ms1_intensity.clone();
        self.ms2_size[i] = r.ms2_size;
        self.ms2_mz[i] = r.ms2_mz.clone();
        self.ms2_intensity[i] = r.ms2_intensity.clone();
        self.annotation_category[i] = r.annotation_category.clone();
        self.annotation_type[i] = r.annotation_type.clone();
        self.annotation_parent_feature[i] = r.annotation_parent_feature.clone();
        self.annotation_element[i] = r.annotation_element.clone();
        self.annotation_mass_error_da[i] = r.annotation_mass_error_da as f32;
        self.annotation_mass_error_ppm[i] = r.annotation_mass_error_ppm as f32;
        self.annotation_rt_error[i] = r.annotation_rt_error as f32;
        self.annotation_rel_intensity[i] = r.annotation_rel_intensity as f32;
        self.annotation_expected_rel_intensity_min[i] =
            r.annotation_expected_rel_intensity_min as f32;
        self.annotation_expected_rel_intensity_max[i] =
            r.annotation_expected_rel_intensity_max as f32;
        self.annotation_score[i] = r.annotation_score as f32;
        self.component_size[i] = r.component_size;
        self.component_rt_center[i] = r.component_rt_center as f32;
        self.component_rt_spread[i] = r.component_rt_spread as f32;
        self.component_density[i] = r.component_density as f32;
        self.component_mean_correlation[i] = r.component_mean_correlation as f32;
        self.component_best_partner[i] = r.component_best_partner.clone();
        self.component_max_correlation[i] = r.component_max_correlation as f32;
        self.component_mean_correlation_to_component[i] =
            r.component_mean_correlation_to_component as f32;
        self.component_membership_score[i] = r.component_membership_score as f32;
        self.component_is_core[i] = r.component_is_core;
        self.component_bridge_flag[i] = r.component_bridge_flag;
    }

    pub fn append_feature(&mut self, r: &NtaFeatureRow) {
        self.feature.push(r.feature.clone());
        self.feature_component.push(r.feature_component.clone());
        self.feature_group.push(r.feature_group.clone());
        self.adduct.push(r.adduct.clone());
        self.rt.push(r.rt as f32);
        self.mz.push(r.mz as f32);
        self.mass.push(r.mass as f32);
        self.intensity.push(r.intensity as f32);
        self.noise.push(r.noise as f32);
        self.sn.push(r.sn as f32);
        self.area.push(r.area as f32);
        self.rtmin.push(r.rtmin as f32);
        self.rtmax.push(r.rtmax as f32);
        self.width.push(r.width as f32);
        self.mzmin.push(r.mzmin as f32);
        self.mzmax.push(r.mzmax as f32);
        self.ppm.push(r.ppm as f32);
        self.fwhm_rt.push(r.fwhm_rt as f32);
        self.fwhm_mz.push(r.fwhm_mz as f32);
        self.gaussian_A.push(r.gaussian_A as f32);
        self.gaussian_mu.push(r.gaussian_mu as f32);
        self.gaussian_sigma.push(r.gaussian_sigma as f32);
        self.gaussian_r2.push(r.gaussian_r2 as f32);
        self.jaggedness.push(r.jaggedness as f32);
        self.sharpness.push(r.sharpness as f32);
        self.asymmetry.push(r.asymmetry as f32);
        self.modality.push(r.modality);
        self.plates.push(r.plates as f32);
        self.polarity.push(r.polarity);
        self.filtered.push(r.filtered);
        self.filter.push(r.filter.clone());
        self.filled.push(r.filled);
        self.correction.push(r.correction as f32);
        self.eic_size.push(r.eic_size);
        self.eic_rt.push(r.eic_rt.clone());
        self.eic_mz.push(r.eic_mz.clone());
        self.eic_intensity.push(r.eic_intensity.clone());
        self.eic_baseline.push(r.eic_baseline.clone());
        self.eic_smoothed.push(r.eic_smoothed.clone());
        self.ms1_size.push(r.ms1_size);
        self.ms1_mz.push(r.ms1_mz.clone());
        self.ms1_intensity.push(r.ms1_intensity.clone());
        self.ms2_size.push(r.ms2_size);
        self.ms2_mz.push(r.ms2_mz.clone());
        self.ms2_intensity.push(r.ms2_intensity.clone());
        self.annotation_category.push(r.annotation_category.clone());
        self.annotation_type.push(r.annotation_type.clone());
        self.annotation_parent_feature
            .push(r.annotation_parent_feature.clone());
        self.annotation_element.push(r.annotation_element.clone());
        self.annotation_mass_error_da
            .push(r.annotation_mass_error_da as f32);
        self.annotation_mass_error_ppm
            .push(r.annotation_mass_error_ppm as f32);
        self.annotation_rt_error.push(r.annotation_rt_error as f32);
        self.annotation_rel_intensity
            .push(r.annotation_rel_intensity as f32);
        self.annotation_expected_rel_intensity_min
            .push(r.annotation_expected_rel_intensity_min as f32);
        self.annotation_expected_rel_intensity_max
            .push(r.annotation_expected_rel_intensity_max as f32);
        self.annotation_score.push(r.annotation_score as f32);
        self.component_size.push(r.component_size);
        self.component_rt_center.push(r.component_rt_center as f32);
        self.component_rt_spread.push(r.component_rt_spread as f32);
        self.component_density.push(r.component_density as f32);
        self.component_mean_correlation
            .push(r.component_mean_correlation as f32);
        self.component_best_partner
            .push(r.component_best_partner.clone());
        self.component_max_correlation
            .push(r.component_max_correlation as f32);
        self.component_mean_correlation_to_component
            .push(r.component_mean_correlation_to_component as f32);
        self.component_membership_score
            .push(r.component_membership_score as f32);
        self.component_is_core.push(r.component_is_core);
        self.component_bridge_flag.push(r.component_bridge_flag);
    }

    pub fn sort_by_mz(&mut self) {
        if self.feature.is_empty() {
            return;
        }
        let mut order: Vec<usize> = (0..self.feature.len()).collect();
        order.sort_by(|&a, &b| self.mz[a].total_cmp(&self.mz[b]));
        let pick = |v: &mut Vec<String>, o: &[usize]| {
            let out: Vec<String> = o.iter().map(|&i| v[i].clone()).collect();
            *v = out;
        };
        let pick_f = |v: &mut Vec<f32>, o: &[usize]| {
            let out: Vec<f32> = o.iter().map(|&i| v[i]).collect();
            *v = out;
        };
        let pick_i = |v: &mut Vec<i32>, o: &[usize]| {
            let out: Vec<i32> = o.iter().map(|&i| v[i]).collect();
            *v = out;
        };
        let pick_b = |v: &mut Vec<bool>, o: &[usize]| {
            let out: Vec<bool> = o.iter().map(|&i| v[i]).collect();
            *v = out;
        };
        pick(&mut self.feature, &order);
        pick(&mut self.feature_group, &order);
        pick(&mut self.feature_component, &order);
        pick(&mut self.adduct, &order);
        pick_f(&mut self.rt, &order);
        pick_f(&mut self.mz, &order);
        pick_f(&mut self.mass, &order);
        pick_f(&mut self.intensity, &order);
        pick_f(&mut self.noise, &order);
        pick_f(&mut self.sn, &order);
        pick_f(&mut self.area, &order);
        pick_f(&mut self.rtmin, &order);
        pick_f(&mut self.rtmax, &order);
        pick_f(&mut self.width, &order);
        pick_f(&mut self.mzmin, &order);
        pick_f(&mut self.mzmax, &order);
        pick_f(&mut self.ppm, &order);
        pick_f(&mut self.fwhm_rt, &order);
        pick_f(&mut self.fwhm_mz, &order);
        pick_f(&mut self.gaussian_A, &order);
        pick_f(&mut self.gaussian_mu, &order);
        pick_f(&mut self.gaussian_sigma, &order);
        pick_f(&mut self.gaussian_r2, &order);
        pick_f(&mut self.jaggedness, &order);
        pick_f(&mut self.sharpness, &order);
        pick_f(&mut self.asymmetry, &order);
        pick_i(&mut self.modality, &order);
        pick_f(&mut self.plates, &order);
        pick_i(&mut self.polarity, &order);
        pick_b(&mut self.filtered, &order);
        pick(&mut self.filter, &order);
        pick_b(&mut self.filled, &order);
        pick_f(&mut self.correction, &order);
        pick_i(&mut self.eic_size, &order);
        pick(&mut self.eic_rt, &order);
        pick(&mut self.eic_mz, &order);
        pick(&mut self.eic_intensity, &order);
        pick(&mut self.eic_baseline, &order);
        pick(&mut self.eic_smoothed, &order);
        pick_i(&mut self.ms1_size, &order);
        pick(&mut self.ms1_mz, &order);
        pick(&mut self.ms1_intensity, &order);
        pick_i(&mut self.ms2_size, &order);
        pick(&mut self.ms2_mz, &order);
        pick(&mut self.ms2_intensity, &order);
        pick(&mut self.annotation_category, &order);
        pick(&mut self.annotation_type, &order);
        pick(&mut self.annotation_parent_feature, &order);
        pick(&mut self.annotation_element, &order);
        pick_f(&mut self.annotation_mass_error_da, &order);
        pick_f(&mut self.annotation_mass_error_ppm, &order);
        pick_f(&mut self.annotation_rt_error, &order);
        pick_f(&mut self.annotation_rel_intensity, &order);
        pick_f(&mut self.annotation_expected_rel_intensity_min, &order);
        pick_f(&mut self.annotation_expected_rel_intensity_max, &order);
        pick_f(&mut self.annotation_score, &order);
        pick_i(&mut self.component_size, &order);
        pick_f(&mut self.component_rt_center, &order);
        pick_f(&mut self.component_rt_spread, &order);
        pick_f(&mut self.component_density, &order);
        pick_f(&mut self.component_mean_correlation, &order);
        pick(&mut self.component_best_partner, &order);
        pick_f(&mut self.component_max_correlation, &order);
        pick_f(&mut self.component_mean_correlation_to_component, &order);
        pick_f(&mut self.component_membership_score, &order);
        pick_b(&mut self.component_is_core, &order);
        pick_b(&mut self.component_bridge_flag, &order);
    }
}

/// Row view of one suspect hit (mirrors `nta::api::NTA_SUSPECT_ROW`).
#[derive(Debug, Clone, Default)]
pub struct NtaSuspectRow {
    pub created_at: String,
    pub analysis: String,
    pub feature: String,
    pub feature_group: String,
    pub candidate_rank: i32,
    pub name: String,
    pub polarity: i32,
    pub db_mass: f64,
    pub exp_mass: f64,
    pub error_mass: f64,
    pub db_rt: f64,
    pub exp_rt: f64,
    pub error_rt: f64,
    pub intensity: f64,
    pub area: f64,
    pub id_level: i32,
    pub score: f64,
    pub shared_fragments: i32,
    pub cosine_similarity: f64,
    pub formula: String,
    pub SMILES: String,
    pub InChI: String,
    pub InChIKey: String,
    pub xLogP: f64,
    pub database_id: String,
    pub db_ms2_size: i32,
    pub db_ms2_mz: String,
    pub db_ms2_intensity: String,
    pub db_ms2_formula: String,
    pub db_ms2_smiles: String,
    pub exp_ms2_size: i32,
    pub exp_ms2_mz: String,
    pub exp_ms2_intensity: String,
}

/// Columnar suspect buffer (mirrors `nta::api::NTA_SUSPECTS`).
#[derive(Debug, Clone, Default)]
pub struct NtaSuspects {
    pub analysis: Vec<String>,
    pub feature: Vec<String>,
    pub candidate_rank: Vec<i32>,
    pub name: Vec<String>,
    pub polarity: Vec<i32>,
    pub db_mass: Vec<f64>,
    pub exp_mass: Vec<f64>,
    pub error_mass: Vec<f64>,
    pub db_rt: Vec<f64>,
    pub exp_rt: Vec<f64>,
    pub error_rt: Vec<f64>,
    pub intensity: Vec<f64>,
    pub area: Vec<f64>,
    pub id_level: Vec<i32>,
    pub score: Vec<f64>,
    pub shared_fragments: Vec<i32>,
    pub cosine_similarity: Vec<f64>,
    pub formula: Vec<String>,
    pub SMILES: Vec<String>,
    pub InChI: Vec<String>,
    pub InChIKey: Vec<String>,
    pub xLogP: Vec<f64>,
    pub database_id: Vec<String>,
    pub db_ms2_size: Vec<i32>,
    pub db_ms2_mz: Vec<String>,
    pub db_ms2_intensity: Vec<String>,
    pub db_ms2_formula: Vec<String>,
    pub db_ms2_smiles: Vec<String>,
    pub exp_ms2_size: Vec<i32>,
    pub exp_ms2_mz: Vec<String>,
    pub exp_ms2_intensity: Vec<String>,
}

impl NtaSuspects {
    pub fn size(&self) -> usize {
        self.analysis.len()
    }

    pub fn get_suspect(&self, i: usize) -> NtaSuspectRow {
        let mut r = NtaSuspectRow::default();
        r.analysis = self.analysis[i].clone();
        r.feature = self.feature[i].clone();
        r.candidate_rank = self.candidate_rank[i];
        r.name = self.name[i].clone();
        r.polarity = self.polarity[i];
        r.db_mass = self.db_mass[i];
        r.exp_mass = self.exp_mass[i];
        r.error_mass = self.error_mass[i];
        r.db_rt = self.db_rt[i];
        r.exp_rt = self.exp_rt[i];
        r.error_rt = self.error_rt[i];
        r.intensity = self.intensity[i];
        r.area = self.area[i];
        r.id_level = self.id_level[i];
        r.score = self.score[i];
        r.shared_fragments = self.shared_fragments[i];
        r.cosine_similarity = self.cosine_similarity[i];
        r.formula = self.formula[i].clone();
        r.SMILES = self.SMILES[i].clone();
        r.InChI = self.InChI[i].clone();
        r.InChIKey = self.InChIKey[i].clone();
        r.xLogP = self.xLogP[i];
        r.database_id = self.database_id[i].clone();
        r.db_ms2_size = self.db_ms2_size[i];
        r.db_ms2_mz = self.db_ms2_mz[i].clone();
        r.db_ms2_intensity = self.db_ms2_intensity[i].clone();
        r.db_ms2_formula = self.db_ms2_formula[i].clone();
        r.db_ms2_smiles = self.db_ms2_smiles[i].clone();
        r.exp_ms2_size = self.exp_ms2_size[i];
        r.exp_ms2_mz = self.exp_ms2_mz[i].clone();
        r.exp_ms2_intensity = self.exp_ms2_intensity[i].clone();
        r
    }

    pub fn set_suspect(&mut self, i: usize, s: &NtaSuspectRow) {
        self.analysis[i] = s.analysis.clone();
        self.feature[i] = s.feature.clone();
        self.candidate_rank[i] = s.candidate_rank;
        self.name[i] = s.name.clone();
        self.polarity[i] = s.polarity;
        self.db_mass[i] = s.db_mass;
        self.exp_mass[i] = s.exp_mass;
        self.error_mass[i] = s.error_mass;
        self.db_rt[i] = s.db_rt;
        self.exp_rt[i] = s.exp_rt;
        self.error_rt[i] = s.error_rt;
        self.intensity[i] = s.intensity;
        self.area[i] = s.area;
        self.id_level[i] = s.id_level;
        self.score[i] = s.score;
        self.shared_fragments[i] = s.shared_fragments;
        self.cosine_similarity[i] = s.cosine_similarity;
        self.formula[i] = s.formula.clone();
        self.SMILES[i] = s.SMILES.clone();
        self.InChI[i] = s.InChI.clone();
        self.InChIKey[i] = s.InChIKey.clone();
        self.xLogP[i] = s.xLogP;
        self.database_id[i] = s.database_id.clone();
        self.db_ms2_size[i] = s.db_ms2_size;
        self.db_ms2_mz[i] = s.db_ms2_mz.clone();
        self.db_ms2_intensity[i] = s.db_ms2_intensity.clone();
        self.db_ms2_formula[i] = s.db_ms2_formula.clone();
        self.db_ms2_smiles[i] = s.db_ms2_smiles.clone();
        self.exp_ms2_size[i] = s.exp_ms2_size;
        self.exp_ms2_mz[i] = s.exp_ms2_mz.clone();
        self.exp_ms2_intensity[i] = s.exp_ms2_intensity.clone();
    }

    pub fn append(&mut self, s: &NtaSuspectRow) {
        self.analysis.push(s.analysis.clone());
        self.feature.push(s.feature.clone());
        self.candidate_rank.push(s.candidate_rank);
        self.name.push(s.name.clone());
        self.polarity.push(s.polarity);
        self.db_mass.push(s.db_mass);
        self.exp_mass.push(s.exp_mass);
        self.error_mass.push(s.error_mass);
        self.db_rt.push(s.db_rt);
        self.exp_rt.push(s.exp_rt);
        self.error_rt.push(s.error_rt);
        self.intensity.push(s.intensity);
        self.area.push(s.area);
        self.id_level.push(s.id_level);
        self.score.push(s.score);
        self.shared_fragments.push(s.shared_fragments);
        self.cosine_similarity.push(s.cosine_similarity);
        self.formula.push(s.formula.clone());
        self.SMILES.push(s.SMILES.clone());
        self.InChI.push(s.InChI.clone());
        self.InChIKey.push(s.InChIKey.clone());
        self.xLogP.push(s.xLogP);
        self.database_id.push(s.database_id.clone());
        self.db_ms2_size.push(s.db_ms2_size);
        self.db_ms2_mz.push(s.db_ms2_mz.clone());
        self.db_ms2_intensity.push(s.db_ms2_intensity.clone());
        self.db_ms2_formula.push(s.db_ms2_formula.clone());
        self.db_ms2_smiles.push(s.db_ms2_smiles.clone());
        self.exp_ms2_size.push(s.exp_ms2_size);
        self.exp_ms2_mz.push(s.exp_ms2_mz.clone());
        self.exp_ms2_intensity.push(s.exp_ms2_intensity.clone());
    }
}

/// Row view of one internal-standard hit (mirrors `nta::api::NTA_INTERNAL_STANDARD_ROW`).
#[derive(Debug, Clone, Default)]
pub struct NtaInternalStandardRow {
    pub created_at: String,
    pub analysis: String,
    pub feature: String,
    pub feature_group: String,
    pub feature_component: String,
    pub adduct: String,
    pub candidate_rank: i32,
    pub name: String,
    pub polarity: i32,
    pub db_mass: f64,
    pub exp_mass: f64,
    pub error_mass: f64,
    pub db_rt: f64,
    pub exp_rt: f64,
    pub error_rt: f64,
    pub intensity: f64,
    pub area: f64,
    pub id_level: i32,
    pub score: f64,
    pub shared_fragments: i32,
    pub cosine_similarity: f64,
    pub formula: String,
    pub SMILES: String,
    pub InChI: String,
    pub InChIKey: String,
    pub xLogP: f64,
    pub database_id: String,
    pub db_ms2_size: i32,
    pub db_ms2_mz: String,
    pub db_ms2_intensity: String,
    pub db_ms2_formula: String,
    pub db_ms2_smiles: String,
    pub exp_ms2_size: i32,
    pub exp_ms2_mz: String,
    pub exp_ms2_intensity: String,
}

/// Columnar internal-standard buffer (mirrors `nta::api::NTA_INTERNAL_STANDARDS`).
#[derive(Debug, Clone, Default)]
pub struct NtaInternalStandards {
    pub analysis: Vec<String>,
    pub feature: Vec<String>,
    pub feature_component: Vec<String>,
    pub adduct: Vec<String>,
    pub candidate_rank: Vec<i32>,
    pub name: Vec<String>,
    pub polarity: Vec<i32>,
    pub db_mass: Vec<f64>,
    pub exp_mass: Vec<f64>,
    pub error_mass: Vec<f64>,
    pub db_rt: Vec<f64>,
    pub exp_rt: Vec<f64>,
    pub error_rt: Vec<f64>,
    pub intensity: Vec<f64>,
    pub area: Vec<f64>,
    pub id_level: Vec<i32>,
    pub score: Vec<f64>,
    pub shared_fragments: Vec<i32>,
    pub cosine_similarity: Vec<f64>,
    pub formula: Vec<String>,
    pub SMILES: Vec<String>,
    pub InChI: Vec<String>,
    pub InChIKey: Vec<String>,
    pub xLogP: Vec<f64>,
    pub database_id: Vec<String>,
    pub db_ms2_size: Vec<i32>,
    pub db_ms2_mz: Vec<String>,
    pub db_ms2_intensity: Vec<String>,
    pub db_ms2_formula: Vec<String>,
    pub db_ms2_smiles: Vec<String>,
    pub exp_ms2_size: Vec<i32>,
    pub exp_ms2_mz: Vec<String>,
    pub exp_ms2_intensity: Vec<String>,
}

impl NtaInternalStandards {
    pub fn size(&self) -> usize {
        self.analysis.len()
    }

    pub fn get_internal_standard(&self, i: usize) -> NtaInternalStandardRow {
        let mut r = NtaInternalStandardRow::default();
        r.analysis = self.analysis[i].clone();
        r.feature = self.feature[i].clone();
        r.feature_component = self.feature_component[i].clone();
        r.adduct = self.adduct[i].clone();
        r.candidate_rank = self.candidate_rank[i];
        r.name = self.name[i].clone();
        r.polarity = self.polarity[i];
        r.db_mass = self.db_mass[i];
        r.exp_mass = self.exp_mass[i];
        r.error_mass = self.error_mass[i];
        r.db_rt = self.db_rt[i];
        r.exp_rt = self.exp_rt[i];
        r.error_rt = self.error_rt[i];
        r.intensity = self.intensity[i];
        r.area = self.area[i];
        r.id_level = self.id_level[i];
        r.score = self.score[i];
        r.shared_fragments = self.shared_fragments[i];
        r.cosine_similarity = self.cosine_similarity[i];
        r.formula = self.formula[i].clone();
        r.SMILES = self.SMILES[i].clone();
        r.InChI = self.InChI[i].clone();
        r.InChIKey = self.InChIKey[i].clone();
        r.xLogP = self.xLogP[i];
        r.database_id = self.database_id[i].clone();
        r.db_ms2_size = self.db_ms2_size[i];
        r.db_ms2_mz = self.db_ms2_mz[i].clone();
        r.db_ms2_intensity = self.db_ms2_intensity[i].clone();
        r.db_ms2_formula = self.db_ms2_formula[i].clone();
        r.db_ms2_smiles = self.db_ms2_smiles[i].clone();
        r.exp_ms2_size = self.exp_ms2_size[i];
        r.exp_ms2_mz = self.exp_ms2_mz[i].clone();
        r.exp_ms2_intensity = self.exp_ms2_intensity[i].clone();
        r
    }

    pub fn set_internal_standard(&mut self, i: usize, s: &NtaInternalStandardRow) {
        self.analysis[i] = s.analysis.clone();
        self.feature[i] = s.feature.clone();
        self.feature_component[i] = s.feature_component.clone();
        self.adduct[i] = s.adduct.clone();
        self.candidate_rank[i] = s.candidate_rank;
        self.name[i] = s.name.clone();
        self.polarity[i] = s.polarity;
        self.db_mass[i] = s.db_mass;
        self.exp_mass[i] = s.exp_mass;
        self.error_mass[i] = s.error_mass;
        self.db_rt[i] = s.db_rt;
        self.exp_rt[i] = s.exp_rt;
        self.error_rt[i] = s.error_rt;
        self.intensity[i] = s.intensity;
        self.area[i] = s.area;
        self.id_level[i] = s.id_level;
        self.score[i] = s.score;
        self.shared_fragments[i] = s.shared_fragments;
        self.cosine_similarity[i] = s.cosine_similarity;
        self.formula[i] = s.formula.clone();
        self.SMILES[i] = s.SMILES.clone();
        self.InChI[i] = s.InChI.clone();
        self.InChIKey[i] = s.InChIKey.clone();
        self.xLogP[i] = s.xLogP;
        self.database_id[i] = s.database_id.clone();
        self.db_ms2_size[i] = s.db_ms2_size;
        self.db_ms2_mz[i] = s.db_ms2_mz.clone();
        self.db_ms2_intensity[i] = s.db_ms2_intensity.clone();
        self.db_ms2_formula[i] = s.db_ms2_formula.clone();
        self.db_ms2_smiles[i] = s.db_ms2_smiles.clone();
        self.exp_ms2_size[i] = s.exp_ms2_size;
        self.exp_ms2_mz[i] = s.exp_ms2_mz.clone();
        self.exp_ms2_intensity[i] = s.exp_ms2_intensity.clone();
    }

    pub fn append(&mut self, s: &NtaInternalStandardRow) {
        self.analysis.push(s.analysis.clone());
        self.feature.push(s.feature.clone());
        self.feature_component.push(s.feature_component.clone());
        self.adduct.push(s.adduct.clone());
        self.candidate_rank.push(s.candidate_rank);
        self.name.push(s.name.clone());
        self.polarity.push(s.polarity);
        self.db_mass.push(s.db_mass);
        self.exp_mass.push(s.exp_mass);
        self.error_mass.push(s.error_mass);
        self.db_rt.push(s.db_rt);
        self.exp_rt.push(s.exp_rt);
        self.error_rt.push(s.error_rt);
        self.intensity.push(s.intensity);
        self.area.push(s.area);
        self.id_level.push(s.id_level);
        self.score.push(s.score);
        self.shared_fragments.push(s.shared_fragments);
        self.cosine_similarity.push(s.cosine_similarity);
        self.formula.push(s.formula.clone());
        self.SMILES.push(s.SMILES.clone());
        self.InChI.push(s.InChI.clone());
        self.InChIKey.push(s.InChIKey.clone());
        self.xLogP.push(s.xLogP);
        self.database_id.push(s.database_id.clone());
        self.db_ms2_size.push(s.db_ms2_size);
        self.db_ms2_mz.push(s.db_ms2_mz.clone());
        self.db_ms2_intensity.push(s.db_ms2_intensity.clone());
        self.db_ms2_formula.push(s.db_ms2_formula.clone());
        self.db_ms2_smiles.push(s.db_ms2_smiles.clone());
        self.exp_ms2_size.push(s.exp_ms2_size);
        self.exp_ms2_mz.push(s.exp_ms2_mz.clone());
        self.exp_ms2_intensity.push(s.exp_ms2_intensity.clone());
    }
}

/// One extraction target (mirrors the fields of
/// `mass_spec::spectra::MASS_SPEC_TARGETS` used by `get_spectra_targets`).
#[derive(Debug, Clone)]
pub struct TargetSpec {
    pub id: String,
    pub level: i32,
    pub polarity: i32,
    pub precursor: bool,
    pub mzmin: f32,
    pub mzmax: f32,
    pub rtmin: f32,
    pub rtmax: f32,
    pub mz: f32,
    pub rt: f32,
    pub mobility: f32,
}

/// One extracted peak row (mirrors the fields of
/// `mass_spec::spectra::MASS_SPEC_TARGETS_SPECTRA`).
#[derive(Debug, Clone)]
pub struct TargetPoint {
    pub id: String,
    pub polarity: i32,
    pub level: i32,
    pub pre_mz: f32,
    pub pre_mzlow: f32,
    pub pre_mzhigh: f32,
    pub pre_ce: f32,
    pub rt: f32,
    pub mobility: f32,
    pub mz: f32,
    pub intensity: f32,
}

/// Port of `MASS_SPEC_FILE::get_spectra_targets` from
/// `core/domains/mass_spec/src/reader.cpp`: for every target, walk every scan
/// of the analysis and emit the peaks of matching scans as flat rows.
pub fn get_spectra_targets(
    spectra: &[Spectrum],
    targets: &[TargetSpec],
    min_int_lv1: f32,
    min_int_lv2: f32,
) -> Vec<TargetPoint> {
    let mut out = Vec::new();
    for target in targets {
        let precursor = target.precursor;
        let level = target.level;
        let polarity = target.polarity;
        let mzmin = target.mzmin;
        let mzmax = target.mzmax;
        let rtmin = target.rtmin;
        let rtmax = target.rtmax;
        let mzcenter = target.mz;
        let mmin = if mzmin == 0.0 && mzmax == 0.0 {
            mzcenter - 0.01
        } else {
            mzmin
        };
        let mmax = if mzmin == 0.0 && mzmax == 0.0 {
            mzcenter + 0.01
        } else {
            mzmax
        };
        for scan in spectra {
            if level != 0 && scan.level != level {
                continue;
            }
            if polarity != 0 && scan.polarity != polarity {
                continue;
            }
            let scan_rt = scan.retention_time;
            if rtmin != 0.0 && scan_rt < rtmin {
                continue;
            }
            if rtmax != 0.0 && scan_rt > rtmax {
                continue;
            }
            if precursor {
                let pmz = scan.precursor_mz;
                if mmin != 0.0 || mmax != 0.0 {
                    if pmz < mmin || pmz > mmax {
                        continue;
                    }
                }
            }
            if scan.mz.len() < 2 {
                continue;
            }
            let scan_pre_mz = if precursor {
                scan.precursor_mz
            } else {
                mzcenter
            };
            let scan_mobility = scan.mobility;
            let scan_ce = scan.collision_energy;
            for k in 0..scan.mz.len() {
                let mzv = scan.mz[k];
                let inv = scan.intensity[k];
                if level == 1 && inv < min_int_lv1 {
                    continue;
                }
                if level >= 2 && inv < min_int_lv2 {
                    continue;
                }
                if !precursor && (mzv < mmin || mzv > mmax) {
                    continue;
                }
                out.push(TargetPoint {
                    id: target.id.clone(),
                    polarity,
                    level,
                    pre_mz: scan_pre_mz,
                    pre_mzlow: mmin,
                    pre_mzhigh: mmax,
                    pre_ce: scan_ce,
                    rt: scan_rt,
                    mobility: scan_mobility,
                    mz: mzv,
                    intensity: inv,
                });
            }
        }
    }
    out
}

/// Non-target analysis project context (mirrors `nta::api::PROJECT_NON_TARGET_ANALYSIS`).
pub struct ProjectNonTargetAnalysis {
    names: Vec<String>,
    paths: Vec<String>,
    blank_names: Vec<String>,
    replicate_names: Vec<String>,
    analysis_indices: Vec<usize>,
    spectra_cache: RefCell<Vec<Option<Arc<Vec<Spectrum>>>>>,
    pub feature_buffers: Vec<NtaFeatures>,
    pub suspect_buffers: Vec<NtaSuspects>,
    pub internal_standard_buffers: Vec<NtaInternalStandards>,
}

impl ProjectNonTargetAnalysis {
    pub fn new(names: Vec<String>, paths: Vec<String>, analysis_indices: Vec<usize>) -> Self {
        let n = names.len();
        let blank_names = vec![String::new(); n];
        let replicate_names = vec![String::new(); n];
        let spectra_cache = RefCell::new(vec![None; n]);
        Self {
            names,
            paths,
            blank_names,
            replicate_names,
            analysis_indices,
            spectra_cache,
            feature_buffers: (0..n).map(|_| NtaFeatures::default()).collect(),
            suspect_buffers: (0..n).map(|_| NtaSuspects::default()).collect(),
            internal_standard_buffers: (0..n).map(|_| NtaInternalStandards::default()).collect(),
        }
    }

    pub fn analysis_names(&self) -> &[String] {
        &self.names
    }

    pub fn file_paths(&self) -> &[String] {
        &self.paths
    }

    pub fn blank_names(&self) -> &[String] {
        &self.blank_names
    }

    pub fn replicate_names(&self) -> &[String] {
        &self.replicate_names
    }

    pub fn analysis_indices(&self) -> &[usize] {
        &self.analysis_indices
    }

    pub fn analysis_index_at(&self, i: usize) -> usize {
        self.analysis_indices[i]
    }

    pub fn size(&self) -> usize {
        self.names.len()
    }

    pub fn set_blank_names(&mut self, mut b: Vec<String>) {
        if b.len() < self.names.len() {
            b.resize(self.names.len(), String::new());
        }
        self.blank_names = b;
    }

    pub fn set_replicate_names(&mut self, mut r: Vec<String>) {
        if r.len() < self.names.len() {
            r.resize(self.names.len(), String::new());
        }
        self.replicate_names = r;
    }

    pub fn set_analysis_indices(&mut self, mut a: Vec<usize>) {
        if a.len() < self.names.len() {
            a.resize(self.names.len(), 0);
        }
        self.analysis_indices = a;
    }

    /// Returns the raw spectra of analysis `i` (cached on first access).
    /// Replaces the C++ `MASS_SPEC_FILE` + `spectra_headers_at` plumbing.
    pub fn spectra(&self, i: usize) -> streamfind_rust_core::Result<Arc<Vec<Spectrum>>> {
        {
            let cache = self.spectra_cache.borrow();
            if let Some(Some(spec)) = cache.get(i) {
                return Ok(spec.clone());
            }
        }
        let path = self.paths.get(i).cloned().unwrap_or_default();
        if path.is_empty() {
            return Err(streamfind_rust_core::Error::new(
                streamfind_rust_core::ErrorCode::InvalidArgument,
                "analysis file path is empty",
            ));
        }
        let mut reader = Reader::open(&path).map_err(|e| {
            streamfind_rust_core::Error::new(
                streamfind_rust_core::ErrorCode::InvalidArgument,
                e.to_string(),
            )
        })?;
        let index = self.analysis_indices.get(i).copied().unwrap_or(0);
        reader.select_analysis(index).map_err(|e| {
            streamfind_rust_core::Error::new(
                streamfind_rust_core::ErrorCode::InvalidArgument,
                e.to_string(),
            )
        })?;
        let spectra: Arc<Vec<Spectrum>> = Arc::new(reader.spectra().to_vec());
        let mut cache = self.spectra_cache.borrow_mut();
        if cache.len() <= i {
            cache.resize(i + 1, None);
        }
        cache[i] = Some(spectra.clone());
        Ok(spectra)
    }
}
