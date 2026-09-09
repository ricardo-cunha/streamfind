#include "streamfind/mass_spec/reader_agilent_chemstation.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace mass_spec::reader::agilent_chemstation
{
namespace detail
{
std::uint16_t be16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 2 > bytes.size())
    throw std::runtime_error("Agilent ChemStation binary field is truncated.");
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

std::int16_t be16s(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  return static_cast<std::int16_t>(be16(bytes, offset));
}

std::int32_t be32s(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 4 > bytes.size())
    throw std::runtime_error("Agilent ChemStation 32-bit field is truncated.");
  return (static_cast<std::int32_t>(bytes[offset]) << 24) |
         (static_cast<std::int32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::int32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::int32_t>(bytes[offset + 3]);
}

std::uint32_t le32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 4 > bytes.size())
    throw std::runtime_error("Agilent ChemStation chromatogram field is truncated.");
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t le16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 2 > bytes.size())
    throw std::runtime_error("Agilent ChemStation chromatogram field is truncated.");
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

double be_double(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(double) > bytes.size())
    throw std::runtime_error("Agilent ChemStation chromatogram double is truncated.");
  std::uint64_t bits = 0;
  for (std::size_t index = 0; index < sizeof(double); ++index)
    bits = (bits << 8) | bytes[offset + index];
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double le_double(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(double) > bytes.size())
    throw std::runtime_error("Agilent ChemStation chromatogram double is truncated.");
  std::uint64_t bits = 0;
  for (std::size_t index = 0; index < sizeof(double); ++index)
    bits |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string pascal_ascii(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset >= bytes.size())
    return {};
  const auto length = static_cast<std::size_t>(bytes[offset]);
  if (offset + 1 + length > bytes.size())
    return {};
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1 + length));
}

std::string pascal_utf16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset >= bytes.size())
    return {};
  const auto length = static_cast<std::size_t>(bytes[offset]);
  if (offset + 1 + 2 * length > bytes.size())
    return {};
  std::string value;
  value.reserve(length);
  for (std::size_t index = 0; index < length; ++index)
  {
    const auto code = static_cast<std::uint16_t>(bytes[offset + 1 + 2 * index]) |
                      (static_cast<std::uint16_t>(bytes[offset + 2 + 2 * index]) << 8);
    value.push_back(code < 128 ? static_cast<char>(code) : '?');
  }
  return value;
}

std::vector<float> decode_delta(const std::vector<std::uint8_t> &bytes, std::size_t offset,
                                std::size_t count, bool little_endian)
{
  std::vector<float> values;
  values.reserve(count);
  std::int32_t accumulated = 0;
  for (std::size_t index = 0; index < count; ++index)
  {
    const auto raw = little_endian ? static_cast<std::int16_t>(le16(bytes, offset))
                                   : be16s(bytes, offset);
    offset += 2;
    if (raw == std::numeric_limits<std::int16_t>::min())
    {
      if (offset + 4 > bytes.size())
        throw std::runtime_error("Agilent ChemStation delta chromatogram escape is truncated.");
      accumulated = little_endian ? static_cast<std::int32_t>(le32(bytes, offset)) : be32s(bytes, offset);
      offset += 4;
    }
    else
    {
      accumulated += raw;
    }
    values.push_back(static_cast<float>(accumulated));
  }
  return values;
}

std::vector<float> decode_ch(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  std::vector<float> values;
  while (offset + 2 <= bytes.size() && bytes[offset] == 16 && bytes[offset + 1] != 0)
  {
    const auto count = static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    for (std::size_t index = 0; index < count; ++index)
    {
      const auto raw = detail::be16s(bytes, offset);
      offset += 2;
      if (raw == std::numeric_limits<std::int16_t>::min())
      {
        if (offset + 4 > bytes.size())
          throw std::runtime_error("Agilent ChemStation .ch delta escape is truncated.");
        values.push_back(static_cast<float>(detail::be32s(bytes, offset)));
        offset += 4;
      }
      else
      {
        const auto previous = values.empty() ? 0.0f : values.back();
        values.push_back(previous + raw);
      }
    }
  }
  return values;
}

std::string text(const std::vector<std::uint8_t> &bytes, std::size_t &offset, std::size_t length, bool skip_extra)
{
  if (offset + length + (skip_extra ? 1 : 0) > bytes.size())
    throw std::runtime_error("Agilent ChemStation header is truncated.");
  auto end = offset;
  while (end < offset + length && bytes[end] != 0)
    ++end;
  std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(end));
  offset += length + (skip_extra ? 1 : 0);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.pop_back();
  return value;
}

