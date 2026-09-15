#pragma once

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace nta::utils
{
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
