#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace streamfind::mass_spec::quantification::calibration
{
struct LinearPoint
{
    double concentration;
    double response;
};

struct LinearFitOptions
{
    std::string weighting = "none";
    std::string intercept_mode = "free";
};

struct LinearFit
{
    double intercept = 0.0;
    double slope = 0.0;
    double r_squared = 0.0;
    double adjusted_r_squared = 0.0;
    double rmse = 0.0;
    std::size_t n_points = 0;
    double range_min = 0.0;
    double range_max = 0.0;
    std::string monotonicity_status = "unknown";
    std::string status = "insufficient_data";
};

struct QuadraticFit
{
    double coefficient_0 = 0.0;
    double coefficient_1 = 0.0;
    double coefficient_2 = 0.0;
    double r_squared = 0.0;
    double adjusted_r_squared = 0.0;
    double rmse = 0.0;
    std::size_t n_points = 0;
    double range_min = 0.0;
    double range_max = 0.0;
    std::string monotonicity_status = "unknown";
    std::string status = "insufficient_data";
};

struct WagnerFit
{
    double coefficient_0 = 0.0;
    double coefficient_1 = 0.0;
    double coefficient_2 = 0.0;
    double r_squared = 0.0;
    double adjusted_r_squared = 0.0;
    double rmse = 0.0;
    std::size_t n_points = 0;
    double range_min = 0.0;
    double range_max = 0.0;
    std::string monotonicity_status = "unknown";
    std::string domain_status = "unknown";
    std::string status = "insufficient_data";
};

struct InverseResult
{
    double concentration = 0.0;
    std::string status = "invalid_model";
};

LinearFit fit_linear(const std::vector<LinearPoint> &points, int minimum_points,
                     const LinearFitOptions &options);
double predict_linear(const LinearFit &fit, double concentration);
QuadraticFit fit_quadratic(const std::vector<LinearPoint> &points, int minimum_points,
                           const LinearFitOptions &options);
double predict_quadratic(const QuadraticFit &fit, double concentration);
WagnerFit fit_wagner(const std::vector<LinearPoint> &points, int minimum_points,
                     const LinearFitOptions &options);
double predict_wagner(const WagnerFit &fit, double concentration);
InverseResult inverse_linear(const LinearFit &fit, double response);
InverseResult inverse_quadratic(const QuadraticFit &fit, double response);
InverseResult inverse_wagner(const WagnerFit &fit, double response);
}
