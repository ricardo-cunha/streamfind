#include "utils/calibration.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace streamfind::mass_spec::quantification::tests
{
void require(const bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void require_near(const double actual, const double expected, const double tolerance,
                  const char *message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}
}

int main()
{
    using streamfind::mass_spec::quantification::calibration::LinearPoint;
    using streamfind::mass_spec::quantification::calibration::LinearFitOptions;
    using streamfind::mass_spec::quantification::calibration::QuadraticFit;
    using streamfind::mass_spec::quantification::calibration::WagnerFit;
    using streamfind::mass_spec::quantification::calibration::fit_linear;
    using streamfind::mass_spec::quantification::calibration::fit_quadratic;
    using streamfind::mass_spec::quantification::calibration::fit_wagner;
    using streamfind::mass_spec::quantification::calibration::inverse_linear;
    using streamfind::mass_spec::quantification::calibration::inverse_quadratic;
    using streamfind::mass_spec::quantification::calibration::inverse_wagner;
    using streamfind::mass_spec::quantification::calibration::predict_wagner;
    using streamfind::mass_spec::quantification::tests::require;
    using streamfind::mass_spec::quantification::tests::require_near;

    const std::vector<LinearPoint> exact{{1.0, 5.0}, {2.0, 8.0}, {3.0, 11.0}, {4.0, 14.0}};
    const auto fit = fit_linear(exact, 3, LinearFitOptions{});
    require(fit.status == "ok", "Exact linear calibration did not fit successfully.");
    require_near(fit.intercept, 2.0, 1.0e-12, "Linear intercept is incorrect.");
    require_near(fit.slope, 3.0, 1.0e-12, "Linear slope is incorrect.");
    require_near(fit.r_squared, 1.0, 1.0e-12, "Linear R squared is incorrect.");
    require_near(fit.rmse, 0.0, 1.0e-12, "Linear RMSE is incorrect.");
    require(fit.n_points == exact.size(), "Linear point count is incorrect.");
    require_near(fit.range_min, 1.0, 1.0e-12, "Linear range minimum is incorrect.");
    require_near(fit.range_max, 4.0, 1.0e-12, "Linear range maximum is incorrect.");

    const auto in_range = inverse_linear(fit, 8.0);
    require(in_range.status == "ok", "In-range inverse was not accepted.");
    require_near(in_range.concentration, 2.0, 1.0e-12, "In-range inverse is incorrect.");

    const auto below = inverse_linear(fit, 3.5);
    require(below.status == "below_calibration_range", "Below-range inverse was not flagged.");
    require_near(below.concentration, 0.5, 1.0e-12, "Below-range inverse concentration is incorrect.");

    const auto above = inverse_linear(fit, 17.0);
    require(above.status == "above_calibration_range", "Above-range inverse was not flagged.");
    require_near(above.concentration, 5.0, 1.0e-12, "Above-range inverse concentration is incorrect.");

    const auto insufficient = fit_linear({{1.0, 5.0}, {2.0, 8.0}}, 3, LinearFitOptions{});
    require(insufficient.status == "insufficient_data", "Insufficient calibration data was accepted.");

    const auto singular = fit_linear({{2.0, 5.0}, {2.0, 8.0}, {2.0, 11.0}}, 3, LinearFitOptions{});
    require(singular.status == "insufficient_data", "Singular calibration data was accepted.");

    const auto negative_slope = fit_linear({{1.0, 11.0}, {2.0, 8.0}, {3.0, 5.0}}, 3, LinearFitOptions{});
    require(negative_slope.status == "failed", "Negative-slope calibration was not rejected.");
    require(inverse_linear(negative_slope, 8.0).status == "invalid_model",
            "Inverse accepted a failed calibration model.");

    require(inverse_linear(fit, 0.0).status == "invalid_response",
            "Zero response was not rejected.");
    require(inverse_linear(fit, -1.0).status == "invalid_response",
            "Negative response was not rejected.");
    require(inverse_linear(fit, std::numeric_limits<double>::quiet_NaN()).status == "invalid_response",
            "Non-finite response was not rejected.");

    const auto nonfinite = fit_linear({{1.0, 5.0}, {2.0, std::numeric_limits<double>::quiet_NaN()},
                                       {3.0, 11.0}}, 3, LinearFitOptions{});
    require(nonfinite.status == "insufficient_data", "Non-finite calibration point was not excluded.");

    const auto weighted = fit_linear(exact, 3, LinearFitOptions{"1/x", "free"});
    require(weighted.status == "ok", "Weighted linear calibration did not fit successfully.");
    require_near(weighted.intercept, 2.0, 1.0e-12, "Weighted linear intercept is incorrect.");
    require_near(weighted.slope, 3.0, 1.0e-12, "Weighted linear slope is incorrect.");

    const auto weighted_squared = fit_linear(exact, 3, LinearFitOptions{"1/x2", "free"});
    require(weighted_squared.status == "ok", "1/x2 calibration did not fit successfully.");
    require_near(weighted_squared.intercept, 2.0, 1.0e-12, "1/x2 intercept is incorrect.");
    require_near(weighted_squared.slope, 3.0, 1.0e-12, "1/x2 slope is incorrect.");

    const auto forced_zero = fit_linear({{1.0, 3.0}, {2.0, 6.0}, {4.0, 12.0}}, 3,
                                        LinearFitOptions{"none", "forced_zero"});
    require(forced_zero.status == "ok", "Forced-zero linear calibration did not fit successfully.");
    require_near(forced_zero.intercept, 0.0, 1.0e-12, "Forced-zero intercept is not zero.");
    require_near(forced_zero.slope, 3.0, 1.0e-12, "Forced-zero slope is incorrect.");

    const auto zero_level = fit_linear({{0.0, 0.0}, {1.0, 2.0}, {2.0, 4.0}}, 2,
                                       LinearFitOptions{"1/x", "free"});
    require(zero_level.status == "ok", "Zero-level weighted calibration did not fit.");
    require(zero_level.n_points == 2, "Zero-level weighted calibration was not excluded from the fit.");
    require_near(zero_level.range_min, 0.0, 1.0e-12, "Zero-level diagnostic range was not retained.");
    require_near(zero_level.slope, 2.0, 1.0e-12, "Zero-level weighted slope is incorrect.");

    require(fit_linear(exact, 3, LinearFitOptions{"bad", "free"}).status == "unsupported_weighting",
            "Unsupported weighting was not rejected explicitly.");
    require(fit_linear(exact, 3, LinearFitOptions{"none", "bad"}).status == "unsupported_intercept_mode",
            "Unsupported intercept mode was not rejected explicitly.");

    const std::vector<LinearPoint> quadratic_points{
        {1.0, 3.5}, {2.0, 7.0}, {3.0, 11.5}, {4.0, 17.0}};
    const auto quadratic = fit_quadratic(quadratic_points, 3, LinearFitOptions{});
    require(quadratic.status == "ok", "Exact quadratic calibration did not fit successfully.");
    require(quadratic.monotonicity_status == "increasing", "Increasing quadratic was not accepted.");
    require_near(quadratic.coefficient_0, 1.0, 1.0e-10, "Quadratic coefficient 0 is incorrect.");
    require_near(quadratic.coefficient_1, 2.0, 1.0e-10, "Quadratic coefficient 1 is incorrect.");
    require_near(quadratic.coefficient_2, 0.5, 1.0e-10, "Quadratic coefficient 2 is incorrect.");
    const auto quadratic_inverse = inverse_quadratic(quadratic, 11.5);
    require(quadratic_inverse.status == "ok", "Quadratic inverse was not accepted.");
    require_near(quadratic_inverse.concentration, 3.0, 1.0e-10, "Quadratic inverse is incorrect.");

    const auto forced_zero_quadratic = fit_quadratic(
        {{1.0, 3.5}, {2.0, 8.0}, {3.0, 13.5}, {4.0, 20.0}}, 3,
        LinearFitOptions{"none", "forced_zero"});
    require(forced_zero_quadratic.status == "ok", "Forced-zero quadratic did not fit successfully.");
    require_near(forced_zero_quadratic.coefficient_0, 0.0, 1.0e-12,
                 "Forced-zero quadratic coefficient 0 is not zero.");
    require_near(forced_zero_quadratic.coefficient_1, 3.0, 1.0e-10,
                 "Forced-zero quadratic coefficient 1 is incorrect.");
    require_near(forced_zero_quadratic.coefficient_2, 0.5, 1.0e-10,
                 "Forced-zero quadratic coefficient 2 is incorrect.");

    QuadraticFit near_linear;
    near_linear.coefficient_0 = 1.0;
    near_linear.coefficient_1 = 2.0;
    near_linear.coefficient_2 = 1.0e-14;
    near_linear.range_min = 1.0;
    near_linear.range_max = 10.0;
    near_linear.status = "ok";
    const auto near_linear_inverse = inverse_quadratic(near_linear, 11.0);
    require(near_linear_inverse.status == "ok", "Near-linear quadratic inverse was not accepted.");
    require_near(near_linear_inverse.concentration, 5.0, 1.0e-10,
                 "Near-linear quadratic was not reduced safely.");

    QuadraticFit two_root;
    two_root.coefficient_0 = 0.0;
    two_root.coefficient_1 = 5.0;
    two_root.coefficient_2 = -1.0;
    two_root.range_min = 0.0;
    two_root.range_max = 6.0;
    two_root.status = "ok";
    const auto ambiguous = inverse_quadratic(two_root, 6.0);
    require(ambiguous.status == "ambiguous_inverse", "Two valid quadratic roots were not rejected.");

    const auto non_monotonic = fit_quadratic(
        {{1.0, 4.0}, {2.0, 5.0}, {3.0, 4.0}, {4.0, 1.0}}, 3, LinearFitOptions{});
    require(non_monotonic.status == "non_monotonic", "Non-monotonic quadratic was accepted.");
    require(non_monotonic.monotonicity_status == "non_monotonic",
            "Non-monotonic quadratic status was not reported.");

    const auto quadratic_above = inverse_quadratic(quadratic, 23.5);
    require(quadratic_above.status == "above_calibration_range",
            "Out-of-range quadratic inverse was not flagged.");
    require_near(quadratic_above.concentration, 5.0, 1.0e-12,
                 "Out-of-range quadratic inverse did not retain its diagnostic concentration.");

    const auto wagner_points = std::vector<LinearPoint>{
        {1.0, std::exp(0.4)}, {2.0, std::exp(0.4 + 0.8 * std::log(2.0) + 0.1 * std::log(2.0) * std::log(2.0))},
        {4.0, std::exp(0.4 + 0.8 * std::log(4.0) + 0.1 * std::log(4.0) * std::log(4.0))},
        {8.0, std::exp(0.4 + 0.8 * std::log(8.0) + 0.1 * std::log(8.0) * std::log(8.0))}};
    const auto wagner = fit_wagner(wagner_points, 3, LinearFitOptions{});
    require(wagner.status == "ok", "Exact Wagner calibration did not fit successfully.");
    require(wagner.plugin_status == "positive_plugin", "Wagner positive-plugin status is incorrect.");
    require(wagner.monotonicity_status == "increasing", "Increasing Wagner fit was not accepted.");
    require_near(wagner.coefficient_0, 0.4, 1.0e-10, "Wagner coefficient 0 is incorrect.");
    require_near(wagner.coefficient_1, 0.8, 1.0e-10, "Wagner coefficient 1 is incorrect.");
    require_near(wagner.coefficient_2, 0.1, 1.0e-10, "Wagner coefficient 2 is incorrect.");
    require_near(predict_wagner(wagner, 4.0), wagner_points[2].response, 1.0e-10,
                 "Wagner prediction is incorrect.");
    const auto wagner_inverse = inverse_wagner(wagner, wagner_points[2].response);
    require(wagner_inverse.status == "ok", "Wagner inverse was not accepted.");
    require_near(wagner_inverse.concentration, 4.0, 1.0e-10, "Wagner inverse is incorrect.");
    const auto wagner_above = inverse_wagner(wagner, predict_wagner(wagner, 16.0));
    require(wagner_above.status == "above_calibration_range",
            "Out-of-range Wagner inverse was not flagged.");
    require_near(wagner_above.concentration, 16.0, 1.0e-10,
                 "Out-of-range Wagner inverse did not retain its diagnostic concentration.");

    WagnerFit mix1_wagner;
    mix1_wagner.coefficient_0 = -0.9302477420165582;
    mix1_wagner.coefficient_1 = 0.9988257650668938;
    mix1_wagner.coefficient_2 = 0.010233759225041947;
    mix1_wagner.range_min = 0.009;
    mix1_wagner.range_max = 22.5;
    mix1_wagner.monotonicity_status = "increasing";
    mix1_wagner.status = "ok";
    const auto mix1_wagner_above = inverse_wagner(mix1_wagner, 95.92976803983291);
    require(mix1_wagner_above.status == "above_calibration_range",
            "Mix1 Wagner out-of-range response was not flagged.");
    require(mix1_wagner_above.concentration > mix1_wagner.range_max,
            "Mix1 Wagner out-of-range response did not retain an extrapolated concentration.");

    const auto forced_zero_wagner = fit_wagner(
        {{1.0, 1.0}, {2.0, std::exp(0.5 * std::log(2.0) + 0.1 * std::log(2.0) * std::log(2.0))},
         {4.0, std::exp(0.5 * std::log(4.0) + 0.1 * std::log(4.0) * std::log(4.0))},
         {8.0, std::exp(0.5 * std::log(8.0) + 0.1 * std::log(8.0) * std::log(8.0))}},
        3, LinearFitOptions{"none", "forced_zero"});
    require(forced_zero_wagner.status == "ok", "Forced-zero Wagner calibration did not fit successfully.");
    require_near(forced_zero_wagner.coefficient_0, 0.0, 1.0e-12,
                 "Forced-zero Wagner coefficient 0 is not zero.");
    require_near(forced_zero_wagner.coefficient_1, 0.5, 1.0e-10,
                 "Forced-zero Wagner coefficient 1 is incorrect.");
    require_near(forced_zero_wagner.coefficient_2, 0.1, 1.0e-10,
                 "Forced-zero Wagner coefficient 2 is incorrect.");
    require(fit_wagner(wagner_points, 3, LinearFitOptions{"1/x", "free"}).status == "unsupported_weighting",
            "Wagner accepted unsupported inverse-concentration weighting.");

    WagnerFit two_wagner_roots;
    two_wagner_roots.coefficient_0 = 0.0;
    two_wagner_roots.coefficient_1 = 1.0;
    two_wagner_roots.coefficient_2 = -0.1;
    two_wagner_roots.range_min = 1.0;
    two_wagner_roots.range_max = 5000.0;
    two_wagner_roots.status = "ok";
    require(inverse_wagner(two_wagner_roots, std::exp(2.0)).status == "ambiguous_inverse",
            "Two valid Wagner inverse roots were not rejected.");
    require(inverse_wagner(two_wagner_roots, std::exp(3.0)).status == "no_real_inverse",
            "Wagner no-real-root response was not rejected.");
    const auto invalid_wagner = fit_wagner({{0.0, 1.0}, {1.0, 2.0}, {2.0, 4.0}}, 3, LinearFitOptions{});
    require(invalid_wagner.status == "insufficient_data", "Wagner accepted a non-positive concentration.");
    require(invalid_wagner.plugin_status == "excluded_nonpositive",
            "Wagner invalid-plugin exclusion was not reported.");

    std::cout << "mass-spec calibration contract passed\n";
    return 0;
}
