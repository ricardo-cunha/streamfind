#include "spectral_savgol.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace streamfind::raman::operations {

namespace detail {

double fit_at_center(const std::vector<double> &values, std::size_t begin,
                     std::size_t target, std::size_t window_length,
                     std::size_t polynomial_order) {
    const auto parameter_count = polynomial_order + 1;
    std::vector<std::vector<double>> matrix(
        window_length, std::vector<double>(parameter_count, 0.0));
    std::vector<double> rhs(window_length, 0.0);
    const auto scale = static_cast<double>(std::max<std::size_t>(1, window_length / 2));
    for (std::size_t row = 0; row < window_length; ++row) {
        const auto sample = begin + row;
        const auto relative = (static_cast<double>(sample) - static_cast<double>(target)) / scale;
        double power = 1.0;
        for (std::size_t col = 0; col < parameter_count; ++col) {
            matrix[row][col] = power;
            power *= relative;
        }
        rhs[row] = values[sample];
    }

    for (std::size_t pivot = 0; pivot < parameter_count; ++pivot) {
        double norm = 0.0;
        for (std::size_t row = pivot; row < window_length; ++row)
            norm = std::hypot(norm, matrix[row][pivot]);
        if (!(norm > 0.0) || !std::isfinite(norm))
            throw std::runtime_error("Savitzky-Golay polynomial fit is rank deficient");
        const auto alpha = -std::copysign(norm, matrix[pivot][pivot]);
        std::vector<double> reflector(window_length - pivot);
        for (std::size_t row = pivot; row < window_length; ++row)
            reflector[row - pivot] = matrix[row][pivot];
        reflector.front() -= alpha;
        double squared_norm = 0.0;
        for (const auto value : reflector) squared_norm += value * value;
        if (!(squared_norm > 0.0) || !std::isfinite(squared_norm))
            throw std::runtime_error("Savitzky-Golay polynomial fit is numerically unstable");
        for (std::size_t col = pivot; col < parameter_count; ++col) {
            double projection = 0.0;
            for (std::size_t row = pivot; row < window_length; ++row)
                projection += reflector[row - pivot] * matrix[row][col];
            const auto factor = 2.0 * projection / squared_norm;
            for (std::size_t row = pivot; row < window_length; ++row)
                matrix[row][col] -= factor * reflector[row - pivot];
        }
        double rhs_projection = 0.0;
        for (std::size_t row = pivot; row < window_length; ++row)
            rhs_projection += reflector[row - pivot] * rhs[row];
        const auto rhs_factor = 2.0 * rhs_projection / squared_norm;
        for (std::size_t row = pivot; row < window_length; ++row)
            rhs[row] -= rhs_factor * reflector[row - pivot];
        matrix[pivot][pivot] = alpha;
        for (std::size_t row = pivot + 1; row < window_length; ++row)
            matrix[row][pivot] = 0.0;
    }

    std::vector<double> coefficients(parameter_count, 0.0);
    for (std::size_t reverse = parameter_count; reverse-- > 0;) {
        double value = rhs[reverse];
        for (std::size_t col = reverse + 1; col < parameter_count; ++col)
            value -= matrix[reverse][col] * coefficients[col];
        const auto diagonal = matrix[reverse][reverse];
        if (diagonal == 0.0 || !std::isfinite(diagonal))
            throw std::runtime_error("Savitzky-Golay polynomial fit is singular");
        coefficients[reverse] = value / diagonal;
    }
    return coefficients.front();
}

void validate_axis(const std::vector<double> &coordinates) {
    if (coordinates.size() < 2) return;
    const auto step = coordinates[1] - coordinates[0];
    if (step == 0.0 || !std::isfinite(step))
        throw std::invalid_argument("Savitzky-Golay smoothing requires a strictly monotonic, evenly spaced spectral axis");
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        if (!std::isfinite(coordinates[index]))
            throw std::invalid_argument("Savitzky-Golay smoothing requires finite spectral coordinates");
        if (index > 0 && (coordinates[index] - coordinates[index - 1]) * step <= 0.0)
            throw std::invalid_argument("Savitzky-Golay smoothing requires a strictly monotonic spectral axis");
        const auto expected = coordinates.front() + step * static_cast<double>(index);
        const auto tolerance = 1e-6 * std::max({1.0, std::abs(coordinates[index]),
                                                std::abs(expected), std::abs(step)});
        if (std::abs(coordinates[index] - expected) > tolerance)
            throw std::invalid_argument("Savitzky-Golay smoothing requires an evenly spaced spectral axis");
    }
}

}  // namespace detail

std::vector<double> savitzky_golay_smooth(const std::vector<double> &intensities,
                                          const std::vector<double> &coordinates,
                                          std::size_t window_length,
                                          std::size_t polynomial_order) {
    if (intensities.size() != coordinates.size())
        throw std::invalid_argument("Savitzky-Golay intensity and coordinate lengths differ");
    if (window_length < 3 || window_length % 2 == 0 || window_length > 1001)
        throw std::invalid_argument("Savitzky-Golay window_length must be odd and between 3 and 1001");
    if (polynomial_order > 6 || polynomial_order >= window_length)
        throw std::invalid_argument("Savitzky-Golay polynomial_order must be less than window_length and no greater than 6");
    if (intensities.size() < window_length)
        throw std::invalid_argument("Savitzky-Golay window_length exceeds the spectrum point count");
    for (const auto intensity : intensities)
        if (!std::isfinite(intensity))
            throw std::invalid_argument("Savitzky-Golay smoothing requires finite intensities");
    detail::validate_axis(coordinates);

    std::vector<double> result(intensities.size(), 0.0);
    const auto half_window = window_length / 2;
    for (std::size_t target = 0; target < intensities.size(); ++target) {
        auto begin = target > half_window ? target - half_window : 0;
        if (begin + window_length > intensities.size())
            begin = intensities.size() - window_length;
        result[target] = detail::fit_at_center(intensities, begin, target,
                                               window_length, polynomial_order);
    }
    return result;
}

}  // namespace streamfind::raman::operations
