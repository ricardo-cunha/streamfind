#include "utils/chromatogram_peaks.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace streamfind::mass_spec::detail {
// ─── Savitzky-Golay smoothing ───────────────────────────────────────────────

static std::vector<std::vector<double>> sg_coefficients(int window, int poly_order) {
    int half = window / 2;
    int n = window;
    int cols = poly_order + 1;

    std::vector<std::vector<double>> X(n, std::vector<double>(cols));
    for (int i = 0; i < n; ++i) {
        double x = static_cast<double>(i - half);
        double xp = 1.0;
        for (int j = 0; j < cols; ++j) { X[i][j] = xp; xp *= x; }
    }

    std::vector<std::vector<double>> xtx(cols, std::vector<double>(cols, 0.0));
    for (int i = 0; i < cols; ++i)
        for (int j = 0; j < cols; ++j)
            for (int k = 0; k < n; ++k)
                xtx[i][j] += X[k][i] * X[k][j];

    std::vector<std::vector<double>> inv(cols, std::vector<double>(cols, 0.0));
    for (int i = 0; i < cols; ++i) inv[i][i] = 1.0;
    for (int col = 0; col < cols; ++col) {
        int pivot = col;
        double maxval = std::abs(xtx[col][col]);
        for (int row = col + 1; row < cols; ++row)
            if (std::abs(xtx[row][col]) > maxval) { maxval = std::abs(xtx[row][col]); pivot = row; }
        if (maxval < 1e-15) continue;
        if (pivot != col) { std::swap(xtx[col], xtx[pivot]); std::swap(inv[col], inv[pivot]); }
        double div = xtx[col][col];
        for (int j = 0; j < cols; ++j) { xtx[col][j] /= div; inv[col][j] /= div; }
        for (int row = 0; row < cols; ++row) {
            if (row == col) continue;
            double factor = xtx[row][col];
            for (int j = 0; j < cols; ++j) { xtx[row][j] -= factor * xtx[col][j]; inv[row][j] -= factor * inv[col][j]; }
        }
    }

    std::vector<std::vector<double>> coeffs(3, std::vector<double>(n, 0.0));
    for (int d = 0; d < 3 && d < cols; ++d)
        for (int j = 0; j < n; ++j) {
            double val = 0.0;
            for (int k = 0; k < cols; ++k) val += X[j][k] * inv[k][d];
            coeffs[d][j] = val;
        }
    return coeffs;
}

void savitzky_golay_smooth(
    const std::vector<double> &input,
    std::vector<double> &smoothed,
    std::vector<double> &d1,
    std::vector<double> &d2,
    int window,
    int poly_order) {

    const std::size_t n = input.size();
    if (n < static_cast<std::size_t>(window)) {
        smoothed = input;
        d1.assign(n, 0.0);
        d2.assign(n, 0.0);
        return;
    }

    auto coeffs = sg_coefficients(window, poly_order);
    int half = window / 2;
    smoothed.resize(n); d1.resize(n); d2.resize(n);

    for (std::size_t i = 0; i < n; ++i) {
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (int j = 0; j < window; ++j) {
            std::size_t idx = i + j - half;
            if (idx >= n) continue;
            s0 += coeffs[0][j] * input[idx];
            s1 += coeffs[1][j] * input[idx];
            s2 += coeffs[2][j] * input[idx];
        }
        smoothed[i] = s0; d1[i] = s1; d2[i] = s2;
    }
}

// ─── Rolling-minimum baseline ────────────────────────────────────────────────

std::vector<double> rolling_min_baseline(
    const std::vector<double> &signal, int window) {
    const std::size_t n = signal.size();
    if (n == 0) return {};
    std::vector<double> baseline(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t lo = (i >= static_cast<std::size_t>(window)) ? i - window : 0;
        std::size_t hi = std::min(i + window, n - 1);
        double mn = signal[lo];
        for (std::size_t j = lo + 1; j <= hi; ++j)
            if (signal[j] < mn) mn = signal[j];
        baseline[i] = mn;
    }
    return baseline;
}

// ─── Robust statistics ──────────────────────────────────────────────────────

