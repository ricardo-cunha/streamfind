#include "readers/reader_agilent.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>

#include <stdexcept>

namespace mass_spec::reader::agilent
{
namespace detail
{
constexpr std::size_t scan_preamble_size = 228;
constexpr std::array<std::size_t, 2> scan_record_sizes = {220, 284};
constexpr std::size_t profile_block_preamble_size = 16;
constexpr std::size_t maximum_profile_uncompressed_size = 512 * 1024 * 1024;

std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open Agilent MassHunter file: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

double le_double(const std::vector<std::uint8_t> &bytes, std::size_t offset);

std::int32_t read_method_polarity(const std::string &path)
{
  const auto bytes = read_file(std::filesystem::path(path) / "AcqData" / "AcqMethod.xml");
  const std::string xml(bytes.begin(), bytes.end());

  const auto id = xml.find("ionPolarity");
  if (id != std::string::npos)
  {
    const auto escaped_value = xml.find("&lt;Value&gt;", id);
    const auto plain_value = xml.find("<Value>", id);
    const auto value = escaped_value != std::string::npos ? escaped_value : plain_value;
    if (value != std::string::npos)
    {
      const auto prefix_length = escaped_value != std::string::npos ? std::string("&lt;Value&gt;").size() : std::string("<Value>").size();
      const auto suffix = escaped_value != std::string::npos ? "&lt;/Value&gt;" : "</Value>";
      const auto start = value + prefix_length;
      const auto end = xml.find(suffix, start);
      if (end == std::string::npos) return 0;
      const auto text = xml.substr(start, end - start);
      if (text.find("Positive") != std::string::npos) return 1;
      if (text.find("Negative") != std::string::npos) return -1;
    }
  }
  return 0;
}

struct PeriodicActuals
{
  double precursor_mz = 0.0;
  double collision_energy = 0.0;
};

std::vector<PeriodicActuals> read_periodic_actuals(const std::string &path, const std::vector<ScanRecord> &records)
{
  const auto bytes = read_file(std::filesystem::path(path) / "AcqData/MSPeriodicActuals.bin");
  struct Group { double time; std::size_t begin; std::size_t end; };
  std::vector<Group> groups;
  for (std::size_t offset = 8; offset + 40 <= bytes.size(); offset += 40)
  {
    const auto time = le_double(bytes, offset + 8);
    if (!(time > 0.0 && time < 100.0)) continue;
    if (groups.empty() || std::fabs(groups.back().time - time) > 1e-7)
      groups.push_back({time, offset, offset + 40});
    else
      groups.back().end = offset + 40;
  }
  std::vector<std::pair<double, PeriodicActuals>> actuals;
  for (const auto &group : groups)
  {
    PeriodicActuals values;
    for (std::size_t offset = group.begin; offset < group.end; offset += 4)
    {
      const auto value = le_double(bytes, offset);
      if (values.precursor_mz == 0.0 && std::isfinite(value) && value > 100.0 && value < 3000.0 && std::fabs(value - std::round(value)) > 0.001)
        values.precursor_mz = value;
      if (std::isfinite(value) && value >= 5.0 && value <= 50.0 && std::fabs(value / 5.0 - std::round(value / 5.0)) < 0.001)
        values.collision_energy = value;
    }
    actuals.push_back({group.time, values});
  }
  std::vector<PeriodicActuals> result(records.size());
  PeriodicActuals current;
  for (std::size_t index = 0; index < records.size(); ++index)
  {
    if (records[index].ms_level >= 2)
      {
      const auto it = std::lower_bound(actuals.begin(), actuals.end(), records[index].scan_time_minutes,
                                       [](const auto &item, double time) { return item.first < time; });
      const auto match = it != actuals.end() && std::fabs(it->first - records[index].scan_time_minutes) < 1e-5 ? it : (it != actuals.begin() && std::fabs((it - 1)->first - records[index].scan_time_minutes) < 1e-5 ? it - 1 : actuals.end());
      if (match != actuals.end())
      {
        if (match->second.precursor_mz > 0.0) current.precursor_mz = match->second.precursor_mz;
        if (match->second.collision_energy > 0.0) current.collision_energy = match->second.collision_energy;
      }
      result[index] = current;
    }
    else
      current = {};
  }
  return result;
}

std::size_t detect_scan_record_size(const std::vector<std::uint8_t> &bytes)
{
  for (const auto size : scan_record_sizes)
    if (bytes.size() >= scan_preamble_size && (bytes.size() - scan_preamble_size) % size == 0)
      return size;
  throw std::runtime_error("Agilent MSScan.bin has an unsupported record layout.");
}

std::uint32_t u32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 4 > bytes.size())
    throw std::runtime_error("Agilent binary record is truncated.");
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t u16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 2 > bytes.size())
    throw std::runtime_error("Agilent binary 16-bit field is truncated.");
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::int16_t i16(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  return static_cast<std::int16_t>(u16(bytes, offset));
}

std::int32_t i32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  return static_cast<std::int32_t>(u32(bytes, offset));
}

float f32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(float) > bytes.size())
    throw std::runtime_error("Agilent binary float field is truncated.");
  float value = 0.0f;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

std::uint64_t u64(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + 8 > bytes.size())
    throw std::runtime_error("Agilent binary 64-bit field is truncated.");
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

double f64(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(double) > bytes.size())
    throw std::runtime_error("Agilent binary double field is truncated.");
  double value = 0.0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

double le_double(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  if (offset + sizeof(double) > bytes.size())
    throw std::runtime_error("Agilent binary little-endian double is truncated.");
  std::uint64_t bits = 0;
  for (std::size_t index = 0; index < sizeof(double); ++index)
    bits |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::vector<std::uint8_t> read_slice(const std::filesystem::path &path, std::uint64_t offset, std::size_t size)
{
  const auto file_size = std::filesystem::file_size(path);
  if (offset > file_size || size > file_size - offset)
    throw std::runtime_error("Agilent profile block is outside MSProfile.bin.");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open Agilent MSProfile.bin: " + path.string());
  input.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::uint8_t> bytes(size);
  input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<std::size_t>(input.gcount()) != bytes.size())
    throw std::runtime_error("Agilent MSProfile.bin profile block is truncated.");
  return bytes;
}

