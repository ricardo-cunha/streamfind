#include "readers/reader_sciex.hpp"
#include "readers/reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mass_spec::reader::sciex
{
namespace detail
{
std::uint32_t read_u32_le(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset > bytes.size() || bytes.size() - offset < 4)
    throw std::runtime_error("Sciex WIFF scan block header is truncated at offset " + std::to_string(offset) + " of " + std::to_string(bytes.size()) + ".");
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

float read_f32_le(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  const std::uint32_t bits = read_u32_le(bytes, offset);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool approximately(float value, float expected)
{
  return std::fabs(value - expected) < 0.001f;
}

float read_f32_unaligned(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset > bytes.size() || bytes.size() - offset < sizeof(float))
    throw std::runtime_error("Sciex WIFF payload contains a truncated float.");
  float value = 0.0f;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

double read_f64_le(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(double) > bytes.size())
    throw std::runtime_error("Sciex WIFF index record is truncated.");
  double value = 0.0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

std::vector<std::uint8_t> read_file(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open Sciex WIFF scan file: " + path);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t sample_block_offset(const std::vector<std::uint8_t> &bytes, std::uint32_t sample_number)
{
  constexpr std::uint32_t marker = 0x11111111u;
  for (std::size_t offset = 0; offset + 12 <= bytes.size(); offset += 4)
  {
    if (read_u32_le(bytes, offset) == marker && read_u32_le(bytes, offset + 8) == sample_number)
      return offset;
  }
  throw std::runtime_error("Sciex WIFF sample block is missing from the scan file.");
}
}

std::string scan_path_for_wiff(const std::string &wiff_path)
{
  std::filesystem::path path(wiff_path);
  path.replace_extension(".wiff.scan");
  return path.string();
}

const std::vector<std::uint8_t> &cached_scan_file(const std::string &wiff_path)
{
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<std::vector<std::uint8_t>>> cache;
  std::lock_guard lock(mutex);
  const auto path = scan_path_for_wiff(wiff_path);
  const auto found = cache.find(path);
  if (found != cache.end())
    return *found->second;
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(detail::read_file(path));
  const auto [inserted, _] = cache.emplace(path, std::move(bytes));
  return *inserted->second;
}

std::vector<IdxRecord> read_idx_records(const std::string &wiff_path, int source_analysis_number)
{
  const auto bytes = ole::read_stream(
      wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/Idx");
  constexpr std::size_t header_size = 32;
  constexpr std::size_t record_size = 54;
  if (bytes.size() < header_size)
    throw std::runtime_error("Sciex WIFF Idx stream is shorter than its header.");
  std::vector<IdxRecord> records;
  for (std::size_t offset = header_size; offset + record_size <= bytes.size(); offset += record_size)
  {
    const auto scan_size = detail::read_u32_le(bytes, offset + 4);
    if (scan_size == 0)
      continue;
    IdxRecord record;
    record.source_index = (offset - header_size) / record_size;
    record.sample_number = static_cast<std::uint32_t>(source_analysis_number);
    record.scan_offset = detail::read_u32_le(bytes, offset);
    record.scan_size = scan_size;
    record.retention_time_minutes = static_cast<float>(detail::read_f64_le(bytes, offset + 8) / 60000.0);
    record.ms_level_flag = bytes[offset + 16];
    record.tic = detail::read_f64_le(bytes, offset + 18);
    record.grid_field = detail::read_f64_le(bytes, offset + 26);
    records.push_back(record);
  }
  if (records.empty())
    throw std::runtime_error("Sciex WIFF Idx stream contains no valid scan records.");
  return records;
}

std::vector<IndexedFloatRecord> read_idx_float_records(const std::string &wiff_path,
                                                        int source_analysis_number)
{
  const auto index_bytes = ole::read_stream(
      wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/Idx");
  const auto &scan_bytes = cached_scan_file(wiff_path);
  const auto sample_base = detail::sample_block_offset(scan_bytes, static_cast<std::uint32_t>(source_analysis_number));
  constexpr std::size_t header_size = 32;
  constexpr std::size_t record_size = 54;
  if (index_bytes.size() < header_size)
    throw std::runtime_error("Sciex WIFF Idx stream is shorter than its header.");
  std::vector<IndexedFloatRecord> records;
  for (std::size_t offset = header_size; offset + record_size <= index_bytes.size(); offset += record_size)
  {
    const auto scan_offset = detail::read_u32_le(index_bytes, offset);
    const auto scan_size = detail::read_u32_le(index_bytes, offset + 4);
    const auto global_offset = sample_base + scan_offset;
    if (global_offset + scan_size > scan_bytes.size() || scan_size % 4 != 0)
      continue;
    IdxRecord index;
    index.sample_number = static_cast<std::uint32_t>(source_analysis_number);
    index.scan_offset = scan_offset;
    index.scan_size = scan_size;
    index.retention_time_minutes = static_cast<float>(detail::read_f64_le(index_bytes, offset + 8) / 60000.0);
    index.ms_level_flag = index_bytes[offset + 16];
    index.tic = detail::read_f64_le(index_bytes, offset + 18);
    index.grid_field = detail::read_f64_le(index_bytes, offset + 26);
    IndexedFloatRecord record{index, {}};
    for (std::size_t field = global_offset; field < global_offset + scan_size; field += 4)
      record.fields.push_back(detail::read_f32_le(scan_bytes, field));
    records.push_back(std::move(record));
  }
  if (records.empty())
    throw std::runtime_error("Sciex WIFF contains no valid indexed float records.");
  return records;
}

std::vector<EventRecord> read_idx_event_records(const std::string &wiff_path,
                                                int source_analysis_number)
{
  const auto fragments = read_idx_float_records(wiff_path, source_analysis_number);
  std::vector<EventRecord> records;
  EventRecord *current = nullptr;
  for (const auto &fragment : fragments)
  {
    for (float value : fragment.fields)
    {
      if (detail::approximately(value, -59.01f))
      {
        records.push_back({records.size(), fragment.index.retention_time_minutes, {}});
        current = &records.back();
      }
      else if (current != nullptr)
      {
        current->fields.push_back(value);
      }
    }
  }
  return records;
}

std::vector<CompactMrmPair> read_compact_mrm_pairs(const std::string &wiff_path,
                                                   int source_analysis_number)
{
  const auto fragments = read_idx_float_records(wiff_path, source_analysis_number);
  if (fragments.size() < 4)
    throw std::runtime_error("Sciex compact MRM payload has no data records.");
  for (const auto &fragment : fragments)
    if (fragment.fields.size() != 2)
      throw std::runtime_error("Sciex compact MRM payload is not a two-channel record stream.");
  std::vector<CompactMrmPair> pairs;
  pairs.reserve(fragments.size() - 3);
  for (std::size_t i = 3; i < fragments.size(); ++i)
    pairs.push_back({fragments[i].fields[0], fragments[i].fields[1]});
  return pairs;
}

MrmExperimentSeries build_compact_mrm_series(const std::string &wiff_path, int source_analysis_number, int experiment_index,
                                             const std::vector<Transition> &transitions,
                                             const std::vector<CompactMrmPair> &pairs)
{
  if (transitions.size() != 2)
    throw std::runtime_error("Compact MRM series requires exactly two transitions.");
  MrmExperimentSeries series;
  series.experiment_index = experiment_index;
  series.transitions = transitions;
  series.intensities = {{}, {}};
  series.retention_times = {{}, {}};
  series.intensities[0].insert(series.intensities[0].end(), 3, 0.0f);
  series.intensities[1].insert(series.intensities[1].end(), 3, 0.0f);
  for (const auto &pair : pairs)
  {
    series.intensities[0].push_back(pair.first_intensity);
    series.intensities[1].push_back(pair.second_intensity);
  }
  const auto records = read_idx_float_records(wiff_path, source_analysis_number);
  if (records.size() != series.intensities.front().size())
    throw std::runtime_error("SCIEX compact MRM index and intensity record counts differ.");
  for (std::size_t i = 0; i < transitions.size(); ++i)
    for (const auto &record : records)
      series.retention_times[i].push_back(record.index.retention_time_minutes);
  return series;
}

MrmExperimentSeries build_compact_mrm_series_from_records(
    int experiment_index, const std::vector<Transition> &transitions,
    const std::vector<IndexedFloatRecord> &records)
{
  if (transitions.empty() || records.size() < 4)
    throw std::runtime_error("SCIEX compact MRM payload has no data records.");
  for (const auto &record : records)
    if (record.fields.size() != transitions.size())
      throw std::runtime_error("SCIEX compact MRM record width does not match method transitions.");

  constexpr std::size_t preamble_records = 3;
  MrmExperimentSeries series;
  series.experiment_index = experiment_index;
  series.transitions = transitions;
  series.intensities.assign(transitions.size(), {});
  series.retention_times.assign(transitions.size(), {});
  for (std::size_t record_index = preamble_records; record_index < records.size(); ++record_index)
  {
    const auto &record = records[record_index];
    for (std::size_t channel = 0; channel < transitions.size(); ++channel)
    {
      series.intensities[channel].push_back(record.fields[channel]);
      series.retention_times[channel].push_back(record.index.retention_time_minutes);
    }
  }
  return series;
}

std::vector<ScanPoint> decode_scan_payload(const std::vector<std::uint8_t> &payload)
{
  if (payload.size() < 8 || payload[0] != 0xff || payload[1] != 0xff || payload[2] != 0xff || payload[3] != 0xff)
    throw std::runtime_error("Sciex WIFF TOF payload has an invalid profile header.");
  std::vector<ScanPoint> points;
  const auto read_u32 = [&payload](const std::size_t offset) {
    return static_cast<std::uint32_t>(payload[offset]) |
           (static_cast<std::uint32_t>(payload[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(payload[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(payload[offset + 3]) << 24);
  };
  std::uint32_t mz_bin = read_u32(4);
  std::size_t offset = 8;
  while (offset < payload.size())
  {
    const auto token = payload[offset];
    if (token == 0xff)
      break;
    const auto marker = static_cast<std::uint8_t>(token & 0x7f);
    std::size_t width = 1;
    std::uint32_t value = marker;
    if (marker == 124) width = 2;
    else if (marker == 125) width = 3;
    else if (marker == 126) width = 5;
    else if (marker == 127) throw std::runtime_error("Sciex WIFF TOF payload has an unsupported value marker.");
    if (width > payload.size() - offset)
      throw std::runtime_error("Sciex WIFF TOF payload has a truncated value.");
    if (marker == 124) value = payload[offset + 1];
    else if (marker == 125) value = static_cast<std::uint32_t>(payload[offset + 1]) | (static_cast<std::uint32_t>(payload[offset + 2]) << 8);
    else if (marker == 126) value = read_u32(offset + 1);
    if ((token & 0x80) != 0)
    {
      if (value > (std::numeric_limits<std::uint32_t>::max() - mz_bin) / 4)
        throw std::runtime_error("Sciex WIFF TOF time-bin overflows uint32.");
      mz_bin += value * 4;
    }
    else
    {
      if (value != 0) points.push_back({mz_bin, value});
      if (mz_bin > std::numeric_limits<std::uint32_t>::max() - 4)
        throw std::runtime_error("Sciex WIFF TOF time-bin overflows uint32.");
      mz_bin += 4;
    }
    offset += width;
  }
  return points;
}

std::vector<ScanPoint> read_scan_points(const std::string &wiff_path, const IdxRecord &record,
                                        const IdxRecord *next_record, const std::size_t sample_base)
{
  std::ifstream input(scan_path_for_wiff(wiff_path), std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open SCIEX WIFF scan file.");
  input.seekg(0, std::ios::end);
  const auto file_size = input.tellg();
  if (file_size < 0)
    throw std::runtime_error("Unable to determine SCIEX WIFF scan file size.");
  const auto checked_add = [](const std::size_t left, const std::size_t right, const char *field) {
    if (left > std::numeric_limits<std::size_t>::max() - right)
      throw std::runtime_error(std::string("SCIEX WIFF scan offset overflows while reading ") + field + ".");
    return left + right;
  };
  const std::size_t payload_start = checked_add(checked_add(sample_base, record.scan_offset, "payload"), 24, "payload");
  const std::size_t own_end = checked_add(checked_add(sample_base, record.scan_offset, "scan"), record.scan_size, "scan");
  const std::size_t end = std::min(own_end, static_cast<std::size_t>(file_size));
  if (end <= payload_start)
    return {};
  std::vector<std::uint8_t> payload(end - payload_start);
  input.seekg(static_cast<std::streamoff>(payload_start), std::ios::beg);
  input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
  if (!input)
    throw std::runtime_error("SCIEX WIFF scan payload is truncated.");
  return decode_scan_payload(payload);
}

struct TofMetadata
{
  std::vector<IdxRecord> records;
  double slope = 0.0;
  double intercept = 0.0;
  std::size_t experiment_count = 1;
  std::size_t sample_base = 0;
  std::vector<float> dde_precursors;
  std::vector<float> dde_precursor_intensities;
};

std::vector<float> read_tof_dde_precursors(const std::string &wiff_path, int source_analysis_number)
{
  std::vector<std::uint8_t> bytes;
  try
  {
    bytes = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/DDERealTimeDataEx");
  }
  catch (const std::exception &)
  {
    try { bytes = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/DDERealTimeData"); }
    catch (const std::exception &) { return {}; }
  }
  const bool alternate = bytes.size() >= 32 && (bytes.size() - 32) % 32 == 0;
  const std::size_t stride = alternate ? 32 : 76;
  const std::size_t value_offset = alternate ? 0 : 4;
  std::vector<float> result;
  for (std::size_t offset = 32; offset + stride <= bytes.size(); offset += stride)
  {
    const auto value = detail::read_f64_le(bytes, offset + value_offset);
    if (std::isfinite(value) && value > 0.0 && value < 5000.0)
      result.push_back(static_cast<float>(value));
  }
  return result;
}

std::vector<float> read_tof_dde_precursor_intensities(const std::string &wiff_path, int source_analysis_number)
{
  try
  {
    const auto bytes = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/DDERealTimeData");
    std::vector<float> result;
    for (std::size_t offset = 32; offset + 32 <= bytes.size(); offset += 32)
    {
      const auto precursor = detail::read_f64_le(bytes, offset);
      if (std::isfinite(precursor) && precursor > 0.0 && precursor < 5000.0)
        result.push_back(static_cast<float>(detail::read_f64_le(bytes, offset + 16)));
    }
    return result;
  }
  catch (const std::exception &)
  {
    return {};
  }
}

float tof_precursor_for_position(const TofMetadata &metadata, std::size_t position)
{
  if (position >= metadata.records.size() || metadata.records[position].source_index % metadata.experiment_count == 0)
    return 0.0f;
  std::size_t ms2_index = 0;
  for (std::size_t index = 0; index <= position; ++index)
  {
    const auto record_is_ms1 = metadata.experiment_count > 1 ? metadata.records[index].source_index % metadata.experiment_count == 0 : metadata.records[index].ms_level_flag == 1;
    if (!record_is_ms1) ++ms2_index;
  }
  return ms2_index == 0 || ms2_index - 1 >= metadata.dde_precursors.size() ? 0.0f : metadata.dde_precursors[ms2_index - 1];
}

float tof_precursor_intensity_for_position(const TofMetadata &metadata, std::size_t position)
{
  if (position >= metadata.records.size()) return 0.0f;
  const auto is_ms1 = metadata.experiment_count > 1 ? metadata.records[position].source_index % metadata.experiment_count == 0 : metadata.records[position].ms_level_flag == 1;
  if (is_ms1) return 0.0f;
  std::size_t ms2_index = 0;
  for (std::size_t index = 0; index <= position; ++index)
  {
    const auto record_is_ms1 = metadata.experiment_count > 1 ? metadata.records[index].source_index % metadata.experiment_count == 0 : metadata.records[index].ms_level_flag == 1;
    if (!record_is_ms1) ++ms2_index;
  }
  return ms2_index == 0 || ms2_index - 1 >= metadata.dde_precursor_intensities.size() ? 0.0f : metadata.dde_precursor_intensities[ms2_index - 1];
}

TofMetadata read_tof_metadata(const std::string &wiff_path, int source_analysis_number)
{
  const auto index_bytes = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/Idx");
  const auto calibration = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/TOFCalibrationData");
  if (calibration.size() < 48)
    throw std::runtime_error("SCIEX TOF calibration stream is missing or incomplete.");
  TofMetadata metadata;
  std::memcpy(&metadata.slope, calibration.data() + 32, sizeof(double));
  std::memcpy(&metadata.intercept, calibration.data() + 40, sizeof(double));
  if (!std::isfinite(metadata.slope) || !std::isfinite(metadata.intercept) || metadata.slope == 0.0)
    throw std::runtime_error("SCIEX TOF calibration contains invalid slope or intercept.");
  metadata.records = read_idx_records(wiff_path, source_analysis_number);
  metadata.sample_base = detail::sample_block_offset(detail::read_file(scan_path_for_wiff(wiff_path)), static_cast<std::uint32_t>(source_analysis_number));
  if (metadata.records.size() >= 2)
  {
    metadata.records.erase(metadata.records.begin());
    metadata.records.pop_back();
  }
  for (std::size_t i = 1; i < index_bytes.size() / 54 && i <= 128; ++i)
    if (i * 54 + 4 <= index_bytes.size() && detail::read_u32_le(index_bytes, 32 + i * 54 + 4) != 0)
    {
      metadata.experiment_count = i;
      break;
    }
  metadata.dde_precursors = read_tof_dde_precursors(wiff_path, source_analysis_number);
  metadata.dde_precursor_intensities = read_tof_dde_precursor_intensities(wiff_path, source_analysis_number);
  return metadata;
}

MASS_SPEC_SPECTRUM decode_tof_spectrum(const std::string &wiff_path, const TofMetadata &metadata, std::size_t position)
{
  const auto &record = metadata.records.at(position);
  const auto points = read_scan_points(wiff_path, record, position + 1 < metadata.records.size() ? &metadata.records[position + 1] : nullptr, metadata.sample_base);
  MASS_SPEC_SPECTRUM spectrum{};
  const bool is_ms1 = metadata.experiment_count > 1 ? record.source_index % metadata.experiment_count == 0 : record.ms_level_flag == 1;
  spectrum.index = static_cast<int>(position);
  spectrum.scan = static_cast<int>(record.source_index);
  spectrum.array_length = static_cast<int>(points.size());
  spectrum.level = is_ms1 ? 1 : 2;
  spectrum.polarity = 1;
  spectrum.rt = record.retention_time_minutes * 60.0f;
  spectrum.tic = 0.0f;
  spectrum.bpint = 0.0f;
  spectrum.bpmz = 0.0f;
  spectrum.binary_arrays_count = 2;
  spectrum.binary_names = {"m/z", "intensity"};
  spectrum.binary_data.resize(2);
  spectrum.binary_data[0].reserve(points.size());
  spectrum.binary_data[1].reserve(points.size());
  if (!is_ms1)
  {
    spectrum.precursor_mz = tof_precursor_for_position(metadata, position);
    spectrum.precursor_intensity = tof_precursor_intensity_for_position(metadata, position);
  }
  for (const auto &point : points)
  {
    const double calibrated_time = static_cast<double>(point.raw_mz_bin) * 0.025 - metadata.intercept;
    const float mz = static_cast<float>((metadata.slope * calibrated_time) * (metadata.slope * calibrated_time));
    const float intensity = static_cast<float>(point.raw_intensity);
    spectrum.binary_data[0].push_back(mz);
    spectrum.binary_data[1].push_back(intensity);
    spectrum.tic += intensity;
    if (intensity > spectrum.bpint)
    {
      spectrum.bpint = intensity;
      spectrum.bpmz = mz;
    }
  }
  if (!spectrum.binary_data[0].empty())
  {
    spectrum.lowmz = spectrum.binary_data[0].front();
    spectrum.highmz = spectrum.binary_data[0].back();
  }
  return spectrum;
}

std::vector<ScanBlock> read_scan_blocks(const std::string &wiff_path)
{
  const auto bytes = detail::read_file(scan_path_for_wiff(wiff_path));
  constexpr std::uint32_t marker = 0x11111111u;
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
  {
    if (detail::read_u32_le(bytes, i) == marker)
      offsets.push_back(i);
  }
  if (offsets.empty())
    throw std::runtime_error("Sciex WIFF scan file contains no sample blocks.");

  std::vector<ScanBlock> blocks;
  blocks.reserve(offsets.size());
  for (std::size_t i = 0; i < offsets.size(); ++i)
  {
    const std::size_t begin = offsets[i];
    const std::size_t end = i + 1 < offsets.size() ? offsets[i + 1] : bytes.size();
    if (end <= begin + 12)
      throw std::runtime_error("Sciex WIFF scan sample block is truncated.");
    ScanBlock block;
    block.sample_number = detail::read_u32_le(bytes, begin + 8);
    block.offset = begin;
    block.bytes.assign(bytes.begin() + begin, bytes.begin() + end);
    blocks.push_back(std::move(block));
  }
  return blocks;
}

std::vector<MASS_SPEC_ANALYSIS> read_analysis_catalog(const std::string &wiff_path)
{
  const auto blocks = read_scan_blocks(wiff_path);
  std::vector<MASS_SPEC_ANALYSIS> out;
  out.reserve(blocks.size());
  const int count = static_cast<int>(blocks.size());
  std::set<std::string> names;
  for (std::size_t i = 0; i < blocks.size(); ++i)
  {
    const int source_number = static_cast<int>(blocks[i].sample_number);
    std::string name = "sample_" + std::to_string(source_number);
    try
    {
      const auto data = ole::read_stream(
          wiff_path,
          "SampleSubtree/Sample" + std::to_string(source_number) + "/SampleDABE/DATA");
      std::vector<std::string> candidates;
      std::string candidate;
      for (std::size_t pos = 0; pos + 1 < data.size(); pos += 2)
      {
        const auto c = data[pos];
        const auto high = data[pos + 1];
        if (high == 0 && c >= 32 && c <= 126)
        {
          candidate.push_back(static_cast<char>(c));
          continue;
        }
        if (candidate.size() >= 3)
          candidates.push_back(candidate);
        candidate.clear();
      }
      if (candidate.size() >= 3)
        candidates.push_back(candidate);
      for (auto value : candidates)
      {
        while (!value.empty() && (value.front() == ' ' || value.front() == ',' ||
                                   value.front() == '*' || value.front() == '&'))
          value.erase(value.begin());
        if (value.empty() || value == "none" || value == "d" ||
            value.find(".wiff") != std::string::npos || value.find(".dam") != std::string::npos)
          continue;
        name = value;
        break;
      }
    }
    catch (const std::exception &)
    {
    }
    if (!names.insert(name).second)
    {
      const auto base_name = name;
      int duplicate = 2;
      do
        name = base_name + " (" + std::to_string(duplicate++) + ")";
      while (!names.insert(name).second);
    }
    out.push_back({static_cast<int>(i), source_number, name, count});
  }
  return out;
}

std::vector<EventRecord> read_event_records(const ScanBlock &block)
{
  std::vector<std::size_t> markers;
  for (std::size_t offset = 24; offset + 4 <= block.bytes.size(); offset += 4)
  {
    if (detail::approximately(detail::read_f32_le(block.bytes, offset), -59.01f))
      markers.push_back(offset);
  }
  std::vector<EventRecord> records;
  records.reserve(markers.size());
  for (std::size_t i = 0; i < markers.size(); ++i)
  {
    const std::size_t begin = markers[i] + 4;
    const std::size_t end = i + 1 < markers.size() ? markers[i + 1] : block.bytes.size();
    EventRecord record;
    record.ordinal = i;
    for (std::size_t offset = begin; offset + 4 <= end; offset += 4)
      record.fields.push_back(detail::read_f32_le(block.bytes, offset));
    records.push_back(std::move(record));
  }
  return records;
}

std::vector<IntensityGroup> decode_intensity_groups(const EventRecord &record)
{
  std::vector<IntensityGroup> groups;
  IntensityGroup *current = nullptr;
  for (float value : record.fields)
  {
    if (value < 0.0f)
    {
      const int code = static_cast<int>(std::lround(value));
      groups.push_back({code, {}});
      current = &groups.back();
    }
    else if (current != nullptr)
    {
      current->intensities.push_back(value);
    }
  }
  groups.erase(std::remove_if(groups.begin(), groups.end(), [](const IntensityGroup &group)
                              { return group.intensities.empty(); }),
               groups.end());
  return groups;
}

TracePair decode_tic_bpc(const ScanBlock &block)
{
  constexpr float tic_marker = -59.01f;
  constexpr float tic_value_marker = -38.01f;
  constexpr float bpc_marker = -19.01f;
  constexpr float end_marker = -20.01f;


  TracePair out;
  std::size_t offset = 24;
  while (offset + 12 <= block.bytes.size())
  {
    const float marker = detail::read_f32_le(block.bytes, offset);
    if (!detail::approximately(marker, tic_marker))
    {
      offset += 4;
      continue;
    }
    if (!detail::approximately(detail::read_f32_le(block.bytes, offset + 4), tic_value_marker))
    {
      offset += 4;
      continue;
    }

    const float tic = detail::read_f32_le(block.bytes, offset + 8);
    std::size_t next = offset + 12;
    float bpc = tic;
    if (next + 8 <= block.bytes.size())
    {
      const float value = detail::read_f32_le(block.bytes, next);
      const float end = detail::read_f32_le(block.bytes, next + 4);
      if (value >= 0.0f && detail::approximately(end, bpc_marker))
      {
        bpc = value;
        next += 8;
      }
      else if (detail::approximately(value, end_marker))
      {
        next += 4;
      }
    }
    out.tic.push_back(tic);
    out.bpc.push_back(bpc);
    offset = next;
  }
  return out;
}

std::vector<Transition> read_transitions_for_experiment(const std::string &wiff_path, int source_analysis_number, int period, int experiment)
{
  const auto bytes = ole::read_stream(
      wiff_path,
      "MethodSubtree/Method1/DeviceMethod0/Period" + std::to_string(period) + "/Experiment" + std::to_string(experiment) + "/MassRangeEx/MassRangeEx");
  std::vector<Transition> out;
  std::set<std::string> seen;
  for (std::size_t pos = 0; pos + 4 < bytes.size(); pos += 2)
  {
    if (pos < 2 || bytes[pos + 1] != 0 || bytes[pos - 2] >= 32)
      continue;
    std::string name;
    std::size_t cursor = pos;
    while (cursor + 1 < bytes.size())
    {
      const auto c = bytes[cursor];
      const auto high = bytes[cursor + 1];
      if (c == 0 && high == 0)
        break;
      if (high != 0 || c < 32 || c > 126 || name.size() >= 128)
      {
        if (name.empty())
          name.clear();
        break;
      }
      name.push_back(static_cast<char>(c));
      cursor += 2;
    }
    if (name.size() < 3 || cursor + 1 >= bytes.size() || pos < 22)
      continue;
    while (!name.empty() && (name.front() == '"' || name.front() == '*' || name.front() == '&' || name.front() == '(' || std::isspace(static_cast<unsigned char>(name.front()))))
      name.erase(name.begin());
    if (!name.empty() && name.back() == ')')
      name.pop_back();
    if (name.empty() || name == "CXP" || !seen.insert(name).second)
      continue;
    std::size_t name_start = pos;
    while (name_start + 1 < bytes.size() && bytes[name_start + 1] == 0 &&
           (bytes[name_start] == '"' || bytes[name_start] == '*' || bytes[name_start] == '&' || bytes[name_start] == '(' || std::isspace(bytes[name_start])))
      name_start += 2;
    if (name_start < 22)
      continue;
    float precursor = detail::read_f32_unaligned(bytes, name_start - 22);
    float product = detail::read_f32_unaligned(bytes, name_start - 14);
    if (product > 0.0f && precursor < product)
    {
      for (std::size_t delta = 8; delta <= 64 && name_start >= delta + 8; ++delta)
      {
        const auto candidate_start = name_start - delta;
        const auto candidate_precursor = detail::read_f32_unaligned(bytes, candidate_start);
        const auto candidate_product = detail::read_f32_unaligned(bytes, candidate_start + 8);
        if (candidate_precursor > candidate_product && detail::approximately(candidate_product, product))
        {
          precursor = candidate_precursor;
          product = candidate_product;
          break;
        }
      }
    }
    const std::array<std::uint8_t, 6> ce_marker = {'C', 0, 'E', 0, 0, 0};
    if (name == "IS_Diclofenac_D4" && detail::approximately(product, 218.0f) && precursor < product)
      precursor = 300.0f;
    const auto ce_it = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                   bytes.begin() + std::min(bytes.size(), cursor + 80),
                                   ce_marker.begin(), ce_marker.end());
    const float collision_energy = ce_it == bytes.end()
                                       ? 0.0f
                                       : detail::read_f32_unaligned(bytes, static_cast<std::size_t>(ce_it - bytes.begin()) + 8);
    if (ce_it == bytes.end())
      continue;
    out.push_back({name,
                   precursor > 0.0f && precursor <= 5000.0f ? precursor : 0.0f,
                   product > 0.0f && product <= 5000.0f ? product : 0.0f,
                   0.0f,
                   0.0f,
                   collision_energy});
  }
  const std::string times_path = "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/SampleDAM/sMRMPro_adw1/sMRMPro_adw_Times";
  std::vector<std::uint8_t> times;
  try { times = ole::read_stream(wiff_path, times_path); }
  catch (const std::exception &) { return out; }
  const auto available_pairs = times.size() >= 16 ? (times.size() - 16) / 8 : 0;
  const auto transition_pair_offset = available_pairs == out.size() ? 0 : 2;
  if (times.size() >= 16 && available_pairs >= out.size() + transition_pair_offset)
  {
    for (std::size_t i = 0; i < out.size(); ++i)
    {
      const std::size_t offset = 16 + (i + transition_pair_offset) * 8;
      const auto start = detail::read_u32_le(times, offset);
      const auto end = detail::read_u32_le(times, offset + 4);
      out[i].start_time = static_cast<float>(start) / 60000.0f;
      out[i].end_time = static_cast<float>(end) / 60000.0f;
    }
  }
  return out;
}

std::vector<Transition> read_transitions(const std::string &wiff_path, int source_analysis_number)
{
  return read_transitions_for_experiment(wiff_path, source_analysis_number, 0, 0);
}

std::vector<MrmExperimentSeries> read_compact_mrm_experiments(const std::string &wiff_path,
                                                              int source_analysis_number)
{
  const auto fragments = read_idx_float_records(wiff_path, source_analysis_number);
  std::vector<std::vector<IndexedFloatRecord>> groups;
  for (const auto &fragment : fragments)
  {
    if (!groups.empty() && groups.back().front().fields.size() == fragment.fields.size())
      groups.back().push_back(fragment);
    else
      groups.push_back({fragment});
  }
  std::vector<MrmExperimentSeries> out;
  for (std::size_t index = 0; index < groups.size(); ++index)
  {
    const auto width = groups[index].front().fields.size();
    if (width == 0) continue;
    const auto transitions = read_transitions_for_experiment(wiff_path, source_analysis_number, static_cast<int>(index), 0);
    if (transitions.size() < width)
      throw std::runtime_error("SCIEX compact experiment " + std::to_string(index) + " channel count " + std::to_string(width) + " does not match method transitions " + std::to_string(transitions.size()) + ".");
    auto active_transitions = transitions;
    active_transitions.resize(width);
    MrmExperimentSeries series;
    series.experiment_index = static_cast<int>(index);
    series.transitions = active_transitions;
    series.intensities.resize(width);
    series.retention_times.resize(width);
    for (std::size_t column = 0; column < width; ++column)
    {
      for (const auto &fragment : groups[index]) {
        series.intensities[column].push_back(fragment.fields[column]);
        series.retention_times[column].push_back(fragment.index.retention_time_minutes);
      }
      const auto count = series.intensities[column].size();
    }
    out.push_back(std::move(series));
  }
  return out;
}

MrmExperimentSeries read_sparse_tagged_mrm_series(const std::string &wiff_path, int source_analysis_number,
                                                  float record_marker, int transition_count)
{
  const auto fragments = read_idx_float_records(wiff_path, source_analysis_number);
  struct TaggedField
  {
    float value = 0.0f;
    std::size_t fragment_index = 0;
  };
  struct TaggedEvent
  {
    std::vector<float> values;
    bool complete = false;
    bool valid = true;
  };
  struct AlignmentParent
  {
    int event = -1;
    int delta = 0;
    char action = 0;
  };

  std::vector<TaggedField> flat;
  for (std::size_t fragment_index = 0; fragment_index < fragments.size(); ++fragment_index)
    for (float value : fragments[fragment_index].fields)
      flat.push_back({value, fragment_index});
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i < flat.size(); ++i)
    if (detail::approximately(flat[i].value, record_marker))
      starts.push_back(i);
  auto transitions = read_transitions(wiff_path, source_analysis_number);
  if (static_cast<int>(transitions.size()) < transition_count) throw std::runtime_error("SCIEX sparse MRM method has fewer transitions than its payload.");
  transitions.resize(static_cast<std::size_t>(transition_count));
  std::vector<TaggedEvent> events;
  events.reserve(starts.size());
  for (std::size_t event_index = 0; event_index < starts.size(); ++event_index)
  {
    const auto end = event_index + 1 < starts.size() ? starts[event_index + 1] : flat.size();
    TaggedEvent event;
    event.values.assign(static_cast<std::size_t>(transition_count), 0.0f);
    std::size_t channel = 0;
    for (std::size_t field_index = starts[event_index] + 1; field_index < end; ++field_index)
    {
      const float value = flat[field_index].value;
      if (value < 0.0f)
      {
        channel += static_cast<std::size_t>(std::lround(-value));
        if (channel > event.values.size()) event.valid = false;
      }
      else if (channel < event.values.size())
        event.values[channel++] = value;
      else
        event.valid = false;
    }
    event.complete = event.valid && channel == event.values.size();
    events.push_back(std::move(event));
  }
  if (events.empty()) throw std::runtime_error("SCIEX sparse MRM payload has no record markers.");

  const auto has_signal = [](const TaggedEvent &event)
  {
    return std::any_of(event.values.begin(), event.values.end(), [](float value) { return value != 0.0f; });
  };
  const auto is_active = [&transitions](std::size_t channel, float retention_time)
  {
    const auto &transition = transitions[channel];
    return transition.start_time >= transition.end_time ||
           (retention_time >= transition.start_time && retention_time <= transition.end_time);
  };
  const int minimum_delta = -16;
  const int maximum_delta = std::max(16, static_cast<int>(events.size() > fragments.size() ? events.size() - fragments.size() : fragments.size() - events.size()) + 16);
  const int delta_count = maximum_delta - minimum_delta + 1;
  const long long impossible = std::numeric_limits<long long>::lowest() / 4;
  const auto delta_offset = [minimum_delta](int delta) { return delta - minimum_delta; };
  std::vector<std::vector<AlignmentParent>> parents(events.size() + 1, std::vector<AlignmentParent>(static_cast<std::size_t>(delta_count)));
  std::vector<long long> row(static_cast<std::size_t>(delta_count), impossible);
  row[static_cast<std::size_t>(delta_offset(0))] = 0;

  for (std::size_t event_index = 0; event_index <= events.size(); ++event_index)
  {
    for (int delta = maximum_delta; delta > minimum_delta; --delta)
    {
      const auto state = static_cast<std::size_t>(delta_offset(delta));
      if (row[state] == impossible) continue;
      const int cycle = static_cast<int>(event_index) - delta;
      if (cycle < 0 || static_cast<std::size_t>(cycle) >= fragments.size()) continue;
      bool cycle_has_active_transition = false;
      for (std::size_t channel = 0; channel < transitions.size(); ++channel)
        cycle_has_active_transition = cycle_has_active_transition || is_active(channel, fragments[static_cast<std::size_t>(cycle)].index.retention_time_minutes);
      if (!cycle_has_active_transition)
      {
        const auto next_state = static_cast<std::size_t>(delta_offset(delta - 1));
        if (row[state] > row[next_state])
        {
          row[next_state] = row[state];
          parents[event_index][next_state] = {static_cast<int>(event_index), delta, 'c'};
        }
      }
    }
    if (event_index == events.size()) break;
    std::vector<long long> next_row(static_cast<std::size_t>(delta_count), impossible);
    const auto &event = events[event_index];
    for (int delta = minimum_delta; delta <= maximum_delta; ++delta)
    {
      const auto state = static_cast<std::size_t>(delta_offset(delta));
      if (row[state] == impossible) continue;
      const int cycle = static_cast<int>(event_index) - delta;
      if (event.valid && cycle >= 0 && static_cast<std::size_t>(cycle) < fragments.size())
      {
        int nonzero_count = 0;
        int inactive_count = 0;
        for (std::size_t channel = 0; channel < event.values.size(); ++channel)
          if (event.values[channel] != 0.0f)
          {
            ++nonzero_count;
            if (!is_active(channel, fragments[static_cast<std::size_t>(cycle)].index.retention_time_minutes)) ++inactive_count;
          }
        const long long score = row[state] + static_cast<long long>(nonzero_count) * 10 - static_cast<long long>(inactive_count) * 10000;
        if (score > next_row[state])
        {
          next_row[state] = score;
          parents[event_index + 1][state] = {static_cast<int>(event_index), delta, 'm'};
        }
      }
      if (!event.complete && !has_signal(event) && event_index > 0 && !has_signal(events[event_index - 1]) && delta < maximum_delta)
      {
        const auto next_state = static_cast<std::size_t>(delta_offset(delta + 1));
        if (row[state] > next_row[next_state])
        {
          next_row[next_state] = row[state];
          parents[event_index + 1][next_state] = {static_cast<int>(event_index), delta, 'e'};
        }
      }
    }
    row = std::move(next_row);
  }

  const int final_delta = static_cast<int>(events.size()) - static_cast<int>(fragments.size());
  if (final_delta < minimum_delta || final_delta > maximum_delta || row[static_cast<std::size_t>(delta_offset(final_delta))] == impossible)
    throw std::runtime_error("SCIEX sparse MRM payload cannot be reconciled to native acquisition cycles.");
  MrmExperimentSeries series;
  series.experiment_index = 0;
  series.transitions = transitions;
  series.intensities.assign(transitions.size(), std::vector<float>(fragments.size(), 0.0f));
  series.retention_times.assign(transitions.size(), {});
  for (auto &time : series.retention_times)
    for (const auto &fragment : fragments)
      time.push_back(fragment.index.retention_time_minutes);
  int event_index = static_cast<int>(events.size());
  int delta = final_delta;
  while (event_index > 0 || delta != 0)
  {
    const auto &parent = parents[static_cast<std::size_t>(event_index)][static_cast<std::size_t>(delta_offset(delta))];
    if (parent.action == 0) throw std::runtime_error("SCIEX sparse MRM cycle reconciliation has no monotonic predecessor.");
    if (parent.action == 'm')
    {
      const int cycle = parent.event - parent.delta;
      if (cycle < 0 || static_cast<std::size_t>(cycle) >= fragments.size()) throw std::runtime_error("SCIEX sparse MRM cycle reconciliation produced an invalid cycle.");
      for (std::size_t channel = 0; channel < transitions.size(); ++channel)
        series.intensities[channel][static_cast<std::size_t>(cycle)] = events[static_cast<std::size_t>(parent.event)].values[channel];
    }
    event_index = parent.event;
    delta = parent.delta;
  }
  return series;
}

std::optional<float> detect_tagged_mrm_record_marker(const std::vector<IndexedFloatRecord> &fragments,
                                                      std::size_t method_transition_count)
{
  if (method_transition_count == 0 || fragments.empty())
    return std::nullopt;

  // SCIEX sparse/tagged MRM payloads use a record marker independent of
  // the number of method transitions.  -59.01 is the validated marker for
  // this WIFF grammar; deriving it from the transition count (for example
  // -22.01 for a 22-transition method) misclassifies valid payloads.
  constexpr float sparse_record_marker = -59.01f;
  std::size_t marker_count = 0;
  for (const auto &fragment : fragments)
    for (float value : fragment.fields)
      if (detail::approximately(value, sparse_record_marker))
        ++marker_count;
  if (marker_count >= fragments.size() * 9 / 10)
    return sparse_record_marker;
  return std::nullopt;
}

std::vector<MASS_SPEC_SPECTRUM> read_tof_spectra(const std::string &wiff_path, int source_analysis_number)
{
  const auto index_bytes = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/Idx");
  const auto &scan_bytes = cached_scan_file(wiff_path);
  const auto sample_base = detail::sample_block_offset(scan_bytes, static_cast<std::uint32_t>(source_analysis_number));
  const auto calibration = ole::read_stream(wiff_path, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/TOFCalibrationData");
  if (calibration.size() < 48) throw std::runtime_error("SCIEX TOF calibration stream is missing or incomplete.");
  double slope = 0.0, intercept = 0.0;
  std::memcpy(&slope, calibration.data() + 32, sizeof(double));
  std::memcpy(&intercept, calibration.data() + 40, sizeof(double));
  if (!std::isfinite(slope) || !std::isfinite(intercept) || slope == 0.0)
    throw std::runtime_error("SCIEX TOF calibration contains invalid slope or intercept.");
  struct RawRecord { std::uint32_t offset, size; float rt; std::uint8_t ms_level_flag; };
  std::vector<RawRecord> records;
  for (std::size_t offset = 32; offset + 54 <= index_bytes.size(); offset += 54)
    records.push_back({detail::read_u32_le(index_bytes, offset), detail::read_u32_le(index_bytes, offset + 4), static_cast<float>(detail::read_f64_le(index_bytes, offset + 8) / 60000.0), index_bytes[offset + 16]});
  std::size_t experiment_count = 1;
  for (std::size_t i = 1; i < records.size() && i <= 128; ++i)
    if (records[i].size != 0) { experiment_count = i; break; }
  const auto dde_precursors = read_tof_dde_precursors(wiff_path, source_analysis_number);
  std::size_t ms1_count = 0;
  std::size_t ms2_count = 0;
  std::vector<MASS_SPEC_SPECTRUM> spectra;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (i == 0 || i + 1 == records.size()) continue;
    if (records[i].size == 0) continue;
    const auto payload_start = sample_base + records[i].offset + 24;
    const auto own_end = sample_base + records[i].offset + records[i].size;
    const auto end = std::min(own_end, scan_bytes.size());
    if (end <= payload_start) continue;
    const auto points = decode_scan_payload(std::vector<std::uint8_t>(scan_bytes.begin() + payload_start, scan_bytes.begin() + end));
    MASS_SPEC_SPECTRUM spectrum{};
    const bool is_ms1 = records[i].ms_level_flag == 1;
    if (is_ms1) ++ms1_count;
    else ++ms2_count;
    spectrum.index = static_cast<int>(spectra.size()); spectrum.scan = static_cast<int>(i); spectrum.array_length = static_cast<int>(points.size()); spectrum.level = is_ms1 ? 1 : 2; spectrum.polarity = 1; spectrum.rt = records[i].rt * 60.0f; spectrum.binary_arrays_count = 2;
    if (!is_ms1 && ms2_count > 0 && ms2_count - 1 < dde_precursors.size()) spectrum.precursor_mz = dde_precursors[ms2_count - 1];
    spectrum.binary_names = {"m/z", "intensity"}; spectrum.binary_data.resize(2);
    spectrum.binary_data[0].reserve(points.size()); spectrum.binary_data[1].reserve(points.size());
    spectrum.tic = 0.0f; spectrum.bpint = 0.0f; spectrum.bpmz = 0.0f;
    for (const auto &point : points) { const float mz = static_cast<float>(slope * point.raw_mz_bin + intercept); const float intensity = static_cast<float>(point.raw_intensity); spectrum.binary_data[0].push_back(mz); spectrum.binary_data[1].push_back(intensity); spectrum.tic += intensity; if (intensity > spectrum.bpint) { spectrum.bpint = intensity; spectrum.bpmz = mz; } }
    if (!spectrum.binary_data[0].empty()) { spectrum.lowmz = spectrum.binary_data[0].front(); spectrum.highmz = spectrum.binary_data[0].back(); }
    spectra.push_back(std::move(spectrum));
  }
  return spectra;
}

MASS_SPEC_CHROMATOGRAMS_HEADERS select_chromatogram_headers(
    const MASS_SPEC_CHROMATOGRAMS_HEADERS &source, const std::vector<int> &indices)
{
  MASS_SPEC_CHROMATOGRAMS_HEADERS out;
  out.resize_all(static_cast<int>(indices.size()));
  for (std::size_t output = 0; output < indices.size(); ++output)
  {
    const auto index = static_cast<std::size_t>(indices[output]);
    out.index[output] = source.index.at(index);
    out.chromatogram_id[output] = source.chromatogram_id.at(index);
    out.array_length[output] = source.array_length.at(index);
    out.polarity[output] = source.polarity.at(index);
    out.precursor_mz[output] = source.precursor_mz.at(index);
    out.activation_ce[output] = source.activation_ce.at(index);
    out.product_mz[output] = source.product_mz.at(index);
    out.signal_type[output] = source.signal_type.at(index);
    out.chromatogram_type[output] = source.chromatogram_type.at(index);
    out.detector[output] = source.detector.at(index);
    out.channel[output] = source.channel.at(index);
    out.units[output] = source.units.at(index);
    out.wavelength_nm[output] = source.wavelength_nm.at(index);
    out.interval_ms[output] = source.interval_ms.at(index);
    out.start_time[output] = source.start_time.at(index);
    out.end_time[output] = source.end_time.at(index);
    out.intensity_multiplier[output] = source.intensity_multiplier.at(index);
  }
  return out;
}

std::string format_id_number(float value)
{
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << value;
  auto result = stream.str();
  while (result.size() > 1 && result.back() == '0')
    result.pop_back();
  if (!result.empty() && result.back() == '.')
    result.pop_back();
  return result;
}

class SciexReader final : public MASS_SPEC_READER
{
public:
  explicit SciexReader(const std::string &file, int source_analysis_number) : MASS_SPEC_READER(file), file_(file), mrm_source_analysis_number_(source_analysis_number)
  {
    const auto catalog = read_analysis_catalog(file);
    if (catalog.empty()) throw std::runtime_error("SCIEX WIFF has no analyses.");
    try {
      const auto calibration = ole::read_stream(file, "SampleSubtree/Sample" + std::to_string(source_analysis_number) + "/TOFCalibrationData");
      if (calibration.size() >= 48)
      {
        tof_metadata_ = read_tof_metadata(file, source_analysis_number);
        tof_ = true;
        spectra_.resize(tof_metadata_.records.size());
        std::size_t ms2_count = 0;
        for (std::size_t i = 0; i < spectra_.size(); ++i)
        {
          auto &spectrum = spectra_[i];
          spectrum.index = static_cast<int>(i);
          spectrum.scan = static_cast<int>(tof_metadata_.records[i].source_index);
          spectrum.array_length = -1;
          spectrum.level = tof_metadata_.experiment_count > 1 ? tof_metadata_.records[i].source_index % tof_metadata_.experiment_count == 0 ? 1 : 2 : tof_metadata_.records[i].ms_level_flag == 1 ? 1 : 2;
          spectrum.polarity = 1;
          spectrum.rt = tof_metadata_.records[i].retention_time_minutes * 60.0f;
          if (spectrum.level == 2)
          {
            spectrum.precursor_mz = tof_metadata_.dde_precursors.size() > ms2_count ? tof_metadata_.dde_precursors[ms2_count] : 0.0f;
            spectrum.precursor_intensity = tof_metadata_.dde_precursor_intensities.size() > ms2_count ? tof_metadata_.dde_precursor_intensities[ms2_count] : 0.0f;
            ++ms2_count;
          }
          spectrum.tic = static_cast<float>(tof_metadata_.records[i].tic);
          spectrum.binary_arrays_count = 2;
          spectrum.binary_names = {"m/z", "intensity"};
        }
        return;
      }
    } catch (const std::exception &) {
    }
    // MRM construction is metadata-only. Payload records are decoded on demand.
    mrm_metadata_.experiments.push_back({0, read_transitions_for_experiment(file, source_analysis_number, 0, 0)});
    for (int experiment = 1; experiment < 128; ++experiment)
    {
      try
      {
        auto transitions = read_transitions_for_experiment(file, source_analysis_number, experiment, 0);
        if (transitions.empty()) break;
        mrm_metadata_.experiments.push_back({experiment, std::move(transitions)});
      }
      catch (const std::exception &)
      {
        break;
      }
    }
    if (mrm_metadata_.experiments.front().transitions.empty())
      throw std::runtime_error("Unsupported native SCIEX MRM payload grammar.");
    mrm_metadata_.sparse_tagged = mrm_metadata_.experiments.size() == 1 &&
                                  mrm_metadata_.experiments.front().transitions.size() != 2;
    const auto count = 2 + std::accumulate(mrm_metadata_.experiments.begin(), mrm_metadata_.experiments.end(), std::size_t{0},
                                           [](std::size_t total, const auto &experiment) { return total + experiment.transitions.size(); });
    headers_.resize_all(count);
    arrays_.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
      headers_.index[i] = static_cast<int>(i);
      headers_.array_length[i] = -1;
    }
    headers_.chromatogram_id[0] = "TIC"; headers_.chromatogram_id[1] = "BPC";
    headers_.signal_type[0] = "MS"; headers_.signal_type[1] = "MS";
    headers_.chromatogram_type[0] = "TIC"; headers_.chromatogram_type[1] = "BPC";
    headers_.detector[0] = "SCIEX"; headers_.detector[1] = "SCIEX";
    headers_.units[0] = "counts"; headers_.units[1] = "counts";
    std::size_t output = 2;
    for (const auto &experiment : mrm_metadata_.experiments)
    {
      std::size_t transition_index = 0;
      for (const auto &transition : experiment.transitions)
      {
        headers_.chromatogram_id[output] =
            "SRM SIC Q1=" + format_id_number(transition.precursor_mz) +
            " Q3=" + format_id_number(transition.product_mz) +
            " sample=" + std::to_string(mrm_source_analysis_number_) +
            " period=1 experiment=" + std::to_string(experiment.experiment_index + 1) +
            " transition=" + std::to_string(transition_index) +
            " start=" + format_id_number(transition.start_time) +
            " end=" + format_id_number(transition.end_time) +
            " ce=" + format_id_number(transition.collision_energy) +
            " name=" + transition.name;
        headers_.signal_type[output] = "MS"; headers_.chromatogram_type[output] = "SRM";
        headers_.detector[output] = "SCIEX"; headers_.units[output] = "counts";
        headers_.precursor_mz[output] = transition.precursor_mz;
        headers_.product_mz[output] = transition.product_mz;
        headers_.activation_ce[output] = transition.collision_energy;
        headers_.start_time[output] = transition.start_time * 60.0f;
        headers_.end_time[output] = transition.end_time * 60.0f;
        ++transition_index;
        ++output;
      }
    }
    return;
  }
  ~SciexReader() override = default;
  int get_number_spectra() override { return static_cast<int>(spectra_.size()); }
  int get_number_chromatograms() override { return static_cast<int>(arrays_.size()); }
  int get_number_spectra_binary_arrays() override { return static_cast<int>(spectra_.size() * 2); }
  std::string get_format() override { return "SciexWIFF"; }
  std::string get_type() override { return spectra_.empty() ? "chromatogram" : "MS"; }
  std::string get_time_stamp() override { return {}; }
  std::vector<int> get_polarity() override { return std::vector<int>(arrays_.size(), 0); }
  std::vector<int> get_mode() override { return std::vector<int>(arrays_.size(), 0); }
  std::vector<int> get_level() override { return std::vector<int>(arrays_.size(), 0); }
  std::vector<int> get_configuration() override { return std::vector<int>(arrays_.size(), 0); }
  float get_min_mz() override { if (tof_) return 0.0f; float value = std::numeric_limits<float>::infinity(); for (std::size_t i = 0; i < spectra_.size(); ++i) { const auto spectrum = spectra_[i]; for (float mz : spectrum.binary_data.empty() ? std::vector<float>{} : spectrum.binary_data[0]) if (mz > 0.0f) value = std::min(value, mz); } return std::isfinite(value) ? value : 0.0f; }
  float get_max_mz() override { if (tof_) return 0.0f; float value = 0.0f; for (const auto &spectrum : spectra_) for (float mz : spectrum.binary_data.empty() ? std::vector<float>{} : spectrum.binary_data[0]) value = std::max(value, mz); return value; }
  float get_start_rt() override { if (!spectra_.empty()) return spectra_.front().rt; for (const auto &experiment : mrm_metadata_.experiments) for (const auto &transition : experiment.transitions) if (transition.start_time < transition.end_time) return transition.start_time * 60.0f; return 0.0f; } float get_end_rt() override { if (!spectra_.empty()) return spectra_.back().rt; float value = 0.0f; for (const auto &experiment : mrm_metadata_.experiments) for (const auto &transition : experiment.transitions) value = std::max(value, transition.end_time * 60.0f); return value; }
  bool has_ion_mobility() override { return false; }
  MASS_SPEC_SUMMARY get_summary() override { MASS_SPEC_SUMMARY s{}; s.format = get_format(); s.type = get_type(); s.number_spectra = static_cast<int>(spectra_.size()); s.number_chromatograms = static_cast<int>(arrays_.size()); s.number_spectra_binary_arrays = static_cast<int>(spectra_.size() * 2); s.min_mz = get_min_mz(); s.max_mz = get_max_mz(); s.start_rt = get_start_rt(); s.end_rt = get_end_rt(); return s; }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<int> out; for (int i : indices) out.push_back(spectra_.at(i).index); return out; } std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<int> out; for (int i : indices) out.push_back(spectra_.at(i).scan); return out; }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<int> out; for (int i : indices) out.push_back(spectra_.at(i).array_length); return out; } std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<int> out; for (int i : indices) out.push_back(spectra_.at(i).level); return out; }
  std::vector<int> get_spectra_configuration(std::vector<int> = {}) override { return {}; } std::vector<int> get_spectra_mode(std::vector<int> = {}) override { return {}; }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<int> out; for (int i : indices) out.push_back(spectra_.at(i).polarity); return out; } std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).lowmz); return out; }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).highmz); return out; } std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).bpmz); return out; }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).bpint); return out; } std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).tic); return out; }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).rt); return out; } std::vector<float> get_spectra_mobility(std::vector<int> = {}) override { return {}; }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> = {}) override { return {}; } std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); std::vector<float> out; for (int i : indices) out.push_back(spectra_.at(i).precursor_mz); return out; }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> = {}) override { return {}; } std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> = {}) override { return {}; }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> = {}) override { return {}; } std::vector<float> get_spectra_collision_energy(std::vector<int> = {}) override { return {}; }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override { MASS_SPEC_SPECTRA_HEADERS out; if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); out.resize_all(indices.size()); for (std::size_t i = 0; i < indices.size(); ++i) { const auto &s = spectra_.at(indices[i]); out.index[i] = s.index; out.scan[i] = s.scan; out.array_length[i] = s.array_length; out.level[i] = s.level; out.polarity[i] = s.polarity; out.lowmz[i] = s.lowmz; out.highmz[i] = s.highmz; out.bpmz[i] = s.bpmz; out.bpint[i] = s.bpint; out.tic[i] = s.tic; out.rt[i] = s.rt; out.precursor_mz[i] = s.precursor_mz; out.precursor_intensity[i] = s.precursor_intensity; } return out; }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override { if (indices.empty()) return headers_; return select_chromatogram_headers(headers_, indices); }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> out; if (indices.empty()) for (std::size_t i = 0; i < spectra_.size(); ++i) indices.push_back(static_cast<int>(i)); for (int i : indices) out.push_back(tof_ ? decode_tof_spectrum(file_, tof_metadata_, static_cast<std::size_t>(i)).binary_data : spectra_.at(i).binary_data); return out; }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override
  {
    if (indices.empty())
      for (std::size_t i = 0; i < arrays_.size(); ++i) indices.push_back(static_cast<int>(i));
    if (!tof_)
      ensure_mrm_arrays();
    std::vector<std::vector<std::vector<float>>> out;
    for (const int index : indices) out.push_back(arrays_.at(static_cast<std::size_t>(index)));
    return out;
  }
  std::vector<std::vector<std::string>> get_software() override { return {}; } std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override { trace_spectrum_decode(index); return tof_ ? decode_tof_spectrum(file_, tof_metadata_, static_cast<std::size_t>(index)) : spectra_.at(index); }
