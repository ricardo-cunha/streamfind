#include "statistics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace streamfind::statistics::detail {
namespace internal {

void validate_matrix(const Matrix &matrix, const char *label) {
    if (matrix.empty() || matrix.front().empty())
        throw std::invalid_argument(std::string(label) + " matrix must not be empty");
    const auto columns = matrix.front().size();
    for (const auto &row : matrix) {
        if (row.size() != columns)
            throw std::invalid_argument(std::string(label) + " matrix is ragged");
        for (const auto value : row)
            if (!std::isfinite(value))
                throw std::invalid_argument(std::string(label) + " matrix contains a non-finite value");
    }
}

std::vector<double> column_means(const Matrix &matrix) {
    std::vector<double> means(matrix.front().size(), 0.0);
    for (const auto &row : matrix)
        for (std::size_t column = 0; column < row.size(); ++column)
            means[column] += row[column];
    for (auto &mean : means) mean /= static_cast<double>(matrix.size());
    return means;
}

double norm(const std::vector<double> &values) {
    double square_sum = 0.0;
    for (const auto value : values) square_sum += value * value;
    return std::sqrt(square_sum);
}

std::vector<double> multiply(const Matrix &matrix, const std::vector<double> &vector) {
    std::vector<double> result(matrix.size(), 0.0);
    for (std::size_t row = 0; row < matrix.size(); ++row)
        for (std::size_t column = 0; column < vector.size(); ++column)
            result[row] += matrix[row][column] * vector[column];
    return result;
}

Matrix transpose(const Matrix &matrix) {
    Matrix result(matrix.front().size(), std::vector<double>(matrix.size(), 0.0));
    for (std::size_t row = 0; row < matrix.size(); ++row)
        for (std::size_t column = 0; column < matrix[row].size(); ++column)
            result[column][row] = matrix[row][column];
    return result;
}

Matrix multiply(const Matrix &left, const Matrix &right) {
    if (left.empty() || right.empty() || left.front().size() != right.size())
        throw std::invalid_argument("incompatible matrix dimensions");
    Matrix result(left.size(), std::vector<double>(right.front().size(), 0.0));
    for (std::size_t row = 0; row < left.size(); ++row)
        for (std::size_t middle = 0; middle < right.size(); ++middle)
            for (std::size_t column = 0; column < right[middle].size(); ++column)
                result[row][column] += left[row][middle] * right[middle][column];
    return result;
}

Matrix inverse(Matrix matrix) {
    const auto size = matrix.size();
    if (size == 0 || matrix.front().size() != size)
        throw std::invalid_argument("PLS coefficient system is not square");
    Matrix result(size, std::vector<double>(size, 0.0));
    for (std::size_t index = 0; index < size; ++index) result[index][index] = 1.0;
    for (std::size_t pivot = 0; pivot < size; ++pivot) {
        auto best = pivot;
        for (std::size_t row = pivot + 1; row < size; ++row)
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) best = row;
        if (std::abs(matrix[best][pivot]) < 1e-14)
            throw std::invalid_argument("PLS coefficient system is singular; reduce n_components");
        std::swap(matrix[pivot], matrix[best]);
        std::swap(result[pivot], result[best]);
        const auto divisor = matrix[pivot][pivot];
        for (std::size_t column = 0; column < size; ++column) {
            matrix[pivot][column] /= divisor;
            result[pivot][column] /= divisor;
        }
        for (std::size_t row = 0; row < size; ++row) {
            if (row == pivot) continue;
            const auto factor = matrix[row][pivot];
            for (std::size_t column = 0; column < size; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
                result[row][column] -= factor * result[pivot][column];
            }
        }
    }
    return result;
}

struct EigenSystem {
    std::vector<double> values;
    Matrix vectors;
};