std::vector<std::uint8_t> decompress_lzf(const std::vector<std::uint8_t> &input, std::size_t maximum_size)
{
  if (maximum_size > maximum_profile_uncompressed_size)
    throw std::runtime_error("Agilent LZF profile block exceeds the decompression safety limit.");
  std::vector<std::uint8_t> output(maximum_size);
  std::size_t in = 0, out = 0;
  while (in < input.size() && out < output.size())
  {
    std::size_t control = input[in++];
    if (control < 32)
    {
      const std::size_t length = control + 1;
      if (in + length > input.size() || out + length > output.size())
        throw std::runtime_error("Agilent LZF literal run is outside the profile block.");
      std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(in), length,
                  output.begin() + static_cast<std::ptrdiff_t>(out));
      in += length;
      out += length;
      continue;
    }

    std::size_t length = control >> 5;
    if (out < ((control & 31) << 8) + 1)
      throw std::runtime_error("Agilent LZF back reference precedes profile output.");
    std::size_t reference = out - ((control & 31) << 8) - 1;
    if (length == 7)
    {
      if (in >= input.size())
        throw std::runtime_error("Agilent LZF profile block has a truncated length.");
      length += input[in++];
    }
    if (in >= input.size())
      throw std::runtime_error("Agilent LZF profile block has a truncated distance.");
    const auto distance = input[in++];
    if (reference < distance)
      throw std::runtime_error("Agilent LZF profile block has an invalid distance.");
    reference -= distance;
    length += 2;
    if (out + length > output.size())
      throw std::runtime_error("Agilent LZF match run exceeds profile output.");
    for (std::size_t index = 0; index < length; ++index)
      output[out++] = output[reference++];
  }
  output.resize(out);
  return output;
}
}

bool is_agilent_mass_hunter_directory(const std::string &path)
{
  const std::filesystem::path root(path);
  const auto acq = root / "AcqData";
  return std::filesystem::is_directory(root) &&
         std::filesystem::is_regular_file(acq / "Contents.xml") &&
         std::filesystem::is_regular_file(acq / "MSScan.bin") &&
         std::filesystem::is_regular_file(acq / "MSProfile.bin");
}

bool is_agilent_ion_mobility_directory(const std::string &path)
{
  const std::filesystem::path root(path);
  const auto acq = root / "AcqData";
  return is_agilent_mass_hunter_directory(path) &&
         std::filesystem::is_regular_file(acq / "IMSFrame.bin") &&
         std::filesystem::is_regular_file(acq / "IMSFrame.xsd");
}

std::vector<ImsFrameRecord> read_ims_frame_records(const std::string &path)
{
  if (!is_agilent_ion_mobility_directory(path))
    throw std::runtime_error("Not an Agilent MassHunter ion-mobility directory: " + path);
  constexpr std::size_t header_size = 76;
  constexpr std::size_t record_size = 130;
  const auto bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "IMSFrame.bin");
  if (bytes.size() < header_size || (bytes.size() - header_size) % record_size != 0)
    throw std::runtime_error("Agilent IMSFrame.bin has an unsupported record layout.");
  std::vector<ImsFrameRecord> frames;
  frames.reserve((bytes.size() - header_size) / record_size);
  for (std::size_t index = 0, offset = header_size; offset < bytes.size(); ++index, offset += record_size)
  {
    ImsFrameRecord frame;
    frame.frame_id = detail::i16(bytes, offset);
    frame.frame_method_id = detail::i16(bytes, offset + 2);
    frame.time_segment_id = detail::i16(bytes, offset + 4);
    frame.actuals_offset = static_cast<std::int64_t>(detail::u64(bytes, offset + 6));
    frame.cycle_number = detail::i16(bytes, offset + 14);
    frame.first_nonzero_drift_bin = detail::i16(bytes, offset + 16);
    frame.frag_class = detail::i16(bytes, offset + 18);
    frame.frag_energy = detail::f32(bytes, offset + 20);
    frame.frame_base_abundance = detail::f64(bytes, offset + 24);
    frame.frame_base_drift_bin = detail::i16(bytes, offset + 32);
    frame.frame_base_ms_bin = detail::i32(bytes, offset + 34);
    frame.frame_scan_time_minutes = detail::f64(bytes, offset + 38);
    frame.frame_spec_abundance_limit = detail::f64(bytes, offset + 46);
    frame.frame_tic = detail::f64(bytes, offset + 54);
    frame.ims_field = detail::f64(bytes, offset + 62);
    frame.ims_pressure = detail::f64(bytes, offset + 70);
    frame.ims_temperature = detail::f64(bytes, offset + 78);
    frame.ims_trap_time = detail::f64(bytes, offset + 86);
    frame.isolation_start_mz = detail::f64(bytes, offset + 94);
    frame.isolation_mz = detail::f64(bytes, offset + 102);
    frame.isolation_end_mz = detail::f64(bytes, offset + 110);
    frame.last_nonzero_drift_bin = detail::i16(bytes, offset + 118);
    frame.mass_cal_offset = static_cast<std::int64_t>(detail::u64(bytes, offset + 120));
    frame.num_transients = detail::i16(bytes, offset + 128);
    if (index > 0 && frame.frame_id <= frames.back().frame_id)
      throw std::runtime_error("Agilent IMSFrame.bin frame IDs are not increasing.");
    frames.push_back(frame);
  }
  return frames;
}