double median_absolute_deviation(const std::vector<double> &values) {
    if (values.size() < 2) return 0.0;
    std::vector<double> sorted_values = values;
    std::sort(sorted_values.begin(), sorted_values.end());
    double median = sorted_values[sorted_values.size() / 2];
    std::vector<double> abs_dev(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        abs_dev[i] = std::abs(values[i] - median);
    std::sort(abs_dev.begin(), abs_dev.end());
    return abs_dev[abs_dev.size() / 2] * 1.4826;
}

// ─── Trapezoidal integration ────────────────────────────────────────────────

double trapezoidal_area(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() < 2) return 0.0;
    double area = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i)
        area += (x[i] - x[i - 1]) * (y[i - 1] + y[i]) * 0.5;
    return area;
}

// ─── Linear interpolation ───────────────────────────────────────────────────

static double linear_interpolate(
    const std::vector<double> &x, const std::vector<double> &y, double target) {
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] <= target && target <= x[i]) {
            if (x[i] == x[i - 1]) return y[i - 1];
            double t = (target - x[i - 1]) / (x[i] - x[i - 1]);
            return y[i - 1] + t * (y[i] - y[i - 1]);
        }
    }
    return y.empty() ? 0.0 : y.back();
}

// ─── FWHM ───────────────────────────────────────────────────────────────────

double calculate_fwhm(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t apex_idx, double height) {
    const double half = height * 0.5;
    double left_rt = rt[apex_idx], right_rt = rt[apex_idx];

    for (std::size_t i = apex_idx; i > 0; --i)
        if (intensity[i - 1] <= half) { left_rt = linear_interpolate({rt[i - 1], rt[i]}, {intensity[i - 1], intensity[i]}, half); break; }
    for (std::size_t i = apex_idx; i + 1 < rt.size(); ++i)
        if (intensity[i + 1] <= half) { right_rt = linear_interpolate({rt[i], rt[i + 1]}, {intensity[i], intensity[i + 1]}, half); break; }

    double fwhm = right_rt - left_rt;
    if (fwhm > 0.0) return fwhm;

    const double sig_level = height * 0.607;
    left_rt = rt[apex_idx]; right_rt = rt[apex_idx];
    for (std::size_t i = apex_idx; i > 0; --i)
        if (intensity[i - 1] <= sig_level) { left_rt = linear_interpolate({rt[i - 1], rt[i]}, {intensity[i - 1], intensity[i]}, sig_level); break; }
    for (std::size_t i = apex_idx; i + 1 < rt.size(); ++i)
        if (intensity[i + 1] <= sig_level) { right_rt = linear_interpolate({rt[i], rt[i + 1]}, {intensity[i], intensity[i + 1]}, sig_level); break; }
    double sigma = (right_rt - left_rt) / 2.0;
    if (sigma <= 0.0) return 0.0;
    return 2.3548 * sigma;
}

// ─── Signal-to-noise (MAD-based) ────────────────────────────────────────────

double calculate_snr(
    const std::vector<double> &intensity, std::size_t start_idx, std::size_t end_idx,
    double height) {
    // S/N = apex height / average of left and right edge intensities.
    // Use raw chromatogram intensities at the peak boundaries.
    if (start_idx >= intensity.size() || end_idx >= intensity.size()) return 0.0;
    double left_edge = intensity[start_idx];
    double right_edge = intensity[end_idx];
    double noise = (left_edge + right_edge) * 0.5;
    if (noise <= 0.0) noise = 1.0;
    return height / noise;
}

// ─── Tailing factor ──────────────────────────────────────────────────────────

