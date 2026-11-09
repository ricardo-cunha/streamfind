#include "operations/nta/nta_deconvolution.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace streamfind::mass_spec::nta::deconvolution
{
namespace detail
{
float quantile(std::vector<float> values, const float q)
{
  if (values.empty()) return 0.0f;
  const auto index = static_cast<std::size_t>(
      (values.size() - 1) * std::clamp(q, 0.0f, 1.0f));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}
}

VectorStats::VectorStats(const std::vector<float> &input_data)
{
  count = input_data.size();
  if (count == 0)
  {
    mean = std_dev = coefficient_variation = 0.0f;
    min_val = max_val = signal_noise_ratio = 0.0f;
    return;
  }
  mean = std::accumulate(input_data.begin(), input_data.end(), 0.0f) /
         static_cast<float>(count);
  float squared_deviations = 0.0f;
  for (const auto value : input_data)
    squared_deviations += (value - mean) * (value - mean);
  std_dev = std::sqrt(squared_deviations / static_cast<float>(count));
  coefficient_variation = mean != 0.0f ? std_dev / mean : 0.0f;
  const auto [min_it, max_it] = std::minmax_element(input_data.begin(), input_data.end());
  min_val = *min_it;
  max_val = *max_it;
  const float q25 = detail::quantile(input_data, 0.25f);
  const float q90 = detail::quantile(input_data, 0.90f);
  signal_noise_ratio = q25 != 0.0f ? q90 / q25 : 0.0f;
}

AdaptiveNoiseParams::AdaptiveNoiseParams(const VectorStats &stats, const int data_size,
                                         const float baseQuantile)
{
  if (!std::isfinite(baseQuantile) || !(baseQuantile > 0.0f && baseQuantile < 1.0f))
    throw std::invalid_argument("base_quantile must be greater than 0 and less than 1");
  if (data_size < 0)
    throw std::invalid_argument("noise-estimation data size cannot be negative");

  bins = std::max(10, std::min(200, static_cast<int>(std::sqrt(data_size) * 1.5)));
  if (stats.coefficient_variation > 2.0f)
    quantile = baseQuantile * 0.5f;
  else if (stats.coefficient_variation > 1.0f)
    quantile = baseQuantile;
  else
    quantile = baseQuantile * 2.0f;

  if (stats.signal_noise_ratio > 100.0f)
  {
    quantile *= 0.5f;
    threshold_multiplier = 1.2f;
  }
  else if (stats.signal_noise_ratio < 10.0f)
  {
    quantile *= 1.5f;
    threshold_multiplier = 0.8f;
  }
  else
  {
    threshold_multiplier = 1.0f;
  }
  quantile = std::clamp(quantile, 0.01f, 0.50f);

  const float data_sparsity = bins > 0 ? static_cast<float>(data_size) / bins : 0.0f;
  if (data_sparsity < 5.0f)
    bins = std::max(5, data_size / 5);
  else if (data_sparsity > 50.0f)
    bins = std::min(200, data_size / 20);
}

std::vector<int> calculate_bin_assignments(const std::vector<float> &data, const int num_bins)
{
  const auto n = data.size();
  std::vector<int> assignments(n);
  if (num_bins <= 1 || n == 0)
  {
    std::fill(assignments.begin(), assignments.end(), 0);
    return assignments;
  }
  for (std::size_t i = 0; i < n; ++i)
    assignments[i] = std::min(static_cast<int>(i * static_cast<std::size_t>(num_bins) / n),
                               num_bins - 1);
  return assignments;
}

std::vector<float> calculate_noise_levels(const std::vector<float> &intensities,
                                          const AdaptiveNoiseParams &params,
                                          const float noise_threshold)
{
  if (params.bins < 1 || !std::isfinite(noise_threshold) || noise_threshold < 0.0f)
    throw std::invalid_argument("noise-estimation bins and threshold must be valid");
  std::vector<float> noise_levels(intensities.size());
  const auto assignments = calculate_bin_assignments(intensities, params.bins);
  std::vector<std::vector<float>> bin_data(static_cast<std::size_t>(params.bins));
  for (std::size_t i = 0; i < intensities.size(); ++i)
    bin_data[static_cast<std::size_t>(assignments[i])].push_back(intensities[i]);

  std::vector<float> thresholds(static_cast<std::size_t>(params.bins), noise_threshold);
  for (int bin = 0; bin < params.bins; ++bin)
  {
    std::vector<float> positive;
    for (const auto value : bin_data[static_cast<std::size_t>(bin)])
      if (std::isfinite(value) && value > 0.0f) positive.push_back(value);
    if (positive.empty()) continue;

    // Keep a robust local background floor. A low requested quantile is useful
    // for preserving weak ions, but using it alone can classify nearly every
    // baseline fluctuation above noise_threshold as a signal. The median/IQR
    // fence separates the local background from sparse high-intensity ions.
    const float q25 = detail::quantile(positive, 0.25f);
    const float q50 = detail::quantile(positive, 0.50f);
    const float q75 = detail::quantile(positive, 0.75f);
    const float robust_background =
        q50 + 1.5f * std::max(0.0f, q75 - q25);
    const float requested_quantile = std::clamp(params.quantile, 0.01f, 0.50f);
    const float quantile_threshold = detail::quantile(positive, requested_quantile) *
                                     params.threshold_multiplier;
    thresholds[static_cast<std::size_t>(bin)] =
        std::max(noise_threshold, std::max(robust_background, quantile_threshold));
  }
  for (std::size_t i = 0; i < intensities.size(); ++i)
    noise_levels[i] = thresholds[static_cast<std::size_t>(assignments[i])];
  return noise_levels;
}
}
