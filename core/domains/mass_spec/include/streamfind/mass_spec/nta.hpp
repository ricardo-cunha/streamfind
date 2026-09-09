#pragma once

#include "streamfind/mass_spec/reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace nta {
namespace utils {
extern std::ofstream debug_log;
void init_debug_log(const std::string &, const std::string & = {});
void close_debug_log();
float mean(const std::vector<float> &);
float standard_deviation(const std::vector<float> &, float);
float quantile(std::vector<float>, float);
std::string encode_floats_base64(const std::vector<float> &, int precision = 4);
std::vector<size_t> get_sort_indices_float(const std::vector<float> &);
void reorder_multiple_vectors(const std::vector<size_t> &, std::vector<float> &, std::vector<float> &, std::vector<float> &);
void reorder_multiple_vectors(const std::vector<size_t> &, std::vector<float> &, std::vector<float> &, std::vector<float> &, std::vector<float> &);
void reorder_multiple_vectors(const std::vector<size_t> &, std::vector<float> &, std::vector<float> &, std::vector<float> &, std::vector<float> &, std::vector<int> &);
std::vector<size_t> filter_above_threshold(const std::vector<float> &, const std::vector<float> &);
std::vector<int> cluster_by_threshold_float(const std::vector<float> &, const std::vector<float> &);
std::vector<float> calculate_baseline(const std::vector<float> &, int);
std::vector<float> smooth_intensity_savitzky_golay(const std::vector<float> &, int, int);
void calculate_derivatives(const std::vector<float> &, std::vector<float> &, std::vector<float> &);
void fit_gaussian(const std::vector<float> &, const std::vector<float> &, float &, float &, float &, float &);
float gaussian_function_with_baseline(float, float, float, float, float);
float calculate_gaussian_rsquared(const std::vector<float> &, const std::vector<float> &, float, float, float, float);
float calculate_area(const std::vector<float> &, const std::vector<float> &);
float calculate_jaggedness(const std::vector<float> &);
float calculate_sharpness(const std::vector<float> &, const std::vector<float> &, float);
float calculate_asymmetry(const std::vector<float> &, const std::vector<float> &);
int calculate_modality(const std::vector<float> &, float);
float calculate_theoretical_plates(float, float);
}
}

#define DEBUG_LOG(value) do { if (::nta::utils::debug_log.is_open()) ::nta::utils::debug_log << value; } while (false)
#define DEBUG_OUT(value)                       \
  do                                           \
  {                                            \
    if (::nta::utils::debug_log.is_open())     \
    {                                          \
      ::nta::utils::debug_log << value;        \
      ::nta::utils::debug_log.flush();         \
    }                                          \
    std::cout << value;                        \
  } while (0)

namespace nta::api {

// MARK: NTA_FEATURE_ROW
struct NTA_FEATURE_ROW {
    std::string analysis;
    std::string feature;
    std::string id;
    std::string feature_component;
    std::string feature_group;
    std::string adduct;
    double rt = 0.0;
    double mz = 0.0;
    double mass = 0.0;
    double intensity = 0.0;
    double noise = 0.0;
    double sn = 0.0;
    double area = 0.0;
    double rtmin = 0.0;
    double rtmax = 0.0;
    double width = 0.0;
    double mzmin = 0.0;
    double mzmax = 0.0;
    double ppm = 0.0;
    double fwhm_rt = 0.0;
    double fwhm_mz = 0.0;
    double gaussian_A = 0.0;
    double gaussian_mu = 0.0;
    double gaussian_sigma = 0.0;
    double gaussian_r2 = 0.0;
    double jaggedness = 0.0;
    double sharpness = 0.0;
    double asymmetry = 0.0;
    int modality = 0;
    double plates = 0.0;
    int polarity = 0;
    bool filtered = false;
    std::string filter;
    bool filled = false;
    double correction = 0.0;
    int eic_size = 0;
    std::string eic_rt;
    std::string eic_mz;
    std::string eic_intensity;
    std::string eic_baseline;
    std::string eic_smoothed;
    int ms1_size = 0;
    std::string ms1_mz;
    std::string ms1_intensity;
    int ms2_size = 0;
    std::string ms2_mz;
    std::string ms2_intensity;
    std::string annotation_category;
    std::string annotation_type;
    std::string annotation_parent_feature;
    std::string annotation_element;
    double annotation_mass_error_da = 0.0;
    double annotation_mass_error_ppm = 0.0;
    double annotation_rt_error = 0.0;
    double annotation_rel_intensity = 0.0;
    double annotation_expected_rel_intensity_min = 0.0;
    double annotation_expected_rel_intensity_max = 0.0;
    double annotation_score = 0.0;
    int component_size = 0;
    double component_rt_center = 0.0;
    double component_rt_spread = 0.0;
    double component_density = 0.0;
    double component_mean_correlation = 0.0;
    std::string component_best_partner;
    double component_max_correlation = 0.0;
    double component_mean_correlation_to_component = 0.0;
    double component_membership_score = 0.0;
    bool component_is_core = false;
    bool component_bridge_flag = false;
};

// MARK: NTA_FEATURES (columnar; structure-of-arrays ported from the R model)
struct NTA_FEATURES {
    std::string analysis;
    std::vector<std::string> feature;
    std::vector<std::string> feature_group;
    std::vector<std::string> feature_component;
    std::vector<std::string> adduct;
    std::vector<float> rt;
    std::vector<float> mz;
    std::vector<float> mass;
    std::vector<float> intensity;
    std::vector<float> noise;
    std::vector<float> sn;
    std::vector<float> area;
    std::vector<float> rtmin;
    std::vector<float> rtmax;
    std::vector<float> width;
    std::vector<float> mzmin;
    std::vector<float> mzmax;
    std::vector<float> ppm;
    std::vector<float> fwhm_rt;
    std::vector<float> fwhm_mz;
    std::vector<float> gaussian_A;
    std::vector<float> gaussian_mu;
    std::vector<float> gaussian_sigma;
    std::vector<float> gaussian_r2;
    std::vector<float> jaggedness;
    std::vector<float> sharpness;
    std::vector<float> asymmetry;
    std::vector<int> modality;
    std::vector<float> plates;
    std::vector<int> polarity;
    std::vector<bool> filtered;
    std::vector<std::string> filter;
    std::vector<bool> filled;
    std::vector<float> correction;
    std::vector<int> eic_size;
    std::vector<std::string> eic_rt;
    std::vector<std::string> eic_mz;
    std::vector<std::string> eic_intensity;
    std::vector<std::string> eic_baseline;
    std::vector<std::string> eic_smoothed;
    std::vector<int> ms1_size;
    std::vector<std::string> ms1_mz;
    std::vector<std::string> ms1_intensity;
    std::vector<int> ms2_size;
    std::vector<std::string> ms2_mz;
    std::vector<std::string> ms2_intensity;
    std::vector<std::string> annotation_category;
    std::vector<std::string> annotation_type;
    std::vector<std::string> annotation_parent_feature;
    std::vector<std::string> annotation_element;
    std::vector<float> annotation_mass_error_da;
    std::vector<float> annotation_mass_error_ppm;
    std::vector<float> annotation_rt_error;
    std::vector<float> annotation_rel_intensity;
    std::vector<float> annotation_expected_rel_intensity_min;
    std::vector<float> annotation_expected_rel_intensity_max;
    std::vector<float> annotation_score;
    std::vector<int> component_size;
    std::vector<float> component_rt_center;
    std::vector<float> component_rt_spread;
    std::vector<float> component_density;
    std::vector<float> component_mean_correlation;
    std::vector<std::string> component_best_partner;
    std::vector<float> component_max_correlation;
    std::vector<float> component_mean_correlation_to_component;
    std::vector<float> component_membership_score;
    std::vector<bool> component_is_core;
    std::vector<bool> component_bridge_flag;

