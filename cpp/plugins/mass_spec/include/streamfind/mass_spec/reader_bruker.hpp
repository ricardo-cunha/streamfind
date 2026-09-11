#ifndef STREAMFIND_MASS_SPEC_READER_BRUKER_HPP
#define STREAMFIND_MASS_SPEC_READER_BRUKER_HPP

#include "streamfind/mass_spec/reader.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mass_spec::reader::bruker
{
enum class Family
{
  Baf,
  Tsf,
  Unknown
};

struct TsfFrame
{
  std::int64_t id = 0;
  double retention_time = 0.0;
  std::string polarity;
  std::int32_t scan_mode = 0;
  std::int32_t msms_type = 0;
  std::int64_t tims_id = 0;
  double max_intensity = 0.0;
  double summed_intensities = 0.0;
  std::int32_t num_peaks = 0;
  std::int32_t mz_calibration = 0;
  double t1 = 0.0;
  double t2 = 0.0;
  std::int32_t property_group = 0;
};

struct TsfMsMsInfo
{
  std::int64_t frame = 0;
  std::int64_t parent = 0;
  double trigger_mass = 0.0;
  double isolation_width = 0.0;
  std::int32_t precursor_charge = 0;
  double collision_energy = 0.0;
};

struct TsfLineSpectrum
{
  std::vector<double> tof;
  std::vector<double> intensity;
};

struct TsfCalibration
{
  std::int32_t id = 0;
  std::int32_t model_type = 0;
  double digitizer_timebase = 0.0;
  double digitizer_delay = 0.0;
  double t1 = 0.0;
  double t2 = 0.0;
  double dc1 = 0.0;
  double dc2 = 0.0;
  double c0 = 0.0;
  double c1 = 0.0;
  double c2 = 0.0;
  double c3 = 0.0;
  double c4 = 0.0;
  double mz_min = 0.0;
  double mz_max = 0.0;
  std::uint32_t tof_max = 0;
  bool otof_control = false;
};

struct BafLineSpectrum
{
  std::vector<double> coordinate;
  std::vector<double> intensity;
  std::vector<double> width;
};

struct BafProfileSpectrum
{
  std::vector<std::uint32_t> intensity;
};

struct BafSpectrumMetadata
{
  std::int64_t id = 0;
  double retention_time = 0.0;
  std::int32_t acquisition_key = 0;
  std::int64_t parent = 0;
  std::int32_t mz_lower = 0;
  std::int32_t mz_upper = 0;
  double summed_intensity = 0.0;
  double maximum_intensity = 0.0;
  std::int32_t transformator_id = 0;
  std::uint64_t profile_mz_id = 0;
  std::uint64_t profile_intensity_id = 0;
  std::uint64_t line_mz_id = 0;
  std::uint64_t line_intensity_id = 0;
  std::int32_t polarity = 0;
  std::int32_t scan_mode = 0;
  std::int32_t acquisition_mode = 0;
  std::int32_t ms_level = 0;
};

Family detect_family(const std::string &path);
std::vector<TsfFrame> read_tsf_frames(const std::string &path);
std::vector<TsfMsMsInfo> read_tsf_msms_info(const std::string &path);
TsfLineSpectrum read_tsf_line_spectrum(const std::string &path, const TsfFrame &frame);
TsfCalibration read_tsf_calibration(const std::string &path, const TsfFrame &frame);
std::vector<double> tsf_tof_to_mz(const TsfCalibration &calibration, const std::vector<double> &tof);
BafLineSpectrum read_baf_line_spectrum(const std::string &path, std::uint64_t line_array_id);
BafProfileSpectrum read_baf_profile_spectrum(const std::string &path, std::uint64_t profile_array_id);
std::vector<BafSpectrumMetadata> read_baf_spectra_metadata(const std::string &path);
std::unique_ptr<MASS_SPEC_READER> create_tsf_reader(const std::string &path);
std::unique_ptr<MASS_SPEC_READER> create_baf_reader(const std::string &path);
}

#endif