namespace ims_detail
{
struct ImsMassCalibration
{
  double coeff = 0.0;
  double base = 0.0;
  double left = 0.0;
  double right = 0.0;
  std::vector<double> coefficients;
  std::uint32_t use_flags = 0;
};

ImsMassCalibration read_ims_mass_calibration(const std::string &path)
{
  const auto xml_bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "DefaultMassCal.xml");
  const std::string xml(xml_bytes.begin(), xml_bytes.end());
  const auto calibration_start = xml.find("<DefaultCalibration DefaultCalibrationID=\"1\"");
  const auto calibration_end = xml.find("</DefaultCalibration>", calibration_start);
  if (calibration_start == std::string::npos || calibration_end == std::string::npos)
    throw std::runtime_error("Agilent DefaultMassCal.xml has no calibration ID 1.");
  const auto calibration = xml.substr(calibration_start, calibration_end - calibration_start);
  const auto read_step_values = [&calibration](const std::string &formula) {
    const auto formula_pos = calibration.find("<CalibrationFormula>" + formula + "</CalibrationFormula>");
    if (formula_pos == std::string::npos)
      return std::vector<double>{};
    const auto step_start = calibration.rfind("<Step", formula_pos);
    const auto step_end = calibration.find("</Step>", formula_pos);
    if (step_start == std::string::npos || step_end == std::string::npos)
      return std::vector<double>{};
    const auto step = calibration.substr(step_start, step_end - step_start);
    std::vector<double> values;
    for (std::size_t position = 0;;)
    {
      const auto value_start = step.find(">", step.find("<Value ", position));
      if (value_start == std::string::npos)
        break;
      const auto value_end = step.find("</Value>", value_start);
      if (value_end == std::string::npos)
        break;
      values.push_back(std::stod(step.substr(value_start + 1, value_end - value_start - 1)));
      position = value_end + 8;
    }
    return values;
  };
  ImsMassCalibration result;
  const auto traditional = read_step_values("Traditional");
  if (traditional.size() < 2)
    throw std::runtime_error("Agilent DefaultMassCal.xml has incomplete traditional calibration.");
  result.coeff = traditional[0];
  result.base = traditional[1];
  const auto polynomial = read_step_values("Polynomial");
  const auto flags_start = calibration.find("<ValueUseFlags>", calibration.find("<CalibrationFormula>Polynomial"));
  const auto flags_end = calibration.find("</ValueUseFlags>", flags_start);
  result.use_flags = flags_start == std::string::npos || flags_end == std::string::npos ? 0u : static_cast<std::uint32_t>(std::stoul(calibration.substr(flags_start + 15, flags_end - flags_start - 15)));
  result.coefficients = polynomial;
  if (result.coefficients.size() < 8)
    result.coefficients.resize(8, 0.0);
  result.left = result.coefficients[0];
  result.right = result.coefficients[1];
  result.coefficients.erase(result.coefficients.begin(), result.coefficients.begin() + 2);
  return result;
}

double ims_calibrate(const ImsMassCalibration &calibration, const double tof)
{
  const auto clipped = std::clamp(tof, calibration.left, calibration.right);
  double mz = std::pow(calibration.coeff * (tof - calibration.base), 2.0);
  if (calibration.use_flags == 0)
    return mz;
  const auto highest_order = 31 - static_cast<int>(std::countl_zero(calibration.use_flags));
  std::vector<double> polynomial(static_cast<std::size_t>(highest_order + 1), 0.0);
  std::size_t coefficient = 0;
  for (int order = 0; order <= highest_order; ++order)
  {
    if ((calibration.use_flags & (1u << order)) != 0)
      polynomial[static_cast<std::size_t>(order)] = calibration.coefficients.at(coefficient++);
  }
  double correction = 0.0;
  for (int order = highest_order; order >= 0; --order)
    correction = correction * clipped + polynomial[static_cast<std::size_t>(order)];
  return mz - correction;
}

double read_ims_frame_dt_period(const std::string &path)
{
  const auto xml_bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "IMSFrameMeth.xml");
  const std::string xml(xml_bytes.begin(), xml_bytes.end());
  const auto start = xml.find("<FrameDtPeriod>");
  const auto end = xml.find("</FrameDtPeriod>", start);
  if (start == std::string::npos || end == std::string::npos)
    throw std::runtime_error("Agilent IMSFrameMeth.xml has no FrameDtPeriod.");
  return std::stod(xml.substr(start + 15, end - start - 15));
}
}

std::vector<ImsScanRecord> read_ims_scan_records(const std::string &path)
{
  if (!is_agilent_ion_mobility_directory(path))
    throw std::runtime_error("Not an Agilent MassHunter ion-mobility directory: " + path);
  const auto bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "MSScan.bin");
  if (bytes.size() < 92 || detail::u32(bytes, 0) != 257)
    throw std::runtime_error("Agilent IMS MSScan.bin has an invalid header.");
  const auto header_size = detail::u32(bytes, 88);
  constexpr std::size_t record_size = 106;
  if (header_size > bytes.size() || (bytes.size() - header_size) % record_size != 0)
    throw std::runtime_error("Agilent IMS MSScan.bin has an unsupported record layout.");
  const auto mobility_period = ims_detail::read_ims_frame_dt_period(path);
  const auto frames = read_ims_frame_records(path);
  std::vector<ImsScanRecord> records;
  records.reserve((bytes.size() - header_size) / record_size);
  for (std::size_t offset = header_size; offset < bytes.size(); offset += record_size)
  {
    ImsScanRecord record;
    record.scan_id = detail::u32(bytes, offset);
    record.frame_id = detail::i16(bytes, offset + 4);
    record.base_abundance = detail::i32(bytes, offset + 6);
    record.base_ms_bin = detail::i32(bytes, offset + 10);
    record.detector_gain = detail::i16(bytes, offset + 14);
    record.drift_bin = detail::i16(bytes, offset + 16);
    record.first_nonzero_ms_bin = detail::i32(bytes, offset + 18);
    record.last_nonzero_ms_bin = detail::i32(bytes, offset + 22);
    record.tic = detail::f64(bytes, offset + 26);
    record.base_peak_abundance = detail::f64(bytes, offset + 34);
    record.base_peak_mz = detail::f64(bytes, offset + 42);
    record.profile_format_id = detail::i16(bytes, offset + 50);
    record.profile_byte_count = detail::u32(bytes, offset + 52);
    record.profile_offset = detail::u64(bytes, offset + 56);
    record.profile_point_count = detail::u32(bytes, offset + 64);
    record.profile_full_byte_count = detail::u32(bytes, offset + 68);
    record.peak_format_id = detail::i16(bytes, offset + 72);
    record.peak_byte_count = detail::u32(bytes, offset + 74);
    record.peak_offset = detail::u64(bytes, offset + 78);
    record.peak_point_count = detail::u32(bytes, offset + 86);
    record.peak_max_x = detail::f64(bytes, offset + 90);
    record.peak_min_x = detail::f64(bytes, offset + 98);
    record.scan_time_minutes = record.frame_id <= 0 || static_cast<std::size_t>(record.frame_id) > frames.size() ? 0.0 : frames[static_cast<std::size_t>(record.frame_id - 1)].frame_scan_time_minutes;
    record.mobility = record.drift_bin * mobility_period;
    records.push_back(record);
  }
  return records;
}

