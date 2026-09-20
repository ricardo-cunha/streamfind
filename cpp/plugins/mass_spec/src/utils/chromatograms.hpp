#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::chromatograms::utils
{
struct Peak
{
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

struct ChromatogramData
{
    int chromatogram_index = 0;
    std::vector<double> rt;
    std::vector<double> intensity;
};

void savitzky_golay_smooth(const std::vector<double> &, std::vector<double> &, std::vector<double> &, std::vector<double> &, int = 11, int = 2);
std::vector<double> rolling_min_baseline(const std::vector<double> &, int = 50);
std::vector<double> als_baseline(const std::vector<double> &, double = 1e6, double = 0.01, int = 10);
std::vector<double> moving_average_smooth(const std::vector<double> &, int = 5);
double median_absolute_deviation(const std::vector<double> &);
std::vector<Peak> find_peaks(const ChromatogramData &, bool = true, double = 45.0, double = 0.0, double = 10.0, double = 5.0, double = 120.0, double = 10.0);
double trapezoidal_area(const std::vector<double> &, const std::vector<double> &);
double calculate_fwhm(const std::vector<double> &, const std::vector<double> &, std::size_t, double);
double calculate_snr(const std::vector<double> &, std::size_t, std::size_t, double);
double calculate_tailing_factor(const std::vector<double> &, const std::vector<double> &, std::size_t, double);
double calculate_gaussian_r2(const std::vector<double> &, const std::vector<double> &, std::size_t, std::size_t, double, double);

struct HeaderInfo
{
    std::string chromatogram_id;
    int polarity = 0;
    double precursor_mz = 0.0;
    double activation_ce = 0.0;
    double product_mz = 0.0;
    std::string signal_type;
    std::string chromatogram_type;
    std::string detector;
    std::string channel;
    double wavelength_nm = 0.0;
    std::string units;
};
using HeaderKey = std::pair<std::string, int>;
using HeaderMap = std::map<HeaderKey, HeaderInfo>;

const std::vector<std::string> &header_columns();
const std::vector<std::string> &point_columns();
const std::vector<std::string> &peak_columns();
HeaderMap make_header_map(const nlohmann::json &);
void append_header_fields(nlohmann::json &, const HeaderInfo &);
std::string text(const nlohmann::json &, const char *);
double real(const nlohmann::json &, const char *);
int integer(const nlohmann::json &, const char *);
bool boolean(const nlohmann::json &, const char *);
bool selected_analysis(const nlohmann::json &, const nlohmann::json &);
bool selected_index(const nlohmann::json &, int);
}
