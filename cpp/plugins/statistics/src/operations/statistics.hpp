#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace streamfind::statistics::detail {

using Matrix = std::vector<std::vector<double>>;

struct PcaResult {
    Matrix scores;
    Matrix loadings;
    std::vector<double> eigenvalues;
    std::vector<double> explained_variance;
    std::vector<double> cumulative_variance;
    double total_variance = 0.0;
};

struct PlsResult {
    Matrix x_scores;
    Matrix y_scores;
    Matrix x_loadings;
    Matrix y_loadings;
    Matrix coefficients;
    std::vector<double> intercept;
    Matrix predictions;
    std::size_t iterations = 0;
    bool converged = true;
};

struct FoldFeatureResult {
    double mean_a = 0.0;
    double mean_b = 0.0;
    double effect_size = 0.0;
    double p_value = 1.0;
    double adjusted_p_value = 1.0;
    bool effect_size_defined = true;
};

PcaResult compute_pca(const Matrix &values, const std::string &preprocessing,
                      std::size_t components);
PlsResult compute_pls(const Matrix &predictors, const Matrix &responses,
                      const std::string &preprocessing, std::size_t components,
                      double tolerance = 1e-10, std::size_t max_iterations = 500);
std::vector<FoldFeatureResult> compute_fold_change(
    const Matrix &values, const std::vector<std::string> &groups,
    const std::string &group_a, const std::string &group_b,
    double pseudocount);
void preprocess(Matrix &values, const std::string &method);

}  // namespace streamfind::statistics::detail