ProfileSpectrum read_ims_profile_spectrum(const std::string &path, const ImsScanRecord &record)
{
  if (record.profile_format_id != 1 && record.profile_format_id != 2)
    throw std::runtime_error("Agilent IMS scan has unsupported profile format ID: " + std::to_string(record.profile_format_id));
  const auto block = detail::read_slice(std::filesystem::path(path) / "AcqData" / "MSProfile.bin", record.profile_offset, record.profile_byte_count);
  if (block.size() < 24)
    throw std::runtime_error("Agilent IMS profile block is truncated.");
  const auto point_count = static_cast<std::size_t>(record.profile_point_count);
  const auto marker = detail::u32(block, 16);
  if ((marker >> 24) != 0x90 || (marker & 0x00ffffffu) != point_count)
    throw std::runtime_error("Agilent IMS profile block is not the validated RLE format.");
  const auto initial = detail::i32(block, 20);
  if (initial > 0)
    throw std::runtime_error("Agilent IMS profile block has an invalid leading-zero count.");
  std::vector<std::uint32_t> intensities(point_count, 0);
  std::size_t output = static_cast<std::size_t>(-static_cast<std::int64_t>(initial));
  std::size_t input = 24;
  std::size_t width = 4;
  while (input < block.size())
  {
    if (input + width > block.size())
      throw std::runtime_error("Agilent IMS profile RLE token is truncated.");
    std::int64_t value = 0;
    if (width == 1) value = static_cast<std::int8_t>(block[input]);
    else if (width == 2) value = detail::i16(block, input);
    else if (width == 4) value = detail::i32(block, input);
    else throw std::runtime_error("Agilent IMS profile RLE has an invalid width.");
    input += width;
    if (value >= 0)
    {
      if (output >= point_count) throw std::runtime_error("Agilent IMS profile RLE exceeds point count.");
      intensities[output++] = static_cast<std::uint32_t>(value);
    }
    else
    {
      const auto encoded = -value;
      const auto zeros = static_cast<std::size_t>(encoded / 4);
      const auto flag = static_cast<int>(encoded % 4);
      if (flag == 1) width = 1;
      else if (flag == 2) width = 2;
      else if (flag == 3) width = 4;
      else throw std::runtime_error("Agilent IMS profile RLE has an invalid width flag.");
      if (zeros > point_count - output) throw std::runtime_error("Agilent IMS profile RLE zero run exceeds point count.");
      output += zeros;
    }
  }
  const auto start = detail::le_double(block, 0);
  const auto delta = detail::le_double(block, 8);
  const auto calibration = ims_detail::read_ims_mass_calibration(path);
  ProfileSpectrum spectrum;
  spectrum.mz.reserve(point_count);
  spectrum.intensity.reserve(point_count);
  for (std::size_t index = 0; index < point_count; ++index)
  {
    spectrum.mz.push_back(static_cast<float>(ims_detail::ims_calibrate(calibration, start + (static_cast<double>(index) - 1.0) * delta)));
    spectrum.intensity.push_back(static_cast<float>(intensities[index]));
  }
  return spectrum;
}