    int size() const { return static_cast<int>(feature.size()); }

    void set_analysis(const std::string &a) { analysis = a; }

    NTA_FEATURE_ROW get_feature(const int &i) const
    {
      NTA_FEATURE_ROW feature_i;
      feature_i.analysis = analysis;
      feature_i.feature = feature[i];
      feature_i.feature_group = feature_group[i];
      feature_i.feature_component = feature_component[i];
      feature_i.adduct = adduct[i];
      feature_i.rt = rt[i];
      feature_i.mz = mz[i];
      feature_i.mass = mass[i];
      feature_i.intensity = intensity[i];
      feature_i.noise = noise[i];
      feature_i.sn = sn[i];
      feature_i.area = area[i];
      feature_i.rtmin = rtmin[i];
      feature_i.rtmax = rtmax[i];
      feature_i.width = width[i];
      feature_i.mzmin = mzmin[i];
      feature_i.mzmax = mzmax[i];
      feature_i.ppm = ppm[i];
      feature_i.fwhm_rt = fwhm_rt[i];
      feature_i.fwhm_mz = fwhm_mz[i];
      feature_i.gaussian_A = gaussian_A[i];
      feature_i.gaussian_mu = gaussian_mu[i];
      feature_i.gaussian_sigma = gaussian_sigma[i];
      feature_i.gaussian_r2 = gaussian_r2[i];
      feature_i.jaggedness = jaggedness[i];
      feature_i.sharpness = sharpness[i];
      feature_i.asymmetry = asymmetry[i];
      feature_i.modality = modality[i];
      feature_i.plates = plates[i];
      feature_i.polarity = polarity[i];
      feature_i.filtered = filtered[i];
      feature_i.filter = filter[i];
      feature_i.filled = filled[i];
      feature_i.correction = correction[i];
      feature_i.eic_size = eic_size[i];
      feature_i.eic_rt = eic_rt[i];
      feature_i.eic_mz = eic_mz[i];
      feature_i.eic_intensity = eic_intensity[i];
      feature_i.eic_baseline = eic_baseline[i];
      feature_i.eic_smoothed = eic_smoothed[i];
      feature_i.ms1_size = ms1_size[i];
      feature_i.ms1_mz = ms1_mz[i];
      feature_i.ms1_intensity = ms1_intensity[i];
      feature_i.ms2_size = ms2_size[i];
      feature_i.ms2_mz = ms2_mz[i];
      feature_i.ms2_intensity = ms2_intensity[i];
      feature_i.annotation_category = annotation_category[i];
      feature_i.annotation_type = annotation_type[i];
      feature_i.annotation_parent_feature = annotation_parent_feature[i];
      feature_i.annotation_element = annotation_element[i];
      feature_i.annotation_mass_error_da = annotation_mass_error_da[i];
      feature_i.annotation_mass_error_ppm = annotation_mass_error_ppm[i];
      feature_i.annotation_rt_error = annotation_rt_error[i];
      feature_i.annotation_rel_intensity = annotation_rel_intensity[i];
      feature_i.annotation_expected_rel_intensity_min = annotation_expected_rel_intensity_min[i];
      feature_i.annotation_expected_rel_intensity_max = annotation_expected_rel_intensity_max[i];
      feature_i.annotation_score = annotation_score[i];
      feature_i.component_size = component_size[i];
      feature_i.component_rt_center = component_rt_center[i];
      feature_i.component_rt_spread = component_rt_spread[i];
      feature_i.component_density = component_density[i];
      feature_i.component_mean_correlation = component_mean_correlation[i];
      feature_i.component_best_partner = component_best_partner[i];
      feature_i.component_max_correlation = component_max_correlation[i];
      feature_i.component_mean_correlation_to_component = component_mean_correlation_to_component[i];
      feature_i.component_membership_score = component_membership_score[i];
      feature_i.component_is_core = component_is_core[i];
      feature_i.component_bridge_flag = component_bridge_flag[i];
      return feature_i;
    }

    void set_feature(const int &i, const NTA_FEATURE_ROW &feature_i)
    {
      feature[i] = feature_i.feature;
      feature_group[i] = feature_i.feature_group;
      feature_component[i] = feature_i.feature_component;
      adduct[i] = feature_i.adduct;
      rt[i] = static_cast<float>(feature_i.rt);
      mz[i] = static_cast<float>(feature_i.mz);
      mass[i] = static_cast<float>(feature_i.mass);
      intensity[i] = static_cast<float>(feature_i.intensity);
      noise[i] = static_cast<float>(feature_i.noise);
      sn[i] = static_cast<float>(feature_i.sn);
      area[i] = static_cast<float>(feature_i.area);
      rtmin[i] = static_cast<float>(feature_i.rtmin);
      rtmax[i] = static_cast<float>(feature_i.rtmax);
      width[i] = static_cast<float>(feature_i.width);
      mzmin[i] = static_cast<float>(feature_i.mzmin);
      mzmax[i] = static_cast<float>(feature_i.mzmax);
      ppm[i] = static_cast<float>(feature_i.ppm);
      fwhm_rt[i] = static_cast<float>(feature_i.fwhm_rt);
      fwhm_mz[i] = static_cast<float>(feature_i.fwhm_mz);
      gaussian_A[i] = static_cast<float>(feature_i.gaussian_A);
      gaussian_mu[i] = static_cast<float>(feature_i.gaussian_mu);
      gaussian_sigma[i] = static_cast<float>(feature_i.gaussian_sigma);
      gaussian_r2[i] = static_cast<float>(feature_i.gaussian_r2);
      jaggedness[i] = static_cast<float>(feature_i.jaggedness);
      sharpness[i] = static_cast<float>(feature_i.sharpness);
      asymmetry[i] = static_cast<float>(feature_i.asymmetry);
      modality[i] = feature_i.modality;
      plates[i] = static_cast<float>(feature_i.plates);
      polarity[i] = feature_i.polarity;
      filtered[i] = feature_i.filtered;
      filter[i] = feature_i.filter;
      filled[i] = feature_i.filled;
      correction[i] = static_cast<float>(feature_i.correction);
      eic_size[i] = feature_i.eic_size;
      eic_rt[i] = feature_i.eic_rt;
      eic_mz[i] = feature_i.eic_mz;
      eic_intensity[i] = feature_i.eic_intensity;
      eic_baseline[i] = feature_i.eic_baseline;
      eic_smoothed[i] = feature_i.eic_smoothed;
      ms1_size[i] = feature_i.ms1_size;
      ms1_mz[i] = feature_i.ms1_mz;
      ms1_intensity[i] = feature_i.ms1_intensity;
      ms2_size[i] = feature_i.ms2_size;
      ms2_mz[i] = feature_i.ms2_mz;
      ms2_intensity[i] = feature_i.ms2_intensity;
      annotation_category[i] = feature_i.annotation_category;
      annotation_type[i] = feature_i.annotation_type;
      annotation_parent_feature[i] = feature_i.annotation_parent_feature;
      annotation_element[i] = feature_i.annotation_element;
      annotation_mass_error_da[i] = static_cast<float>(feature_i.annotation_mass_error_da);
      annotation_mass_error_ppm[i] = static_cast<float>(feature_i.annotation_mass_error_ppm);
      annotation_rt_error[i] = static_cast<float>(feature_i.annotation_rt_error);
      annotation_rel_intensity[i] = static_cast<float>(feature_i.annotation_rel_intensity);
      annotation_expected_rel_intensity_min[i] = static_cast<float>(feature_i.annotation_expected_rel_intensity_min);
      annotation_expected_rel_intensity_max[i] = static_cast<float>(feature_i.annotation_expected_rel_intensity_max);
      annotation_score[i] = static_cast<float>(feature_i.annotation_score);
      component_size[i] = feature_i.component_size;
      component_rt_center[i] = static_cast<float>(feature_i.component_rt_center);
      component_rt_spread[i] = static_cast<float>(feature_i.component_rt_spread);
      component_density[i] = static_cast<float>(feature_i.component_density);
      component_mean_correlation[i] = static_cast<float>(feature_i.component_mean_correlation);
      component_best_partner[i] = feature_i.component_best_partner;
      component_max_correlation[i] = static_cast<float>(feature_i.component_max_correlation);
      component_mean_correlation_to_component[i] = static_cast<float>(feature_i.component_mean_correlation_to_component);
      component_membership_score[i] = static_cast<float>(feature_i.component_membership_score);
      component_is_core[i] = feature_i.component_is_core;
      component_bridge_flag[i] = feature_i.component_bridge_flag;
    }