double calculate_tailing_factor(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t apex_idx, double rt_apex) {
    const double height = intensity[apex_idx];
    const double five_pct = height * 0.05;
    double t_left = rt.front(), t_right = rt.back();

    // Cap walk distance to 5× FWHM or 30s, whichever is larger.
    double fwhm_est = calculate_fwhm(rt, intensity, apex_idx, height);
    double max_dist = std::max(30.0, 5.0 * fwhm_est);

    for (std::size_t i = apex_idx; i > 0; --i) {
        if (rt_apex - rt[i - 1] > max_dist) break;
        if (intensity[i - 1] <= five_pct && intensity[i] > five_pct) {
            t_left = linear_interpolate({rt[i - 1], rt[i]}, {intensity[i - 1], intensity[i]}, five_pct); break; }
    }
    for (std::size_t i = apex_idx; i + 1 < rt.size(); ++i) {
        if (rt[i + 1] - rt_apex > max_dist) break;
        if (intensity[i + 1] <= five_pct && intensity[i] > five_pct) {
            t_right = linear_interpolate({rt[i], rt[i + 1]}, {intensity[i], intensity[i + 1]}, five_pct); break; }
    }

    double w_half = (t_right - t_left) * 0.5;
    if (w_half <= 0.0) return 1.0;
    double d2 = std::max(rt_apex - t_left, t_right - rt_apex);
    return (t_right - t_left) / (2.0 * d2);
}

// ─── Gaussian R² ─────────────────────────────────────────────────────────────

double calculate_gaussian_r2(
    const std::vector<double> &rt, const std::vector<double> &intensity,
    std::size_t start_idx, std::size_t end_idx, double apex_rt, double height) {
    if (end_idx <= start_idx + 2) return 0.0;

    const double sig_level = height * 0.607;
    std::size_t apex_local = start_idx;
    { double bv = 0.0;
      for (std::size_t i = start_idx; i <= end_idx && i < rt.size(); ++i)
          if (intensity[i] > bv) { bv = intensity[i]; apex_local = i; }
    }

    double left_sig = rt[apex_local], right_sig = rt[apex_local];
    for (std::size_t i = apex_local; i > start_idx; --i)
        if (intensity[i - 1] <= sig_level && intensity[i] > sig_level) {
            left_sig = linear_interpolate({rt[i - 1], rt[i]}, {intensity[i - 1], intensity[i]}, sig_level); break; }
    for (std::size_t i = apex_local; i < end_idx; ++i)
        if (intensity[i + 1] <= sig_level && intensity[i] > sig_level) {
            right_sig = linear_interpolate({rt[i], rt[i + 1]}, {intensity[i], intensity[i + 1]}, sig_level); break; }
    double sigma = (right_sig - left_sig) / 2.0;
    if (sigma <= 0.0) return 0.0;

    double ss_res = 0.0, ss_tot = 0.0, mean_int = 0.0;
    std::size_t count = 0;
    for (std::size_t i = start_idx; i <= end_idx && i < rt.size(); ++i) { mean_int += intensity[i]; ++count; }
    if (count > 0) mean_int /= count;

    for (std::size_t i = start_idx; i <= end_idx && i < rt.size(); ++i) {
        double predicted = height * std::exp(-0.5 * std::pow((rt[i] - apex_rt) / sigma, 2.0));
        ss_res += (intensity[i] - predicted) * (intensity[i] - predicted);
        ss_tot += (intensity[i] - mean_int) * (intensity[i] - mean_int);
    }
    if (ss_tot <= 0.0) return 0.0;
    return 1.0 - (ss_res / ss_tot);
}

// ─── Peak integration ──────────────────────────────────────────────────────

static double integrate_peak_area(
    const std::vector<double> &rt, const std::vector<double> &corrected,
    std::size_t start_idx, std::size_t end_idx) {
    double area = 0.0;
    for (std::size_t i = start_idx + 1; i <= end_idx && i < rt.size(); ++i) {
        double y = std::max(0.0, corrected[i]);
        double y_prev = std::max(0.0, corrected[i - 1]);
        area += (rt[i] - rt[i - 1]) * (y_prev + y) * 0.5;
    }
    return area;
}

// ─── Core peak detection algorithm ──────────────────────────────────────────
//
// Algorithm inspired by NTA find_features and literature:
// 1. SG smoothing + first/second derivative
// 2. Rolling-minimum baseline with running-median smoothing
// 3. Corrected signal = smoothed - baseline
// 4. Peak detection via local maxima with first-derivative sign change
// 5. Boundary extension: walk to baseline + 10% of peak height OR until
//    signal stops descending (valley detection)
// 6. Smart merging: merge when valley between peaks < 50% of smaller peak
// 7. Integration: ∫max(0, corrected) dt via trapezoidal rule