std::vector<ScanRecord> read_scan_records(const std::string &path)
{
  if (!is_agilent_mass_hunter_directory(path))
    throw std::runtime_error("Not an Agilent MassHunter .d directory: " + path);
  const auto bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "MSScan.bin");
  const auto scan_record_size = detail::detect_scan_record_size(bytes);
  std::int32_t polarity = 0;
  try { polarity = detail::read_method_polarity(path); } catch (const std::exception &) {}

  std::vector<ScanRecord> records;
  records.reserve((bytes.size() - detail::scan_preamble_size) / scan_record_size);
  for (std::size_t offset = detail::scan_preamble_size; offset < bytes.size(); offset += scan_record_size)
  {
    ScanRecord record;
    record.scan_id = detail::u32(bytes, offset);
    record.scan_method_id = detail::u32(bytes, offset + 4);
    record.time_segment_id = detail::u32(bytes, offset + 8);
    record.scan_time_minutes = detail::f64(bytes, offset + 12);
    record.ms_level = static_cast<std::int32_t>(detail::u32(bytes, offset + 20));
    record.scan_type = static_cast<std::int32_t>(detail::u32(bytes, offset + 24));
    record.tic = detail::f64(bytes, offset + 28);
    record.base_peak_mz = detail::f64(bytes, offset + 36);
    record.base_peak_value = detail::f64(bytes, offset + 44);
    record.calibration_id = static_cast<std::int32_t>(detail::u32(bytes, offset + 52));
    record.cycle_number = static_cast<std::int32_t>(detail::u32(bytes, offset + 56));
    record.spectrum_format_id = detail::u32(bytes, offset + 156);
    record.spectrum_offset = detail::u64(bytes, offset + 160);
    record.spectrum_byte_count = detail::u32(bytes, offset + 168);
    record.spectrum_point_count = detail::u32(bytes, offset + 172);
    record.spectrum_uncompressed_byte_count = detail::u32(bytes, offset + 176);
    record.spectrum_min_x = detail::f64(bytes, offset + 180);
    record.spectrum_max_x = detail::f64(bytes, offset + 188);
    record.spectrum_min_y = detail::f64(bytes, offset + 196);
    record.spectrum_max_y = detail::f64(bytes, offset + 204);
    record.spectrum_measured_noise = detail::f64(bytes, offset + 212);
    record.record_index = static_cast<std::uint32_t>((offset - detail::scan_preamble_size) / scan_record_size);
    if (scan_record_size >= 284)
    {
      record.centroid_format_id = detail::u32(bytes, offset + 220);
      record.centroid_offset = detail::u64(bytes, offset + 224);
      record.centroid_byte_count = detail::u32(bytes, offset + 232);
      record.centroid_point_count = detail::u32(bytes, offset + 236);
      if (record.ms_level >= 2)
      {
        record.collision_energy = detail::f64(bytes, offset + 76);
        record.precursor_mz = detail::f64(bytes, offset + 84);
          }
        }
        record.polarity = polarity;
    records.push_back(record);
  }
  const auto periodic_actuals = detail::read_periodic_actuals(path, records);
  for (std::size_t index = 0; index < records.size(); ++index)
    if (records[index].ms_level >= 2 && index < periodic_actuals.size())
    {
      records[index].precursor_intensity = records[index].tic;
      if (records[index].precursor_mz == 0.0)
        records[index].precursor_mz = periodic_actuals[index].precursor_mz;
      if (records[index].collision_energy == 0.0)
        records[index].collision_energy = periodic_actuals[index].collision_energy;
    }
  for (auto &record : records)
    record.precursor_intensity = record.ms_level >= 2 ? record.tic : 0.0;
  return records;
}

ProfileSpectrum read_profile_spectrum(const std::string &path, const ScanRecord &record)
{
  if (record.spectrum_format_id != 1)
    throw std::runtime_error("Agilent scan does not contain a SpectrumFormatID=1 profile block.");
  if (record.spectrum_byte_count < detail::profile_block_preamble_size)
    throw std::runtime_error("Agilent profile block is shorter than its preamble.");
  if (record.spectrum_point_count == 0)
    return {};
  const std::size_t point_bytes = static_cast<std::size_t>(record.spectrum_point_count) * sizeof(std::uint32_t);
  if (record.spectrum_uncompressed_byte_count < point_bytes)
    throw std::runtime_error("Agilent profile uncompressed byte count is smaller than its point array.");

  const auto block = detail::read_slice(std::filesystem::path(path) / "AcqData" / "MSProfile.bin",
                                        record.spectrum_offset, record.spectrum_byte_count);
  std::vector<std::uint8_t> decoded;
  try
  {
    decoded = detail::decompress_lzf(block, record.spectrum_uncompressed_byte_count);
  }
  catch (const std::exception &)
  {
    const std::vector<std::uint8_t> compressed(block.begin() + detail::profile_block_preamble_size, block.end());
    decoded = detail::decompress_lzf(compressed, record.spectrum_uncompressed_byte_count);
  }
  const std::size_t data_offset = decoded.size() == point_bytes ? 0 : detail::profile_block_preamble_size;
  if (data_offset > decoded.size() || decoded.size() - data_offset < point_bytes)
    throw std::runtime_error("Agilent LZF profile block is shorter than its declared point count.");

  ProfileSpectrum spectrum;
  spectrum.mz.reserve(record.spectrum_point_count);
  spectrum.intensity.reserve(record.spectrum_point_count);
  const double step = record.spectrum_point_count > 1
                          ? (record.spectrum_max_x - record.spectrum_min_x) / (record.spectrum_point_count - 1)
                          : 0.0;
  for (std::size_t index = 0; index < record.spectrum_point_count; ++index)
  {
    spectrum.mz.push_back(static_cast<float>(record.spectrum_min_x + index * step));
    spectrum.intensity.push_back(static_cast<float>(detail::u32(decoded, data_offset + index * 4)));
  }
  return spectrum;
}

namespace centroid_detail
{
struct Calibration
{
  std::array<double, 10> values{};
  std::uint32_t flags = 0;
  bool valid = false;
};

std::uint32_t default_flags(const std::string &path, const std::int32_t calibration_id)
{
  const auto bytes = detail::read_file(std::filesystem::path(path) / "AcqData" / "DefaultMassCal.xml");
  const std::string xml(bytes.begin(), bytes.end());
  const auto start = xml.find("<DefaultCalibration DefaultCalibrationID=\"" + std::to_string(calibration_id) + "\">");
  const auto end = xml.find("</DefaultCalibration>", start);
  const auto polynomial = xml.find("<CalibrationFormula>Polynomial", start);
  const auto flags_start = xml.find("<ValueUseFlags>", polynomial);
  const auto flags_end = xml.find("</ValueUseFlags>", flags_start);
  if (start == std::string::npos || end == std::string::npos || flags_start == std::string::npos || flags_start > end || flags_end == std::string::npos)
    return 0;
  return static_cast<std::uint32_t>(std::stoul(xml.substr(flags_start + 15, flags_end - flags_start - 15)));
}

Calibration read_calibration(const std::string &path, const ScanRecord &record)
{
  Calibration calibration;
  const auto file = std::filesystem::path(path) / "AcqData" / "MSMassCal.bin";
  if (!std::filesystem::is_regular_file(file))
    return calibration;
  const auto bytes = detail::read_file(file);
  constexpr std::size_t header = 76;
  constexpr std::size_t stride = 84;
  const auto offset = header + static_cast<std::size_t>(record.record_index) * stride;
  if (offset + sizeof(double) * calibration.values.size() > bytes.size())
    return calibration;
  for (std::size_t index = 0; index < calibration.values.size(); ++index)
    calibration.values[index] = detail::le_double(bytes, offset + index * sizeof(double));
  calibration.flags = default_flags(path, record.calibration_id);
  calibration.valid = true;
  return calibration;
}

double calibrate(const Calibration &calibration, const double value)
{
  if (!calibration.valid)
    return value;
  const auto mz = std::pow(calibration.values[0] * (value - calibration.values[1]), 2.0);
  if (calibration.flags == 0)
    return mz;
  const auto clipped = std::clamp(value, calibration.values[2], calibration.values[3]);
  std::array<double, 32> polynomial{};
  std::size_t coefficient = 4;
  int highest = 0;
  for (int order = 0; order < 32; ++order)
  {
    if ((calibration.flags & (1u << order)) != 0 && coefficient < calibration.values.size())
    {
      polynomial[static_cast<std::size_t>(order)] = calibration.values[coefficient++];
      highest = order;
    }
  }
  double correction = 0.0;
  for (int order = highest; order >= 0; --order)
    correction = correction * clipped + polynomial[static_cast<std::size_t>(order)];
  return mz - correction;
}
}