    void append_feature(const NTA_FEATURE_ROW &feature_i)
    {
      feature.push_back(feature_i.feature);
      feature_group.push_back(feature_i.feature_group);
      feature_component.push_back(feature_i.feature_component);
      adduct.push_back(feature_i.adduct);
      rt.push_back(static_cast<float>(feature_i.rt));
      mz.push_back(static_cast<float>(feature_i.mz));
      mass.push_back(static_cast<float>(feature_i.mass));
      intensity.push_back(static_cast<float>(feature_i.intensity));
      noise.push_back(static_cast<float>(feature_i.noise));
      sn.push_back(static_cast<float>(feature_i.sn));
      area.push_back(static_cast<float>(feature_i.area));
      rtmin.push_back(static_cast<float>(feature_i.rtmin));
      rtmax.push_back(static_cast<float>(feature_i.rtmax));
      width.push_back(static_cast<float>(feature_i.width));
      mzmin.push_back(static_cast<float>(feature_i.mzmin));
      mzmax.push_back(static_cast<float>(feature_i.mzmax));
      ppm.push_back(static_cast<float>(feature_i.ppm));
      fwhm_rt.push_back(static_cast<float>(feature_i.fwhm_rt));
      fwhm_mz.push_back(static_cast<float>(feature_i.fwhm_mz));
      gaussian_A.push_back(static_cast<float>(feature_i.gaussian_A));
      gaussian_mu.push_back(static_cast<float>(feature_i.gaussian_mu));
      gaussian_sigma.push_back(static_cast<float>(feature_i.gaussian_sigma));
      gaussian_r2.push_back(static_cast<float>(feature_i.gaussian_r2));
      jaggedness.push_back(static_cast<float>(feature_i.jaggedness));
      sharpness.push_back(static_cast<float>(feature_i.sharpness));
      asymmetry.push_back(static_cast<float>(feature_i.asymmetry));
      modality.push_back(feature_i.modality);
      plates.push_back(static_cast<float>(feature_i.plates));
      polarity.push_back(feature_i.polarity);
      filtered.push_back(feature_i.filtered);
      filter.push_back(feature_i.filter);
      filled.push_back(feature_i.filled);
      correction.push_back(static_cast<float>(feature_i.correction));
      eic_size.push_back(feature_i.eic_size);
      eic_rt.push_back(feature_i.eic_rt);
      eic_mz.push_back(feature_i.eic_mz);
      eic_intensity.push_back(feature_i.eic_intensity);
      eic_baseline.push_back(feature_i.eic_baseline);
      eic_smoothed.push_back(feature_i.eic_smoothed);
      ms1_size.push_back(feature_i.ms1_size);
      ms1_mz.push_back(feature_i.ms1_mz);
      ms1_intensity.push_back(feature_i.ms1_intensity);
      ms2_size.push_back(feature_i.ms2_size);
      ms2_mz.push_back(feature_i.ms2_mz);
      ms2_intensity.push_back(feature_i.ms2_intensity);
      annotation_category.push_back(feature_i.annotation_category);
      annotation_type.push_back(feature_i.annotation_type);
      annotation_parent_feature.push_back(feature_i.annotation_parent_feature);
      annotation_element.push_back(feature_i.annotation_element);
      annotation_mass_error_da.push_back(static_cast<float>(feature_i.annotation_mass_error_da));
      annotation_mass_error_ppm.push_back(static_cast<float>(feature_i.annotation_mass_error_ppm));
      annotation_rt_error.push_back(static_cast<float>(feature_i.annotation_rt_error));
      annotation_rel_intensity.push_back(static_cast<float>(feature_i.annotation_rel_intensity));
      annotation_expected_rel_intensity_min.push_back(static_cast<float>(feature_i.annotation_expected_rel_intensity_min));
      annotation_expected_rel_intensity_max.push_back(static_cast<float>(feature_i.annotation_expected_rel_intensity_max));
      annotation_score.push_back(static_cast<float>(feature_i.annotation_score));
      component_size.push_back(feature_i.component_size);
      component_rt_center.push_back(static_cast<float>(feature_i.component_rt_center));
      component_rt_spread.push_back(static_cast<float>(feature_i.component_rt_spread));
      component_density.push_back(static_cast<float>(feature_i.component_density));
      component_mean_correlation.push_back(static_cast<float>(feature_i.component_mean_correlation));
      component_best_partner.push_back(feature_i.component_best_partner);
      component_max_correlation.push_back(static_cast<float>(feature_i.component_max_correlation));
      component_mean_correlation_to_component.push_back(static_cast<float>(feature_i.component_mean_correlation_to_component));
      component_membership_score.push_back(static_cast<float>(feature_i.component_membership_score));
      component_is_core.push_back(feature_i.component_is_core);
      component_bridge_flag.push_back(feature_i.component_bridge_flag);
    }

