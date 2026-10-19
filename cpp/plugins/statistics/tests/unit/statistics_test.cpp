#include "statistics.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::statistics::tests {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, double tolerance,
                  const std::string &message) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message + ": got " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected));
}

void test_pca_order_and_reconstruction() {
    using namespace detail;
    const Matrix values{{1.0, 2.0}, {2.0, 4.0}, {3.0, 6.0}, {4.0, 8.0}};
    const auto result = compute_pca(values, "center", 2);
    require(result.eigenvalues[0] >= result.eigenvalues[1],
            "PCA components are not sorted by explained variance");
    require(result.explained_variance[0] > 0.999999,
            "PCA did not identify the rank-one direction");
    const auto largest_loading = std::max_element(
        result.loadings.begin(), result.loadings.end(), [](const auto &left, const auto &right) {
            return std::abs(left[0]) < std::abs(right[0]);
        });
    require(largest_loading->at(0) > 0.0,
            "PCA component sign was not oriented deterministically");
    const std::vector<double> means{2.5, 5.0};
    for (std::size_t sample = 0; sample < values.size(); ++sample)
        for (std::size_t variable = 0; variable < values[sample].size(); ++variable) {
            double reconstructed = means[variable];
            for (std::size_t component = 0; component < 2; ++component)
                reconstructed += result.scores[sample][component] * result.loadings[variable][component];
            require_near(reconstructed, values[sample][variable], 1e-9,
                         "PCA scores and loadings did not reconstruct the centered matrix");
        }
}

void test_pca_none_uses_covariance_centering() {
    using namespace detail;
    const Matrix values{{10.0, 0.0}, {10.0, 1.0}, {12.0, 2.0}, {12.0, 0.0}};
    const auto result = compute_pca(values, "none", 1);
    double score_sum = 0.0;
    for (const auto &row : result.scores) score_sum += row[0];
    require_near(score_sum, 0.0, 1e-10,
                 "covariance PCA must center observations when preprocessing is none");
}

void test_pls_regression_fitted_values() {
    using namespace detail;
    const Matrix x{{0.0}, {1.0}, {2.0}, {3.0}, {4.0}};
    const Matrix y{{3.0}, {5.0}, {7.0}, {9.0}, {11.0}};
    const auto result = compute_pls(x, y, "center", 1);
    require(result.predictions.size() == y.size(), "PLS omitted fitted sample values");
    require_near(result.intercept[0], 3.0, 1e-9, "PLS regression intercept is incorrect");
    require_near(result.coefficients[0][0], 2.0, 1e-9, "PLS regression coefficient is incorrect");
    for (std::size_t sample = 0; sample < y.size(); ++sample)
        require_near(result.predictions[sample][0], y[sample][0], 1e-8,
                     "PLS fitted value is incorrect");

    const auto scaled = compute_pls(x, y, "scale", 1);
    require_near(scaled.intercept[0], 3.0, 1e-8,
                 "scaled PLS intercept is not reported in source predictor units");
    require_near(scaled.coefficients[0][0], 2.0, 1e-8,
                 "scaled PLS coefficient is not reported in source predictor units");
}

void test_pls_da_scores_separable_classes() {
    using namespace detail;
    const Matrix x{{0.0}, {0.1}, {0.2}, {4.8}, {4.9}, {5.0}};
    const Matrix one_hot{{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0},
                         {0.0, 1.0}, {0.0, 1.0}, {0.0, 1.0}};
    const auto result = compute_pls(x, one_hot, "center", 1);
    for (std::size_t sample = 0; sample < x.size(); ++sample) {
        const auto predicted = static_cast<std::size_t>(std::max_element(
            result.predictions[sample].begin(), result.predictions[sample].end()) -
            result.predictions[sample].begin());
        require(predicted == (sample < 3 ? 0U : 1U),
                "PLS-DA selected the wrong fitted class");
    }
}

void test_fold_change_and_degenerate_groups() {
    using namespace detail;
    const Matrix values{{1.0, 2.0}, {1.0, 2.0}, {3.0, 2.0}, {3.0, 2.0}};
    const auto result = compute_fold_change(values, {"A", "A", "B", "B"}, "A", "B", 1.0);
    require_near(result[0].mean_a, 1.0, 1e-12, "fold-change group A mean is incorrect");
    require_near(result[0].mean_b, 3.0, 1e-12, "fold-change group B mean is incorrect");
    require(result[0].p_value == 0.0 && result[0].adjusted_p_value == 0.0,
            "different constant groups must have zero p and q values");
    require(!result[0].effect_size_defined,
            "Hedges' g must be undefined for zero pooled variance");
    require(result[1].p_value == 1.0 && result[1].adjusted_p_value == 1.0,
            "matching constant groups must have p=q=1");
}

void test_welch_probability_and_bh_ordering() {
    using namespace detail;
    const Matrix values{{1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0},
                        {3.0, 1.0}, {4.0, 2.0}, {5.0, 3.0}};
    const auto result = compute_fold_change(values,
        {"A", "A", "A", "B", "B", "B"}, "A", "B", 1.0);
    require(result[0].p_value > 0.05 && result[0].p_value < 0.2,
            "Welch two-sided p-value is outside the expected range");
    require_near(result[0].p_value, 0.0704839969102199, 1e-10,
                 "Welch p-value does not match the two-sided t distribution");
    require(result[1].p_value == 1.0,
            "identical group distributions should have p=1");
    require(result[0].adjusted_p_value >= result[0].p_value,
            "Benjamini-Hochberg adjustment reduced a p-value");
    require_near(result[0].adjusted_p_value, 0.14096799382044, 1e-10,
                 "Benjamini-Hochberg adjustment is incorrect");
    require_near(result[0].effect_size, 1.6, 1e-10,
                 "Hedges' g does not use its small-sample correction");
}

}  // namespace streamfind::statistics::tests

int main() {
    using namespace streamfind::statistics::tests;
    try {
        test_pca_order_and_reconstruction();
        test_pca_none_uses_covariance_centering();
        test_pls_regression_fitted_values();
        test_pls_da_scores_separable_classes();
        test_fold_change_and_degenerate_groups();
        test_welch_probability_and_bh_ordering();
        std::cout << "Statistics numerical unit tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