EigenSystem symmetric_eigen(Matrix matrix) {
    const auto size = matrix.size();
    Matrix vectors(size, std::vector<double>(size, 0.0));
    for (std::size_t index = 0; index < size; ++index) vectors[index][index] = 1.0;
    double scale = 0.0;
    for (const auto &row : matrix)
        for (const auto value : row) scale = std::max(scale, std::abs(value));
    const auto tolerance = std::max(std::numeric_limits<double>::min(), scale * 1e-13);
    const auto max_iterations = std::max<std::size_t>(1, 50 * size * size);
    for (std::size_t iteration = 0; iteration < max_iterations; ++iteration) {
        std::size_t p = 0, q = 0;
        double largest = 0.0;
        for (std::size_t row = 0; row < size; ++row)
            for (std::size_t column = row + 1; column < size; ++column)
                if (std::abs(matrix[row][column]) > largest) {
                    largest = std::abs(matrix[row][column]);
                    p = row;
                    q = column;
                }
        if (largest <= tolerance) break;
        const auto tau = (matrix[q][q] - matrix[p][p]) / (2.0 * matrix[p][q]);
        const auto sign = tau < 0.0 ? -1.0 : 1.0;
        const auto tangent = sign / (std::abs(tau) + std::sqrt(1.0 + tau * tau));
        const auto cosine = 1.0 / std::sqrt(1.0 + tangent * tangent);
        const auto sine = tangent * cosine;
        const auto pp = matrix[p][p];
        const auto qq = matrix[q][q];
        const auto pq = matrix[p][q];
        matrix[p][p] = pp - tangent * pq;
        matrix[q][q] = qq + tangent * pq;
        matrix[p][q] = matrix[q][p] = 0.0;
        for (std::size_t index = 0; index < size; ++index) {
            if (index != p && index != q) {
                const auto ip = matrix[index][p];
                const auto iq = matrix[index][q];
                matrix[index][p] = matrix[p][index] = cosine * ip - sine * iq;
                matrix[index][q] = matrix[q][index] = sine * ip + cosine * iq;
            }
            const auto vp = vectors[index][p];
            const auto vq = vectors[index][q];
            vectors[index][p] = cosine * vp - sine * vq;
            vectors[index][q] = sine * vp + cosine * vq;
        }
    }
    std::vector<std::size_t> order(size);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto left, auto right) {
        return matrix[left][left] > matrix[right][right];
    });
    EigenSystem result{std::vector<double>(size), Matrix(size, std::vector<double>(size))};
    for (std::size_t component = 0; component < size; ++component) {
        result.values[component] = std::max(0.0, matrix[order[component]][order[component]]);
        for (std::size_t row = 0; row < size; ++row)
            result.vectors[row][component] = vectors[row][order[component]];
        std::size_t largest = 0;
        for (std::size_t row = 1; row < size; ++row)
            if (std::abs(result.vectors[row][component]) > std::abs(result.vectors[largest][component])) largest = row;
        if (result.vectors[largest][component] < 0.0)
            for (std::size_t row = 0; row < size; ++row) result.vectors[row][component] *= -1.0;
    }
    return result;
}

double beta_fraction(double a, double b, double x) {
    constexpr int maximum_iterations = 300;
    constexpr double epsilon = 3e-14;
    constexpr double tiny = 1e-300;
    const auto qab = a + b;
    const auto qap = a + 1.0;
    const auto qam = a - 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::abs(d) < tiny) d = tiny;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= maximum_iterations; ++m) {
        const auto m2 = 2.0 * m;
        auto aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        const auto delta = d * c;
        h *= delta;
        if (std::abs(delta - 1.0) < epsilon) break;
    }
    return h;
}

double regularized_beta(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const auto factor = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                                 a * std::log(x) + b * std::log1p(-x));
    if (x < (a + 1.0) / (a + b + 2.0))
        return factor * beta_fraction(a, b, x) / a;
    return 1.0 - factor * beta_fraction(b, a, 1.0 - x) / b;
}