    void sort_by_mz()
    {
      if (feature.empty())
        return;

      std::vector<int> new_order(feature.size());
      std::iota(new_order.begin(), new_order.end(), 0);

      std::sort(new_order.begin(), new_order.end(), [this](int i1, int i2)
                { return mz[i1] < mz[i2]; });

      std::vector<std::string> feature_sorted(feature.size());
      std::vector<std::string> feature_group_sorted(feature.size());
      std::vector<std::string> feature_component_sorted(feature.size());
      std::vector<std::string> adduct_sorted(feature.size());
      std::vector<float> rt_sorted(feature.size());
      std::vector<float> mz_sorted(feature.size());
      std::vector<float> mass_sorted(feature.size());
      std::vector<float> intensity_sorted(feature.size());
      std::vector<float> noise_sorted(feature.size());
      std::vector<float> sn_sorted(feature.size());
      std::vector<float> area_sorted(feature.size());
      std::vector<float> rtmin_sorted(feature.size());
      std::vector<float> rtmax_sorted(feature.size());
      std::vector<float> width_sorted(feature.size());
      std::vector<float> mzmin_sorted(feature.size());
      std::vector<float> mzmax_sorted(feature.size());
      std::vector<float> ppm_sorted(feature.size());
      std::vector<float> fwhm_rt_sorted(feature.size());
      std::vector<float> fwhm_mz_sorted(feature.size());
      std::vector<float> gaussian_A_sorted(feature.size());
      std::vector<float> gaussian_mu_sorted(feature.size());
      std::vector<float> gaussian_sigma_sorted(feature.size());
      std::vector<float> gaussian_r2_sorted(feature.size());
      std::vector<float> jaggedness_sorted(feature.size());
      std::vector<float> sharpness_sorted(feature.size());
      std::vector<float> asymmetry_sorted(feature.size());
      std::vector<int> modality_sorted(feature.size());
      std::vector<float> plates_sorted(feature.size());
      std::vector<int> polarity_sorted(feature.size());
      std::vector<bool> filtered_sorted(feature.size());
      std::vector<std::string> filter_sorted(feature.size());
      std::vector<bool> filled_sorted(feature.size());
      std::vector<float> correction_sorted(feature.size());
      std::vector<int> eic_size_sorted(feature.size());
      std::vector<std::string> eic_rt_sorted(feature.size());
      std::vector<std::string> eic_mz_sorted(feature.size());
      std::vector<std::string> eic_intensity_sorted(feature.size());
      std::vector<std::string> eic_baseline_sorted(feature.size());
      std::vector<std::string> eic_smoothed_sorted(feature.size());
      std::vector<int> ms1_size_sorted(feature.size());
      std::vector<std::string> ms1_mz_sorted(feature.size());
      std::vector<std::string> ms1_intensity_sorted(feature.size());
      std::vector<int> ms2_size_sorted(feature.size());
      std::vector<std::string> ms2_mz_sorted(feature.size());
      std::vector<std::string> ms2_intensity_sorted(feature.size());
      std::vector<std::string> annotation_category_sorted(feature.size());
      std::vector<std::string> annotation_type_sorted(feature.size());
      std::vector<std::string> annotation_parent_feature_sorted(feature.size());
      std::vector<std::string> annotation_element_sorted(feature.size());
      std::vector<float> annotation_mass_error_da_sorted(feature.size());
      std::vector<float> annotation_mass_error_ppm_sorted(feature.size());
      std::vector<float> annotation_rt_error_sorted(feature.size());
      std::vector<float> annotation_rel_intensity_sorted(feature.size());
      std::vector<float> annotation_expected_rel_intensity_min_sorted(feature.size());
      std::vector<float> annotation_expected_rel_intensity_max_sorted(feature.size());
      std::vector<float> annotation_score_sorted(feature.size());
      std::vector<int> component_size_sorted(feature.size());
      std::vector<float> component_rt_center_sorted(feature.size());
      std::vector<float> component_rt_spread_sorted(feature.size());
      std::vector<float> component_density_sorted(feature.size());
      std::vector<float> component_mean_correlation_sorted(feature.size());
      std::vector<std::string> component_best_partner_sorted(feature.size());
      std::vector<float> component_max_correlation_sorted(feature.size());
      std::vector<float> component_mean_correlation_to_component_sorted(feature.size());
      std::vector<float> component_membership_score_sorted(feature.size());
      std::vector<bool> component_is_core_sorted(feature.size());
      std::vector<bool> component_bridge_flag_sorted(feature.size());

      for (size_t i = 0; i < feature.size(); i++)
      {
        int idx = new_order[i];
        feature_sorted[i] = feature[idx];
        feature_group_sorted[i] = feature_group[idx];
        feature_component_sorted[i] = feature_component[idx];
        adduct_sorted[i] = adduct[idx];
        rt_sorted[i] = rt[idx];
        mz_sorted[i] = mz[idx];
        mass_sorted[i] = mass[idx];
        intensity_sorted[i] = intensity[idx];
        noise_sorted[i] = noise[idx];
        sn_sorted[i] = sn[idx];
        area_sorted[i] = area[idx];
        rtmin_sorted[i] = rtmin[idx];
        rtmax_sorted[i] = rtmax[idx];
        width_sorted[i] = width[idx];
        mzmin_sorted[i] = mzmin[idx];
        mzmax_sorted[i] = mzmax[idx];
        ppm_sorted[i] = ppm[idx];
        fwhm_rt_sorted[i] = fwhm_rt[idx];
        fwhm_mz_sorted[i] = fwhm_mz[idx];
        gaussian_A_sorted[i] = gaussian_A[idx];
        gaussian_mu_sorted[i] = gaussian_mu[idx];
        gaussian_sigma_sorted[i] = gaussian_sigma[idx];
        gaussian_r2_sorted[i] = gaussian_r2[idx];
        jaggedness_sorted[i] = jaggedness[idx];
        sharpness_sorted[i] = sharpness[idx];
        asymmetry_sorted[i] = asymmetry[idx];
        modality_sorted[i] = modality[idx];
        plates_sorted[i] = plates[idx];
        polarity_sorted[i] = polarity[idx];
        filtered_sorted[i] = filtered[idx];
        filter_sorted[i] = filter[idx];
        filled_sorted[i] = filled[idx];
        correction_sorted[i] = correction[idx];
        eic_size_sorted[i] = eic_size[idx];
        eic_rt_sorted[i] = eic_rt[idx];
        eic_mz_sorted[i] = eic_mz[idx];
        eic_intensity_sorted[i] = eic_intensity[idx];
        eic_baseline_sorted[i] = eic_baseline[idx];
        eic_smoothed_sorted[i] = eic_smoothed[idx];
        ms1_size_sorted[i] = ms1_size[idx];
        ms1_mz_sorted[i] = ms1_mz[idx];
        ms1_intensity_sorted[i] = ms1_intensity[idx];
        ms2_size_sorted[i] = ms2_size[idx];
        ms2_mz_sorted[i] = ms2_mz[idx];
        ms2_intensity_sorted[i] = ms2_intensity[idx];
        annotation_category_sorted[i] = annotation_category[idx];
        annotation_type_sorted[i] = annotation_type[idx];
        annotation_parent_feature_sorted[i] = annotation_parent_feature[idx];
        annotation_element_sorted[i] = annotation_element[idx];
        annotation_mass_error_da_sorted[i] = annotation_mass_error_da[idx];
        annotation_mass_error_ppm_sorted[i] = annotation_mass_error_ppm[idx];
        annotation_rt_error_sorted[i] = annotation_rt_error[idx];
        annotation_rel_intensity_sorted[i] = annotation_rel_intensity[idx];
        annotation_expected_rel_intensity_min_sorted[i] = annotation_expected_rel_intensity_min[idx];
        annotation_expected_rel_intensity_max_sorted[i] = annotation_expected_rel_intensity_max[idx];
        annotation_score_sorted[i] = annotation_score[idx];
        component_size_sorted[i] = component_size[idx];
        component_rt_center_sorted[i] = component_rt_center[idx];
        component_rt_spread_sorted[i] = component_rt_spread[idx];
        component_density_sorted[i] = component_density[idx];
        component_mean_correlation_sorted[i] = component_mean_correlation[idx];
        component_best_partner_sorted[i] = component_best_partner[idx];
        component_max_correlation_sorted[i] = component_max_correlation[idx];
        component_mean_correlation_to_component_sorted[i] = component_mean_correlation_to_component[idx];
        component_membership_score_sorted[i] = component_membership_score[idx];
        component_is_core_sorted[i] = component_is_core[idx];
        component_bridge_flag_sorted[i] = component_bridge_flag[idx];
      }

      feature = feature_sorted;
      feature_group = feature_group_sorted;
      feature_component = feature_component_sorted;
      adduct = adduct_sorted;
      rt = rt_sorted;
      mz = mz_sorted;
      mass = mass_sorted;
      intensity = intensity_sorted;
      noise = noise_sorted;
      sn = sn_sorted;
      area = area_sorted;
      rtmin = rtmin_sorted;
      rtmax = rtmax_sorted;
      width = width_sorted;
      mzmin = mzmin_sorted;
      mzmax = mzmax_sorted;
      ppm = ppm_sorted;
      fwhm_rt = fwhm_rt_sorted;
      fwhm_mz = fwhm_mz_sorted;
      gaussian_A = gaussian_A_sorted;
      gaussian_mu = gaussian_mu_sorted;
      gaussian_sigma = gaussian_sigma_sorted;
      gaussian_r2 = gaussian_r2_sorted;
      jaggedness = jaggedness_sorted;
      sharpness = sharpness_sorted;
      asymmetry = asymmetry_sorted;
      modality = modality_sorted;
      plates = plates_sorted;
      polarity = polarity_sorted;
      filtered = filtered_sorted;
      filter = filter_sorted;
      filled = filled_sorted;
      correction = correction_sorted;
      eic_size = eic_size_sorted;
      eic_rt = eic_rt_sorted;
      eic_mz = eic_mz_sorted;
      eic_intensity = eic_intensity_sorted;
      eic_baseline = eic_baseline_sorted;
      eic_smoothed = eic_smoothed_sorted;
      ms1_size = ms1_size_sorted;
      ms1_mz = ms1_mz_sorted;
      ms1_intensity = ms1_intensity_sorted;
      ms2_size = ms2_size_sorted;
      ms2_mz = ms2_mz_sorted;
      ms2_intensity = ms2_intensity_sorted;
      annotation_category = annotation_category_sorted;
      annotation_type = annotation_type_sorted;
      annotation_parent_feature = annotation_parent_feature_sorted;
      annotation_element = annotation_element_sorted;
      annotation_mass_error_da = annotation_mass_error_da_sorted;
      annotation_mass_error_ppm = annotation_mass_error_ppm_sorted;
      annotation_rt_error = annotation_rt_error_sorted;
      annotation_rel_intensity = annotation_rel_intensity_sorted;
      annotation_expected_rel_intensity_min = annotation_expected_rel_intensity_min_sorted;
      annotation_expected_rel_intensity_max = annotation_expected_rel_intensity_max_sorted;
      annotation_score = annotation_score_sorted;
      component_size = component_size_sorted;
      component_rt_center = component_rt_center_sorted;
      component_rt_spread = component_rt_spread_sorted;
      component_density = component_density_sorted;
      component_mean_correlation = component_mean_correlation_sorted;
      component_best_partner = component_best_partner_sorted;
      component_max_correlation = component_max_correlation_sorted;
      component_mean_correlation_to_component = component_mean_correlation_to_component_sorted;
      component_membership_score = component_membership_score_sorted;
      component_is_core = component_is_core_sorted;
      component_bridge_flag = component_bridge_flag_sorted;
    }
};

// MARK: NTA_SUSPECT_ROW
struct NTA_SUSPECT_ROW {
    std::string created_at;
    std::string analysis;
    std::string feature;
    std::string feature_group;
    int candidate_rank = 0;
    std::string name;
    int polarity = 0;
    double db_mass = 0.0;
    double exp_mass = 0.0;
    double error_mass = 0.0;
    double db_rt = 0.0;
    double exp_rt = 0.0;
    double error_rt = 0.0;
    double intensity = 0.0;
    double area = 0.0;
    int id_level = 0;
    double score = 0.0;
    int shared_fragments = 0;
    double cosine_similarity = 0.0;
    std::string formula;
    std::string SMILES;
    std::string InChI;
    std::string InChIKey;
    double xLogP = 0.0;
    std::string database_id;
    int db_ms2_size = 0;
    std::string db_ms2_mz;
    std::string db_ms2_intensity;
    std::string db_ms2_formula;
    std::string db_ms2_smiles;
    int exp_ms2_size = 0;
    std::string exp_ms2_mz;
    std::string exp_ms2_intensity;
};

// MARK: NTA_SUSPECTS (columnar)
struct NTA_SUSPECTS {
    std::vector<std::string> analysis;
    std::vector<std::string> feature;
    std::vector<int> candidate_rank;
    std::vector<std::string> name;
    std::vector<int> polarity;
    std::vector<double> db_mass;
    std::vector<double> exp_mass;
    std::vector<double> error_mass;
    std::vector<double> db_rt;
    std::vector<double> exp_rt;
    std::vector<double> error_rt;
    std::vector<double> intensity;
    std::vector<double> area;
    std::vector<int> id_level;
    std::vector<double> score;
    std::vector<int> shared_fragments;
    std::vector<double> cosine_similarity;
    std::vector<std::string> formula;
    std::vector<std::string> SMILES;
    std::vector<std::string> InChI;
    std::vector<std::string> InChIKey;
    std::vector<double> xLogP;
    std::vector<std::string> database_id;
    std::vector<int> db_ms2_size;
    std::vector<std::string> db_ms2_mz;
    std::vector<std::string> db_ms2_intensity;
    std::vector<std::string> db_ms2_formula;
    std::vector<std::string> db_ms2_smiles;
    std::vector<int> exp_ms2_size;
    std::vector<std::string> exp_ms2_mz;
    std::vector<std::string> exp_ms2_intensity;

