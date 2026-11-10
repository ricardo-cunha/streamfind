#include "readers/reader_bruker.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mass_spec::reader::bruker
{
static std::vector<std::uint8_t> decode_hex_blob(const std::string &value)
{
  if (value.size() % 2 != 0)
    throw std::runtime_error("transformator blob has an odd number of hexadecimal digits");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(value.size() / 2);
  const auto nibble = [](const char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    throw std::runtime_error("transformator blob contains a non-hexadecimal character");
  };
  for (std::size_t i = 0; i < value.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>((nibble(value[i]) << 4) | nibble(value[i + 1])));
  return bytes;
}

static double read_le_double(const std::vector<std::uint8_t> &bytes, const std::size_t offset,
                             const char *field)
{
  if (offset > bytes.size() || sizeof(double) > bytes.size() - offset)
    throw std::runtime_error(std::string("transformator blob is truncated at ") + field);
  double value = 0.0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  if (!std::isfinite(value))
    throw std::runtime_error(std::string("transformator coefficient ") + field + " is non-finite");
  return value;
}

BafCalibration parse_baf_calibration(const std::string &hex_blob)
{
  const auto bytes = decode_hex_blob(hex_blob);
  if (bytes.size() < 60)
    throw std::runtime_error("transformator blob has unsupported size " + std::to_string(bytes.size()) +
                             " bytes (at least 60 required)");
  BafCalibration calibration;
  calibration.digitizer_delay = read_le_double(bytes, 4, "digitizer delay");
  calibration.digitizer_timebase = read_le_double(bytes, 12, "digitizer timebase");
  calibration.c0 = read_le_double(bytes, 20, "C0");
  calibration.c1 = read_le_double(bytes, 28, "C1");
  calibration.c2 = read_le_double(bytes, 36, "C2");
  calibration.c3 = read_le_double(bytes, 44, "C3");
  calibration.c4 = read_le_double(bytes, 52, "C4");
  if (!(calibration.digitizer_timebase > 0.0) || !(calibration.c1 > 0.0))
    throw std::runtime_error("transformator calibration requires positive digitizer timebase and C1");
  calibration.valid = true;
  return calibration;
}

double baf_index_to_mz(const BafCalibration &calibration, const double index)
{
  if (!calibration.valid || !(calibration.digitizer_timebase > 0.0) || !(calibration.c1 > 0.0) ||
      !std::isfinite(index) || index < 0.0)
    throw std::runtime_error("transformator conversion received invalid calibration or index");
  const double tof = calibration.digitizer_delay + calibration.digitizer_timebase * index;
  const double linear = std::sqrt(1.0e12 / calibration.c1);
  const auto equation = [&](const double root) {
    return calibration.c0 + linear * root + calibration.c2 * root * root +
           calibration.c3 * root * root * root - tof;
  };
  double lower = 0.0;
  double upper = 100.0;
  double upper_value = equation(upper);
  while (std::isfinite(upper_value) && upper_value < 0.0 && upper < 1.0e6)
  {
    upper = std::min(upper * 2.0, 1.0e6);
    upper_value = equation(upper);
  }
  const double lower_value = equation(lower);
  if (!std::isfinite(tof) || !std::isfinite(lower_value) || !std::isfinite(upper_value) ||
      lower_value > 0.0 || upper_value < 0.0)
    throw std::runtime_error("transformator root is outside supported range [0, 1000000] "
                             "for digitizer index " + std::to_string(index));
  for (int iteration = 0; iteration < 80; ++iteration)
  {
    const double middle = lower + (upper - lower) * 0.5;
    const double middle_value = equation(middle);
    if (!std::isfinite(middle_value))
      throw std::runtime_error("transformator root equation became non-finite at index " +
                               std::to_string(index));
    if (middle_value < 0.0) lower = middle;
    else upper = middle;
  }
  const double root = lower + (upper - lower) * 0.5;
  const double mz = root * root - calibration.c4;
  if (!std::isfinite(mz) || mz < 0.0)
    throw std::runtime_error("transformator produced invalid m/z at digitizer index " +
                             std::to_string(index));
  return mz;
}

static bool solve_baf_polynomial(const std::vector<BafCalibrantError> &calibrants, const int degree,
                                 const std::size_t omitted, const double center, const double scale,
                                 double coefficients[3])
{
  double matrix[3][4] = {};
  std::size_t included = 0;
  double maximum_weight = 0.0;
  for (std::size_t i = 0; i < calibrants.size(); ++i)
    if (i != omitted && std::isfinite(calibrants[i].weight) && calibrants[i].weight > maximum_weight)
      maximum_weight = calibrants[i].weight;
  if (!(maximum_weight > 0.0)) return false;

  for (std::size_t i = 0; i < calibrants.size(); ++i)
  {
    if (i == omitted) continue;
    const auto &point = calibrants[i];
    if (!(std::isfinite(point.reference_mz) && point.reference_mz > 0.0 &&
          std::isfinite(point.error_ppm) && std::isfinite(point.weight) && point.weight > 0.0))
      continue;
    const double x = (point.reference_mz - center) / scale;
    const double weight = point.weight / maximum_weight;
    double powers[5] = {1.0, x, x * x, x * x * x, x * x * x * x};
    for (int row = 0; row <= degree; ++row)
    {
      for (int column = 0; column <= degree; ++column)
        matrix[row][column] += weight * powers[row + column];
      matrix[row][degree + 1] += weight * powers[row] * point.error_ppm;
    }
    ++included;
  }
  if (included < static_cast<std::size_t>(degree + 1)) return false;

  for (int column = 0; column <= degree; ++column)
  {
    int pivot = column;
    for (int row = column + 1; row <= degree; ++row)
      if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
    if (std::abs(matrix[pivot][column]) < 1.0e-12) return false;
    if (pivot != column)
      for (int entry = column; entry <= degree + 1; ++entry)
        std::swap(matrix[pivot][entry], matrix[column][entry]);
    const double divisor = matrix[column][column];
    for (int entry = column; entry <= degree + 1; ++entry) matrix[column][entry] /= divisor;
    for (int row = 0; row <= degree; ++row)
    {
      if (row == column) continue;
      const double factor = matrix[row][column];
      for (int entry = column; entry <= degree + 1; ++entry)
        matrix[row][entry] -= factor * matrix[column][entry];
    }
  }
  coefficients[0] = coefficients[1] = coefficients[2] = 0.0;
  for (int row = 0; row <= degree; ++row) coefficients[row] = matrix[row][degree + 1];
  return true;
}

static double evaluate_baf_polynomial(const double coefficients[3], const int degree, const double x)
{
  double value = coefficients[degree];
  for (int i = degree - 1; i >= 0; --i) value = value * x + coefficients[i];
  return value;
}

BafMassCorrection fit_baf_mass_correction(const std::vector<BafCalibrantError> &calibrants)
{
  BafMassCorrection correction;
  if (calibrants.size() < 3) return correction;
  correction.degree = calibrants.size() >= 5 ? 2 : 1;
  correction.reference_count = calibrants.size();
  correction.mz_min = std::numeric_limits<double>::max();
  correction.mz_max = 0.0;
  for (const auto &point : calibrants)
  {
    if (!(std::isfinite(point.reference_mz) && point.reference_mz > 0.0 &&
          std::isfinite(point.error_ppm) && std::isfinite(point.weight) && point.weight > 0.0))
      throw std::runtime_error("Invalid BAF calibrant error supplied to mass correction.");
    correction.mz_min = std::min(correction.mz_min, point.reference_mz);
    correction.mz_max = std::max(correction.mz_max, point.reference_mz);
  }
  correction.mz_center = (correction.mz_min + correction.mz_max) * 0.5;
  correction.mz_scale = (correction.mz_max - correction.mz_min) * 0.5;
  if (!(correction.mz_scale > 0.0) ||
      !solve_baf_polynomial(calibrants, correction.degree, calibrants.size(), correction.mz_center,
                            correction.mz_scale, correction.coefficients))
    throw std::runtime_error("Unable to fit BAF calibrant correction.");

  double before_squared = 0.0;
  double after_squared = 0.0;
  for (const auto &point : calibrants)
  {
    before_squared += point.error_ppm * point.error_ppm;
    const double x = (point.reference_mz - correction.mz_center) / correction.mz_scale;
    const double residual = point.error_ppm - evaluate_baf_polynomial(correction.coefficients,
                                                                       correction.degree, x);
    after_squared += residual * residual;
  }
  correction.rms_before_ppm = std::sqrt(before_squared / calibrants.size());
  correction.rms_after_ppm = std::sqrt(after_squared / calibrants.size());

  double loo_squared = 0.0;
  std::size_t loo_count = 0;
  for (std::size_t omitted = 0; omitted < calibrants.size(); ++omitted)
  {
    double loo_coefficients[3] = {};
    if (!solve_baf_polynomial(calibrants, correction.degree, omitted, correction.mz_center,
                              correction.mz_scale, loo_coefficients))
      continue;
    const double x = (calibrants[omitted].reference_mz - correction.mz_center) / correction.mz_scale;
    const double residual = calibrants[omitted].error_ppm -
                            evaluate_baf_polynomial(loo_coefficients, correction.degree, x);
    loo_squared += residual * residual;
    ++loo_count;
  }
  correction.loo_rms_ppm = loo_count ? std::sqrt(loo_squared / loo_count) : 0.0;
  correction.applied = correction.rms_before_ppm > 1.0 &&
                       correction.rms_after_ppm < correction.rms_before_ppm &&
                       loo_count == calibrants.size() && correction.loo_rms_ppm <= 1.0;
  return correction;
}

double apply_baf_mass_correction(const BafMassCorrection &correction, const double mz)
{
  if (!correction.applied) return mz;
  if (!std::isfinite(mz) || !(mz > 0.0))
    throw std::runtime_error("Invalid m/z supplied to BAF recalibration.");
  const double bounded_mz = std::clamp(mz, correction.mz_min, correction.mz_max);
  const double x = (bounded_mz - correction.mz_center) / correction.mz_scale;
  const double error_ppm = evaluate_baf_polynomial(correction.coefficients, correction.degree, x);
  const double corrected = mz / (1.0 + error_ppm * 1.0e-6);
  if (!std::isfinite(corrected) || !(corrected > 0.0))
    throw std::runtime_error("BAF recalibration produced an invalid m/z value.");
  return corrected;
}

std::vector<double> tsf_tof_to_mz(
    const TsfCalibration &calibration,
    const double frame_t1,
    const double frame_t2,
    const std::vector<double> &tof)
{
  if (calibration.model_type != 1 || !(calibration.c1 > 0.0))
    throw std::runtime_error("Unsupported or incomplete TSF m/z calibration model.");
  const double temperature_factor = calibration.data_analysis_recalibration
                                        ? 1.0
                                        : 1.0 + (calibration.dc1 * (calibration.t1 - frame_t1) +
                                                 calibration.dc2 * (calibration.t2 - frame_t2)) /
                                                    1.0e6;
  if (!(temperature_factor > 0.0))
    throw std::runtime_error("Invalid TSF m/z calibration temperature factor.");
  const double c1 = calibration.c1 * temperature_factor;
  const double sqrt_term = std::sqrt(1.0e12 / c1);
  const double max_mz = std::max(calibration.mz_max + std::abs(calibration.c4), 1.0);
  const double max_x = std::sqrt(max_mz);
  std::vector<double> mz;
  mz.reserve(tof.size());
  for (const auto value : tof)
  {
    const double target = value * calibration.digitizer_timebase + calibration.digitizer_delay - calibration.c0;
    if (!(target >= 0.0))
      throw std::runtime_error("TSF TOF is below the calibration model intercept.");
    const auto equation = [&](const double x) {
      return sqrt_term * x + calibration.c2 * x * x + calibration.c3 * x * x * x - target;
    };
    double low = 0.0;
    double high = max_x;
    while (equation(high) < 0.0)
    {
      if (high > std::numeric_limits<double>::max() / 2.0)
        throw std::runtime_error("TSF TOF is outside the supported calibration range.");
      high *= 2.0;
    }
    for (int iteration = 0; iteration < 80; ++iteration)
    {
      const double middle = (low + high) / 2.0;
      if (equation(middle) < 0.0)
        low = middle;
      else
        high = middle;
    }
    const double x = (low + high) / 2.0;
    const double calibrated_mz = x * x - calibration.c4;
    if (!std::isfinite(calibrated_mz) || calibrated_mz < 0.0)
      throw std::runtime_error("Invalid TSF calibrated m/z.");
    mz.push_back(calibrated_mz);
  }
  return mz;
}
}
