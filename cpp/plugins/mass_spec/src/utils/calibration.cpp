#include "utils/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace streamfind::mass_spec::quantification::calibration
{
namespace detail
{
constexpr double kEpsilon = 1.0e-15;

bool finite_point(const LinearPoint &point)
{
    return std::isfinite(point.concentration) && std::isfinite(point.response);
}

struct WeightedPoint
{
    LinearPoint point;
    double weight;
};

struct LeastSquaresResult
{
    std::vector<double> coefficients;
    bool solved = false;
};

bool point_weight(const LinearPoint &point, const std::string &weighting, double &weight)
{
    if (weighting == "none")
    {
        weight = 1.0;
    }
    else if (weighting == "1/x")
    {
        if (point.concentration <= kEpsilon) return false;
        weight = 1.0 / point.concentration;
    }
    else if (weighting == "1/x2")
    {
        if (point.concentration <= kEpsilon) return false;
        weight = 1.0 / (point.concentration * point.concentration);
    }
    else
    {
        return false;
    }
    return std::isfinite(weight) && weight > 0.0;
}

std::string unsupported_option(const LinearFitOptions &options)
{
    if (options.weighting != "none" && options.weighting != "1/x" && options.weighting != "1/x2")
        return "unsupported_weighting";
    if (options.intercept_mode != "free" && options.intercept_mode != "forced_zero")
        return "unsupported_intercept_mode";
    return {};
}

LeastSquaresResult solve_weighted(const std::vector<WeightedPoint> &points,
                                  const bool forced_zero, const int degree)
{
    const auto column_count = forced_zero ? degree : degree + 1;
    const auto row_count = points.size();
    std::vector<std::vector<double>> columns(
        static_cast<std::size_t>(column_count), std::vector<double>(row_count, 0.0));
    std::vector<double> weighted_response(row_count, 0.0);
    for (std::size_t row = 0; row < row_count; ++row)
    {
        const auto x = points[row].point.concentration;
        const auto sqrt_weight = std::sqrt(points[row].weight);
        weighted_response[row] = sqrt_weight * points[row].point.response;
        for (int column = 0; column < column_count; ++column)
        {
            const auto power = forced_zero ? column + 1 : column;
            columns[static_cast<std::size_t>(column)][row] =
                sqrt_weight * std::pow(x, power);
        }
    }

    // Modified Gram-Schmidt with column scaling and re-orthogonalization
    // avoids the squared condition number of a normal-equation solve.
    std::vector<std::vector<double>> q(
        static_cast<std::size_t>(column_count), std::vector<double>(row_count, 0.0));
    std::vector<std::vector<double>> r(
        static_cast<std::size_t>(column_count), std::vector<double>(column_count, 0.0));
    std::vector<double> scale(static_cast<std::size_t>(column_count), 0.0);
    for (int column = 0; column < column_count; ++column)
    {
        auto &values = columns[static_cast<std::size_t>(column)];
        double norm = 0.0;
        for (const auto value : values) norm += value * value;
        norm = std::sqrt(norm);
        if (!std::isfinite(norm) || norm <= kEpsilon) return {};
        scale[static_cast<std::size_t>(column)] = norm;
        for (auto &value : values) value /= norm;
    }

    for (int column = 0; column < column_count; ++column)
    {
        auto values = columns[static_cast<std::size_t>(column)];
        for (int previous = 0; previous < column; ++previous)
        {
            auto projection = 0.0;
            for (std::size_t row = 0; row < row_count; ++row)
                projection += q[static_cast<std::size_t>(previous)][row] * values[row];
            r[static_cast<std::size_t>(previous)][static_cast<std::size_t>(column)] += projection;
            for (std::size_t row = 0; row < row_count; ++row)
                values[row] -= projection * q[static_cast<std::size_t>(previous)][row];
        }
        // A second pass makes the rank decision less sensitive to nearly
        // collinear concentration levels.
        for (int previous = 0; previous < column; ++previous)
        {
            auto correction = 0.0;
            for (std::size_t row = 0; row < row_count; ++row)
                correction += q[static_cast<std::size_t>(previous)][row] * values[row];
            r[static_cast<std::size_t>(previous)][static_cast<std::size_t>(column)] += correction;
            for (std::size_t row = 0; row < row_count; ++row)
                values[row] -= correction * q[static_cast<std::size_t>(previous)][row];
        }
        auto norm = 0.0;
        for (const auto value : values) norm += value * value;
        norm = std::sqrt(norm);
        if (!std::isfinite(norm) || norm <= kEpsilon) return {};
        r[static_cast<std::size_t>(column)][static_cast<std::size_t>(column)] = norm;
        for (std::size_t row = 0; row < row_count; ++row)
            q[static_cast<std::size_t>(column)][row] = values[row] / norm;
    }

    std::vector<double> scaled_coefficients(static_cast<std::size_t>(column_count), 0.0);
    for (int column = 0; column < column_count; ++column)
        for (std::size_t row = 0; row < row_count; ++row)
            scaled_coefficients[static_cast<std::size_t>(column)] +=
                q[static_cast<std::size_t>(column)][row] * weighted_response[row];
    for (int row = column_count - 1; row >= 0; --row)
    {
        auto value = scaled_coefficients[static_cast<std::size_t>(row)];
        for (int column = row + 1; column < column_count; ++column)
            value -= r[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] *
                     scaled_coefficients[static_cast<std::size_t>(column)];
        const auto diagonal = r[static_cast<std::size_t>(row)][static_cast<std::size_t>(row)];
        if (!std::isfinite(diagonal) || std::abs(diagonal) <= kEpsilon) return {};
        scaled_coefficients[static_cast<std::size_t>(row)] = value / diagonal;
    }

    LeastSquaresResult result;
    result.coefficients.resize(static_cast<std::size_t>(column_count), 0.0);
    for (int column = 0; column < column_count; ++column)
        result.coefficients[static_cast<std::size_t>(column)] =
            scaled_coefficients[static_cast<std::size_t>(column)] /
            scale[static_cast<std::size_t>(column)];
    result.solved = std::all_of(result.coefficients.begin(), result.coefficients.end(),
                                 [](const auto value) { return std::isfinite(value); });
    return result;
}

void set_range(const std::vector<LinearPoint> &points, double &minimum, double &maximum)
{
    minimum = points.front().concentration;
    maximum = points.front().concentration;
    for (const auto &point : points)
    {
        minimum = std::min(minimum, point.concentration);
        maximum = std::max(maximum, point.concentration);
    }
}

double response_tolerance(const double left, const double right)
{
    return 1.0e-10 * std::max({1.0, std::abs(left), std::abs(right)});
}
}

LinearFit fit_linear(const std::vector<LinearPoint> &points, const int minimum_points,
                     const LinearFitOptions &options)
{
    LinearFit result;
    if (const auto error = detail::unsupported_option(options); !error.empty())
    {
        result.status = error;
        return result;
    }

    std::vector<LinearPoint> finite_points;
    std::vector<detail::WeightedPoint> valid;
    finite_points.reserve(points.size());
    for (const auto &point : points)
    {
        if (!detail::finite_point(point)) continue;
        finite_points.push_back(point);
        double weight = 0.0;
        if (detail::point_weight(point, options.weighting, weight))
            valid.push_back({point, weight});
    }

    const auto required = static_cast<std::size_t>(std::max(minimum_points, 2));
    if (valid.size() < required) return result;

    double sum_w = 0.0;
    double sum_wx = 0.0;
    double sum_wy = 0.0;
    double sum_wxx = 0.0;
    double sum_wxy = 0.0;
    for (const auto &weighted : valid)
    {
        const auto x = weighted.point.concentration;
        const auto y = weighted.point.response;
        const auto w = weighted.weight;
        sum_w += w;
        sum_wx += w * x;
        sum_wy += w * y;
        sum_wxx += w * x * x;
        sum_wxy += w * x * y;
    }

    if (!std::isfinite(sum_w) || sum_w <= detail::kEpsilon)
        return result;
    if (options.intercept_mode == "forced_zero")
    {
        if (!std::isfinite(sum_wxx) || std::abs(sum_wxx) <= detail::kEpsilon)
            return result;
        result.intercept = 0.0;
        result.slope = sum_wxy / sum_wxx;
    }
    else
    {
        const auto denominator = sum_w * sum_wxx - sum_wx * sum_wx;
        if (!std::isfinite(denominator) || std::abs(denominator) <= detail::kEpsilon)
            return result;
        result.slope = (sum_w * sum_wxy - sum_wx * sum_wy) / denominator;
        result.intercept = (sum_wy - result.slope * sum_wx) / sum_w;
    }
    if (!std::isfinite(result.slope) || !std::isfinite(result.intercept) ||
        std::abs(result.slope) <= detail::kEpsilon)
        return result;

    const auto mean_y = sum_wy / sum_w;
    double ss_res = 0.0;
    double ss_tot = 0.0;
    for (const auto &point : finite_points)
    {
        const auto predicted = result.intercept + result.slope * point.concentration;
        double weight = 0.0;
        if (!detail::point_weight(point, options.weighting, weight)) continue;
        ss_res += weight * (point.response - predicted) * (point.response - predicted);
        ss_tot += weight * (point.response - mean_y) * (point.response - mean_y);
    }

    detail::set_range(finite_points, result.range_min, result.range_max);
    result.n_points = valid.size();
    result.r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    result.rmse = std::sqrt(ss_res / sum_w);
    result.adjusted_r_squared = valid.size() > 2
        ? 1.0 - (1.0 - result.r_squared) * (static_cast<double>(valid.size()) - 1.0) /
              (static_cast<double>(valid.size()) - 2.0)
        : 0.0;
    result.monotonicity_status = result.slope > 0.0 ? "increasing" : "non_monotonic";
    result.status = result.slope > 0.0 ? "ok" : "failed";
    return result;
}

double predict_linear(const LinearFit &fit, const double concentration)
{
    return fit.intercept + fit.slope * concentration;
}

QuadraticFit fit_quadratic(const std::vector<LinearPoint> &points, const int minimum_points,
                           const LinearFitOptions &options)
{
    QuadraticFit result;
    if (const auto error = detail::unsupported_option(options); !error.empty())
    {
        result.status = error;
        return result;
    }

    std::vector<LinearPoint> finite_points;
    std::vector<detail::WeightedPoint> valid;
    finite_points.reserve(points.size());
    for (const auto &point : points)
    {
        if (!detail::finite_point(point)) continue;
        finite_points.push_back(point);
        double weight = 0.0;
        if (detail::point_weight(point, options.weighting, weight))
            valid.push_back({point, weight});
    }

    const auto required = static_cast<std::size_t>(std::max(
        minimum_points, options.intercept_mode == "forced_zero" ? 2 : 3));
    if (valid.size() < required) return result;
    const auto solved = detail::solve_weighted(valid, options.intercept_mode == "forced_zero", 2);
    if (!solved.solved)
    {
        result.status = "singular_model";
        return result;
    }
    if (options.intercept_mode == "forced_zero")
    {
        result.coefficient_0 = 0.0;
        result.coefficient_1 = solved.coefficients[0];
        result.coefficient_2 = solved.coefficients[1];
    }
    else
    {
        result.coefficient_0 = solved.coefficients[0];
        result.coefficient_1 = solved.coefficients[1];
        result.coefficient_2 = solved.coefficients[2];
    }

    double sum_weight = 0.0;
    double weighted_mean = 0.0;
    for (const auto &weighted : valid)
    {
        sum_weight += weighted.weight;
        weighted_mean += weighted.weight * weighted.point.response;
    }
    weighted_mean /= sum_weight;
    double ss_res = 0.0;
    double ss_tot = 0.0;
    for (const auto &weighted : valid)
    {
        const auto residual = weighted.point.response -
            (result.coefficient_0 + result.coefficient_1 * weighted.point.concentration +
             result.coefficient_2 * weighted.point.concentration * weighted.point.concentration);
        ss_res += weighted.weight * residual * residual;
        const auto centered = weighted.point.response - weighted_mean;
        ss_tot += weighted.weight * centered * centered;
    }

    detail::set_range(finite_points, result.range_min, result.range_max);
    result.n_points = valid.size();
    result.r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    result.rmse = std::sqrt(ss_res / sum_weight);
    const auto parameter_count = options.intercept_mode == "forced_zero" ? 2.0 : 3.0;
    result.adjusted_r_squared = valid.size() > static_cast<std::size_t>(parameter_count)
        ? 1.0 - (1.0 - result.r_squared) * (static_cast<double>(valid.size()) - 1.0) /
              (static_cast<double>(valid.size()) - parameter_count)
        : 0.0;

    const auto derivative_min = result.coefficient_1 + 2.0 * result.coefficient_2 * result.range_min;
    const auto derivative_max = result.coefficient_1 + 2.0 * result.coefficient_2 * result.range_max;
    if (std::isfinite(derivative_min) && std::isfinite(derivative_max) &&
        derivative_min > detail::kEpsilon && derivative_max > detail::kEpsilon)
    {
        result.monotonicity_status = "increasing";
        result.status = "ok";
    }
    else
    {
        result.monotonicity_status = "non_monotonic";
        result.status = "non_monotonic";
    }
    return result;
}

double predict_quadratic(const QuadraticFit &fit, const double concentration)
{
    return fit.coefficient_0 + fit.coefficient_1 * concentration +
           fit.coefficient_2 * concentration * concentration;
}

WagnerFit fit_wagner(const std::vector<LinearPoint> &points, const int minimum_points,
                     const LinearFitOptions &options)
{
    WagnerFit result;
    if (options.weighting != "none")
    {
        result.status = "unsupported_weighting";
        return result;
    }
    if (options.intercept_mode != "free" && options.intercept_mode != "forced_zero")
    {
        result.status = "unsupported_intercept_mode";
        return result;
    }

    std::vector<LinearPoint> valid_points;
    std::vector<detail::WeightedPoint> transformed;
    valid_points.reserve(points.size());
    transformed.reserve(points.size());
    for (const auto &point : points)
    {
        if (!detail::finite_point(point)) continue;
        if (point.concentration <= 0.0 || point.response <= 0.0)
        {
            result.plugin_status = "excluded_nonpositive";
            continue;
        }
        const auto log_concentration = std::log(point.concentration);
        const auto log_response = std::log(point.response);
        if (!std::isfinite(log_concentration) || !std::isfinite(log_response))
        {
            result.plugin_status = "excluded_nonfinite_log";
            continue;
        }
        valid_points.push_back(point);
        transformed.push_back({{log_concentration, log_response}, 1.0});
    }

    const auto required = static_cast<std::size_t>(std::max(
        minimum_points, options.intercept_mode == "forced_zero" ? 2 : 3));
    if (transformed.size() < required)
    {
        if (result.plugin_status == "unknown") result.plugin_status = "insufficient_positive_points";
        return result;
    }
    const auto solved = detail::solve_weighted(transformed, options.intercept_mode == "forced_zero", 2);
    if (!solved.solved)
    {
        result.status = "singular_model";
        result.plugin_status = "positive_plugin";
        return result;
    }

    if (options.intercept_mode == "forced_zero")
    {
        result.coefficient_0 = 0.0;
        result.coefficient_1 = solved.coefficients[0];
        result.coefficient_2 = solved.coefficients[1];
    }
    else
    {
        result.coefficient_0 = solved.coefficients[0];
        result.coefficient_1 = solved.coefficients[1];
        result.coefficient_2 = solved.coefficients[2];
    }

    double sum_weight = 0.0;
    double weighted_mean = 0.0;
    for (const auto &weighted : transformed)
    {
        sum_weight += weighted.weight;
        weighted_mean += weighted.weight * weighted.point.response;
    }
    weighted_mean /= sum_weight;
    double ss_res = 0.0;
    double ss_tot = 0.0;
    for (const auto &weighted : transformed)
    {
        const auto predicted = result.coefficient_0 + result.coefficient_1 * weighted.point.concentration +
                               result.coefficient_2 * weighted.point.concentration * weighted.point.concentration;
        const auto residual = weighted.point.response - predicted;
        ss_res += weighted.weight * residual * residual;
        const auto centered = weighted.point.response - weighted_mean;
        ss_tot += weighted.weight * centered * centered;
    }

    detail::set_range(valid_points, result.range_min, result.range_max);
    result.n_points = transformed.size();
    result.r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    result.rmse = std::sqrt(ss_res / sum_weight);
    const auto parameter_count = options.intercept_mode == "forced_zero" ? 2.0 : 3.0;
    result.adjusted_r_squared = transformed.size() > static_cast<std::size_t>(parameter_count)
        ? 1.0 - (1.0 - result.r_squared) * (static_cast<double>(transformed.size()) - 1.0) /
              (static_cast<double>(transformed.size()) - parameter_count)
        : 0.0;
    const auto log_min = std::log(result.range_min);
    const auto log_max = std::log(result.range_max);
    const auto derivative_min = result.coefficient_1 + 2.0 * result.coefficient_2 * log_min;
    const auto derivative_max = result.coefficient_1 + 2.0 * result.coefficient_2 * log_max;
    result.plugin_status = "positive_plugin";
    if (std::isfinite(derivative_min) && std::isfinite(derivative_max) &&
        derivative_min > detail::kEpsilon && derivative_max > detail::kEpsilon)
    {
        result.monotonicity_status = "increasing";
        result.status = "ok";
    }
    else
    {
        result.monotonicity_status = "non_monotonic";
        result.status = "non_monotonic";
    }
    return result;
}

double predict_wagner(const WagnerFit &fit, const double concentration)
{
    if (!std::isfinite(concentration) || concentration <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    const auto log_concentration = std::log(concentration);
    const auto log_response = fit.coefficient_0 + fit.coefficient_1 * log_concentration +
                              fit.coefficient_2 * log_concentration * log_concentration;
    if (!std::isfinite(log_response)) return std::numeric_limits<double>::quiet_NaN();
    const auto response = std::exp(log_response);
    return std::isfinite(response) ? response : std::numeric_limits<double>::quiet_NaN();
}

InverseResult inverse_linear(const LinearFit &fit, const double response)
{
    InverseResult result;
    if (fit.status != "ok" || !std::isfinite(fit.slope) ||
        std::abs(fit.slope) <= detail::kEpsilon)
        return result;
    if (!std::isfinite(response) || response <= 0.0)
    {
        result.status = "invalid_response";
        return result;
    }

    result.concentration = (response - fit.intercept) / fit.slope;
    if (!std::isfinite(result.concentration))
    {
        result.status = "invalid_response";
        return result;
    }
    if (result.concentration < fit.range_min)
        result.status = "below_calibration_range";
    else if (result.concentration > fit.range_max)
        result.status = "above_calibration_range";
    else
        result.status = "ok";
    return result;
}

InverseResult inverse_quadratic(const QuadraticFit &fit, const double response)
{
    InverseResult result;
    if (fit.status != "ok" || !std::isfinite(fit.coefficient_0) ||
        !std::isfinite(fit.coefficient_1) || !std::isfinite(fit.coefficient_2) ||
        !std::isfinite(fit.range_min) || !std::isfinite(fit.range_max))
        return result;
    if (!std::isfinite(response) || response <= 0.0)
    {
        result.status = "invalid_response";
        return result;
    }

    const auto linear_tolerance = 1.0e-12 * std::max({1.0, std::abs(fit.coefficient_0),
                                                       std::abs(fit.coefficient_1)});
    if (std::abs(fit.coefficient_2) <= linear_tolerance)
    {
        LinearFit linear;
        linear.intercept = fit.coefficient_0;
        linear.slope = fit.coefficient_1;
        linear.range_min = fit.range_min;
        linear.range_max = fit.range_max;
        linear.status = "ok";
        return inverse_linear(linear, response);
    }

    const auto a = fit.coefficient_2;
    const auto b = fit.coefficient_1;
    const auto c = fit.coefficient_0 - response;
    const auto discriminant = b * b - 4.0 * a * c;
    const auto discriminant_tolerance = 1.0e-12 * std::max({1.0, std::abs(b * b), std::abs(4.0 * a * c)});
    if (!std::isfinite(discriminant) || discriminant < -discriminant_tolerance)
    {
        result.status = "no_real_inverse";
        return result;
    }

    std::vector<double> roots;
    if (std::abs(discriminant) <= discriminant_tolerance)
    {
        roots.push_back(-b / (2.0 * a));
    }
    else
    {
        const auto square_root = std::sqrt(discriminant);
        const auto q = -0.5 * (b + std::copysign(square_root, b));
        if (std::abs(q) > detail::kEpsilon)
        {
            roots.push_back(q / a);
            roots.push_back(c / q);
        }
        else
        {
            roots.push_back((-b + square_root) / (2.0 * a));
            roots.push_back((-b - square_root) / (2.0 * a));
        }
    }

    const auto range_tolerance = 1.0e-10 * std::max({1.0, std::abs(fit.range_min), std::abs(fit.range_max)});
    std::vector<double> candidates;
    for (const auto root : roots)
        if (std::isfinite(root) && root >= fit.range_min - range_tolerance &&
            root <= fit.range_max + range_tolerance)
        {
            const auto clamped = std::clamp(root, fit.range_min, fit.range_max);
            if (std::none_of(candidates.begin(), candidates.end(), [&](const auto existing) {
                    return std::abs(existing - clamped) <= range_tolerance;
                }))
                candidates.push_back(clamped);
        }
    if (candidates.size() > 1)
    {
        result.status = "ambiguous_inverse";
        return result;
    }
    if (candidates.size() == 1)
    {
        result.concentration = candidates.front();
        result.status = "ok";
        return result;
    }

    const auto minimum_response = predict_quadratic(fit, fit.range_min);
    const auto maximum_response = predict_quadratic(fit, fit.range_max);
    const auto below = response < minimum_response - detail::response_tolerance(response, minimum_response);
    const auto above = response > maximum_response + detail::response_tolerance(response, maximum_response);
    if (fit.monotonicity_status == "increasing" && (below || above))
    {
        std::vector<double> extrapolated;
        for (const auto root : roots)
            if (std::isfinite(root) && root >= 0.0 &&
                ((below && root < fit.range_min) || (above && root > fit.range_max)))
                extrapolated.push_back(root);
        if (!extrapolated.empty())
        {
            const auto boundary = below ? fit.range_min : fit.range_max;
            const auto nearest = std::min_element(extrapolated.begin(), extrapolated.end(), [&](const auto left, const auto right) {
                return std::abs(left - boundary) < std::abs(right - boundary);
            });
            result.concentration = *nearest;
            result.status = below ? "below_calibration_range" : "above_calibration_range";
            return result;
        }
    }
    result.status = below ? "below_calibration_range" : above ? "above_calibration_range" : "out_of_range";
    return result;
}

InverseResult inverse_wagner(const WagnerFit &fit, const double response)
{
    InverseResult result;
    if (fit.status != "ok" || !std::isfinite(fit.coefficient_0) ||
        !std::isfinite(fit.coefficient_1) || !std::isfinite(fit.coefficient_2) ||
        !std::isfinite(fit.range_min) || !std::isfinite(fit.range_max) ||
        fit.range_min <= 0.0 || fit.range_max <= 0.0)
        return result;
    if (!std::isfinite(response) || response <= 0.0)
    {
        result.status = "invalid_response";
        return result;
    }

    const auto log_response = std::log(response);
    const auto linear_tolerance = 1.0e-12 * std::max({1.0, std::abs(fit.coefficient_0),
                                                       std::abs(fit.coefficient_1)});
    std::vector<double> log_roots;
    if (std::abs(fit.coefficient_2) <= linear_tolerance)
    {
        if (std::abs(fit.coefficient_1) <= detail::kEpsilon)
        {
            result.status = "no_real_inverse";
            return result;
        }
        log_roots.push_back((log_response - fit.coefficient_0) / fit.coefficient_1);
    }
    else
    {
        const auto a = fit.coefficient_2;
        const auto b = fit.coefficient_1;
        const auto c = fit.coefficient_0 - log_response;
        const auto discriminant = b * b - 4.0 * a * c;
        const auto discriminant_tolerance =
            1.0e-12 * std::max({1.0, std::abs(b * b), std::abs(4.0 * a * c)});
        if (!std::isfinite(discriminant) || discriminant < -discriminant_tolerance)
        {
            result.status = "no_real_inverse";
            return result;
        }
        if (std::abs(discriminant) <= discriminant_tolerance)
        {
            log_roots.push_back(-b / (2.0 * a));
        }
        else
        {
            const auto square_root = std::sqrt(discriminant);
            const auto q = -0.5 * (b + std::copysign(square_root, b));
            if (std::abs(q) > detail::kEpsilon)
            {
                log_roots.push_back(q / a);
                log_roots.push_back(c / q);
            }
            else
            {
                log_roots.push_back((-b + square_root) / (2.0 * a));
                log_roots.push_back((-b - square_root) / (2.0 * a));
            }
        }
    }

    const auto log_min = std::log(fit.range_min);
    const auto log_max = std::log(fit.range_max);
    const auto range_tolerance = 1.0e-10 * std::max({1.0, std::abs(log_min), std::abs(log_max)});
    std::vector<double> candidates;
    for (const auto log_root : log_roots)
        if (std::isfinite(log_root) && log_root >= log_min - range_tolerance &&
            log_root <= log_max + range_tolerance)
        {
            const auto clamped = std::clamp(log_root, log_min, log_max);
            if (std::none_of(candidates.begin(), candidates.end(), [&](const auto existing) {
                    return std::abs(std::log(existing) - clamped) <= range_tolerance;
                }))
                candidates.push_back(std::exp(clamped));
        }
    if (candidates.size() > 1)
    {
        result.status = "ambiguous_inverse";
        return result;
    }
    if (candidates.size() == 1)
    {
        result.concentration = candidates.front();
        result.status = "ok";
        return result;
    }

    const auto minimum_log_response = fit.coefficient_0 + fit.coefficient_1 * log_min +
                                      fit.coefficient_2 * log_min * log_min;
    const auto maximum_log_response = fit.coefficient_0 + fit.coefficient_1 * log_max +
                                      fit.coefficient_2 * log_max * log_max;
    const auto below =
        log_response < minimum_log_response - detail::response_tolerance(log_response, minimum_log_response);
    const auto above =
        log_response > maximum_log_response + detail::response_tolerance(log_response, maximum_log_response);
    if (fit.monotonicity_status == "increasing" && (below || above))
    {
        std::vector<double> extrapolated;
        for (const auto log_root : log_roots)
            if (std::isfinite(log_root) &&
                ((below && log_root < log_min) || (above && log_root > log_max)))
            {
                const auto root = std::exp(log_root);
                if (std::isfinite(root) && root > 0.0) extrapolated.push_back(root);
            }
        if (!extrapolated.empty())
        {
            const auto boundary = below ? log_min : log_max;
            const auto nearest = std::min_element(log_roots.begin(), log_roots.end(), [&](const auto left, const auto right) {
                const auto left_on_side = (below && left < log_min) || (above && left > log_max);
                const auto right_on_side = (below && right < log_min) || (above && right > log_max);
                if (left_on_side != right_on_side) return left_on_side;
                return std::abs(left - boundary) < std::abs(right - boundary);
            });
            result.concentration = std::exp(*nearest);
            result.status = below ? "below_calibration_range" : "above_calibration_range";
            return result;
        }
    }
    result.status = below ? "below_calibration_range" : above ? "above_calibration_range" : "out_of_range";
    return result;
}
}
