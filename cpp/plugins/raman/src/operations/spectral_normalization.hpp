#pragma once

#include <optional>
#include <string>
#include <vector>

namespace streamfind::raman::operations {

enum class NormalizationMethod {
    min_max,
    standard_normal_variate,
    standard_deviation_scale,
    length_block_weighting,
    mean_center
};

NormalizationMethod parse_normalization_method(const std::string &name);
std::vector<std::optional<double>> normalize_spectrum(
    const std::vector<std::optional<double>> &intensities,
    NormalizationMethod method,
    bool lift_to_zero = false);

}  // namespace streamfind::raman::operations
