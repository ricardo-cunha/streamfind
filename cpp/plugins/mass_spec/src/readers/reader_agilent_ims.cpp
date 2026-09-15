#include "readers/reader_agilent.hpp"

#include <algorithm>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace mass_spec::reader::agilent
{
namespace detail_ims_reader
{
std::vector<int> normalize(std::vector<int> indices, const std::size_t count)
{
  if (indices.empty())
  {
    indices.resize(count);
    std::iota(indices.begin(), indices.end(), 0);
  }
  for (const auto index : indices)
    if (index < 0 || static_cast<std::size_t>(index) >= count)
      throw std::out_of_range("Agilent IMS spectrum index is out of range: " + std::to_string(index));
  return indices;
}
}

class AgilentImsReader final : public MASS_SPEC_READER
{
public:
  explicit AgilentImsReader(const std::string &path) : MASS_SPEC_READER(path), records_(read_ims_scan_records(path)), chromatograms_(read_dad_chromatograms(path)) {}
  int get_number_spectra() override { return static_cast<int>(records_.size()); }
  int get_number_chromatograms() override { return static_cast<int>(chromatograms_.size()); }
  int get_number_spectra_binary_arrays() override { return get_number_spectra() * 2; }
  std::string get_format() override { return "AgilentMassHunterD"; }
  std::string get_type() override { return "MS"; }
  std::string get_time_stamp() override { return {}; }
  std::vector<int> get_polarity() override { return std::vector<int>(records_.size(), 1); }
  std::vector<int> get_mode() override { return std::vector<int>(records_.size(), 0); }
  std::vector<int> get_level() override { return std::vector<int>(records_.size(), 1); }
  std::vector<int> get_configuration() override { return std::vector<int>(records_.size(), 0); }
  float get_min_mz() override { return records_.empty() ? 0.0f : static_cast<float>(records_.front().peak_min_x); }
  float get_max_mz() override { return records_.empty() ? 0.0f : static_cast<float>(records_.front().peak_max_x); }
  float get_start_rt() override { return records_.empty() ? 0.0f : static_cast<float>(records_.front().scan_time_minutes * 60.0); }
  float get_end_rt() override { return records_.empty() ? 0.0f : static_cast<float>(records_.back().scan_time_minutes * 60.0); }
  bool has_ion_mobility() override { return true; }
  MASS_SPEC_SUMMARY get_summary() override
  {
    MASS_SPEC_SUMMARY summary{}; summary.file_name = std::filesystem::path(file_).filename().string(); summary.file_path = file_; summary.format = get_format(); summary.type = get_type(); summary.number_spectra = get_number_spectra(); summary.number_chromatograms = get_number_chromatograms(); summary.number_spectra_binary_arrays = get_number_spectra_binary_arrays(); summary.min_mz = get_min_mz(); summary.max_mz = get_max_mz(); summary.start_rt = get_start_rt(); summary.end_rt = get_end_rt(); summary.has_ion_mobility = true; summary.level = get_level(); summary.mode = get_mode(); summary.polarity = get_polarity(); return summary;
  }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { return detail_ims_reader::normalize(std::move(indices), records_.size()); }
  std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<int> result; for (const auto index : selected) result.push_back(static_cast<int>(records_[index].scan_id)); return result; }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<int> result; for (const auto index : selected) result.push_back(static_cast<int>(records_[index].profile_point_count)); return result; }
  std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { return std::vector<int>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 1); }
  std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return std::vector<int>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0); }
  std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return std::vector<int>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0); }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { return std::vector<int>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 1); }
  std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return std::vector<float>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0.0f); }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return std::vector<float>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0.0f); }
  std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<float> result; for (const auto index : selected) result.push_back(static_cast<float>(records_[index].base_peak_mz)); return result; }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<float> result; for (const auto index : selected) result.push_back(static_cast<float>(records_[index].base_peak_abundance)); return result; }
  std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<float> result; for (const auto index : selected) result.push_back(static_cast<float>(records_[index].tic)); return result; }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<float> result; for (const auto index : selected) result.push_back(static_cast<float>(records_[index].scan_time_minutes * 60.0)); return result; }
  std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); std::vector<float> result; for (const auto index : selected) result.push_back(static_cast<float>(records_[index].mobility)); return result; }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return std::vector<int>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0); }
  std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return std::vector<float>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0.0f); }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return std::vector<float>(detail_ims_reader::normalize(std::move(indices), records_.size()).size(), 0.0f); }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
  {
    const auto selected = detail_ims_reader::normalize(std::move(indices), records_.size()); MASS_SPEC_SPECTRA_HEADERS result; result.resize_all(selected.size());
    for (std::size_t output = 0; output < selected.size(); ++output) { const auto &record = records_[selected[output]]; result.index[output] = static_cast<int>(selected[output]); result.scan[output] = static_cast<int>(record.scan_id); result.array_length[output] = static_cast<int>(record.profile_point_count); result.level[output] = 1; result.polarity[output] = 1; result.rt[output] = static_cast<float>(record.scan_time_minutes * 60.0); result.mobility[output] = static_cast<float>(record.mobility); result.tic[output] = static_cast<float>(record.tic); result.bpint[output] = static_cast<float>(record.base_peak_abundance); result.bpmz[output] = static_cast<float>(record.base_peak_mz); }
    return result;
  }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override
  {
    if (indices.empty()) { indices.resize(chromatograms_.size()); std::iota(indices.begin(), indices.end(), 0); }
    MASS_SPEC_CHROMATOGRAMS_HEADERS out; out.resize_all(indices.size());
    for (std::size_t output = 0; output < indices.size(); ++output) { const auto &chromatogram = chromatograms_.at(static_cast<std::size_t>(indices[output])); out.index[output] = indices[output]; out.chromatogram_id[output] = chromatogram.id; out.array_length[output] = static_cast<int>(chromatogram.time.size()); out.signal_type[output] = chromatogram.signal_type; out.chromatogram_type[output] = chromatogram.chromatogram_type; out.detector[output] = chromatogram.detector; out.channel[output] = chromatogram.channel; out.units[output] = chromatogram.units; out.wavelength_nm[output] = chromatogram.wavelength_nm; out.interval_ms[output] = chromatogram.interval_ms; if (!chromatogram.time.empty()) { out.start_time[output] = chromatogram.time.front(); out.end_time[output] = chromatogram.time.back(); } }
    return out;
  }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> result; for (const auto index : detail_ims_reader::normalize(std::move(indices), records_.size())) result.push_back(get_spectrum(static_cast<int>(index)).binary_data); return result; }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override { if (indices.empty()) { indices.resize(chromatograms_.size()); std::iota(indices.begin(), indices.end(), 0); } std::vector<std::vector<std::vector<float>>> result; for (const auto index : indices) { const auto &chromatogram = chromatograms_.at(static_cast<std::size_t>(index)); result.push_back({chromatogram.time, chromatogram.intensity}); } return result; }
  std::vector<std::vector<std::string>> get_software() override { return {}; }
  std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
  {
    trace_spectrum_decode(index);
    if (index < 0 || static_cast<std::size_t>(index) >= records_.size()) return {};
    const auto &record = records_[static_cast<std::size_t>(index)]; const auto profile = read_ims_profile_spectrum(file_, record); MASS_SPEC_SPECTRUM result{}; result.index = index; result.scan = static_cast<int>(record.scan_id); result.array_length = static_cast<int>(profile.mz.size()); result.level = 1; result.polarity = 1; result.rt = static_cast<float>(record.scan_time_minutes * 60.0); result.mobility = static_cast<float>(record.mobility); result.binary_arrays_count = 2; result.binary_names = {"m/z", "intensity"}; result.binary_data = {profile.mz, profile.intensity}; result.lowmz = profile.mz.empty() ? 0.0f : profile.mz.front(); result.highmz = profile.mz.empty() ? 0.0f : profile.mz.back(); result.tic = std::accumulate(profile.intensity.begin(), profile.intensity.end(), 0.0f); auto peak = std::max_element(profile.intensity.begin(), profile.intensity.end()); if (peak != profile.intensity.end()) { result.bpint = *peak; result.bpmz = profile.mz[static_cast<std::size_t>(std::distance(profile.intensity.begin(), peak))]; } return result;
  }
private:
  std::vector<ImsScanRecord> records_;
  std::vector<Chromatogram> chromatograms_;
};

std::unique_ptr<MASS_SPEC_READER> create_ims_reader(const std::string &path)
{
  return std::make_unique<AgilentImsReader>(path);
}
}