double welch_p_value(const std::vector<double> &a, const std::vector<double> &b) {
    const auto mean_a = std::accumulate(a.begin(), a.end(), 0.0) / a.size();
    const auto mean_b = std::accumulate(b.begin(), b.end(), 0.0) / b.size();
    auto variance = [](const std::vector<double> &values, double mean) {
        double sum = 0.0;
        for (const auto value : values) sum += (value - mean) * (value - mean);
        return sum / static_cast<double>(values.size() - 1);
    };
    const auto variance_a = variance(a, mean_a);
    const auto variance_b = variance(b, mean_b);
    const auto term_a = variance_a / a.size();
    const auto term_b = variance_b / b.size();
    const auto standard_error_squared = term_a + term_b;
    if (standard_error_squared == 0.0) return mean_a == mean_b ? 1.0 : 0.0;
    const auto t = std::abs(mean_b - mean_a) / std::sqrt(standard_error_squared);
    const auto degrees = standard_error_squared * standard_error_squared /
        (term_a * term_a / (a.size() - 1.0) + term_b * term_b / (b.size() - 1.0));
    const auto x = degrees / (degrees + t * t);
    return std::clamp(regularized_beta(x, degrees / 2.0, 0.5), 0.0, 1.0);
}

}  // namespace internal

void preprocess(Matrix &values, const std::string &method) {
    internal::validate_matrix(values, "input");
    if (method != "none" && method != "center" && method != "scale")
        throw std::invalid_argument("preprocessing must be none, center, or scale");
    if (method == "none") return;
    const auto means = internal::column_means(values);
    for (std::size_t column = 0; column < means.size(); ++column) {
        double square_sum = 0.0;
        for (const auto &row : values) square_sum += (row[column] - means[column]) * (row[column] - means[column]);
        const auto sd = std::sqrt(square_sum / static_cast<double>(values.size() - 1));
        if (method == "scale" && (!(sd > 0.0) || !std::isfinite(sd)))
            throw std::invalid_argument("scale preprocessing cannot be applied to a constant variable");
        for (auto &row : values)
            row[column] = (row[column] - means[column]) / (method == "scale" ? sd : 1.0);
    }
}

PcaResult compute_pca(const Matrix &values, const std::string &preprocessing,
                      std::size_t components) {
    internal::validate_matrix(values, "PCA input");
    if (values.size() < 2) throw std::invalid_argument("PCA requires at least two samples");
    const auto variables = values.front().size();
    if (components == 0 || components > std::min(values.size(), variables))
        throw std::invalid_argument("n_components must be between 1 and the smaller matrix dimension");
    auto transformed = values;
    preprocess(transformed, preprocessing);
    const auto means = internal::column_means(transformed);
    for (auto &row : transformed)
        for (std::size_t variable = 0; variable < variables; ++variable)
            row[variable] -= means[variable];
    const auto denominator = static_cast<double>(transformed.size() - 1);
    Matrix covariance(variables, std::vector<double>(variables, 0.0));
    for (const auto &row : transformed)
        for (std::size_t left = 0; left < variables; ++left)
            for (std::size_t right = left; right < variables; ++right)
                covariance[left][right] += row[left] * row[right] / denominator;
    for (std::size_t left = 0; left < variables; ++left)
        for (std::size_t right = left + 1; right < variables; ++right)
            covariance[right][left] = covariance[left][right];
    const auto eigen = internal::symmetric_eigen(std::move(covariance));
    const auto total = std::accumulate(eigen.values.begin(), eigen.values.end(), 0.0);
    PcaResult result;
    result.total_variance = total;
    result.scores.assign(values.size(), std::vector<double>(components, 0.0));
    result.loadings.assign(variables, std::vector<double>(components, 0.0));
    result.eigenvalues.assign(eigen.values.begin(), eigen.values.begin() + static_cast<std::ptrdiff_t>(components));
    result.explained_variance.resize(components, 0.0);
    result.cumulative_variance.resize(components, 0.0);
    for (std::size_t component = 0; component < components; ++component) {
        result.explained_variance[component] = total > 0.0 ? eigen.values[component] / total : 0.0;
        result.cumulative_variance[component] = result.explained_variance[component] +
            (component ? result.cumulative_variance[component - 1] : 0.0);
        for (std::size_t variable = 0; variable < variables; ++variable)
            result.loadings[variable][component] = eigen.vectors[variable][component];
        for (std::size_t sample = 0; sample < values.size(); ++sample)
            for (std::size_t variable = 0; variable < variables; ++variable)
                result.scores[sample][component] += transformed[sample][variable] * result.loadings[variable][component];
    }
    return result;
}

