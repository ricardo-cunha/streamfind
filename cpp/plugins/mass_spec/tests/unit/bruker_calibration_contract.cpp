#include "readers/reader_bruker.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::tests::bruker_calibration
{
double tof_for_mass(const mass_spec::reader::bruker::TsfCalibration &calibration, const double mz, const double c1)
{
  const double x = std::sqrt(mz + calibration.c4);
  const double model_time = calibration.c0 +
                            std::sqrt(1.0e12 / c1) * x +
                            calibration.c2 * x * x +
                            calibration.c3 * x * x * x;
  return (model_time - calibration.digitizer_delay) / calibration.digitizer_timebase;
}

void require_near(const double actual, const double expected, const double tolerance, const char *message)
{
  if (std::abs(actual - expected) > tolerance)
    throw std::runtime_error(message);
}

std::string calibration_blob(const double delay, const double timebase, const double c0,
                             const double c1, const double c2, const double c3, const double c4)
{
  std::vector<std::uint8_t> bytes(60, 0);
  const double values[] = {delay, timebase, c0, c1, c2, c3, c4};
  for (std::size_t i = 0; i < std::size(values); ++i)
    std::memcpy(bytes.data() + 4 + i * sizeof(double), &values[i], sizeof(double));
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes)
  {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}
}

int main()
{
  using mass_spec::reader::bruker::TsfCalibration;
  using mass_spec::reader::bruker::BafCalibration;
  using mass_spec::reader::bruker::BafCalibrantError;
  using mass_spec::reader::bruker::fit_baf_mass_correction;
  using mass_spec::reader::bruker::apply_baf_mass_correction;
  using mass_spec::reader::bruker::baf_index_to_mz;
  using mass_spec::reader::bruker::parse_baf_calibration;
  using streamfind::tests::bruker_calibration::calibration_blob;
  using mass_spec::reader::bruker::tsf_tof_to_mz;
  using streamfind::tests::bruker_calibration::require_near;
  using streamfind::tests::bruker_calibration::tof_for_mass;

  TsfCalibration calibration;
  calibration.model_type = 1;
  calibration.digitizer_timebase = 0.2;
  calibration.digitizer_delay = 25159.4;
  calibration.t1 = 10.0;
  calibration.t2 = 20.0;
  calibration.dc1 = 100.0;
  calibration.dc2 = -50.0;
  calibration.c0 = 333.6035842669991;
  calibration.c1 = 154127.85949518575;
  calibration.c2 = 0.00030807613744260147;
  calibration.c3 = 0.0;
  calibration.c4 = -0.003941800581493701;
  calibration.mz_max = 3000.0;

  const double expected_mass = 922.00980;
  const double acquisition_factor = 1.0 + (calibration.dc1 * (calibration.t1 - 12.0) +
                                            calibration.dc2 * (calibration.t2 - 19.0)) /
                                               1.0e6;
  const double acquisition_tof = tof_for_mass(calibration, expected_mass, calibration.c1 * acquisition_factor);
  const auto acquisition_result = tsf_tof_to_mz(calibration, 12.0, 19.0, {acquisition_tof});
  require_near(acquisition_result.at(0), expected_mass, 1.0e-7,
               "Acquisition TSF calibration did not apply its temperature correction.");

  calibration.data_analysis_recalibration = true;
  const double recalibrated_tof = tof_for_mass(calibration, expected_mass, calibration.c1);
  const auto recalibrated_result = tsf_tof_to_mz(calibration, 12.0, 19.0, {recalibrated_tof});
  require_near(recalibrated_result.at(0), expected_mass, 1.0e-7,
               "DataAnalysis TSF calibration unexpectedly applied the acquisition temperature correction.");

  calibration.model_type = 2;
  bool rejected_model = false;
  try
  {
    (void)tsf_tof_to_mz(calibration, 12.0, 19.0, {recalibrated_tof});
  }
  catch (const std::runtime_error &)
  {
    rejected_model = true;
  }
  if (!rejected_model)
    throw std::runtime_error("Unsupported TSF calibration model was not rejected.");

  const auto baf = parse_baf_calibration(calibration_blob(100.0, 0.5, 10.0, 1.0e12, 0.0, 0.0, -1.0));
  require_near(baf_index_to_mz(baf, 20.0), 10001.0, 1.0e-8,
               "BAF transformator equation did not return the expected calibrated mass.");

  bool rejected_nonfinite_coefficient = false;
  try
  {
    (void)parse_baf_calibration(calibration_blob(0.0, 0.5, 0.0,
                                                std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  }
  catch (const std::runtime_error &error)
  {
    rejected_nonfinite_coefficient = std::string(error.what()).find("C1 is non-finite") != std::string::npos;
  }
  if (!rejected_nonfinite_coefficient)
    throw std::runtime_error("Non-finite BAF coefficient did not produce a field-specific diagnostic.");

  auto outside_range = baf;
  outside_range.digitizer_timebase = 10.0;
  bool rejected_outside_range = false;
  try
  {
    (void)baf_index_to_mz(outside_range, 200000.0);
  }
  catch (const std::runtime_error &error)
  {
    rejected_outside_range = std::string(error.what()).find("supported range") != std::string::npos;
  }
  if (!rejected_outside_range)
    throw std::runtime_error("Out-of-range BAF transformator root did not produce a bounded diagnostic.");

  std::vector<BafCalibrantError> calibrants;
  for (const double reference_mz : {322.04812, 622.02896, 922.00980, 1221.99064,
                                    1521.97148, 1821.95231, 2121.93315, 2421.91399})
  {
    const double x = (reference_mz - 1371.981055) / 1049.932935;
    const double error_ppm = 2.0 + 1.5 * x + 0.5 * x * x;
    calibrants.push_back({reference_mz, error_ppm, 100.0});
  }
  const auto correction = fit_baf_mass_correction(calibrants);
  if (!correction.applied)
    throw std::runtime_error("Validated multi-reference BAF calibration correction was not applied.");
  require_near(correction.rms_after_ppm, 0.0, 1.0e-9,
               "Polynomial BAF correction did not fit the synthetic calibrant errors.");
  if (correction.loo_rms_ppm > 1.0)
    throw std::runtime_error("Polynomial BAF correction exceeded the leave-one-out error limit.");
  for (const auto &point : calibrants)
    require_near(apply_baf_mass_correction(
                     correction, point.reference_mz * (1.0 + point.error_ppm * 1.0e-6)),
                 point.reference_mz, 0.00001,
                 "BAF correction did not return a calibrant to its reference mass.");

  auto low_error_calibrants = calibrants;
  for (auto &point : low_error_calibrants) point.error_ppm *= 0.1;
  const auto low_error_correction = fit_baf_mass_correction(low_error_calibrants);
  if (low_error_correction.applied)
    throw std::runtime_error("BAF correction was applied despite acquisition error below the threshold.");

  std::cout << "Bruker TSF and BAF calibration contracts passed.\n";
  return 0;
}