bool has_centroid(const ScanRecord &record)
{
  if (record.centroid_point_count == 0 || record.centroid_byte_count == 0)
    return false;
  const auto bytes_per_peak = record.centroid_byte_count / record.centroid_point_count;
  return record.centroid_byte_count % record.centroid_point_count == 0 &&
         (bytes_per_peak == 8 || bytes_per_peak == 12 || bytes_per_peak == 16);
}

ProfileSpectrum read_centroid_spectrum(const std::string &path, const ScanRecord &record)
{
  if (!has_centroid(record))
    throw std::runtime_error("Agilent scan does not contain a valid MSPeak.bin centroid block.");
  const auto block = detail::read_slice(std::filesystem::path(path) / "AcqData" / "MSPeak.bin", record.centroid_offset, record.centroid_byte_count);
  const auto count = static_cast<std::size_t>(record.centroid_point_count);
  const auto bytes_per_peak = static_cast<std::size_t>(record.centroid_byte_count / record.centroid_point_count);
  const auto calibration = centroid_detail::read_calibration(path, record);
  ProfileSpectrum spectrum;
  spectrum.mz.reserve(count); spectrum.intensity.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
  {
    const auto mz = bytes_per_peak == 8 ? detail::f32(block, index * 4) : detail::f64(block, index * 8);
    const auto intensity_offset = (bytes_per_peak == 8 ? count * 4 : count * 8) + index * (bytes_per_peak == 16 ? 8 : 4);
    const auto intensity = bytes_per_peak == 16 ? detail::f64(block, intensity_offset) : detail::f32(block, intensity_offset);
    spectrum.mz.push_back(static_cast<float>(centroid_detail::calibrate(calibration, mz)));
    spectrum.intensity.push_back(static_cast<float>(static_cast<std::uint64_t>(intensity)));
  }
  return spectrum;
}

namespace dad_detail
{
struct Signal
{
  std::string letter;
  std::string description;
  std::string units;
  std::uint32_t offset = 0;
  std::uint32_t point_count = 0;
};

std::pair<std::string, std::size_t> pascal(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
  if (offset >= bytes.size() || offset + 1 + bytes[offset] > bytes.size())
    return {{}, std::string::npos};
  return {std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1 + bytes[offset])), offset + 1 + bytes[offset]};
}

bool candidate(const std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uintmax_t data_size)
{
  const auto [letter, after_letter] = pascal(bytes, offset);
  if (letter.size() != 1 || !std::isalnum(static_cast<unsigned char>(letter.front())))
    return false;
  const auto [description, after_description] = pascal(bytes, after_letter);
  if (description.empty() || after_description == std::string::npos || after_description + 16 > bytes.size())
    return false;
  const auto data_offset = detail::u32(bytes, after_description + 4);
  const auto point_count = detail::u32(bytes, after_description + 12);
  return (detail::u32(bytes, after_description) == 1 || detail::u32(bytes, after_description) == 2) && data_offset >= 68 && point_count > 0 && data_offset + 16ull + point_count * 8ull <= data_size;
}

std::vector<Signal> read_signals(const std::filesystem::path &descriptor, const std::uintmax_t data_size)
{
  const auto bytes = detail::read_file(descriptor);
  if (bytes.size() < 80 || detail::u16(bytes, 0) != 0x0200)
    return {};
  const auto count = detail::u32(bytes, 76);
  std::vector<Signal> signals;
  std::size_t position = 80;
  for (std::uint32_t index = 0; index < count; ++index)
  {
    const auto [letter, after_letter] = pascal(bytes, position);
    const auto [description, after_description] = pascal(bytes, after_letter);
    if (letter.size() != 1 || after_description == std::string::npos || after_description + 16 > bytes.size())
      break;
    Signal signal{letter, description, {}, detail::u32(bytes, after_description + 4), detail::u32(bytes, after_description + 12)};
    if ((detail::u32(bytes, after_description) != 1 && detail::u32(bytes, after_description) != 2) || signal.offset < 68 || signal.point_count == 0 || signal.offset + 16ull + signal.point_count * 8ull > data_size)
      break;
    const auto fields_end = after_description + 16;
    std::size_t next = bytes.size();
    for (std::size_t probe = fields_end; probe + 2 < bytes.size(); ++probe)
      if (candidate(bytes, probe, data_size)) { next = probe; break; }
    for (std::size_t probe = fields_end; probe < next; ++probe)
    {
      const auto [unit, after_unit] = pascal(bytes, probe);
      if (after_unit != std::string::npos && unit.size() >= 1 && unit.size() <= 8 &&
          std::all_of(unit.begin(), unit.end(), [](const unsigned char value) { return value >= 32 && value != 127; }))
        signal.units = unit;
    }
    signals.push_back(std::move(signal));
    position = next;
    if (position == bytes.size()) break;
  }
  return signals;
}
}

