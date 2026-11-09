#include "operations/nta/nta_deconvolution.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

int main()
{
  using namespace streamfind::mass_spec::nta::deconvolution;
  std::vector<float> intensities;
  for (int value = 1; value <= 100; ++value)
    intensities.push_back(static_cast<float>(value));

  const VectorStats stats(intensities);
  AdaptiveNoiseParams low_quantile(stats, static_cast<int>(intensities.size()), 0.1f);
  AdaptiveNoiseParams very_low_quantile(stats, static_cast<int>(intensities.size()), 0.01f);
  if (!(low_quantile.quantile > very_low_quantile.quantile))
    throw std::runtime_error("base_quantile did not change the adaptive quantile.");

  low_quantile.bins = 1;
  very_low_quantile.bins = 1;
  low_quantile.threshold_multiplier = 1.0f;
  very_low_quantile.threshold_multiplier = 1.0f;
  std::vector<float> background_heavy(20);
  for (std::size_t i = 0; i < background_heavy.size(); ++i)
    background_heavy[i] = static_cast<float>(10 + i);
  background_heavy.insert(background_heavy.end(), 80, 100.0f);

  // The robust local background dominates a low quantile, preventing weak
  // quantile settings from treating baseline variation as signal.
  const auto low_thresholds = calculate_noise_levels(background_heavy, low_quantile, 0.0f);
  if (low_thresholds.empty() || std::abs(low_thresholds.front() - 100.0f) > 1.0e-6f)
    throw std::runtime_error("robust local background floor was not applied.");

  // A higher selected quantile can raise the threshold above the robust floor.
  low_quantile.quantile = 0.03f;
  very_low_quantile.quantile = 0.30f;
  low_quantile.threshold_multiplier = 2.0f;
  very_low_quantile.threshold_multiplier = 2.0f;
  const auto higher_quantile_thresholds = calculate_noise_levels(background_heavy, very_low_quantile, 0.0f);
  if (higher_quantile_thresholds.empty() ||
      !(higher_quantile_thresholds.front() > low_thresholds.front()))
    throw std::runtime_error("base_quantile did not change the estimated noise threshold.");

  std::vector<float> constant_background(20, 5.0f);
  very_low_quantile.quantile = 0.1f;
  very_low_quantile.threshold_multiplier = 1.0f;
  const auto floor_thresholds = calculate_noise_levels(constant_background, very_low_quantile, 7.0f);
  if (std::abs(floor_thresholds.front() - 7.0f) > 1.0e-6f)
    throw std::runtime_error("noise_threshold was not preserved as a minimum threshold.");

  return 0;
}