std::vector<std::uint8_t> read_file(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open Agilent ChemStation file: " + path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::int32_t packed_abundance(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  const auto word = be16(bytes, offset);
  const auto scale = static_cast<std::uint32_t>(bytes[offset] >> 6);
  return static_cast<std::int32_t>((word & 0x3fff) * (1u << (3u * scale)));
}

std::vector<int> normalize(std::vector<int> indices, std::size_t count)
{
  if (indices.empty())
  {
    indices.resize(count);
    std::iota(indices.begin(), indices.end(), 0);
  }
  for (const auto index : indices)
    if (index < 0 || static_cast<std::size_t>(index) >= count)
      throw std::out_of_range("Agilent ChemStation spectrum index is out of range: " + std::to_string(index));
  return indices;
}
}

bool is_chemstation_directory(const std::string &path)
{
  const std::filesystem::path root(path);
  if (!std::filesystem::is_directory(root))
    return false;
  for (const auto &entry : std::filesystem::directory_iterator(root))
  {
    if (!entry.is_regular_file())
      continue;
    std::string name = entry.path().filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (name == "DATA.MS" || name == "MSD1.MS" || name == "MSD2.MS" || name.ends_with(".CH") || name.ends_with(".UV"))
      return true;
  }
  return false;
}

DataFile read_data_file(const std::string &path)
{
  if (!is_chemstation_directory(std::filesystem::path(path).parent_path().string()) &&
      !std::filesystem::is_regular_file(path))
    throw std::runtime_error("Not an Agilent ChemStation data file: " + path);
  const auto bytes = detail::read_file(path);
  if (bytes.size() < 300)
    throw std::runtime_error("Agilent ChemStation data file is too short: " + path);
  std::size_t offset = 1;
  DataFile data_file;
  data_file.path = path;
  detail::text(bytes, offset, 3, true);
  data_file.file_type = detail::text(bytes, offset, 19, true);
  data_file.data_name = detail::text(bytes, offset, 61, true);
  detail::text(bytes, offset, 61, true);
  data_file.operator_name = detail::text(bytes, offset, 29, true);
  data_file.acquisition_date = detail::text(bytes, offset, 29, true);
  data_file.instrument_model = detail::text(bytes, offset, 9, true);
  data_file.inlet = detail::text(bytes, offset, 9, true);
  data_file.method_file = detail::text(bytes, offset, 19, false);
  const auto file_type = detail::be32s(bytes, offset); offset += 4;
  offset += 2 * 4;
  const auto directory_words = detail::be32s(bytes, offset); offset += 4;
  offset += 3 * 4;
  offset += 2;
  data_file.record_count = detail::be32s(bytes, offset); offset += 4;
  data_file.retention_time_start_ms = detail::be32s(bytes, offset); offset += 4;
  data_file.retention_time_end_ms = detail::be32s(bytes, offset); offset += 4;
  offset += 2 * 4;
  if (file_type < -1 || directory_words <= 0 || data_file.record_count < 0)
    throw std::runtime_error("Invalid Agilent ChemStation data header: " + path);
  const auto directory_offset = static_cast<std::uint64_t>(directory_words - 1) * 2;
  const auto directory_bytes = static_cast<std::uint64_t>(data_file.record_count) * 12;
  if (directory_offset > bytes.size() || directory_bytes > bytes.size() - directory_offset)
    throw std::runtime_error("Agilent ChemStation directory exceeds data file: " + path);
  data_file.index.reserve(static_cast<std::size_t>(data_file.record_count));
  for (std::int32_t index = 0; index < data_file.record_count; ++index)
  {
    const auto base = static_cast<std::size_t>(directory_offset) + static_cast<std::size_t>(index) * 12;
    const auto offset_words = detail::be32s(bytes, base);
    if (offset_words <= 0)
      throw std::runtime_error("Invalid Agilent ChemStation spectrum offset at index " + std::to_string(index));
    data_file.index.push_back({static_cast<std::uint64_t>(offset_words - 1) * 2,
                               detail::be32s(bytes, base + 4), detail::be32s(bytes, base + 8)});
  }
  return data_file;
}