    int size() const { return static_cast<int>(analysis.size()); }

    NTA_SUSPECT_ROW get_suspect(const int &i) const
    {
      NTA_SUSPECT_ROW suspect_i;
      suspect_i.analysis = analysis[i];
      suspect_i.feature = feature[i];
      suspect_i.candidate_rank = candidate_rank[i];
      suspect_i.name = name[i];
      suspect_i.polarity = polarity[i];
      suspect_i.db_mass = db_mass[i];
      suspect_i.exp_mass = exp_mass[i];
      suspect_i.error_mass = error_mass[i];
      suspect_i.db_rt = db_rt[i];
      suspect_i.exp_rt = exp_rt[i];
      suspect_i.error_rt = error_rt[i];
      suspect_i.intensity = intensity[i];
      suspect_i.area = area[i];
      suspect_i.id_level = id_level[i];
      suspect_i.score = score[i];
      suspect_i.shared_fragments = shared_fragments[i];
      suspect_i.cosine_similarity = cosine_similarity[i];
      suspect_i.formula = formula[i];
      suspect_i.SMILES = SMILES[i];
      suspect_i.InChI = InChI[i];
      suspect_i.InChIKey = InChIKey[i];
      suspect_i.xLogP = xLogP[i];
      suspect_i.database_id = database_id[i];
      suspect_i.db_ms2_size = db_ms2_size[i];
      suspect_i.db_ms2_mz = db_ms2_mz[i];
      suspect_i.db_ms2_intensity = db_ms2_intensity[i];
      suspect_i.db_ms2_formula = db_ms2_formula[i];
      suspect_i.db_ms2_smiles = db_ms2_smiles[i];
      suspect_i.exp_ms2_size = exp_ms2_size[i];
      suspect_i.exp_ms2_mz = exp_ms2_mz[i];
      suspect_i.exp_ms2_intensity = exp_ms2_intensity[i];
      return suspect_i;
    }