std::vector<Peak> find_peaks(
    const ChromatogramData &chrom,
    bool merge,
    double merge_distance,
    double min_peak_height,
    double min_peak_distance,
    double min_peak_width,
    double max_peak_width,
    double min_snr) {

    const auto &rt = chrom.rt;
    const auto &intensity = chrom.intensity;
    const std::size_t n = rt.size();
    if (n < 5) return {};

    // Step 1: SG smoothing + first/second derivative.
    std::vector<double> smoothed, d1, d2;
    int sg_window = std::min(11, static_cast<int>(n) | 1);
    if (sg_window < 5) sg_window = 5;
    if (sg_window % 2 == 0) ++sg_window;
    savitzky_golay_smooth(intensity, smoothed, d1, d2, sg_window, 2);

    // Step 2: Adaptive baseline.
    // Rolling-minimum with window=25 points (~1.1s) for good local accuracy.
    // Must be narrow enough to follow baseline changes between closely-spaced peaks.
    auto baseline = rolling_min_baseline(smoothed, 25);
    // Smooth baseline with running median to remove noise.
    {
        std::vector<double> smooth_bl(n);
        int half_w = 10;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t lo = (i >= static_cast<std::size_t>(half_w)) ? i - half_w : 0;
            std::size_t hi = std::min(i + half_w, n - 1);
            std::vector<double> w(baseline.begin() + lo, baseline.begin() + hi + 1);
            std::sort(w.begin(), w.end());
            smooth_bl[i] = w[w.size() / 2];
        }
        baseline = smooth_bl;
    }

    // Step 3: Corrected signal = smoothed - baseline.
    std::vector<double> corrected(n);
    for (std::size_t i = 0; i < n; ++i) {
        corrected[i] = smoothed[i] - baseline[i];
        if (corrected[i] < 0.0) corrected[i] = 0.0;
    }

    double max_corrected = corrected.empty() ? 0.0 : *std::max_element(corrected.begin(), corrected.end());

    // Step 4: Detect peaks using first-derivative zero crossings.
    // A peak apex is where d1 crosses from positive to negative.
    // This is more robust than looking at local maxima of corrected signal.
    struct RawPeak {
        std::size_t apex_idx = 0;
        std::size_t start_idx = 0;
        std::size_t end_idx = 0;
        double apex_rt = 0.0;
        double height = 0.0;
    };

    std::vector<RawPeak> raw;

    // Find peaks via first-derivative sign changes (positive → negative).
    for (std::size_t i = 1; i + 1 < n; ++i) {
        // First derivative zero crossing: d1[i-1] > 0, d1[i] <= 0 (or d1[i+1] < 0).
        if (!(d1[i - 1] > 0.0 && d1[i] <= 0.0)) continue;

        // Refine apex to the actual maximum of corrected signal in ±5 window.
        std::size_t apex = i;
        double best = corrected[i];
        for (std::size_t j = (i > 5 ? i - 5 : 0); j <= std::min(i + 5, n - 1); ++j) {
            if (corrected[j] > best) { best = corrected[j]; apex = j; }
        }

        if (best < min_peak_height) continue;
        if (best < max_corrected * 0.005) continue;  // Skip peaks < 0.5% of max.

        // Phase 1: Walk to inflection points (d2 zero crossings).
        std::size_t left_infl = (apex > 0) ? apex - 1 : 0;
        for (std::size_t j = apex; j > 0; --j) {
            if (d2[j] < 0.0 && d2[j - 1] >= 0.0) { left_infl = j - 1; break; }
            if (corrected[j] <= corrected[j - 1] && j < apex) { left_infl = j; break; }
        }
        std::size_t right_infl = (apex + 1 < n) ? apex + 1 : n - 1;
        for (std::size_t j = apex; j + 1 < n; ++j) {
            if (d2[j] < 0.0 && d2[j + 1] >= 0.0) { right_infl = j + 1; break; }
            if (corrected[j] <= corrected[j + 1] && j > apex) { right_infl = j; break; }
        }

        // Phase 2: Extend boundaries to the peak base.
        // KEY INSIGHT: Use the CORRECTED signal for boundary detection.
        // The corrected signal is baseline-subtracted, so it naturally reaches
        // zero at the peak base. Walk outward until corrected drops to <2% of
        // peak height — this gives true valley-to-valley integration.
        //
        // Allow small local rises in corrected (noise) as long as the overall
        // trend is toward zero. Use a "running minimum" approach: track the
        // lowest corrected value seen so far, and stop when the signal rises
        // significantly above that minimum (indicating a true valley to a
        // separate peak).

        // Sigma estimate from inflection distance.
        double sigma_est = std::max(0.1, rt[apex] - rt[left_infl]);
        // Max walk: at least 60s or 8× sigma — peaks can be broad.
        double max_walk_time = std::max(60.0, 8.0 * sigma_est);
        // Stop threshold: 2% of peak height on the corrected signal.
        double corr_stop = corrected[apex] * 0.02;

        // Walk LEFT on corrected signal.
        std::size_t left = left_infl;
        {
            double running_min = corrected[left_infl];
            for (std::size_t k = left_infl; k > 0; --k) {
                if (rt[apex] - rt[k] > max_walk_time) break;
                double val = corrected[k];
                if (val < running_min) running_min = val;
                // Stop if corrected drops to near-zero (peak base reached).
                if (val <= corr_stop) { left = k; break; }
                // Stop if corrected rises significantly above the running minimum
                // AND we're past the inflection point (true valley to next peak).
                if (k < left_infl && val > running_min * 3.0 && val > corrected[apex] * 0.15) break;
                left = k;
            }
        }

        // Walk RIGHT on corrected signal.
        std::size_t right = right_infl;
        {
            double running_min = corrected[right_infl];
            for (std::size_t k = right_infl; k + 1 < n; ++k) {
                if (rt[k] - rt[apex] > max_walk_time) break;
                double val = corrected[k];
                if (val < running_min) running_min = val;
                if (val <= corr_stop) { right = k; break; }
                if (k > right_infl && val > running_min * 3.0 && val > corrected[apex] * 0.15) break;
                right = k;
            }
        }

        raw.push_back({apex, left, right, rt[apex], corrected[apex]});
    }

    if (raw.empty()) return {};

    // Step 5: Sort by retention time.
    std::sort(raw.begin(), raw.end(), [](const auto &a, const auto &b) { return a.apex_rt < b.apex_rt; });

    // Step 6: Minimum distance filter — keep taller peak when two are too close.
    {
        std::vector<RawPeak> filtered;
        for (const auto &pk : raw) {
            if (filtered.empty() || pk.apex_rt - filtered.back().apex_rt >= min_peak_distance) {
                filtered.push_back(pk);
            } else if (pk.height > filtered.back().height) {
                filtered.back() = pk;
            }
        }
        raw = std::move(filtered);
    }

    // Step 7: Smart merge — merge when valley between peaks is not deep enough.
    // Use the RAW signal valley depth relative to the smaller peak's HALF-HEIGHT.
    // This correctly handles cases where the valley raw value is between the two
    // peak heights (valley above smaller peak apex but below half-height).
    if (merge && raw.size() > 1) {
        std::vector<RawPeak> merged;
        merged.push_back(raw[0]);
        for (std::size_t i = 1; i < raw.size(); ++i) {
            auto &back = merged.back();
            double pk_diff = raw[i].apex_rt - back.apex_rt;
            if (pk_diff <= merge_distance) {
                // Find minimum RAW signal between the two peaks.
                double valley_raw_min = intensity[back.apex_idx];
                for (std::size_t j = back.apex_idx; j <= raw[i].apex_idx && j < n; ++j)
                    valley_raw_min = std::min(valley_raw_min, intensity[j]);
                // Compare valley to half-height of the smaller peak.
                double raw_smaller = std::min(intensity[back.apex_idx], intensity[raw[i].apex_idx]);
                double half_height = raw_smaller * 0.5;
                // Merge only if valley is above half-height of smaller peak.
                // If valley drops below half-height, peaks are clearly separate.
                if (valley_raw_min > half_height) {
                    // Merge: extend boundaries, keep taller apex.
                    back.start_idx = std::min(back.start_idx, raw[i].start_idx);
                    back.end_idx = std::max(back.end_idx, raw[i].end_idx);
                    if (raw[i].height > back.height) {
                        back.apex_idx = raw[i].apex_idx;
                        back.apex_rt = raw[i].apex_rt;
                        back.height = raw[i].height;
                    }
                    continue;
                }
            }
            merged.push_back(raw[i]);
        }
        raw = std::move(merged);
    }

    // Step 7b: Add missed peaks via corrected-signal local maxima.
    // The first-derivative approach may miss smaller peaks within a broad peak.
    // Find additional local maxima of the corrected signal that are not already
    // covered by existing peaks, and add them if they meet the height criteria.
    {
        std::vector<bool> covered(n, false);
        for (const auto &rp : raw)
            for (std::size_t j = rp.start_idx; j <= rp.end_idx; ++j)
                covered[j] = true;

        std::vector<RawPeak> extra;
        for (std::size_t i = 2; i + 2 < n; ++i) {
            if (covered[i]) continue;
            if (corrected[i] <= corrected[i - 1] || corrected[i] <= corrected[i + 1]) continue;
            if (corrected[i] < min_peak_height) continue;
            if (corrected[i] < max_corrected * 0.005) continue;
            // Find boundaries: walk left/right to valley or covered region.
            std::size_t left = i;
            for (std::size_t j = i; j > 0; --j) {
                if (covered[j] && j < i) { left = j + 1; break; }
                if (corrected[j] <= corrected[i] * 0.02) { left = j; break; }
                left = j;
            }
            std::size_t right = i;
            for (std::size_t j = i; j + 1 < n; ++j) {
                if (covered[j] && j > i) { right = j - 1; break; }
                if (corrected[j] <= corrected[i] * 0.02) { right = j; break; }
                right = j;
            }
            extra.push_back({i, left, right, rt[i], corrected[i]});
        }
        // Add extras that don't overlap existing peaks.
        for (const auto &ep : extra) {
            bool overlaps = false;
            for (const auto &rp : raw)
                if (ep.start_idx <= rp.end_idx && ep.end_idx >= rp.start_idx) { overlaps = true; break; }
            if (!overlaps) raw.push_back(ep);
        }
    }

    // Step 8: Build final peaks with full metrics.
    // All reported properties (height, area, S/N) are computed from the
    // ORIGINAL raw chromatogram data — not the smoothed/corrected signal
    // used only for peak detection and boundary finding.
    std::vector<Peak> peaks;
    int peak_id = 0;

    for (const auto &rp : raw) {
        double pk_width = rt[rp.end_idx] - rt[rp.start_idx];
        if (pk_width < min_peak_width || pk_width > max_peak_width) continue;

        Peak pk;
        pk.chromatogram_index = chrom.chromatogram_index;
        pk.peak_id = ++peak_id;
        pk.rt_start = rt[rp.start_idx];
        pk.rt_end = rt[rp.end_idx];
        pk.width = pk_width;
        pk.number_points = static_cast<int>(rp.end_idx - rp.start_idx + 1);

        // Find apex on the RAW signal within the detected boundaries.
        std::size_t best_idx = rp.apex_idx;
        double best_raw = intensity[rp.apex_idx];
        for (std::size_t j = rp.start_idx; j <= rp.end_idx; ++j) {
            if (intensity[j] > best_raw) { best_raw = intensity[j]; best_idx = j; }
        }

        pk.rt = rt[best_idx];
        pk.raw_height = best_raw;
        pk.height = best_raw;  // Primary height from raw signal.

        // Area: integrate raw signal with linear baseline from peak edges.
        // This is the standard chromatographic integration approach.
        {
            double bl_left = intensity[rp.start_idx];
            double bl_right = intensity[rp.end_idx];
            double area = 0.0;
            for (std::size_t i = rp.start_idx + 1; i <= rp.end_idx && i < rt.size(); ++i) {
                double t = static_cast<double>(i - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double bl = bl_left + t * (bl_right - bl_left);
                double y = std::max(0.0, intensity[i] - bl);
                double y_prev = std::max(0.0, intensity[i - 1] - bl_left);
                area += (rt[i] - rt[i - 1]) * (y_prev + y) * 0.5;
            }
            pk.area = area;
        }
        pk.raw_area = pk.area;  // Same — both from raw.
        pk.baseline_area = 0.0;  // Baseline already subtracted via linear baseline.

        // FWHM from raw intensity with local linear baseline.
        {
            double bl_left = intensity[rp.start_idx];
            double bl_right = intensity[rp.end_idx];
            double local_max = 0.0;
            std::size_t local_apex = best_idx;
            for (std::size_t k = rp.start_idx; k <= rp.end_idx; ++k) {
                double t = static_cast<double>(k - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double bl = bl_left + t * (bl_right - bl_left);
                double corr = std::max(0.0, intensity[k] - bl);
                if (corr > local_max) { local_max = corr; local_apex = k; }
            }
            double half = local_max * 0.5;
            double left_fwhm = rt[local_apex], right_fwhm = rt[local_apex];
            for (std::size_t k = local_apex; k > rp.start_idx; --k) {
                double t1 = static_cast<double>(k - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double t2 = static_cast<double>(k - 1 - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double c1 = std::max(0.0, intensity[k] - (bl_left + t1 * (bl_right - bl_left)));
                double c2 = std::max(0.0, intensity[k - 1] - (bl_left + t2 * (bl_right - bl_left)));
                if (c2 <= half && c1 > half) {
                    left_fwhm = rt[k - 1] + (half - c2) / (c1 - c2) * (rt[k] - rt[k - 1]); break;
                }
            }
            for (std::size_t k = local_apex; k < rp.end_idx; ++k) {
                double t1 = static_cast<double>(k - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double t2 = static_cast<double>(k + 1 - rp.start_idx) / static_cast<double>(rp.end_idx - rp.start_idx);
                double c1 = std::max(0.0, intensity[k] - (bl_left + t1 * (bl_right - bl_left)));
                double c2 = std::max(0.0, intensity[k + 1] - (bl_left + t2 * (bl_right - bl_left)));
                if (c2 <= half && c1 > half) {
                    right_fwhm = rt[k] + (half - c1) / (c2 - c1) * (rt[k + 1] - rt[k]); break;
                }
            }
            pk.fwhm = std::max(0.0, right_fwhm - left_fwhm);
        }

        // S/N from raw signal — noise estimated from boundary regions.
        pk.snr = calculate_snr(intensity, rp.start_idx, rp.end_idx, best_raw);

        // Tailing factor from raw signal.
        pk.tailing_factor = calculate_tailing_factor(rt, intensity, best_idx, pk.rt);

        // Asymmetry at 10% height — from raw signal.
        {
            const double ten_pct = best_raw * 0.1;
            double t_left = pk.rt_start, t_right = pk.rt_end;
            for (std::size_t j = best_idx; j > rp.start_idx; --j)
                if (intensity[j - 1] <= ten_pct && intensity[j] > ten_pct) {
                    t_left = linear_interpolate({rt[j - 1], rt[j]}, {intensity[j - 1], intensity[j]}, ten_pct); break; }
            for (std::size_t j = best_idx; j < rp.end_idx; ++j)
                if (intensity[j + 1] <= ten_pct && intensity[j] > ten_pct) {
                    t_right = linear_interpolate({rt[j], rt[j + 1]}, {intensity[j], intensity[j + 1]}, ten_pct); break; }
            double a = pk.rt - t_left, b = t_right - pk.rt;
            pk.asymmetry = (a > 0.0 && b > 0.0) ? (b / a) : 1.0;
        }

        pk.plates = (pk.fwhm > 0.0 && pk.rt > 0.0)
            ? 5.54 * (pk.rt / pk.fwhm) * (pk.rt / pk.fwhm) : 0.0;
        pk.sharpness = (pk.width > 0.0) ? (pk.height / pk.width) : 0.0;
        pk.gaussian_r2 = calculate_gaussian_r2(rt, intensity, rp.start_idx, rp.end_idx, pk.rt, pk.height);

        // S/N filter.
        if (pk.snr < min_snr) continue;

        peaks.push_back(pk);
    }

    return peaks;
}

}  // namespace streamfind::mass_spec::detail