Spectrum read_spectrum(const DataFile &data_file, const std::size_t index)
{
  if (index >= data_file.index.size())
    throw std::out_of_range("Agilent ChemStation spectrum index is out of range: " + std::to_string(index));
  const auto bytes = detail::read_file(data_file.path);
  const auto offset = data_file.index[index].offset;
  if (offset + 18 > bytes.size())
    throw std::runtime_error("Agilent ChemStation spectrum header is truncated.");
  const auto words = detail::be16s(bytes, static_cast<std::size_t>(offset));
  const auto retention_time = detail::be32s(bytes, static_cast<std::size_t>(offset + 2));
  const auto status_word = detail::be16s(bytes, static_cast<std::size_t>(offset + 10));
  const auto peak_count = detail::be16s(bytes, static_cast<std::size_t>(offset + 12));
  if (words <= 0 || peak_count < 0 || static_cast<std::uint64_t>(words) * 2 > bytes.size() - offset)
    throw std::runtime_error("Invalid Agilent ChemStation spectrum record at index " + std::to_string(index));
  const auto payload_bytes = static_cast<std::uint64_t>(peak_count) * 4;
  if (payload_bytes > bytes.size() - (offset + 18))
    throw std::runtime_error("Agilent ChemStation spectrum peaks are truncated at index " + std::to_string(index));
  std::vector<std::pair<float, float>> points;
  points.reserve(static_cast<std::size_t>(peak_count));
  auto cursor = static_cast<std::size_t>(offset + 18);
  for (std::int16_t point = 0; point < peak_count; ++point)
  {
    const auto mz = static_cast<float>(detail::be16(bytes, cursor) / 20.0);
    cursor += 2;
    const auto intensity = static_cast<float>(detail::packed_abundance(bytes, cursor));
    cursor += 2;
    points.emplace_back(mz, intensity);
  }
  std::sort(points.begin(), points.end());
  Spectrum spectrum;
  spectrum.retention_time_ms = retention_time;
  spectrum.status_word = status_word;
  spectrum.mz.reserve(points.size());
  spectrum.intensity.reserve(points.size());
  for (const auto &[mz, intensity] : points)
  {
    spectrum.mz.push_back(mz);
    spectrum.intensity.push_back(intensity);
  }
  return spectrum;
}