    void set_suspect(const int &i, const NTA_SUSPECT_ROW &s)
    {
      analysis[i] = s.analysis;
      feature[i] = s.feature;
      candidate_rank[i] = s.candidate_rank;
      name[i] = s.name;
      polarity[i] = s.polarity;
      db_mass[i] = s.db_mass;
      exp_mass[i] = s.exp_mass;
      error_mass[i] = s.error_mass;
      db_rt[i] = s.db_rt;
      exp_rt[i] = s.exp_rt;
      error_rt[i] = s.error_rt;
      intensity[i] = s.intensity;
      area[i] = s.area;
      id_level[i] = s.id_level;
      score[i] = s.score;
      shared_fragments[i] = s.shared_fragments;
      cosine_similarity[i] = s.cosine_similarity;
      formula[i] = s.formula;
      SMILES[i] = s.SMILES;
      InChI[i] = s.InChI;
      InChIKey[i] = s.InChIKey;
      xLogP[i] = s.xLogP;
      database_id[i] = s.database_id;
      db_ms2_size[i] = s.db_ms2_size;
      db_ms2_mz[i] = s.db_ms2_mz;
      db_ms2_intensity[i] = s.db_ms2_intensity;
      db_ms2_formula[i] = s.db_ms2_formula;
      db_ms2_smiles[i] = s.db_ms2_smiles;
      exp_ms2_size[i] = s.exp_ms2_size;
      exp_ms2_mz[i] = s.exp_ms2_mz;
      exp_ms2_intensity[i] = s.exp_ms2_intensity;
    }

    void append(const NTA_SUSPECT_ROW &s)
    {
      analysis.push_back(s.analysis);
      feature.push_back(s.feature);
      candidate_rank.push_back(s.candidate_rank);
      name.push_back(s.name);
      polarity.push_back(s.polarity);
      db_mass.push_back(s.db_mass);
      exp_mass.push_back(s.exp_mass);
      error_mass.push_back(s.error_mass);
      db_rt.push_back(s.db_rt);
      exp_rt.push_back(s.exp_rt);
      error_rt.push_back(s.error_rt);
      intensity.push_back(s.intensity);
      area.push_back(s.area);
      id_level.push_back(s.id_level);
      score.push_back(s.score);
      shared_fragments.push_back(s.shared_fragments);
      cosine_similarity.push_back(s.cosine_similarity);
      formula.push_back(s.formula);
      SMILES.push_back(s.SMILES);
      InChI.push_back(s.InChI);
      InChIKey.push_back(s.InChIKey);
      xLogP.push_back(s.xLogP);
      database_id.push_back(s.database_id);
      db_ms2_size.push_back(s.db_ms2_size);
      db_ms2_mz.push_back(s.db_ms2_mz);
      db_ms2_intensity.push_back(s.db_ms2_intensity);
      db_ms2_formula.push_back(s.db_ms2_formula);
      db_ms2_smiles.push_back(s.db_ms2_smiles);
      exp_ms2_size.push_back(s.exp_ms2_size);
      exp_ms2_mz.push_back(s.exp_ms2_mz);
      exp_ms2_intensity.push_back(s.exp_ms2_intensity);
    }
};

// MARK: NTA_TRANSFORMATION_PRODUCT_ROW
struct NTA_TRANSFORMATION_PRODUCT_ROW {
    std::string created_at;
    std::string name;
    std::string formula;
    double mass = 0.0;
    std::string SMILES;
    std::string InChI;
    std::string InChIKey;
    double xLogP = 0.0;
    std::string transformation;
    std::string precursor_name;
    std::string precursor_formula;
    double precursor_mass = 0.0;
    std::string precursor_SMILES;
    std::string precursor_InChI;
    std::string precursor_InChIKey;
    double precursor_xLogP = 0.0;
    std::string main_precursor_name;
    std::string main_precursor_formula;
    double main_precursor_mass = 0.0;
    std::string main_precursor_SMILES;
    std::string main_precursor_InChI;
    std::string main_precursor_InChIKey;
    double main_precursor_xLogP = 0.0;
    std::string feature_group;
    std::string precursor_feature_group;
    std::string main_precursor_feature_group;
    double cosine_similarity = 0.0;
    double main_precursor_cosine_similarity = 0.0;
    double rt_plausibility = 0.0;
    double main_precursor_rt_plausibility = 0.0;
    std::string product_structure_key;
    std::string precursor_structure_key;
    std::string main_precursor_structure_key;
    std::string resolved_direct_parent_feature_group;
    std::string resolved_main_parent_feature_group;
    std::string assignment_status;
    bool is_direct_assignment = false;
    bool is_main_parent_consistent = false;
    bool transformation_valid = false;
    int assignment_rank = 0;
    int network_level = 0;
    double assignment_score = 0.0;
    double transformation_mass_delta_expected = 0.0;
    double transformation_mass_delta_observed = 0.0;
    double transformation_mass_delta_error = 0.0;
};

// MARK: NTA_TRANSFORMATION_PRODUCTS (columnar; structure-of-arrays ported from the R model)
struct NTA_TRANSFORMATION_PRODUCTS {
    std::vector<std::string> name;
    std::vector<std::string> formula;
    std::vector<double> mass;
    std::vector<std::string> SMILES;
    std::vector<std::string> InChI;
    std::vector<std::string> InChIKey;
    std::vector<double> xLogP;
    std::vector<std::string> transformation;
    std::vector<std::string> precursor_name;
    std::vector<std::string> precursor_formula;
    std::vector<double> precursor_mass;
    std::vector<std::string> precursor_SMILES;
    std::vector<std::string> precursor_InChI;
    std::vector<std::string> precursor_InChIKey;
    std::vector<double> precursor_xLogP;
    std::vector<std::string> main_precursor_name;
    std::vector<std::string> main_precursor_formula;
    std::vector<double> main_precursor_mass;
    std::vector<std::string> main_precursor_SMILES;
    std::vector<std::string> main_precursor_InChI;
    std::vector<std::string> main_precursor_InChIKey;
    std::vector<double> main_precursor_xLogP;
    std::vector<std::string> feature_group;
    std::vector<std::string> precursor_feature_group;
    std::vector<std::string> main_precursor_feature_group;
    std::vector<double> cosine_similarity;
    std::vector<double> main_precursor_cosine_similarity;
    std::vector<double> rt_plausibility;
    std::vector<double> main_precursor_rt_plausibility;
    std::vector<std::string> product_structure_key;
    std::vector<std::string> precursor_structure_key;
    std::vector<std::string> main_precursor_structure_key;
    std::vector<std::string> resolved_direct_parent_feature_group;
    std::vector<std::string> resolved_main_parent_feature_group;
    std::vector<std::string> assignment_status;
    std::vector<bool> is_direct_assignment;
    std::vector<bool> is_main_parent_consistent;
    std::vector<bool> transformation_valid;
    std::vector<int> assignment_rank;
    std::vector<int> network_level;
    std::vector<double> assignment_score;
    std::vector<double> transformation_mass_delta_expected;
    std::vector<double> transformation_mass_delta_observed;
    std::vector<double> transformation_mass_delta_error;

    int size() const { return static_cast<int>(name.size()); }

