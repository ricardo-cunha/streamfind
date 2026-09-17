#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace streamfind::mass_spec::detail {

struct Peak {
    int chromatogram_index = 0;
    int peak_id = 0;
    double rt = 0.0;
    double rt_start = 0.0;
    double rt_end = 0.0;
    double height = 0.0;
    double raw_height = 0.0;
    double area = 0.0;
    double raw_area = 0.0;
    double baseline_area = 0.0;
    double width = 0.0;
    double fwhm = 0.0;
    double snr = 0.0;
    double asymmetry = 0.0;
    double tailing_factor = 0.0;
    double sharpness = 0.0;
    double plates = 0.0;
    double gaussian_r2 = 0.0;
    int number_points = 0;
};

struct ChromatogramData {
    int chromatogram_index = 0;
    std::vector<double> rt;
    std::vector<double> intensity;
};

/// Savitzky-Golay smoothing with configurable window and polynomial order.
/// Returns smoothed signal. Optionally computes first and second derivatives.
void savitzky_golay_smooth(
    const std::vector<double> &input,
    std::vector<double> &smoothed,
    std::vector<double> &d1,
    std::vector<double> &d2,
    int window = 11,
    int poly_order = 2);

/// Rolling-minimum baseline estimation.
std::vector<double> rolling_min_baseline(
    const std::vector<double> &signal,
    int window = 50);

/// Asymmetric least-squares baseline estimation (Boels & Eilers, 2005).
/// Lambda controls smoothness; p is the asymmetry penalty (0..1).
std::vector<double> als_baseline(
    const std::vector<double> &signal,
    double lambda = 1e6,
    double p = 0.01,
    int max_iterations = 10);

/// Moving-average smoothing.
std::vector<double> moving_average_smooth(
    const std::vector<double> &signal,
    int window = 5);

/// Median absolute deviation — robust noise estimator.
double median_absolute_deviation(const std::vector<double> &values);

/// Savitzky-Golay peak finder with second-derivative detection.
/// Core algorithm combining literature best practices:
/// 1. SG smoothing + second derivative
/// 2. Peak detection via d2 zero-crossings (inflection points)
/// 3. Adaptive baseline via ALS
/// 4. Robust S/N via MAD
/// 5. Valley-to-valley integration
/// 6. Gaussian quality assessment
std::vector<Peak> find_peaks(
    const ChromatogramData &chrom,
    bool merge = true,
    double merge_distance = 45.0,
    double min_peak_height = 0.0,
    double min_peak_distance = 10.0,
    double min_peak_width = 5.0,
    double max_peak_width = 120.0,
    double min_snr = 10.0);

/// Trapezoidal integration of y over x.
double trapezoidal_area(const std::vector<double> &x, const std::vector<double> &y);

/// Calculate FWHM via linear interpolation around half-height.
double calculate_fwhm(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t apex_idx, double height);

/// Signal-to-noise using MAD-based noise estimation from baseline regions.
double calculate_snr(
    const std::vector<double> &intensity, std::size_t start_idx, std::size_t end_idx,
    double height);

/// Tailing factor at 10% height: Tb = W_0.05 / (2 * d2).
double calculate_tailing_factor(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t apex_idx, double rt_apex);

/// Gaussian R² — goodness of fit of a Gaussian to the peak.
double calculate_gaussian_r2(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t start_idx, std::size_t end_idx, double apex_rt, double height);

}  // namespace streamfind::mass_spec::detail