PlsResult compute_pls(const Matrix &predictors, const Matrix &responses,
                      const std::string &preprocessing, std::size_t components,
                      double tolerance, std::size_t max_iterations) {
    internal::validate_matrix(predictors, "PLS predictor");
    internal::validate_matrix(responses, "PLS response");
    if (predictors.size() != responses.size())
        throw std::invalid_argument("PLS predictors and responses must have the same sample count");
    const auto n = predictors.size();
    const auto p = predictors.front().size();
    const auto q = responses.front().size();
    if (n < 2) throw std::invalid_argument("PLS requires at least two samples");
    if (components == 0 || components > std::min(n, p))
        throw std::invalid_argument("n_components must be between 1 and the smaller predictor matrix dimension");
    if (!(tolerance > 0.0) || !std::isfinite(tolerance) || max_iterations == 0)
        throw std::invalid_argument("invalid NIPALS convergence configuration");
    auto x = predictors;
    preprocess(x, preprocessing);
    auto x_residual = x;
    auto y_residual = responses;
    const auto y_means = internal::column_means(responses);
    for (auto &row : y_residual)
        for (std::size_t column = 0; column < q; ++column) row[column] -= y_means[column];

    PlsResult result;
    result.x_scores.assign(n, std::vector<double>(components, 0.0));
    result.y_scores.assign(n, std::vector<double>(components, 0.0));
    result.x_loadings.assign(p, std::vector<double>(components, 0.0));
    result.y_loadings.assign(q, std::vector<double>(components, 0.0));
    Matrix weights(p, std::vector<double>(components, 0.0));
    for (std::size_t component = 0; component < components; ++component) {
        std::size_t initial_column = 0;
        double initial_norm = 0.0;
        for (std::size_t column = 0; column < q; ++column) {
            double candidate_norm = 0.0;
            for (std::size_t row = 0; row < n; ++row) candidate_norm += y_residual[row][column] * y_residual[row][column];
            if (candidate_norm > initial_norm) { initial_norm = candidate_norm; initial_column = column; }
        }
        if (initial_norm < 1e-24) {
            if (component == 0) throw std::invalid_argument("PLS response has no remaining variation");
            result.converged = false;
            break;
        }
        std::vector<double> u(n, 0.0);
        for (std::size_t row = 0; row < n; ++row) u[row] = y_residual[row][initial_column];
        std::vector<double> w(p, 0.0), t(n, 0.0), c(q, 0.0), u_new(n, 0.0);
        bool converged = false;
        for (std::size_t iteration = 0; iteration < max_iterations; ++iteration) {
            for (std::size_t column = 0; column < p; ++column)
                for (std::size_t row = 0; row < n; ++row) w[column] += x_residual[row][column] * u[row];
            const auto w_norm = internal::norm(w);
            if (w_norm < 1e-14) break;
            for (auto &value : w) value /= w_norm;
            for (std::size_t row = 0; row < n; ++row)
                for (std::size_t column = 0; column < p; ++column) t[row] += x_residual[row][column] * w[column];
            double t_square = 0.0;
            for (const auto value : t) t_square += value * value;
            if (t_square < 1e-24) break;
            for (std::size_t column = 0; column < q; ++column)
                for (std::size_t row = 0; row < n; ++row) c[column] += y_residual[row][column] * t[row] / t_square;
            const auto c_norm = internal::norm(c);
            if (c_norm < 1e-14) break;
            for (std::size_t row = 0; row < n; ++row)
                for (std::size_t column = 0; column < q; ++column) u_new[row] += y_residual[row][column] * c[column];
            const auto denominator = std::max(1.0, internal::norm(u_new));
            double difference = 0.0;
            for (std::size_t row = 0; row < n; ++row) {
                const auto delta = u_new[row] - u[row];
                difference += delta * delta;
            }
            result.iterations += 1;
            if (std::sqrt(difference) / denominator <= tolerance) { converged = true; break; }
            u = u_new;
            std::fill(w.begin(), w.end(), 0.0);
            std::fill(t.begin(), t.end(), 0.0);
            std::fill(c.begin(), c.end(), 0.0);
            std::fill(u_new.begin(), u_new.end(), 0.0);
        }
        if (!converged) result.converged = false;
        if (internal::norm(t) < 1e-14) {
            if (component == 0) throw std::invalid_argument("PLS could not extract a non-degenerate component");
            break;
        }
        double t_square = 0.0;
        for (const auto value : t) t_square += value * value;
        std::vector<double> p_loading(p, 0.0), q_loading(q, 0.0);
        for (std::size_t column = 0; column < p; ++column)
            for (std::size_t row = 0; row < n; ++row) p_loading[column] += x_residual[row][column] * t[row] / t_square;
        for (std::size_t column = 0; column < q; ++column)
            for (std::size_t row = 0; row < n; ++row) q_loading[column] += y_residual[row][column] * t[row] / t_square;
        for (std::size_t variable = 0; variable < p; ++variable) {
            weights[variable][component] = w[variable];
            result.x_loadings[variable][component] = p_loading[variable];
        }
        for (std::size_t response = 0; response < q; ++response) result.y_loadings[response][component] = q_loading[response];
        for (std::size_t sample = 0; sample < n; ++sample) {
            result.x_scores[sample][component] = t[sample];
            result.y_scores[sample][component] = u_new[sample];
            for (std::size_t variable = 0; variable < p; ++variable)
                x_residual[sample][variable] -= t[sample] * p_loading[variable];
            for (std::size_t response = 0; response < q; ++response)
                y_residual[sample][response] -= t[sample] * q_loading[response];
        }
        if (component + 1 < components) {
            std::fill(w.begin(), w.end(), 0.0);
            std::fill(t.begin(), t.end(), 0.0);
            std::fill(c.begin(), c.end(), 0.0);
            std::fill(u_new.begin(), u_new.end(), 0.0);
        }
    }
    // Drop uncomputed trailing components after a degenerate residual block.
    std::size_t computed = 0;
    for (std::size_t component = 0; component < components; ++component) {
        double score_norm = 0.0;
        for (const auto &row : result.x_scores) score_norm += row[component] * row[component];
        if (score_norm > 0.0) ++computed;
    }
    if (computed == 0) throw std::invalid_argument("PLS did not produce a component");
    if (computed < components) {
        for (auto &row : result.x_scores) row.resize(computed);
        for (auto &row : result.y_scores) row.resize(computed);
        for (auto &row : result.x_loadings) row.resize(computed);
        for (auto &row : result.y_loadings) row.resize(computed);
        for (auto &row : weights) row.resize(computed);
        components = computed;
    }
    auto ptw = internal::multiply(internal::transpose(result.x_loadings), weights);
    auto projection = internal::multiply(internal::multiply(weights, internal::inverse(std::move(ptw))),
                                         internal::transpose(result.y_loadings));
    const auto predictor_means = internal::column_means(predictors);
    result.coefficients = std::move(projection);
    if (preprocessing == "scale") {
        for (std::size_t variable = 0; variable < p; ++variable) {
            double square_sum = 0.0;
            for (const auto &row : predictors)
                square_sum += (row[variable] - predictor_means[variable]) *
                              (row[variable] - predictor_means[variable]);
            const auto standard_deviation = std::sqrt(square_sum / static_cast<double>(n - 1));
            for (std::size_t response = 0; response < q; ++response)
                result.coefficients[variable][response] /= standard_deviation;
        }
    }
    result.intercept.assign(q, 0.0);
    result.predictions.assign(n, std::vector<double>(q, 0.0));
    for (std::size_t response = 0; response < q; ++response) {
        result.intercept[response] = y_means[response];
        if (preprocessing != "none")
            for (std::size_t variable = 0; variable < p; ++variable)
                result.intercept[response] -= predictor_means[variable] * result.coefficients[variable][response];
        for (std::size_t sample = 0; sample < n; ++sample) {
            result.predictions[sample][response] = result.intercept[response];
            for (std::size_t variable = 0; variable < p; ++variable)
                result.predictions[sample][response] += predictors[sample][variable] * result.coefficients[variable][response];
        }
    }
    return result;
}

