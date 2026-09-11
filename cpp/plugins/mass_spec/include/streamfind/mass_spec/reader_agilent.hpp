#ifndef STREAMFIND_MASS_SPEC_READER_AGILENT_HPP
#define STREAMFIND_MASS_SPEC_READER_AGILENT_HPP

#include "streamfind/mass_spec/reader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mass_spec::reader::agilent
{
struct ScanRecord
{
  std::uint32_t scan_id = 0;
  std::uint32_t scan_method_id = 0;
  std::uint32_t time_segment_id = 0;
  double scan_time_minutes = 0.0;
  std::int32_t ms_level = 0;
  std::int32_t scan_type = 0;
  double tic = 0.0;
  double base_peak_mz = 0.0;
  double base_peak_value = 0.0;
  std::int32_t calibration_id = 0;
  std::int32_t cycle_number = 0;
  std::uint32_t spectrum_format_id = 0;
  std::uint64_t spectrum_offset = 0;
  std::uint32_t spectrum_byte_count = 0;
  std::uint32_t spectrum_point_count = 0;
  std::uint32_t spectrum_uncompressed_byte_count = 0;
  double spectrum_min_x = 0.0;
  double spectrum_max_x = 0.0;
  double spectrum_min_y = 0.0;
  double spectrum_max_y = 0.0;
  double spectrum_measured_noise = 0.0;
  std::uint32_t centroid_format_id = 0;
  std::uint64_t centroid_offset = 0;
  std::uint32_t centroid_byte_count = 0;
  std::uint32_t centroid_point_count = 0;
  std::uint32_t record_index = 0;
  double precursor_mz = 0.0;
  double precursor_intensity = 0.0;
  double collision_energy = 0.0;
  std::int32_t polarity = 0;
};

struct ProfileSpectrum
{
  std::vector<float> mz;
  std::vector<float> intensity;
};

struct Chromatogram
{
  std::string id;
  std::string signal_type;
  std::string chromatogram_type;
  std::string detector;
  std::string channel;
  std::string units;
  float wavelength_nm = std::numeric_limits<float>::quiet_NaN();
  float interval_ms = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> time;
  std::vector<float> intensity;
};

struct ImsFrameRecord
{
  std::int16_t frame_id = 0;
  std::int16_t frame_method_id = 0;
  std::int16_t time_segment_id = 0;
  std::int64_t actuals_offset = 0;
  std::int16_t cycle_number = 0;
  std::int16_t first_nonzero_drift_bin = 0;
  std::int16_t frag_class = 0;
  float frag_energy = 0.0f;
  double frame_base_abundance = 0.0;
  std::int16_t frame_base_drift_bin = 0;
  std::int32_t frame_base_ms_bin = 0;
  double frame_scan_time_minutes = 0.0;
  double frame_spec_abundance_limit = 0.0;
  double frame_tic = 0.0;
  double ims_field = 0.0;
  double ims_pressure = 0.0;
  double ims_temperature = 0.0;
  double ims_trap_time = 0.0;
  double isolation_start_mz = 0.0;
  double isolation_mz = 0.0;
  double isolation_end_mz = 0.0;
  std::int16_t last_nonzero_drift_bin = 0;
  std::int64_t mass_cal_offset = 0;
  std::int16_t num_transients = 0;
};

struct ImsScanRecord
{
  std::uint32_t scan_id = 0;
  std::int16_t frame_id = 0;
  std::int32_t base_abundance = 0;
  std::int32_t base_ms_bin = 0;
  std::int16_t detector_gain = 0;
  std::int16_t drift_bin = 0;
  std::int32_t first_nonzero_ms_bin = 0;
  std::int32_t last_nonzero_ms_bin = 0;
  double tic = 0.0;
  double base_peak_abundance = 0.0;
  double base_peak_mz = 0.0;
  std::int16_t profile_format_id = 0;
  std::uint32_t profile_byte_count = 0;
  std::uint64_t profile_offset = 0;
  std::uint32_t profile_point_count = 0;
  std::uint32_t profile_full_byte_count = 0;
  std::int16_t peak_format_id = 0;
  std::uint32_t peak_byte_count = 0;
  std::uint64_t peak_offset = 0;
  std::uint32_t peak_point_count = 0;
  double peak_max_x = 0.0;
  double peak_min_x = 0.0;
  double scan_time_minutes = 0.0;
  double mobility = 0.0;
};

bool is_agilent_mass_hunter_directory(const std::string &path);
bool is_agilent_ion_mobility_directory(const std::string &path);
std::vector<ImsFrameRecord> read_ims_frame_records(const std::string &path);
std::vector<ImsScanRecord> read_ims_scan_records(const std::string &path);
ProfileSpectrum read_ims_profile_spectrum(const std::string &path, const ImsScanRecord &record);
std::unique_ptr<MASS_SPEC_READER> create_ims_reader(const std::string &path);
std::vector<ScanRecord> read_scan_records(const std::string &path);
ProfileSpectrum read_profile_spectrum(const std::string &path, const ScanRecord &record);
ProfileSpectrum read_centroid_spectrum(const std::string &path, const ScanRecord &record);
std::vector<Chromatogram> read_dad_chromatograms(const std::string &path);
std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &file);
}

#endif
