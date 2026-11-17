#include "utils/chromatograms.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

int main()
{
    using namespace streamfind::mass_spec::chromatograms::utils;

    // The peak bases are deliberately much larger than the surrounding
    // baseline. Noise must come from the regions outside the boundaries.
    std::vector<double> signal(32, 100.0);
    for (std::size_t index = 0; index < 8; ++index)
        signal[index] += index % 2 == 0 ? 2.0 : -2.0;
    for (std::size_t index = 22; index < signal.size(); ++index)
        signal[index] += index % 2 == 0 ? 2.0 : -2.0;
    signal[10] = 1000.0;
    signal[11] = 1500.0;
    signal[12] = 1000.0;
    signal[18] = 1000.0;
    signal[19] = 1500.0;
    signal[20] = 1000.0;

    const double noise = calculate_noise(signal, 10, 20);
    if (!(noise > 100.0 && noise < 120.0))
        throw std::runtime_error("robust local noise estimate was outside the background range.");

    const auto &point_columns = streamfind::mass_spec::chromatograms::utils::point_columns();
    for (const auto *column : {"chromatogram_id", "precursor_mz", "product_mz", "signal_type", "channel"})
        if (std::find(point_columns.begin(), point_columns.end(), column) == point_columns.end())
            throw std::runtime_error("chromatogram point metadata is not part of the table contract.");

    const std::vector<std::string> expected_peak_columns = {
        "analysis", "chromatogram_index", "peak_id", "chromatogram_id", "polarity", "precursor_mz",
        "product_mz", "activation_ce", "signal_type", "chromatogram_type", "detector", "channel",
        "wavelength_nm", "units", "rt", "rt_start", "rt_end", "height", "raw_height", "area",
        "raw_area", "baseline_area", "width", "fwhm", "noise", "snr", "asymmetry", "sharpness",
        "plates", "number_points", "eic_size", "eic_rt", "eic_intensity", "eic_raw_intensity",
        "eic_baseline", "integration_algorithm", "integration_status", "manual_override"};
    if (peak_columns() != expected_peak_columns)
        throw std::runtime_error("chromatogram peak columns do not match their declared result schema.");

    const double snr = calculate_snr(signal, 10, 20, 1500.0);
    if (!(snr > 10.0))
        throw std::runtime_error("flank-based S/N was not calculated from persisted noise.");

    if (normalize_mz(254.100006) != 254.1 || normalize_mz(258.108002) != 258.11 || normalize_mz(96.099998) != 96.1)
        throw std::runtime_error("chromatogram m/z metadata was not normalized to the shared precision.");

    return 0;
}