    NTA_TRANSFORMATION_PRODUCT_ROW get_transformation_product(const int &i) const
    {
        NTA_TRANSFORMATION_PRODUCT_ROW row;
        row.name = name[i];
        row.formula = formula[i];
        row.mass = mass[i];
        row.SMILES = SMILES[i];
        row.InChI = InChI[i];
        row.InChIKey = InChIKey[i];
        row.xLogP = xLogP[i];
        row.transformation = transformation[i];
        row.precursor_name = precursor_name[i];
        row.precursor_formula = precursor_formula[i];
        row.precursor_mass = precursor_mass[i];
        row.precursor_SMILES = precursor_SMILES[i];
        row.precursor_InChI = precursor_InChI[i];
        row.precursor_InChIKey = precursor_InChIKey[i];
        row.precursor_xLogP = precursor_xLogP[i];
        row.main_precursor_name = main_precursor_name[i];
        row.main_precursor_formula = main_precursor_formula[i];
        row.main_precursor_mass = main_precursor_mass[i];
        row.main_precursor_SMILES = main_precursor_SMILES[i];
        row.main_precursor_InChI = main_precursor_InChI[i];
        row.main_precursor_InChIKey = main_precursor_InChIKey[i];
        row.main_precursor_xLogP = main_precursor_xLogP[i];
        row.feature_group = feature_group[i];
        row.precursor_feature_group = precursor_feature_group[i];
        row.main_precursor_feature_group = main_precursor_feature_group[i];
        row.cosine_similarity = cosine_similarity[i];
        row.main_precursor_cosine_similarity = main_precursor_cosine_similarity[i];
        row.rt_plausibility = rt_plausibility[i];
        row.main_precursor_rt_plausibility = main_precursor_rt_plausibility[i];
        row.product_structure_key = product_structure_key[i];
        row.precursor_structure_key = precursor_structure_key[i];
        row.main_precursor_structure_key = main_precursor_structure_key[i];
        row.resolved_direct_parent_feature_group = resolved_direct_parent_feature_group[i];
        row.resolved_main_parent_feature_group = resolved_main_parent_feature_group[i];
        row.assignment_status = assignment_status[i];
        row.is_direct_assignment = is_direct_assignment[i];
        row.is_main_parent_consistent = is_main_parent_consistent[i];
        row.transformation_valid = transformation_valid[i];
        row.assignment_rank = assignment_rank[i];
        row.network_level = network_level[i];
        row.assignment_score = assignment_score[i];
        row.transformation_mass_delta_expected = transformation_mass_delta_expected[i];
        row.transformation_mass_delta_observed = transformation_mass_delta_observed[i];
        row.transformation_mass_delta_error = transformation_mass_delta_error[i];
        return row;
    }

    void append(const NTA_TRANSFORMATION_PRODUCT_ROW &row)
    {
        name.push_back(row.name);
        formula.push_back(row.formula);
        mass.push_back(row.mass);
        SMILES.push_back(row.SMILES);
        InChI.push_back(row.InChI);
        InChIKey.push_back(row.InChIKey);
        xLogP.push_back(row.xLogP);
        transformation.push_back(row.transformation);
        precursor_name.push_back(row.precursor_name);
        precursor_formula.push_back(row.precursor_formula);
        precursor_mass.push_back(row.precursor_mass);
        precursor_SMILES.push_back(row.precursor_SMILES);
        precursor_InChI.push_back(row.precursor_InChI);
        precursor_InChIKey.push_back(row.precursor_InChIKey);
        precursor_xLogP.push_back(row.precursor_xLogP);
        main_precursor_name.push_back(row.main_precursor_name);
        main_precursor_formula.push_back(row.main_precursor_formula);
        main_precursor_mass.push_back(row.main_precursor_mass);
        main_precursor_SMILES.push_back(row.main_precursor_SMILES);
        main_precursor_InChI.push_back(row.main_precursor_InChI);
        main_precursor_InChIKey.push_back(row.main_precursor_InChIKey);
        main_precursor_xLogP.push_back(row.main_precursor_xLogP);
        feature_group.push_back(row.feature_group);
        precursor_feature_group.push_back(row.precursor_feature_group);
        main_precursor_feature_group.push_back(row.main_precursor_feature_group);
        cosine_similarity.push_back(row.cosine_similarity);
        main_precursor_cosine_similarity.push_back(row.main_precursor_cosine_similarity);
        rt_plausibility.push_back(row.rt_plausibility);
        main_precursor_rt_plausibility.push_back(row.main_precursor_rt_plausibility);
        product_structure_key.push_back(row.product_structure_key);
        precursor_structure_key.push_back(row.precursor_structure_key);
        main_precursor_structure_key.push_back(row.main_precursor_structure_key);
        resolved_direct_parent_feature_group.push_back(row.resolved_direct_parent_feature_group);
        resolved_main_parent_feature_group.push_back(row.resolved_main_parent_feature_group);
        assignment_status.push_back(row.assignment_status);
        is_direct_assignment.push_back(row.is_direct_assignment);
        is_main_parent_consistent.push_back(row.is_main_parent_consistent);
        transformation_valid.push_back(row.transformation_valid);
        assignment_rank.push_back(row.assignment_rank);
        network_level.push_back(row.network_level);
        assignment_score.push_back(row.assignment_score);
        transformation_mass_delta_expected.push_back(row.transformation_mass_delta_expected);
        transformation_mass_delta_observed.push_back(row.transformation_mass_delta_observed);
        transformation_mass_delta_error.push_back(row.transformation_mass_delta_error);
    }
};

// MARK: NTA_INTERNAL_STANDARD_ROW
struct NTA_INTERNAL_STANDARD_ROW {
    std::string created_at;
    std::string analysis;
    std::string feature;
    std::string feature_group;
    std::string feature_component;
    std::string adduct;
    int candidate_rank = 0;
    std::string name;
    int polarity = 0;
    double db_mass = 0.0;
    double exp_mass = 0.0;
    double error_mass = 0.0;
    double db_rt = 0.0;
    double exp_rt = 0.0;
    double error_rt = 0.0;
    double intensity = 0.0;
    double area = 0.0;
    int id_level = 0;
    double score = 0.0;
    int shared_fragments = 0;
    double cosine_similarity = 0.0;
    std::string formula;
    std::string SMILES;
    std::string InChI;
    std::string InChIKey;
    double xLogP = 0.0;
    std::string database_id;
    int db_ms2_size = 0;
    std::string db_ms2_mz;
    std::string db_ms2_intensity;
    std::string db_ms2_formula;
    std::string db_ms2_smiles;
    int exp_ms2_size = 0;
    std::string exp_ms2_mz;
    std::string exp_ms2_intensity;
};

// MARK: NTA_INTERNAL_STANDARDS (columnar)
struct NTA_INTERNAL_STANDARDS {
    std::vector<std::string> analysis;
    std::vector<std::string> feature;
    std::vector<int> candidate_rank;
    std::vector<std::string> name;
    std::vector<int> polarity;
    std::vector<double> db_mass;
    std::vector<double> exp_mass;
    std::vector<double> error_mass;
    std::vector<double> db_rt;
    std::vector<double> exp_rt;
    std::vector<double> error_rt;
    std::vector<double> intensity;
    std::vector<double> area;
    std::vector<int> id_level;
    std::vector<double> score;
    std::vector<int> shared_fragments;
    std::vector<double> cosine_similarity;
    std::vector<std::string> formula;
    std::vector<std::string> SMILES;
    std::vector<std::string> InChI;
    std::vector<std::string> InChIKey;
    std::vector<double> xLogP;
    std::vector<std::string> database_id;
    std::vector<int> db_ms2_size;
    std::vector<std::string> db_ms2_mz;
    std::vector<std::string> db_ms2_intensity;
    std::vector<std::string> db_ms2_formula;
    std::vector<std::string> db_ms2_smiles;
    std::vector<int> exp_ms2_size;
    std::vector<std::string> exp_ms2_mz;
    std::vector<std::string> exp_ms2_intensity;

    int size() const { return static_cast<int>(analysis.size()); }