std::vector<Chromatogram> read_chromatograms(const std::string &directory)
{
  const std::filesystem::path root(directory);
  if (!std::filesystem::is_directory(root))
    throw std::runtime_error("Not an Agilent ChemStation directory: " + directory);
  std::vector<Chromatogram> chromatograms;
  for (const auto &entry : std::filesystem::directory_iterator(root))
  {
    if (!entry.is_regular_file())
      continue;
    auto name = entry.path().filename().string();
    const auto bytes = detail::read_file(entry.path().string());
    if (entry.path().extension() == ".ch" || entry.path().extension() == ".CH")
    {
      if (detail::pascal_ascii(bytes, 0) != "130")
        throw std::runtime_error("Unsupported Agilent ChemStation .ch version in " + entry.path().string());
      constexpr std::size_t header_end = 0x1800;
      if (bytes.size() <= header_end)
        throw std::runtime_error("Agilent ChemStation .ch file has no signal body: " + entry.path().string());
      const auto scale = detail::be_double(bytes, 0x127c);
      const auto start_ms = detail::be32s(bytes, 0x11a);
      const auto end_ms = detail::be32s(bytes, 0x11e);
      const auto raw = detail::decode_ch(bytes, header_end);
      if (raw.empty())
        throw std::runtime_error("Agilent ChemStation .ch file has no decoded signal values: " + entry.path().string());
      Chromatogram chromatogram;
      chromatogram.id = name;
      chromatogram.detector = "UV";
      chromatogram.channel = name;
      chromatogram.units = detail::pascal_utf16(bytes, 0x104c);
      const auto signal = detail::pascal_utf16(bytes, 0x1075);
      const auto marker = signal.find("Sig=");
      if (marker != std::string::npos)
        chromatogram.wavelength_nm = std::stof(signal.substr(marker + 4));
      chromatogram.time_minutes.reserve(raw.size());
      chromatogram.intensity.reserve(raw.size());
      const auto step = raw.size() > 1 ? static_cast<double>(end_ms - start_ms) / (raw.size() - 1) : 0.0;
      for (std::size_t index = 0; index < raw.size(); ++index)
      {
        chromatogram.time_minutes.push_back(static_cast<float>((start_ms + index * step) / 60000.0));
        chromatogram.intensity.push_back(static_cast<float>(raw[index] * scale));
      }
      chromatograms.push_back(std::move(chromatogram));
    }
    else if (entry.path().extension() == ".UV" || entry.path().extension() == ".uv")
    {
      if (detail::pascal_ascii(bytes, 0) != "131")
        throw std::runtime_error("Unsupported Agilent ChemStation .UV version in " + entry.path().string());
      constexpr std::size_t header_end = 0x1000;
      const auto scale = detail::be_double(bytes, 0x0c0d);
      const auto time_count = detail::be32s(bytes, 0x116);
      if (time_count <= 0)
        continue;
      std::vector<float> times;
      std::vector<std::vector<float>> values;
      std::vector<float> wavelengths;
      std::size_t offset = header_end;
      for (std::int32_t time_index = 0; time_index < time_count; ++time_index)
      {
        if (offset + 22 > bytes.size())
          throw std::runtime_error("Agilent ChemStation .UV segment header is truncated.");
        const auto label = detail::le16(bytes, offset);
        const auto segment_length = detail::le16(bytes, offset + 2);
        const auto time_ms = detail::le32(bytes, offset + 4);
        const auto low = detail::le16(bytes, offset + 8);
        const auto high = detail::le16(bytes, offset + 10);
        const auto step = detail::le16(bytes, offset + 12);
        if (step == 0 || high < low)
          throw std::runtime_error("Agilent ChemStation .UV segment has an invalid wavelength grid.");
        const auto channel_count = static_cast<std::size_t>((high - low) / step) + 1;
        if (wavelengths.empty())
          for (std::size_t channel = 0; channel < channel_count; ++channel)
            wavelengths.push_back((low + channel * step) / 20.0f);
        if (channel_count != wavelengths.size())
          throw std::runtime_error("Agilent ChemStation .UV wavelength grids change between segments.");
        const auto body = offset + 22;
        std::vector<float> row(channel_count);
        if (label == 70)
          for (std::size_t channel = 0; channel < channel_count; ++channel)
            row[channel] = static_cast<float>(detail::le_double(bytes, body + channel * sizeof(double)) * scale);
        else
        {
          const auto raw = detail::decode_delta(bytes, body, channel_count, true);
          for (std::size_t channel = 0; channel < channel_count; ++channel)
            row[channel] = raw[channel] * static_cast<float>(scale);
        }
        times.push_back(time_ms / 60000.0f);
        values.push_back(std::move(row));
        offset += segment_length > 0 ? segment_length : 22;
      }
      for (std::size_t channel = 0; channel < wavelengths.size(); ++channel)
      {
        Chromatogram chromatogram;
        chromatogram.id = name + ":" + std::to_string(wavelengths[channel]);
        chromatogram.detector = "UV";
        chromatogram.channel = name;
        chromatogram.units = detail::pascal_utf16(bytes, 0xc15);
        chromatogram.wavelength_nm = wavelengths[channel];
        chromatogram.time_minutes = times;
        chromatogram.intensity.reserve(values.size());
        for (const auto &row : values)
          chromatogram.intensity.push_back(row[channel]);
        chromatograms.push_back(std::move(chromatogram));
      }
    }
  }
  return chromatograms;
}