std::vector<Chromatogram> read_dad_chromatograms(const std::string &path)
{
  const auto acq = std::filesystem::path(path) / "AcqData";
  if (!std::filesystem::is_directory(acq))
    return {};
  std::vector<Chromatogram> output;
  for (const auto &entry : std::filesystem::directory_iterator(acq))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".cd")
      continue;
    auto data_path = entry.path();
    data_path.replace_extension(".cg");
    if (!std::filesystem::is_regular_file(data_path))
      continue;
    const auto data_size = std::filesystem::file_size(data_path);
    const auto signals = dad_detail::read_signals(entry.path(), data_size);
    const auto data = detail::read_file(data_path);
    for (const auto &signal : signals)
    {
      if (signal.offset + 16ull + signal.point_count * 8ull > data.size())
        continue;
      const auto first_time = detail::f64(data, signal.offset);
      const auto interval = detail::f64(data, signal.offset + 8);
      Chromatogram chromatogram;
      chromatogram.id = entry.path().stem().string() + signal.letter;

      chromatogram.units = signal.units;
      const auto signal_pos = signal.description.find("Sig=");
      const bool absorbance = chromatogram.units == "mAU" || signal_pos != std::string::npos;
      chromatogram.signal_type = absorbance ? "UV" : "AUX";
      chromatogram.chromatogram_type = absorbance ? "DAD" : "AUX";
      chromatogram.detector = entry.path().stem().string();
      chromatogram.channel = signal.letter;
      chromatogram.interval_ms = static_cast<float>(interval * 60000.0);
      if (signal_pos != std::string::npos)
        chromatogram.wavelength_nm = std::stof(signal.description.substr(signal_pos + 4));
      chromatogram.time.reserve(signal.point_count); chromatogram.intensity.reserve(signal.point_count);
      for (std::uint32_t index = 0; index < signal.point_count; ++index)
      {
        chromatogram.time.push_back(static_cast<float>((first_time + index * interval) * 60.0));
        chromatogram.intensity.push_back(static_cast<float>(detail::f64(data, signal.offset + 16 + index * 8)));
      }
      output.push_back(std::move(chromatogram));
    }
  }
  return output;
}

class AgilentReader final : public MASS_SPEC_READER
{
public:
  explicit AgilentReader(const std::string &file) : MASS_SPEC_READER(file), records_(read_scan_records(file)), chromatograms_(read_dad_chromatograms(file)) {}