std::vector<FoldFeatureResult> compute_fold_change(
    const Matrix &values, const std::vector<std::string> &groups,
    const std::string &group_a, const std::string &group_b, double pseudocount) {
    internal::validate_matrix(values, "fold-change input");
    if (values.size() != groups.size()) throw std::invalid_argument("group labels must match the sample count");
    if (group_a.empty() || group_b.empty() || group_a == group_b)
        throw std::invalid_argument("group A and group B must be distinct non-empty values");
    if (!(pseudocount > 0.0) || !std::isfinite(pseudocount))
        throw std::invalid_argument("pseudocount must be finite and greater than zero");
    std::vector<std::vector<double>> a(values.front().size()), b(values.front().size());
    for (std::size_t sample = 0; sample < values.size(); ++sample) {
        if (groups[sample] == group_a)
            for (std::size_t variable = 0; variable < values[sample].size(); ++variable) a[variable].push_back(values[sample][variable]);
        if (groups[sample] == group_b)
            for (std::size_t variable = 0; variable < values[sample].size(); ++variable) b[variable].push_back(values[sample][variable]);
    }
    for (const auto &column : a)
        if (column.size() < 2) throw std::invalid_argument("group A must contain at least two samples");
    for (const auto &column : b)
        if (column.size() < 2) throw std::invalid_argument("group B must contain at least two samples");
    std::vector<FoldFeatureResult> result(values.front().size());
    std::vector<double> p_values(result.size(), 1.0);
    for (std::size_t variable = 0; variable < result.size(); ++variable) {
        auto &feature = result[variable];
        feature.mean_a = std::accumulate(a[variable].begin(), a[variable].end(), 0.0) / a[variable].size();
        feature.mean_b = std::accumulate(b[variable].begin(), b[variable].end(), 0.0) / b[variable].size();
        feature.p_value = internal::welch_p_value(a[variable], b[variable]);
        p_values[variable] = feature.p_value;
        feature.effect_size = 0.0;
        const auto mean_a = feature.mean_a;
        const auto mean_b = feature.mean_b;
        auto variance = [](const std::vector<double> &column, double mean) {
            double sum = 0.0;
            for (const auto value : column) sum += (value - mean) * (value - mean);
            return sum / static_cast<double>(column.size() - 1);
        };
        const auto degrees = static_cast<double>(a[variable].size() + b[variable].size() - 2);
        const auto pooled_variance = ((a[variable].size() - 1.0) * variance(a[variable], mean_a) +
                                      (b[variable].size() - 1.0) * variance(b[variable], mean_b)) / degrees;
        if (pooled_variance > 0.0) {
            const auto correction = 1.0 - 3.0 / (4.0 * (a[variable].size() + b[variable].size()) - 9.0);
            feature.effect_size = correction * (mean_b - mean_a) / std::sqrt(pooled_variance);
        } else {
            feature.effect_size_defined = false;
        }
    }
    std::vector<std::size_t> order(p_values.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto left, auto right) { return p_values[left] < p_values[right]; });
    double adjusted = 1.0;
    for (std::size_t reverse = order.size(); reverse > 0; --reverse) {
        const auto rank = reverse;
        const auto index = order[reverse - 1];
        adjusted = std::min(adjusted, p_values[index] * static_cast<double>(order.size()) / rank);
        result[index].adjusted_p_value = std::clamp(adjusted, 0.0, 1.0);
    }
    return result;
}

}  // namespace streamfind::statistics::detail