class Reader final : public MASS_SPEC_READER
{
public:
  explicit Reader(const std::string &file) : MASS_SPEC_READER(file)
  {
    const auto path = std::filesystem::path(file);
    const auto root = std::filesystem::is_directory(path) ? path : path.parent_path();
    if (path.extension() == ".MS" || path.extension() == ".ms")
      data_file_ = read_data_file(file);
    else
    {
      for (const auto &entry : std::filesystem::directory_iterator(root))
      {
        auto name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (entry.is_regular_file() && (name == "MSD1.MS" || name == "DATA.MS"))
        {
          data_file_ = read_data_file(entry.path().string());
          break;
        }
      }
    }
    chromatograms_ = read_chromatograms(root.string());
  }
  int get_number_spectra() override { return static_cast<int>(data_file_.index.size()); }
  int get_number_chromatograms() override { return static_cast<int>(chromatograms_.size()); }
  int get_number_spectra_binary_arrays() override { return get_number_spectra() * 2; }
  std::string get_format() override { return "AgilentChemStationD"; }
  std::string get_type() override { return data_file_.index.empty() ? "UV" : "MS"; }
  std::string get_time_stamp() override { return data_file_.acquisition_date; }
  std::vector<int> get_polarity() override { return std::vector<int>(data_file_.index.size(), 0); }
  std::vector<int> get_mode() override { return std::vector<int>(data_file_.index.size(), 0); }
  std::vector<int> get_level() override { return std::vector<int>(data_file_.index.size(), 1); }
  std::vector<int> get_configuration() override { return std::vector<int>(data_file_.index.size(), 0); }
  float get_min_mz() override { return data_file_.index.empty() ? 0.0f : first_mz(false); }
  float get_max_mz() override { return data_file_.index.empty() ? 0.0f : first_mz(true); }
  float get_start_rt() override { return data_file_.index.empty() ? (chromatograms_.empty() ? 0.0f : chromatograms_.front().time_minutes.front() * 60.0f) : data_file_.retention_time_start_ms / 1000.0f; }
  float get_end_rt() override { return data_file_.index.empty() ? (chromatograms_.empty() ? 0.0f : chromatograms_.front().time_minutes.back() * 60.0f) : data_file_.retention_time_end_ms / 1000.0f; }
  bool has_ion_mobility() override { return false; }
  MASS_SPEC_SUMMARY get_summary() override
  {
    MASS_SPEC_SUMMARY summary{}; summary.file_name = std::filesystem::path(file_).filename().string(); summary.file_path = file_; summary.format = get_format(); summary.type = get_type();
    summary.number_spectra = get_number_spectra(); summary.number_chromatograms = get_number_chromatograms(); summary.number_spectra_binary_arrays = get_number_spectra_binary_arrays(); summary.min_mz = get_min_mz(); summary.max_mz = get_max_mz(); summary.start_rt = get_start_rt(); summary.end_rt = get_end_rt(); summary.has_ion_mobility = false; summary.level = get_level(); summary.mode = get_mode(); summary.polarity = get_polarity(); return summary;
  }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { return detail::normalize(std::move(indices), data_file_.index.size()); }
  std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { const auto selected = detail::normalize(std::move(indices), data_file_.index.size()); std::vector<int> out; for (const auto index : selected) out.push_back(static_cast<int>(index + 1)); return out; }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override { return selected_int(std::move(indices), [this](const auto &entry) { return static_cast<int>(read_spectrum(data_file_, static_cast<std::size_t>(&entry - data_file_.index.data())).mz.size()); }); }
  std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { return selected_int(std::move(indices), [](const auto &) { return 1; }); }
  std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return std::vector<int>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0); }
  std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return std::vector<int>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0); }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { return std::vector<int>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0); }
  std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [this](const auto &) { return get_min_mz(); }); }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [this](const auto &) { return get_max_mz(); }); }
  std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [this](const auto &entry) { return base_peak(data_file_, static_cast<std::size_t>(&entry - data_file_.index.data()), true); }); }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [this](const auto &entry) { return base_peak(data_file_, static_cast<std::size_t>(&entry - data_file_.index.data()), false); }); }
  std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [this](const auto &entry) { const auto spectrum = read_spectrum(data_file_, static_cast<std::size_t>(&entry - data_file_.index.data())); return std::accumulate(spectrum.intensity.begin(), spectrum.intensity.end(), 0.0f); }); }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { return selected_float(std::move(indices), [](const auto &entry) { return entry.retention_time_ms / 1000.0f; }); }
  std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { return std::vector<float>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0.0f); }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return std::vector<int>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0); }
  std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return std::vector<float>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0.0f); }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return std::vector<float>(detail::normalize(std::move(indices), data_file_.index.size()).size(), 0.0f); }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
  {
    const auto selected = detail::normalize(std::move(indices), data_file_.index.size()); MASS_SPEC_SPECTRA_HEADERS out; out.resize_all(selected.size());
    for (std::size_t n = 0; n < selected.size(); ++n) { const auto spectrum = read_spectrum(data_file_, selected[n]); out.index[n] = static_cast<int>(selected[n]); out.scan[n] = static_cast<int>(selected[n] + 1); out.array_length[n] = static_cast<int>(spectrum.mz.size()); out.level[n] = 1; out.rt[n] = spectrum.retention_time_ms / 1000.0f; out.lowmz[n] = spectrum.mz.empty() ? 0.0f : spectrum.mz.front(); out.highmz[n] = spectrum.mz.empty() ? 0.0f : spectrum.mz.back(); out.bpint[n] = base_peak(data_file_, selected[n], false); out.bpmz[n] = base_peak(data_file_, selected[n], true); out.tic[n] = std::accumulate(spectrum.intensity.begin(), spectrum.intensity.end(), 0.0f); }
    return out;
  }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override
  {
    const auto selected = detail::normalize(std::move(indices), chromatograms_.size()); MASS_SPEC_CHROMATOGRAMS_HEADERS out; out.resize_all(selected.size());
    for (std::size_t n = 0; n < selected.size(); ++n) { const auto &chromatogram = chromatograms_[selected[n]]; out.index[n] = static_cast<int>(selected[n]); out.chromatogram_id[n] = chromatogram.id; out.array_length[n] = static_cast<int>(chromatogram.intensity.size()); out.signal_type[n] = "UV"; out.chromatogram_type[n] = "UV"; out.detector[n] = chromatogram.detector; out.channel[n] = chromatogram.channel; out.units[n] = chromatogram.units; out.wavelength_nm[n] = chromatogram.wavelength_nm; out.interval_ms[n] = chromatogram.time_minutes.size() > 1 ? (chromatogram.time_minutes[1] - chromatogram.time_minutes[0]) * 60000.0f : 0.0f; out.start_time[n] = chromatogram.time_minutes.empty() ? 0.0f : chromatogram.time_minutes.front() * 60.0f; out.end_time[n] = chromatogram.time_minutes.empty() ? 0.0f : chromatogram.time_minutes.back() * 60.0f; }
    return out;
  }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> out; for (const auto index : detail::normalize(std::move(indices), data_file_.index.size())) out.push_back(get_spectrum(static_cast<int>(index)).binary_data); return out; }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> out; for (const auto index : detail::normalize(std::move(indices), chromatograms_.size())) { auto time = chromatograms_[index].time_minutes; for (auto &value : time) value *= 60.0f; out.push_back({std::move(time), chromatograms_[index].intensity}); } return out; }
  std::vector<std::vector<std::string>> get_software() override { return {}; }
  std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
  {
    trace_spectrum_decode(index);
    const auto spectrum = read_spectrum(data_file_, static_cast<std::size_t>(index)); MASS_SPEC_SPECTRUM out{}; out.index = index; out.scan = index + 1; out.array_length = static_cast<int>(spectrum.mz.size()); out.level = 1; out.rt = spectrum.retention_time_ms / 1000.0f; out.binary_arrays_count = 2; out.binary_names = {"m/z", "intensity"}; out.binary_data = {spectrum.mz, spectrum.intensity}; out.lowmz = spectrum.mz.empty() ? 0.0f : spectrum.mz.front(); out.highmz = spectrum.mz.empty() ? 0.0f : spectrum.mz.back(); out.tic = std::accumulate(spectrum.intensity.begin(), spectrum.intensity.end(), 0.0f); auto it = std::max_element(spectrum.intensity.begin(), spectrum.intensity.end()); if (it != spectrum.intensity.end()) { out.bpint = *it; out.bpmz = spectrum.mz[static_cast<std::size_t>(std::distance(spectrum.intensity.begin(), it))]; } return out;
  }