  int get_number_spectra() override { return static_cast<int>(records_.size()); }
  int get_number_chromatograms() override { return static_cast<int>(chromatograms_.size()); }
  int get_number_spectra_binary_arrays() override { return static_cast<int>(records_.size() * 2); }
  std::string get_format() override { return "AgilentMassHunterD"; }
  std::string get_type() override { return "MS"; }
  std::string get_time_stamp() override { return {}; }
  std::vector<int> get_polarity() override { return std::vector<int>(records_.size(), 0); }
  std::vector<int> get_mode() override { return std::vector<int>(records_.size(), 0); }
  std::vector<int> get_level() override
  {
    std::vector<int> out;
    for (const auto &record : records_) out.push_back(record.ms_level);
    return out;
  }
  std::vector<int> get_configuration() override { return std::vector<int>(records_.size(), 0); }
  float get_min_mz() override
  {
    float value = std::numeric_limits<float>::infinity();
    for (const auto &record : records_) value = std::min(value, static_cast<float>(record.spectrum_min_x));
    return std::isfinite(value) ? value : 0.0f;
  }
  float get_max_mz() override
  {
    float value = 0.0f;
    for (const auto &record : records_) value = std::max(value, static_cast<float>(record.spectrum_max_x));
    return value;
  }
  float get_start_rt() override { return records_.empty() ? 0.0f : static_cast<float>(records_.front().scan_time_minutes * 60.0); }
  float get_end_rt() override { return records_.empty() ? 0.0f : static_cast<float>(records_.back().scan_time_minutes * 60.0); }
  bool has_ion_mobility() override { return false; }
  MASS_SPEC_SUMMARY get_summary() override
  {
    MASS_SPEC_SUMMARY summary{};
    summary.number_spectra = get_number_spectra();
    summary.number_chromatograms = get_number_chromatograms();
    summary.number_spectra_binary_arrays = get_number_spectra_binary_arrays();
    summary.min_mz = get_min_mz();
    summary.max_mz = get_max_mz();
    summary.start_rt = get_start_rt();
    summary.end_rt = get_end_rt();
    return summary;
  }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<int> out;
    for (const auto index : selected) out.push_back(index);
    return out;
  }
  std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<int> out;
    for (const auto index : selected) out.push_back(static_cast<int>(records_.at(index).scan_id));
    return out;
  }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<int> out;
    for (const auto index : selected)
      out.push_back(static_cast<int>(has_centroid(records_.at(index)) ? records_.at(index).centroid_point_count : records_.at(index).spectrum_point_count));
    return out;
  }
  std::vector<int> get_spectra_level(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<int> out;
    for (const auto index : selected) out.push_back(records_.at(index).ms_level);
    return out;
  }
  std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
  std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<int> out;
    for (const auto index : selected) out.push_back(records_.at(index).polarity);
    return out;
  }
  std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.spectrum_min_x); }); }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.spectrum_max_x); }); }
  std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.base_peak_mz); }); }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.base_peak_value); }); }
  std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.tic); }); }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &r) { return static_cast<float>(r.scan_time_minutes * 60.0); }); }
  std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { return std::vector<float>(normalize(std::move(indices)).size(), 0.0f); }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
  std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &record) { return static_cast<float>(record.precursor_mz); }); }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(std::move(indices)); }
  std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return metadata_float(indices, [](const ScanRecord &record) { return static_cast<float>(record.collision_energy); }); }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
  {
    auto selected = normalize(indices); MASS_SPEC_SPECTRA_HEADERS out; out.resize_all(selected.size());
    for (std::size_t output = 0; output < selected.size(); ++output)
    {
      const auto &record = records_.at(selected[output]);
      out.index[output] = static_cast<int>(selected[output]); out.scan[output] = static_cast<int>(record.scan_id);
      out.array_length[output] = static_cast<int>(has_centroid(record) ? record.centroid_point_count : record.spectrum_point_count); out.level[output] = record.ms_level; out.polarity[output] = record.polarity;
      out.lowmz[output] = static_cast<float>(record.spectrum_min_x); out.highmz[output] = static_cast<float>(record.spectrum_max_x);
      out.bpmz[output] = static_cast<float>(record.base_peak_mz); out.bpint[output] = static_cast<float>(record.base_peak_value);
      out.tic[output] = static_cast<float>(record.tic); out.rt[output] = static_cast<float>(record.scan_time_minutes * 60.0);
      out.precursor_mz[output] = static_cast<float>(record.precursor_mz); out.precursor_intensity[output] = static_cast<float>(record.precursor_intensity); out.activation_ce[output] = static_cast<float>(record.collision_energy);
      if (has_centroid(record))
      {
        const auto spectrum = get_spectrum(selected[output]);
        out.lowmz[output] = spectrum.lowmz; out.highmz[output] = spectrum.highmz;
        out.bpmz[output] = spectrum.bpmz; out.bpint[output] = spectrum.bpint; out.tic[output] = spectrum.tic;
      }
    }
    return out;
  }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override
  {
    if (indices.empty()) { indices.resize(chromatograms_.size()); std::iota(indices.begin(), indices.end(), 0); }
    MASS_SPEC_CHROMATOGRAMS_HEADERS out; out.resize_all(indices.size());
    for (std::size_t output = 0; output < indices.size(); ++output)
    {
      const auto &chromatogram = chromatograms_.at(static_cast<std::size_t>(indices[output]));
      out.index[output] = indices[output]; out.chromatogram_id[output] = chromatogram.id;
      out.array_length[output] = static_cast<int>(chromatogram.time.size()); out.signal_type[output] = chromatogram.signal_type;
      out.chromatogram_type[output] = chromatogram.chromatogram_type; out.detector[output] = chromatogram.detector;
      out.channel[output] = chromatogram.channel; out.units[output] = chromatogram.units;
      out.wavelength_nm[output] = chromatogram.wavelength_nm; out.interval_ms[output] = chromatogram.interval_ms;
      if (!chromatogram.time.empty()) { out.start_time[output] = chromatogram.time.front(); out.end_time[output] = chromatogram.time.back(); }
    }
    return out;
  }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override
  {
    auto selected = normalize(indices); std::vector<std::vector<std::vector<float>>> out;
    for (const auto index : selected) { const auto spectrum = get_spectrum(static_cast<int>(index)); out.push_back(spectrum.binary_data); }
    return out;
  }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override
  {
    if (indices.empty()) { indices.resize(chromatograms_.size()); std::iota(indices.begin(), indices.end(), 0); }
    std::vector<std::vector<std::vector<float>>> out;
    for (const auto index : indices) { const auto &chromatogram = chromatograms_.at(static_cast<std::size_t>(index)); out.push_back({chromatogram.time, chromatogram.intensity}); }
    return out;
  }
  std::vector<std::vector<std::string>> get_software() override { return {}; }
  std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
  {
    trace_spectrum_decode(index);
    const auto &record = records_.at(static_cast<std::size_t>(index));
    const auto profile = has_centroid(record) ? read_centroid_spectrum(file_, record) : read_profile_spectrum(file_, record);
    MASS_SPEC_SPECTRUM spectrum{};
    spectrum.index = index; spectrum.scan = static_cast<int>(record.scan_id); spectrum.array_length = static_cast<int>(profile.mz.size());
    spectrum.level = record.ms_level; spectrum.polarity = record.polarity; spectrum.lowmz = static_cast<float>(record.spectrum_min_x);
    spectrum.highmz = static_cast<float>(record.spectrum_max_x); spectrum.bpmz = static_cast<float>(record.base_peak_mz);
    spectrum.bpint = static_cast<float>(record.base_peak_value); spectrum.tic = static_cast<float>(record.tic);
    spectrum.rt = static_cast<float>(record.scan_time_minutes * 60.0); spectrum.binary_arrays_count = 2;
    spectrum.precursor_mz = static_cast<float>(record.precursor_mz); spectrum.precursor_intensity = static_cast<float>(record.precursor_intensity); spectrum.activation_ce = static_cast<float>(record.collision_energy);
    spectrum.binary_names = {"m/z", "intensity"}; spectrum.binary_data = {std::move(profile.mz), std::move(profile.intensity)};
    if (!spectrum.binary_data[0].empty())
    {
      spectrum.lowmz = spectrum.binary_data[0].front(); spectrum.highmz = spectrum.binary_data[0].back();
      spectrum.tic = std::accumulate(spectrum.binary_data[1].begin(), spectrum.binary_data[1].end(), 0.0f);
      const auto peak = std::max_element(spectrum.binary_data[1].begin(), spectrum.binary_data[1].end());
      if (peak != spectrum.binary_data[1].end()) { spectrum.bpint = *peak; spectrum.bpmz = spectrum.binary_data[0][static_cast<std::size_t>(std::distance(spectrum.binary_data[1].begin(), peak))]; }
    }
    return spectrum;
  }

private:
  std::vector<int> normalize(std::vector<int> indices) const
  {
    if (indices.empty()) { indices.resize(records_.size()); std::iota(indices.begin(), indices.end(), 0); }
    return indices;
  }
  template <typename Function>
  std::vector<float> metadata_float(const std::vector<int> &indices, Function function)
  {
    std::vector<float> out; for (const auto index : normalize(indices)) out.push_back(function(records_.at(index))); return out;
  }
  std::vector<ScanRecord> records_;
  std::vector<Chromatogram> chromatograms_;
};

std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &file)
{
  if (is_agilent_ion_mobility_directory(file))
    return create_ims_reader(file);
  return std::make_unique<AgilentReader>(file);
}
}
