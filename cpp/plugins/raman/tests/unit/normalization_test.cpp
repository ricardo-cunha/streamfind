#include "spectral_normalization.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::tests {

using operations::NormalizationMethod;
using operations::normalize_spectrum;
using Values = std::vector<std::optional<double>>;

void require_near(double actual, double expected, const std::string &label) {
    if (std::abs(actual - expected) > 1e-12)
        throw std::runtime_error(label + " mismatch");
}

void require_throws(const Values &values, NormalizationMethod method, const std::string &label) {
    try {
        (void)normalize_spectrum(values, method);
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error(label + " should have thrown");
}

}  // namespace streamfind::raman::tests

int main() {
    using namespace streamfind::raman::tests;
    try {
        const Values input{2.0, 4.0, 6.0};
        const auto minmax = normalize_spectrum(input, NormalizationMethod::min_max);
        require_near(*minmax[0], 0.0, "minmax minimum");
        require_near(*minmax[1], 0.5, "minmax midpoint");
        require_near(*minmax[2], 1.0, "minmax maximum");

        const auto snv = normalize_spectrum(input, NormalizationMethod::standard_normal_variate);
        require_near(*snv[0], -1.0, "SNV low");
        require_near(*snv[1], 0.0, "SNV mean");
        require_near(*snv[2], 1.0, "SNV high");
        const auto offset_snv = normalize_spectrum(
            {1e12, 1e12 + 1.0, 1e12 + 2.0}, NormalizationMethod::standard_normal_variate);
        require_near(*offset_snv[0], -1.0, "offset SNV low");
        require_near(*offset_snv[2], 1.0, "offset SNV high");
        const auto lifted = normalize_spectrum(input, NormalizationMethod::standard_normal_variate, true);
        require_near(*lifted[0], 0.0, "lifted SNV minimum");
        require_near(*lifted[2], 2.0, "lifted SNV maximum");

        const auto scaled = normalize_spectrum(input, NormalizationMethod::standard_deviation_scale);
        require_near(*scaled[0], 1.0, "SD scaling low");
        require_near(*scaled[2], 3.0, "SD scaling high");
        const auto weighted = normalize_spectrum(input, NormalizationMethod::length_block_weighting);
        require_near(*weighted[1], 4.0 / std::sqrt(3.0), "block weighting");
        const auto centered = normalize_spectrum(input, NormalizationMethod::mean_center);
        require_near(*centered[0], -2.0, "mean center low");
        require_near(*centered[2], 2.0, "mean center high");

        const Values with_missing{1.0, std::nullopt, 3.0};
        const auto missing_result = normalize_spectrum(with_missing, NormalizationMethod::min_max);
        require_near(*missing_result[0], 0.0, "missing minmax low");
        if (missing_result[1].has_value()) throw std::runtime_error("missing intensity was not preserved");
        require_near(*missing_result[2], 1.0, "missing minmax high");

        require_throws({2.0, 2.0}, NormalizationMethod::min_max, "constant minmax");
        require_throws({2.0}, NormalizationMethod::standard_normal_variate, "single-point SNV");
        require_throws({2.0, 2.0}, NormalizationMethod::standard_deviation_scale, "constant SD scaling");
        require_throws({std::nullopt}, NormalizationMethod::mean_center, "all-missing spectrum");

        std::cout << "Raman normalization tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