private:
  template <typename F> std::vector<int> selected_int(std::vector<int> indices, F f) const { const auto selected = detail::normalize(std::move(indices), data_file_.index.size()); std::vector<int> out; for (const auto i : selected) out.push_back(f(data_file_.index[i])); return out; }
  template <typename F> std::vector<float> selected_float(std::vector<int> indices, F f) const { const auto selected = detail::normalize(std::move(indices), data_file_.index.size()); std::vector<float> out; for (const auto i : selected) out.push_back(f(data_file_.index[i])); return out; }
  float first_mz(bool last) const { const auto spectrum = read_spectrum(data_file_, 0); return spectrum.mz.empty() ? 0.0f : (last ? spectrum.mz.back() : spectrum.mz.front()); }
  static float base_peak(const DataFile &file, std::size_t index, bool mz) { const auto spectrum = read_spectrum(file, index); const auto it = std::max_element(spectrum.intensity.begin(), spectrum.intensity.end()); if (it == spectrum.intensity.end()) return 0.0f; return mz ? spectrum.mz[static_cast<std::size_t>(std::distance(spectrum.intensity.begin(), it))] : *it; }
  DataFile data_file_;
  std::vector<Chromatogram> chromatograms_;
};

std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &path)
{
  const auto requested = std::filesystem::path(path);
  const auto root = std::filesystem::is_directory(requested) ? requested : requested.parent_path();
  for (const auto &entry : std::filesystem::directory_iterator(root))
  {
  auto name = entry.path().filename().string();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (entry.is_regular_file() && (name == "MSD1.MS" || name == "DATA.MS" || name.ends_with(".CH") || name.ends_with(".UV")))
    return std::make_unique<Reader>(root.string());
  }
  throw std::runtime_error("Agilent ChemStation directory has no supported MSD1.MS/DATA.MS file: " + path);
}
}