    NTA_INTERNAL_STANDARD_ROW get_internal_standard(const int &i) const
    {
      NTA_INTERNAL_STANDARD_ROW standard_i;
      standard_i.analysis = analysis[i];
      standard_i.feature = feature[i];
      standard_i.candidate_rank = candidate_rank[i];
      standard_i.name = name[i];
      standard_i.polarity = polarity[i];
      standard_i.db_mass = db_mass[i];
      standard_i.exp_mass = exp_mass[i];
      standard_i.error_mass = error_mass[i];
      standard_i.db_rt = db_rt[i];
      standard_i.exp_rt = exp_rt[i];
      standard_i.error_rt = error_rt[i];
      standard_i.intensity = intensity[i];
      standard_i.area = area[i];
      standard_i.id_level = id_level[i];
      standard_i.score = score[i];
      standard_i.shared_fragments = shared_fragments[i];
      standard_i.cosine_similarity = cosine_similarity[i];
      standard_i.formula = formula[i];
      standard_i.SMILES = SMILES[i];
      standard_i.InChI = InChI[i];
      standard_i.InChIKey = InChIKey[i];
      standard_i.xLogP = xLogP[i];
      standard_i.database_id = database_id[i];
      standard_i.db_ms2_size = db_ms2_size[i];
      standard_i.db_ms2_mz = db_ms2_mz[i];
      standard_i.db_ms2_intensity = db_ms2_intensity[i];
      standard_i.db_ms2_formula = db_ms2_formula[i];
      standard_i.db_ms2_smiles = db_ms2_smiles[i];
      standard_i.exp_ms2_size = exp_ms2_size[i];
      standard_i.exp_ms2_mz = exp_ms2_mz[i];
      standard_i.exp_ms2_intensity = exp_ms2_intensity[i];
      return standard_i;
    }

    void set_internal_standard(const int &i, const NTA_INTERNAL_STANDARD_ROW &is)
    {
      analysis[i] = is.analysis;
      feature[i] = is.feature;
      candidate_rank[i] = is.candidate_rank;
      name[i] = is.name;
      polarity[i] = is.polarity;
      db_mass[i] = is.db_mass;
      exp_mass[i] = is.exp_mass;
      error_mass[i] = is.error_mass;
      db_rt[i] = is.db_rt;
      exp_rt[i] = is.exp_rt;
      error_rt[i] = is.error_rt;
      intensity[i] = is.intensity;
      area[i] = is.area;
      id_level[i] = is.id_level;
      score[i] = is.score;
      shared_fragments[i] = is.shared_fragments;
      cosine_similarity[i] = is.cosine_similarity;
      formula[i] = is.formula;
      SMILES[i] = is.SMILES;
      InChI[i] = is.InChI;
      InChIKey[i] = is.InChIKey;
      xLogP[i] = is.xLogP;
      database_id[i] = is.database_id;
      db_ms2_size[i] = is.db_ms2_size;
      db_ms2_mz[i] = is.db_ms2_mz;
      db_ms2_intensity[i] = is.db_ms2_intensity;
      db_ms2_formula[i] = is.db_ms2_formula;
      db_ms2_smiles[i] = is.db_ms2_smiles;
      exp_ms2_size[i] = is.exp_ms2_size;
      exp_ms2_mz[i] = is.exp_ms2_mz;
      exp_ms2_intensity[i] = is.exp_ms2_intensity;
    }

    void append(const NTA_INTERNAL_STANDARD_ROW &is)
    {
      analysis.push_back(is.analysis);
      feature.push_back(is.feature);
      candidate_rank.push_back(is.candidate_rank);
      name.push_back(is.name);
      polarity.push_back(is.polarity);
      db_mass.push_back(is.db_mass);
      exp_mass.push_back(is.exp_mass);
      error_mass.push_back(is.error_mass);
      db_rt.push_back(is.db_rt);
      exp_rt.push_back(is.exp_rt);
      error_rt.push_back(is.error_rt);
      intensity.push_back(is.intensity);
      area.push_back(is.area);
      id_level.push_back(is.id_level);
      score.push_back(is.score);
      shared_fragments.push_back(is.shared_fragments);
      cosine_similarity.push_back(is.cosine_similarity);
      formula.push_back(is.formula);
      SMILES.push_back(is.SMILES);
      InChI.push_back(is.InChI);
      InChIKey.push_back(is.InChIKey);
      xLogP.push_back(is.xLogP);
      database_id.push_back(is.database_id);
      db_ms2_size.push_back(is.db_ms2_size);
      db_ms2_mz.push_back(is.db_ms2_mz);
      db_ms2_intensity.push_back(is.db_ms2_intensity);
      db_ms2_formula.push_back(is.db_ms2_formula);
      db_ms2_smiles.push_back(is.db_ms2_smiles);
      exp_ms2_size.push_back(is.exp_ms2_size);
      exp_ms2_mz.push_back(is.exp_ms2_mz);
      exp_ms2_intensity.push_back(is.exp_ms2_intensity);
    }
};

} // namespace nta::api

namespace nta::api {
class PROJECT_NON_TARGET_ANALYSIS {
public:
    PROJECT_NON_TARGET_ANALYSIS(std::vector<std::string> names, std::vector<std::string> paths,
                                std::vector<mass_spec::reader::MASS_SPEC_SPECTRA_HEADERS> headers)
        : names_(std::move(names)), paths_(std::move(paths)), headers_(std::move(headers)),
          buffers_(names_.size()), blank_names_(names_.size()), replicate_names_(names_.size()),
          suspect_buffers_(names_.size()), internal_standard_buffers_(names_.size()),
          analysis_indices_(names_.size(), 0) {}
    const std::vector<std::string> &analysis_names() const { return names_; }
    const std::vector<std::string> &file_paths() const { return paths_; }
    const std::vector<std::string> &blank_names() const { return blank_names_; }
    const std::vector<std::string> &replicate_names() const { return replicate_names_; }
    const auto &spectra_headers_at(size_t i) const { return headers_.at(i); }
    const std::vector<int> &analysis_indices() const { return analysis_indices_; }
    int analysis_index_at(size_t i) const { return analysis_indices_.at(i); }
    auto &feature_buffers() { return buffers_; }
    std::vector<api::NTA_SUSPECTS> &suspect_buffers() { return suspect_buffers_; }
    const std::vector<api::NTA_SUSPECTS> &suspect_buffers() const { return suspect_buffers_; }
    std::vector<api::NTA_INTERNAL_STANDARDS> &internal_standard_buffers() { return internal_standard_buffers_; }
    const std::vector<api::NTA_INTERNAL_STANDARDS> &internal_standard_buffers() const { return internal_standard_buffers_; }
    void set_blank_names(std::vector<std::string> b) {
        blank_names_ = std::move(b);
        if (blank_names_.size() < names_.size()) blank_names_.resize(names_.size());
    }
    void set_replicate_names(std::vector<std::string> r) {
        replicate_names_ = std::move(r);
        if (replicate_names_.size() < names_.size()) replicate_names_.resize(names_.size());
    }
    void set_analysis_indices(std::vector<int> a) {
        analysis_indices_ = std::move(a);
        if (analysis_indices_.size() < names_.size()) analysis_indices_.resize(names_.size(), 0);
    }
    int size() const { return static_cast<int>(names_.size()); }
private:
    std::vector<std::string> names_, paths_;
    std::vector<std::string> blank_names_, replicate_names_;
    std::vector<mass_spec::reader::MASS_SPEC_SPECTRA_HEADERS> headers_;
    std::vector<api::NTA_FEATURES> buffers_;
    std::vector<api::NTA_SUSPECTS> suspect_buffers_;
    std::vector<api::NTA_INTERNAL_STANDARDS> internal_standard_buffers_;
    std::vector<int> analysis_indices_;
};
} // namespace nta::api

namespace nta {
using PROJECT_NON_TARGET_ANALYSIS = api::PROJECT_NON_TARGET_ANALYSIS;
using namespace api;
}
