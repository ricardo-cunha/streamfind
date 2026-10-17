#include "spectral_normalization.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace streamfind::raman::operations {

NormalizationMethod parse_normalization_method(const std::string &name) {
    if (name == "minmax") return NormalizationMethod::min_max;
    if (name == "snv") return NormalizationMethod::standard_normal_variate;
    if (name == "scale") return NormalizationMethod::standard_deviation_scale;
    if (name == "blockweight") return NormalizationMethod::length_block_weighting;
    if (name == "meancenter") return NormalizationMethod::mean_center;
    throw std::invalid_argument("algorithm must be minmax, snv, scale, blockweight, or meancenter");
}

std::vector<std::optional<double>> normalize_spectrum(
    const std::vector<std::optional<double>> &intensities,
    NormalizationMethod method,
    bool lift_to_zero) {
    if (lift_to_zero && method != NormalizationMethod::standard_normal_variate)
        throw std::invalid_argument("lift_to_zero is only valid with the snv algorithm");
    if (intensities.empty()) throw std::invalid_argument("cannot normalize an empty spectrum");

    std::vector<double> values;
    values.reserve(intensities.size());
    double mean = 0.0;
    double sum_squared_deviations = 0.0;
    for (const auto &intensity : intensities) {
        if (!intensity) continue;
        if (!std::isfinite(*intensity))
            throw std::invalid_argument("spectrum intensity contains a non-finite value");
        values.push_back(*intensity);
        const auto count = static_cast<double>(values.size());
        const auto delta = *intensity - mean;
        mean += delta / count;
        sum_squared_deviations += delta * (*intensity - mean);
    }
    if (values.empty()) throw std::invalid_argument("spectrum has no valid intensity values");

    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    double sample_sd = 0.0;
    if (values.size() > 1) {
        sample_sd = std::sqrt(std::max(0.0, sum_squared_deviations) /
                              static_cast<double>(values.size() - 1));
    }

    std::vector<std::optional<double>> output;
    output.reserve(intensities.size());
    for (const auto &intensity : intensities) {
        if (!intensity) {
            output.emplace_back(std::nullopt);
            continue;
        }
        double result = *intensity;
        switch (method) {
        case NormalizationMethod::min_max:
            if (*maximum == *minimum)
                throw std::invalid_argument("minmax is undefined for a constant spectrum");
            result = (result - *minimum) / (*maximum - *minimum);
            break;
        case NormalizationMethod::standard_normal_variate:
            if (values.size() < 2 || sample_sd == 0.0)
                throw std::invalid_argument("snv requires at least two non-constant valid values");
            result = (result - mean) / sample_sd;
            break;
        case NormalizationMethod::standard_deviation_scale:
            if (values.size() < 2 || sample_sd == 0.0)
                throw std::invalid_argument("scale requires at least two non-constant valid values");
            result /= sample_sd;
            break;
        case NormalizationMethod::length_block_weighting:
            result /= std::sqrt(static_cast<double>(intensities.size()));
            break;
        case NormalizationMethod::mean_center:
            result -= mean;
            break;
        }
        if (!std::isfinite(result))
            throw std::invalid_argument("normalization produced a non-finite intensity");
        output.emplace_back(result);
    }
    if (lift_to_zero) {
        auto smallest = std::numeric_limits<double>::infinity();
        for (const auto &value : output)
            if (value) smallest = std::min(smallest, *value);
        for (auto &value : output)
            if (value) *value -= smallest;
    }
    return output;
}

}  // namespace streamfind::raman::operations
