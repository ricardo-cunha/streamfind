#pragma once

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::nta::utils
{
  struct NTA_FEATURE_ROW
  {
    std::string analysis;
    std::string feature;
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
  std::string utc_now();
  std::string text(const nlohmann::json &, const char *);
  int integer(const nlohmann::json &, const char *);
  double real(const nlohmann::json &, const char *);
  const std::vector<std::string> &feature_columns();
  const std::vector<std::string> &feature_types();
  const std::vector<std::string> &suspects_columns();
  const std::vector<std::string> &internal_standards_columns();
  const std::vector<std::string> &transformation_products_columns();
  nlohmann::json feature_row(const NTA_FEATURE_ROW &);
  }

#define DEBUG_LOG(value)                                          \
  do                                                              \
  {                                                               \
    if (::streamfind::mass_spec::nta::utils::debug_log.is_open()) \
      ::streamfind::mass_spec::nta::utils::debug_log << value;    \
  } while (false)
#define DEBUG_OUT(value)                                          \
  do                                                              \
  {                                                               \
    if (::streamfind::mass_spec::nta::utils::debug_log.is_open()) \
    {                                                             \
      ::streamfind::mass_spec::nta::utils::debug_log << value;    \
      ::streamfind::mass_spec::nta::utils::debug_log.flush();     \
    }                                                             \
    std::cout << value;                                           \
  } while (0)