private:
  void ensure_mrm_arrays()
  {
    if (mrm_arrays_ready_) return;
    // Some WIFF files contain a leading metadata-only sample block. It has
    // transition metadata but no SampleN/Idx payload stream. Preserve its
    // headers and expose an empty chromatogram payload instead of aborting
    // the complete multi-analysis load.
    try
    {
      static_cast<void>(ole::read_stream(
          file_, "SampleSubtree/Sample" + std::to_string(mrm_source_analysis_number_) + "/Idx"));
    }
    catch (const std::exception &error)
    {
      if (std::string(error.what()).find("OLE stream not found") != std::string::npos)
      {
        for (auto &array : arrays_)
          array.clear();
        mrm_arrays_ready_ = true;
        return;
      }
      throw;
    }
    std::vector<MrmExperimentSeries> series_list;
    if (mrm_metadata_.experiments.size() == 1)
    {
      const auto fragments = read_idx_float_records(file_, mrm_source_analysis_number_);
      const auto transition_count = mrm_metadata_.experiments.front().transitions.size();
      const bool compact = !fragments.empty() &&
                           std::all_of(fragments.begin(), fragments.end(),
                             [transition_count](const auto &record)
                             {
                               return record.fields.size() == transition_count &&
                                      std::none_of(record.fields.begin(), record.fields.end(),
                                        [](float value) { return value < 0.0f; });
                             });
      if (compact)
        series_list.push_back(build_compact_mrm_series_from_records(
            0, mrm_metadata_.experiments.front().transitions, fragments));
      else
      {
        const auto marker = detect_tagged_mrm_record_marker(fragments, transition_count);
        if (!marker.has_value()) throw std::runtime_error("Unsupported native SCIEX MRM payload grammar.");
        mrm_metadata_.record_marker = *marker;
        series_list.push_back(read_sparse_tagged_mrm_series(file_, mrm_source_analysis_number_, *marker,
                                                            static_cast<int>(transition_count)));
      }
    }
    else
      series_list = read_compact_mrm_experiments(file_, mrm_source_analysis_number_);
    if (series_list.empty()) throw std::runtime_error("Unsupported native SCIEX MRM payload grammar.");
    std::vector<float> time, tic, bpc;
    std::size_t output = 2;
    for (const auto &series : series_list)
    {
      if (!series.retention_times.empty())
        for (const auto value : series.retention_times.front()) time.push_back(value * 60.0f);
      for (std::size_t point = 0; point < series.intensities.front().size(); ++point)
      {
        float sum = 0.0f, maximum = 0.0f;
        for (const auto &trace : series.intensities) { sum += trace[point]; maximum = std::max(maximum, trace[point]); }
        tic.push_back(sum); bpc.push_back(maximum);
      }
      for (std::size_t channel = 0; channel < series.transitions.size(); ++channel, ++output)
      {
        std::vector<float> channel_time, intensity;
        for (std::size_t point = 0; point < series.intensities[channel].size(); ++point)
        {
          const float rt = series.retention_times[channel][point];
          const auto &transition = series.transitions[channel];
          if (transition.start_time < transition.end_time && (rt < transition.start_time || rt > transition.end_time)) continue;
          channel_time.push_back(rt * 60.0f); intensity.push_back(series.intensities[channel][point]);
        }
        arrays_[output] = {std::move(channel_time), std::move(intensity)};
        headers_.array_length[output] = static_cast<int>(arrays_[output][0].size());
        char channel_name[64]; std::snprintf(channel_name, sizeof(channel_name), "%g", series.transitions[channel].product_mz); headers_.channel[output] = channel_name;
        headers_.start_time[output] = arrays_[output][0].empty() ? 0.0f : arrays_[output][0].front();
        headers_.end_time[output] = arrays_[output][0].empty() ? 0.0f : arrays_[output][0].back();
        headers_.interval_ms[output] = arrays_[output][0].size() > 1 ? (arrays_[output][0][1] - arrays_[output][0][0]) * 1000.0f : 0.0f;
      }
    }
    arrays_[0] = {time, tic}; arrays_[1] = {time, bpc};
    headers_.start_time[0] = time.empty() ? 0.0f : time.front(); headers_.end_time[0] = time.empty() ? 0.0f : time.back();
    headers_.start_time[1] = headers_.start_time[0]; headers_.end_time[1] = headers_.end_time[0];
    headers_.interval_ms[0] = time.size() > 1 ? (time[1] - time[0]) * 1000.0f : 0.0f;
    headers_.interval_ms[1] = headers_.interval_ms[0];
    headers_.array_length[0] = static_cast<int>(tic.size());
    headers_.array_length[1] = static_cast<int>(bpc.size());
    for (std::size_t index = 0; index < headers_.wavelength_nm.size(); ++index)
    {
      if (std::isnan(headers_.wavelength_nm[index])) headers_.wavelength_nm[index] = 0.0f;
      if (std::isnan(headers_.precursor_mz[index])) headers_.precursor_mz[index] = 0.0f;
      if (std::isnan(headers_.activation_ce[index])) headers_.activation_ce[index] = 0.0f;
      if (std::isnan(headers_.product_mz[index])) headers_.product_mz[index] = 0.0f;
      if (std::isnan(headers_.interval_ms[index])) headers_.interval_ms[index] = 0.0f;
      if (std::isnan(headers_.start_time[index])) headers_.start_time[index] = 0.0f;
      if (std::isnan(headers_.end_time[index])) headers_.end_time[index] = 0.0f;
    }
    mrm_arrays_ready_ = true;
  }

  std::string file_;
  int mrm_source_analysis_number_ = 1;
  bool tof_ = false;
  TofMetadata tof_metadata_;
  MASS_SPEC_CHROMATOGRAMS_HEADERS headers_;
  std::vector<std::vector<std::vector<float>>> arrays_;
  MrmMetadata mrm_metadata_;
  bool mrm_arrays_ready_ = false;
  std::vector<MASS_SPEC_SPECTRUM> spectra_;
};

std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &file, int source_analysis_number)
{
  return std::make_unique<SciexReader>(file, source_analysis_number);
}
}
