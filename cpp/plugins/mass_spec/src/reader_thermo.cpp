#include "streamfind/mass_spec/reader_thermo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace mass_spec::reader::thermo
{
  namespace detail
  {
    struct ScanMetadata
    {
      int scan = 0;
      std::size_t packet_start = 0;
      std::size_t centroid_offset = 0;
      std::size_t centroid_count = 0;
      int level = 0;
      int mode = 0;
      int polarity = 0;
      double precursor_mz = 0.0;
      int precursor_charge = 0;
      float collision_energy = 0.0f;
      float isolation_width = 0.0f;
      int profile_bins = 0;
      double profile_first = 0.0;
      double profile_step = 0.0;
      std::vector<double> profile_coefficients;
      float low_mz = 0.0f;
      float high_mz = 0.0f;
      float base_peak_mz = 0.0f;
      float base_peak_intensity = 0.0f;
      float tic = 0.0f;
      float retention_time = 0.0f;
    };

    std::vector<std::uint8_t> read_file(const std::string &path)
    {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      if (!input)
        throw std::runtime_error("Unable to open Thermo RAW file: " + path);
      const auto size = input.tellg();
      if (size < 0)
        throw std::runtime_error("Unable to determine Thermo RAW file size: " + path);
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
      input.seekg(0);
      if (!bytes.empty())
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!input)
        throw std::runtime_error("Unable to read Thermo RAW file: " + path);
      return bytes;
    }

    std::uint32_t u32_at(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Thermo RAW uint32 field is truncated.");
      std::uint32_t value = 0;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      return value;
    }

    std::uint16_t u16_at(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("Thermo RAW uint16 field is truncated.");
      std::uint16_t value = 0;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      return value;
    }

    std::uint64_t u64_at(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      if (offset > bytes.size() || bytes.size() - offset < 8)
        throw std::runtime_error("Thermo RAW uint64 field is truncated.");
      std::uint64_t value = 0;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      return value;
    }

    float f32_at(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      std::uint32_t bits = u32_at(bytes, offset);
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }

    double f64_at(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      std::uint64_t bits = u64_at(bytes, offset);
      double value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }

    std::size_t pascal_end(const std::vector<std::uint8_t> &bytes, std::size_t offset)
    {
      const auto chars = static_cast<std::size_t>(u32_at(bytes, offset));
      if (offset > bytes.size() || bytes.size() - offset < 4 || chars > (bytes.size() - offset - 4) / 2)
        throw std::runtime_error("Thermo RAW Pascal string is truncated.");
      return offset + 4 + chars * 2;
    }

    struct Layout
    {
      std::string timestamp;
      std::size_t data_addr = 0;
      std::size_t scan_index_addr = 0;
      std::uint32_t first_scan = 0;
      std::uint32_t last_scan = 0;
      std::size_t error_log_addr = 0;
      std::size_t scan_trailer_addr = 0;
      std::size_t scan_params_addr = 0;
      std::size_t event_count = 0;
    };

    Layout read_layout(const std::vector<std::uint8_t> &bytes)
    {
      if ((u32_at(bytes, 0) & 0xffffU) != 0xa101U)
        throw std::runtime_error("invalid Thermo RAW file header.");
      const auto version = u32_at(bytes, 0x24);
      if (version < 64)
        throw std::runtime_error("unsupported Thermo RAW format version: " + std::to_string(version) + ".");
      std::size_t cursor = 1356 + 64;
      for (int i = 0; i < 14; ++i)
        cursor = pascal_end(bytes, cursor);
      for (int i = 0; i < 2; ++i)
        cursor = pascal_end(bytes, cursor);
      cursor += 4;
      for (int i = 0; i < 15; ++i)
        cursor = pascal_end(bytes, cursor);
      cursor += 24;
      cursor = pascal_end(bytes, cursor);
      const auto raw_info = cursor;
      const auto year = static_cast<unsigned>(u16_at(bytes, raw_info + 4));
      const auto month = static_cast<unsigned>(u16_at(bytes, raw_info + 6));
      const auto day = static_cast<unsigned>(u16_at(bytes, raw_info + 10));
      const auto hour = static_cast<unsigned>(u16_at(bytes, raw_info + 12));
      const auto minute = static_cast<unsigned>(u16_at(bytes, raw_info + 14));
      const auto second = static_cast<unsigned>(u16_at(bytes, raw_info + 16));
      const auto millis = static_cast<unsigned>(u16_at(bytes, raw_info + 18));
      char timestamp[64] = {};
      if (year >= 1900 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 && minute < 60 && second < 60 && millis < 1000)
        std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03u", year, month, day, hour, minute, second, millis);
      if (timestamp[0] == '\0')
      {
        constexpr std::uint64_t windows_epoch_100ns = 116444736000000000ULL;
        const auto filetime = u64_at(bytes, 0x28);
        if (filetime >= windows_epoch_100ns)
        {
          const auto unix_ms = (filetime - windows_epoch_100ns) / 10000ULL;
          const auto seconds = static_cast<std::time_t>(unix_ms / 1000ULL);
          const auto milliseconds = static_cast<unsigned>(unix_ms % 1000ULL);
          const auto *utc = std::gmtime(&seconds);
          if (utc != nullptr)
            std::snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                          utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                          utc->tm_hour, utc->tm_min, utc->tm_sec, milliseconds);
        }
      }
      const auto controller_count = u32_at(bytes, raw_info + 28);
      const auto data_addr = static_cast<std::size_t>(u64_at(bytes, raw_info + 808));
      const auto run_header_addr = static_cast<std::size_t>(u64_at(bytes, raw_info + 824));
      if (controller_count == 0 || data_addr >= bytes.size() || run_header_addr >= bytes.size())
        throw std::runtime_error("Thermo RAW controller pointers are invalid.");
      const auto first_scan = u32_at(bytes, run_header_addr + 8);
      const auto last_scan = u32_at(bytes, run_header_addr + 12);
      if (last_scan < first_scan)
        throw std::runtime_error("Thermo RAW scan range is invalid.");
      const auto address_base = run_header_addr + 592 + 13 * 520 + 16 + 40;
      const auto scan_index_addr = static_cast<std::size_t>(u64_at(bytes, address_base));
      const auto run_data_addr = static_cast<std::size_t>(u64_at(bytes, address_base + 8));
      const auto error_log_addr = static_cast<std::size_t>(u64_at(bytes, address_base + 24));
      const auto scan_trailer_addr = static_cast<std::size_t>(u64_at(bytes, address_base + 40));
      const auto scan_params_addr = static_cast<std::size_t>(u64_at(bytes, address_base + 48));
      const auto event_count = static_cast<std::size_t>(u32_at(bytes, run_header_addr + 592 + 13 * 520 + 16 + 8));
      if (run_data_addr != data_addr || scan_index_addr >= bytes.size() ||
          scan_trailer_addr >= bytes.size() || scan_params_addr > bytes.size() || event_count == 0)
        throw std::runtime_error("Thermo RAW run-header pointers are inconsistent.");
      return {timestamp, data_addr, scan_index_addr, first_scan, last_scan, error_log_addr, scan_trailer_addr, scan_params_addr, event_count};
    }

    struct EventInfo
    {
      int level = 0;
      int polarity = 0;
      int mode = 0;
      double precursor_mz = 0.0;
      double isolation_width = 0.0;
      double collision_energy = 0.0;
      std::vector<double> coefficients;
    };

    EventInfo event_info(const std::vector<std::uint8_t> &bytes, const Layout &layout, std::size_t index)
    {
      const auto stream_start = layout.scan_trailer_addr + 4;
      const auto stream_size = layout.scan_params_addr - stream_start;
      if (layout.event_count == 0 || stream_size % layout.event_count != 0)
        return {};
      const auto event_size = stream_size / layout.event_count;
      const auto offset = stream_start + index * event_size;
      if (event_size < 136 || offset + 136 > bytes.size())
        return {};
      const auto power = bytes[offset + 6];
      const auto polarity = bytes[offset + 4];
      const auto mode = bytes[offset + 5] <= 1 ? static_cast<int>(bytes[offset + 5]) : 0;
      const auto level = power >= 1 && power <= 8 ? static_cast<int>(power) : 0;
      const auto body = offset + 136;
      const bool dependent = level >= 2 && event_size >= 144 && body + 32 <= bytes.size();
      std::vector<double> coefficients;
      if (body + 84 <= bytes.size())
      {
        const auto count = std::min<std::size_t>(u32_at(bytes, body + 80), (bytes.size() - body - 84) / 8);
        coefficients.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
          coefficients.push_back(f64_at(bytes, body + 84 + i * 8));
      }
      return {level,
              polarity == 0 ? -1 : polarity == 1 ? 1 : 0,
              mode,
              dependent ? f64_at(bytes, body + 4) : 0.0,
              dependent ? static_cast<double>(f32_at(bytes, body + 12)) : 0.0,
              dependent ? f64_at(bytes, body + 20) : 0.0, std::move(coefficients)};
    }

    double profile_mz(double frequency, const std::vector<double> &coefficients)
    {
      if (frequency == 0.0)
        return 0.0;
      if (coefficients.size() == 4)
        return coefficients[1] + coefficients[2] / frequency + coefficients[3] / (frequency * frequency);
      if (coefficients.size() == 5 || coefficients.size() == 7)
      {
        const auto f2 = frequency * frequency;
        return coefficients[2] + coefficients[3] / f2 + coefficients[4] / (f2 * f2);
      }
      return frequency;
    }

    std::size_t generic_field_size(std::uint32_t type_code, std::uint32_t length)
    {
      if (type_code >= 1 && type_code <= 5)
        return 1;
      if (type_code == 6 || type_code == 7)
        return 2;
      if (type_code >= 8 && type_code <= 10)
        return 4;
      if (type_code == 11)
        return 8;
      if (type_code == 12)
        return length;
      if (type_code == 13)
        return static_cast<std::size_t>(length) * 2;
      return 0;
    }

    std::string utf16_label(const std::vector<std::uint8_t> &bytes, std::size_t offset, std::size_t chars)
    {
      std::string label;
      label.reserve(chars);
      for (std::size_t i = 0; i < chars; ++i)
      {
        const auto unit = static_cast<std::uint16_t>(bytes[offset + i * 2]) |
                          (static_cast<std::uint16_t>(bytes[offset + i * 2 + 1]) << 8);
        if (unit == 0)
          break;
        label.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
      }
      return label;
    }

    struct GenericField
    {
      std::string label;
      std::uint32_t type_code = 0;
      std::uint32_t length = 0;
    };

    struct ScanParamsHeader
    {
      std::vector<GenericField> fields;
      std::size_t record_size = 0;
    };

    bool generic_header_at(const std::vector<std::uint8_t> &bytes, std::size_t offset, ScanParamsHeader &header)
    {
      const auto field_count = static_cast<std::size_t>(u32_at(bytes, offset));
      if (field_count < 2 || field_count > 500)
        return false;
      std::size_t cursor = offset + 4;
      std::vector<GenericField> fields;
      fields.reserve(field_count);
      std::size_t size = 0;
      for (std::size_t i = 0; i < field_count; ++i)
      {
        const auto type_code = u32_at(bytes, cursor);
        const auto length = u32_at(bytes, cursor + 4);
        cursor += 8;
        if (type_code > 13 || length > 4096)
          return false;
        const auto chars = static_cast<std::size_t>(u32_at(bytes, cursor));
        cursor += 4;
        if (chars > 200 || cursor > bytes.size() || chars * 2 > bytes.size() - cursor)
          return false;
        fields.push_back({utf16_label(bytes, cursor, chars), type_code, length});
        cursor += chars * 2;
        size += generic_field_size(type_code, length);
      }
      std::size_t named = 0;
      for (const auto &field : fields)
        if (!field.label.empty())
          ++named;
      if (named < 2 || size == 0)
        return false;
      header = {std::move(fields), size};
      return true;
    }

    bool find_scan_params(const std::vector<std::uint8_t> &bytes, const Layout &layout, std::size_t scan_count, ScanParamsHeader &header)
    {
      const auto tail = bytes.size() > layout.scan_params_addr ? bytes.size() - layout.scan_params_addr : 0;
      const auto expected = scan_count > 0 && tail >= 4 ? tail / scan_count : 0;
      const auto start = std::min(layout.error_log_addr, bytes.size() > 4 ? bytes.size() - 4 : 0);
      const auto end = std::min(layout.scan_trailer_addr, bytes.size() > 4 ? bytes.size() - 4 : 0);
      const auto probe_end = std::min<std::size_t>(end - start, 4 * 1024 * 1024);
      for (int pass = 0; pass < 2; ++pass)
      {
        for (std::size_t offset = start; offset + 4 <= start + probe_end; offset += 2)
        {
          ScanParamsHeader candidate;
          if (!generic_header_at(bytes, offset, candidate))
            continue;
          const bool size_ok = pass == 0 && expected > 0 ? candidate.record_size == expected : true;
          if (!size_ok)
            continue;
          if (layout.scan_params_addr > bytes.size() ||
              scan_count * std::max<std::size_t>(candidate.record_size, 1) > bytes.size() - layout.scan_params_addr)
            continue;
          header = std::move(candidate);
          return true;
        }
        if (expected == 0)
          break;
      }
      return false;
    }

    struct ScanParamsValues
    {
      double precursor_mz = 0.0;
      int precursor_charge = 0;
      std::string hcd_energy;
      double hcd_energy_ev = 0.0;
      double isolation_width = 0.0;
    };

    ScanParamsValues scan_params_values(const std::vector<std::uint8_t> &bytes, const Layout &layout,
                                        const ScanParamsHeader &header, std::size_t index)
    {
      ScanParamsValues values;
      std::size_t cursor = layout.scan_params_addr + index * std::max<std::size_t>(header.record_size, 1);
      for (const auto &field : header.fields)
      {
        const auto type_code = field.type_code;
        const auto length = field.length;
        if (type_code == 0)
          continue;
        if (type_code >= 1 && type_code <= 5)
        {
          if (field.label == "Charge State:" && values.precursor_charge == 0)
            values.precursor_charge = bytes[cursor];
          cursor += 1;
        }
        else if (type_code == 6 || type_code == 7)
        {
          cursor += 2;
        }
        else if (type_code >= 8 && type_code <= 10)
        {
          if (field.label == "Charge State:" && type_code == 8)
            values.precursor_charge = static_cast<std::int32_t>(u32_at(bytes, cursor));
          cursor += 4;
        }
        else if (type_code == 11)
        {
          const auto value = f64_at(bytes, cursor);
          if (field.label == "Monoisotopic M/Z:" || field.label == "MS2 Isolation M/Z:" ||
              field.label == "Isolation Center M/Z:" || field.label == "Precursor M/Z:")
            values.precursor_mz = value;
          else if (field.label == "HCD Energy (eV):" || field.label == "HCD Energy eV:")
            values.hcd_energy_ev = value;
          else if (field.label == "MS2 Isolation Width:" || field.label == "MSn Isolation Width:")
            values.isolation_width = value;
          cursor += 8;
        }
        else if (type_code == 12)
        {
          if (field.label == "HCD Energy:" || field.label == "HCD Energy V:" || field.label == "CE:")
          {
            const auto end = std::min(cursor + length, bytes.size());
            values.hcd_energy.assign(reinterpret_cast<const char *>(bytes.data() + cursor), end - cursor);
            values.hcd_energy = values.hcd_energy.substr(0, values.hcd_energy.find('\0'));
          }
          cursor += length;
        }
        else if (type_code == 13)
        {
          cursor += static_cast<std::size_t>(length) * 2;
        }
        else
        {
          return {};
        }
      }
      return values;
    }

    float collision_energy_from(const ScanParamsValues &values)
    {
      auto trimmed = values.hcd_energy;
      while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '%' || trimmed.back() == '\r' || trimmed.back() == '\n'))
        trimmed.pop_back();
      if (!trimmed.empty())
      {
        char *end = nullptr;
        const auto energy = std::strtod(trimmed.c_str(), &end);
        if (end != trimmed.c_str() && *end == '\0')
          return static_cast<float>(energy);
      }
      if (values.hcd_energy_ev > 0.0)
        return static_cast<float>(values.hcd_energy_ev);
      return 0.0f;
    }

    bool packet_header(const std::vector<std::uint8_t> &bytes, std::size_t offset,
                       std::size_t &centroid_offset, std::size_t &centroid_count,
                       float &low_mz, float &high_mz)
    {
      if (offset > bytes.size() || bytes.size() - offset < 72)
        return false;
      const auto profile_tokens = static_cast<std::size_t>(u32_at(bytes, offset + 4));
      const auto profile_groups = u32_at(bytes, offset + 8);
      const auto encoding = u32_at(bytes, offset + 12);
      centroid_count = static_cast<std::size_t>(u32_at(bytes, offset + 16));
      const auto centroid_count_plus_one = static_cast<std::size_t>(u32_at(bytes, offset + 20));
      low_mz = f32_at(bytes, offset + 32);
      high_mz = f32_at(bytes, offset + 36);
      if (encoding != 128 || profile_tokens < 7 ||
          profile_groups != centroid_count * 2 + 1 ||
          centroid_count_plus_one != centroid_count + 1 ||
          !std::isfinite(low_mz) || !std::isfinite(high_mz) || low_mz <= 0.0f ||
          high_mz <= low_mz || high_mz > 100000.0f)
        return false;
      if (profile_tokens > (std::numeric_limits<std::size_t>::max() - 11) / 4)
        return false;
      centroid_offset = offset + (profile_tokens + 11) * 4;
      if (centroid_offset > bytes.size() || bytes.size() - centroid_offset < centroid_count * 8)
        return false;
      if (centroid_count == 0)
        return true;
      const float first_mz = f32_at(bytes, centroid_offset);
      const float first_intensity = f32_at(bytes, centroid_offset + 4);
      return std::isfinite(first_mz) && std::isfinite(first_intensity) && first_intensity >= 0.0f &&
             first_mz >= low_mz - 5.0f && first_mz <= high_mz + 5.0f;
    }

    int polarity_for_path(const std::string &path)
    {
      std::string lower = path;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value)
                     { return static_cast<char>(std::tolower(value)); });
      if (lower.find("_neg") != std::string::npos)
        return -1;
      if (lower.find("_pos") != std::string::npos)
        return 1;
      return 0;
    }

    int scan_level(const std::vector<std::uint8_t> &bytes, std::size_t packet_start)
    {
      const float marker = f32_at(bytes, packet_start + 52);
      if (std::fabs(marker + 0.8125f) < 0.01f)
        return 1;
      if (std::fabs(marker + 0.9375f) < 0.01f)
        return 2;
      return 0;
    }

    void decode_profile(const std::vector<std::uint8_t> &bytes, const ScanMetadata &metadata,
                        std::vector<float> &mz, std::vector<float> &intensity)
    {
      const auto profile = metadata.packet_start + 40;
      const auto first = f64_at(bytes, profile);
      const auto step = f64_at(bytes, profile + 8);
      const auto chunks = static_cast<std::size_t>(u32_at(bytes, profile + 16));
      const auto bins = static_cast<std::size_t>(u32_at(bytes, profile + 20));
      if (!std::isfinite(first) || !std::isfinite(step) || step == 0.0 || bins == 0 || bins > 8000000 || chunks > 65536)
        throw std::runtime_error("Thermo RAW profile preamble is invalid.");
      mz.resize(bins);
      intensity.assign(bins, 0.0f);
      for (std::size_t bin = 0; bin < bins; ++bin)
        mz[bin] = static_cast<float>(profile_mz(first + static_cast<double>(bin) * step, metadata.profile_coefficients));
      std::size_t cursor = profile + 24;
      const auto encoding = u32_at(bytes, metadata.packet_start + 12);
      for (std::size_t chunk = 0; chunk < chunks; ++chunk)
      {
        const auto first_bin = static_cast<std::size_t>(u32_at(bytes, cursor));
        const auto count = static_cast<std::size_t>(u32_at(bytes, cursor + 4));
        cursor += 8;
        double fudge = 0.0;
        if (encoding != 0)
        {
          fudge = f32_at(bytes, cursor);
          cursor += 4;
        }
        if (count == 0 || first_bin >= bins || count > bins - first_bin || cursor > bytes.size() || count * 4 > bytes.size() - cursor)
          throw std::runtime_error("Thermo RAW profile chunk is invalid.");
        for (std::size_t i = 0; i < count; ++i)
        {
          const auto value = f32_at(bytes, cursor + i * 4);
          if (value > 0.0f)
          {
            const auto bin = first_bin + i;
            mz[bin] = static_cast<float>(profile_mz(first + static_cast<double>(bin) * step + fudge, metadata.profile_coefficients));
            intensity[bin] = value;
          }
        }
        cursor += count * 4;
      }
    }

    class ThermoReader final : public MASS_SPEC_READER
    {
    public:
      explicit ThermoReader(const std::string &path) : MASS_SPEC_READER(path), path_(path)
      {
        const auto bytes = read_file(path);
        const auto layout = read_layout(bytes);
        timestamp_ = layout.timestamp;
        const int fallback_polarity = polarity_for_path(path);
        ScanParamsHeader scan_params;
        const auto scan_count = static_cast<std::size_t>(layout.last_scan - layout.first_scan + 1);
        const bool has_scan_params = find_scan_params(bytes, layout, scan_count, scan_params);
        constexpr std::size_t record_size = 88;
        std::uint64_t previous_packet_offset = 0;
        bool first = true;
        for (std::size_t index = 0; index < scan_count; ++index)
        {
          const std::size_t record = layout.scan_index_addr + index * record_size;
          const auto packet_offset = u64_at(bytes, record + 72);
          if (!first && packet_offset <= previous_packet_offset)
            throw std::runtime_error("Thermo RAW scan-index offsets are not increasing.");
          if (packet_offset > std::numeric_limits<std::size_t>::max() - layout.data_addr)
            throw std::runtime_error("Thermo RAW packet offset overflows the host size.");
          const std::size_t packet_start = layout.data_addr + static_cast<std::size_t>(packet_offset);
          const auto packet_size = static_cast<std::size_t>(u32_at(bytes, record + 20));
          if (packet_start > bytes.size() || packet_size > bytes.size() - packet_start)
            throw std::runtime_error("Thermo RAW scan packet is truncated at scan " + std::to_string(index + 1) + ".");
          std::size_t centroid_offset = 0;
          std::size_t centroid_count = 0;
          float packet_low = 0.0f;
          float packet_high = 0.0f;
          if (!packet_header(bytes, packet_start, centroid_offset, centroid_count, packet_low, packet_high))
            throw std::runtime_error("Thermo RAW packet header is invalid at scan " + std::to_string(index + 1) + ".");
          const auto event = event_info(bytes, layout, index);
          const auto params = has_scan_params
                                  ? scan_params_values(bytes, layout, scan_params, index)
                                  : ScanParamsValues{};
          const auto level = event.level == 0 ? scan_level(bytes, packet_start) : event.level;
          const auto params_energy = collision_energy_from(params);
          const auto profile_offset = packet_start + 40;
          const auto profile_first = profile_offset + 24 <= bytes.size() ? f64_at(bytes, profile_offset) : 0.0;
          const auto profile_step = profile_offset + 24 <= bytes.size() ? f64_at(bytes, profile_offset + 8) : 0.0;
          const auto profile_bins = profile_offset + 24 <= bytes.size()
                                        ? static_cast<int>(u32_at(bytes, profile_offset + 20))
                                        : 0;
          scans_.push_back({static_cast<int>(u32_at(bytes, record + 4) + 1), packet_start, centroid_offset, centroid_count,
                            level, event.mode,
                            event.polarity == 0 ? fallback_polarity : event.polarity,
                            params.precursor_mz > 0.0 ? params.precursor_mz
                                                      : (level >= 2 && event.precursor_mz > 0.0 ? event.precursor_mz : 0.0),
                            level >= 2 ? params.precursor_charge : 0,
                            level >= 2 ? (params_energy > 0.0f ? params_energy : static_cast<float>(event.collision_energy)) : 0.0f,
                            level >= 2 ? (params.isolation_width > 0.0 ? static_cast<float>(params.isolation_width)
                                                                       : static_cast<float>(event.isolation_width))
                                       : 0.0f,
                            profile_bins, profile_first, profile_step, event.coefficients,
                            static_cast<float>(f64_at(bytes, record + 56)),
                            static_cast<float>(f64_at(bytes, record + 64)),
                            static_cast<float>(f64_at(bytes, record + 48)),
                            static_cast<float>(f64_at(bytes, record + 40)),
                            static_cast<float>(f64_at(bytes, record + 32)),
                            static_cast<float>(f64_at(bytes, record + 24) * 60.0)});
          previous_packet_offset = packet_offset;
          first = false;
        }
        if (scans_.empty())
          throw std::runtime_error("Thermo RAW scan index is empty.");
      }

      int get_number_spectra() override { return static_cast<int>(scans_.size()); }
      int get_number_chromatograms() override { return 2; }
      int get_number_spectra_binary_arrays() override { return static_cast<int>(scans_.size() * 2); }
      std::string get_format() override { return "ThermoRAW"; }
      std::string get_type() override { return "MS"; }
      std::string get_time_stamp() override { return timestamp_; }
      std::vector<int> get_polarity() override { return values(&ScanMetadata::polarity); }
      std::vector<int> get_mode() override { return values(&ScanMetadata::mode); }
      std::vector<int> get_level() override { return values(&ScanMetadata::level); }
      std::vector<int> get_configuration() override { return std::vector<int>(scans_.size(), 0); }
      float get_min_mz() override
      {
        return scans_.empty() ? 0.0f : std::min_element(scans_.begin(), scans_.end(), [](const auto &a, const auto &b)
                                                          { return a.low_mz < b.low_mz; })->low_mz;
      }
      float get_max_mz() override
      {
        return scans_.empty() ? 0.0f : std::max_element(scans_.begin(), scans_.end(), [](const auto &a, const auto &b)
                                                          { return a.high_mz < b.high_mz; })->high_mz;
      }
      float get_start_rt() override { return scans_.empty() ? 0.0f : scans_.front().retention_time; }
      float get_end_rt() override { return scans_.empty() ? 0.0f : scans_.back().retention_time; }
      bool has_ion_mobility() override { return false; }
      MASS_SPEC_SUMMARY get_summary() override
      {
        MASS_SPEC_SUMMARY summary{};
        const std::filesystem::path file(path_);
        summary.file_name = file.filename().string();
        summary.file_path = path_;
        summary.file_dir = file.parent_path().string();
        summary.file_extension = file.extension().string();
        if (!summary.file_extension.empty())
          summary.file_extension.erase(summary.file_extension.begin());
        summary.number_spectra = get_number_spectra();
        summary.number_chromatograms = get_number_chromatograms();
        summary.number_spectra_binary_arrays = get_number_spectra_binary_arrays();
        summary.format = get_format();
        summary.time_stamp = timestamp_;
        summary.type = get_type();
        summary.min_mz = get_min_mz();
        summary.max_mz = get_max_mz();
        summary.start_rt = get_start_rt();
        summary.end_rt = get_end_rt();
        summary.has_ion_mobility = false;
        return summary;
      }

      std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { return selected_int(indices, &ScanMetadata::scan); }
      std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { return selected_int(indices, &ScanMetadata::scan); }
      std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override
      {
        indices = normalize(std::move(indices));
        std::vector<int> output;
        output.reserve(indices.size());
        for (int index : indices)
          if (index >= 0 && static_cast<std::size_t>(index) < scans_.size())
            output.push_back(static_cast<int>(scans_[static_cast<std::size_t>(index)].centroid_count));
        return output;
      }
      std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { return selected_int(indices, &ScanMetadata::level); }
      std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
      std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return selected_int(indices, &ScanMetadata::mode); }
      std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { return selected_int(indices, &ScanMetadata::polarity); }
      std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::low_mz); }
      std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::high_mz); }
      std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::base_peak_mz); }
      std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::base_peak_intensity); }
      std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::tic); }
      std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { return selected(indices, &ScanMetadata::retention_time); }
      std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { return std::vector<float>(normalize(std::move(indices)).size(), 0.0f); }
      std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
      std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return selected_double(std::move(indices), &ScanMetadata::precursor_mz); }
      std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return selected_double(std::move(indices), &ScanMetadata::precursor_mz); }
      std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override
      {
        indices = normalize(std::move(indices));
        std::vector<float> output;
        output.reserve(indices.size());
        for (const int index : indices)
        {
          const auto &scan = scans_.at(static_cast<std::size_t>(index));
          output.push_back(scan.level >= 2 && scan.precursor_mz > 0.0 ? static_cast<float>(scan.precursor_mz - scan.isolation_width / 2.0) : 0.0f);
        }
        return output;
      }
      std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override
      {
        indices = normalize(std::move(indices));
        std::vector<float> output;
        output.reserve(indices.size());
        for (const int index : indices)
        {
          const auto &scan = scans_.at(static_cast<std::size_t>(index));
          output.push_back(scan.level >= 2 && scan.precursor_mz > 0.0 ? static_cast<float>(scan.precursor_mz + scan.isolation_width / 2.0) : 0.0f);
        }
        return output;
      }
      std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return selected_float(std::move(indices), &ScanMetadata::collision_energy); }

      MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
      {
        indices = normalize(std::move(indices));
        MASS_SPEC_SPECTRA_HEADERS headers;
        headers.resize_all(static_cast<int>(indices.size()));
        for (std::size_t i = 0; i < indices.size(); ++i)
        {
          const auto &scan = scans_.at(static_cast<std::size_t>(indices[i]));
          headers.index[i] = static_cast<int>(i);
          headers.scan[i] = scan.scan;
          headers.array_length[i] = static_cast<int>(scan.centroid_count);
          headers.level[i] = scan.level;
          headers.mode[i] = scan.mode;
          headers.polarity[i] = scan.polarity;
          headers.lowmz[i] = scan.low_mz;
          headers.highmz[i] = scan.high_mz;
          headers.bpmz[i] = scan.base_peak_mz;
          headers.bpint[i] = scan.base_peak_intensity;
          headers.tic[i] = scan.tic;
          headers.configuration[i] = 0;
          headers.rt[i] = scan.retention_time;
          headers.mobility[i] = 0.0f;
          headers.window_mz[i] = scan.level >= 2 ? static_cast<float>(scan.precursor_mz) : 0.0f;
          headers.window_mzlow[i] = scan.level >= 2 && scan.precursor_mz > 0.0
                                        ? static_cast<float>(scan.precursor_mz - scan.isolation_width / 2.0)
                                        : 0.0f;
          headers.window_mzhigh[i] = scan.level >= 2 && scan.precursor_mz > 0.0
                                         ? static_cast<float>(scan.precursor_mz + scan.isolation_width / 2.0)
                                         : 0.0f;
          headers.precursor_mz[i] = static_cast<float>(scan.precursor_mz);
          headers.precursor_intensity[i] = 0.0f;
          headers.precursor_charge[i] = scan.precursor_charge;
          headers.activation_ce[i] = scan.collision_energy;
        }
        return headers;
      }

      MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> indices = {}) override
      {
        if (indices.empty())
          indices = {0, 1};
        MASS_SPEC_CHROMATOGRAMS_HEADERS headers;
        headers.resize_all(2);
        const float start = get_start_rt();
        const float end = get_end_rt();
        headers.index = {0, 1};
        headers.chromatogram_id = {"TIC", "BPC"};
        headers.array_length = {static_cast<int>(scans_.size()), static_cast<int>(scans_.size())};
        headers.polarity = {0, 0};
        headers.signal_type = {"MS", "MS"};
        headers.chromatogram_type = {"TIC", "BPC"};
        headers.detector = {"Thermo", "Thermo"};
        headers.channel = {"", ""};
        headers.units = {"counts", "counts"};
        headers.wavelength_nm = {0.0f, 0.0f};
        headers.interval_ms = {0.0f, 0.0f};
        headers.start_time = {start, start};
        headers.end_time = {end, end};
        headers.intensity_multiplier = {1.0f, 1.0f};
        if (indices.size() == 2 && indices[0] == 0 && indices[1] == 1)
          return headers;
        if (!indices.empty())
        {
          MASS_SPEC_CHROMATOGRAMS_HEADERS selected_headers;
          selected_headers.resize_all(static_cast<int>(indices.size()));
          for (std::size_t i = 0; i < indices.size(); ++i)
          {
            const int source = indices[i];
            if (source < 0 || source >= 2)
              continue;
            selected_headers.index[i] = headers.index[source];
            selected_headers.chromatogram_id[i] = headers.chromatogram_id[source];
            selected_headers.array_length[i] = headers.array_length[source];
            selected_headers.polarity[i] = headers.polarity[source];
            selected_headers.signal_type[i] = headers.signal_type[source];
            selected_headers.chromatogram_type[i] = headers.chromatogram_type[source];
            selected_headers.detector[i] = headers.detector[source];
            selected_headers.units[i] = headers.units[source];
            selected_headers.start_time[i] = headers.start_time[source];
            selected_headers.end_time[i] = headers.end_time[source];
            selected_headers.intensity_multiplier[i] = headers.intensity_multiplier[source];
          }
          return selected_headers;
        }
        return headers;
      }

      std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override
      {
        indices = normalize(std::move(indices));
        std::vector<std::vector<std::vector<float>>> output;
        output.reserve(indices.size());
        for (int index : indices)
        {
          const auto spectrum = get_spectrum(index);
          output.push_back(spectrum.binary_data);
        }
        return output;
      }

      std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> indices = {}) override
      {
        if (indices.empty())
          indices = {0, 1};
        std::vector<float> time;
        std::vector<float> tic;
        std::vector<float> bpc;
        time.reserve(scans_.size());
        tic.reserve(scans_.size());
        bpc.reserve(scans_.size());
        for (const auto &scan : scans_)
        {
          time.push_back(scan.retention_time);
          tic.push_back(scan.tic);
          bpc.push_back(scan.base_peak_intensity);
        }
        std::vector<std::vector<std::vector<float>>> output;
        for (int index : indices)
        {
          if (index == 0)
            output.push_back({time, tic});
          else if (index == 1)
            output.push_back({time, bpc});
        }
        return output;
      }

      std::vector<std::vector<std::string>> get_software() override { return {}; }
      std::vector<std::vector<std::string>> get_hardware() override { return {}; }

      MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
      {
        trace_spectrum_decode(index);
        if (index < 0 || static_cast<std::size_t>(index) >= scans_.size())
          return {};
        const auto &metadata = scans_[static_cast<std::size_t>(index)];
        if (metadata.centroid_count == 0)
        {
          const auto bytes = read_file(path_);
          std::vector<float> mz;
          std::vector<float> intensity;
          decode_profile(bytes, metadata, mz, intensity);
          MASS_SPEC_SPECTRUM spectrum{};
          spectrum.index = index;
          spectrum.scan = metadata.scan;
          spectrum.array_length = static_cast<int>(mz.size());
          spectrum.level = metadata.level;
          spectrum.mode = metadata.mode;
          spectrum.polarity = metadata.polarity;
          spectrum.lowmz = metadata.low_mz;
          spectrum.highmz = metadata.high_mz;
          spectrum.bpmz = metadata.base_peak_mz;
          spectrum.bpint = metadata.base_peak_intensity;
          spectrum.tic = metadata.tic;
          spectrum.rt = metadata.retention_time;
          spectrum.binary_arrays_count = 2;
          spectrum.binary_names = {"mz", "intensity"};
          spectrum.binary_data = {std::move(mz), std::move(intensity)};
          return spectrum;
        }
        std::ifstream input(path_, std::ios::binary);
        if (!input)
          throw std::runtime_error("Unable to open Thermo RAW file: " + path_);
        const auto byte_count = metadata.centroid_count * 8;
        std::vector<std::uint8_t> payload(byte_count);
        input.seekg(static_cast<std::streamoff>(metadata.centroid_offset));
        input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input)
          throw std::runtime_error("Thermo RAW centroid section is truncated at scan " + std::to_string(index + 1) + ".");
        MASS_SPEC_SPECTRUM spectrum{};
        spectrum.index = index;
        spectrum.scan = metadata.scan;
        spectrum.array_length = static_cast<int>(metadata.centroid_count);
        spectrum.level = metadata.level;
        spectrum.mode = metadata.mode;
        spectrum.polarity = metadata.polarity;
        spectrum.lowmz = metadata.low_mz;
        spectrum.highmz = metadata.high_mz;
        spectrum.bpmz = metadata.base_peak_mz;
        spectrum.bpint = metadata.base_peak_intensity;
        spectrum.tic = metadata.tic;
        spectrum.configuration = 0;
        spectrum.rt = metadata.retention_time;
        spectrum.mobility = 0.0f;
        spectrum.window_mz = metadata.level >= 2 ? static_cast<float>(metadata.precursor_mz) : 0.0f;
        spectrum.window_mzlow = metadata.level >= 2 && metadata.precursor_mz > 0.0
                                    ? static_cast<float>(metadata.precursor_mz - metadata.isolation_width / 2.0)
                                    : 0.0f;
        spectrum.window_mzhigh = metadata.level >= 2 && metadata.precursor_mz > 0.0
                                     ? static_cast<float>(metadata.precursor_mz + metadata.isolation_width / 2.0)
                                     : 0.0f;
        spectrum.precursor_mz = static_cast<float>(metadata.precursor_mz);
        spectrum.precursor_intensity = 0.0f;
        spectrum.precursor_charge = metadata.precursor_charge;
        spectrum.activation_ce = metadata.collision_energy;
        spectrum.binary_arrays_count = 2;
        spectrum.binary_names = {"mz", "intensity"};
        spectrum.binary_data.resize(2);
        spectrum.binary_data[0].reserve(metadata.centroid_count);
        spectrum.binary_data[1].reserve(metadata.centroid_count);
        for (std::size_t point = 0; point < metadata.centroid_count; ++point)
        {
          spectrum.binary_data[0].push_back(f32_at(payload, point * 8));
          spectrum.binary_data[1].push_back(f32_at(payload, point * 8 + 4));
        }
        return spectrum;
      }

    private:
      std::vector<int> normalize(std::vector<int> indices) const
      {
        if (indices.empty())
        {
          indices.resize(scans_.size());
          for (std::size_t i = 0; i < scans_.size(); ++i)
            indices[i] = static_cast<int>(i);
        }
        return indices;
      }

      template <typename Member>
      std::vector<Member> selected_int(std::vector<int> indices, Member ScanMetadata::*member) const
      {
        indices = normalize(std::move(indices));
        std::vector<Member> output;
        output.reserve(indices.size());
        for (int index : indices)
          if (index >= 0 && static_cast<std::size_t>(index) < scans_.size())
            output.push_back(scans_[static_cast<std::size_t>(index)].*member);
        return output;
      }

      std::vector<int> values(int ScanMetadata::*member) const
      {
        std::vector<int> output;
        output.reserve(scans_.size());
        for (const auto &scan : scans_)
          output.push_back(scan.*member);
        return output;
      }

      template <typename Member>
      std::vector<float> selected(std::vector<int> indices, Member ScanMetadata::*member) const
      {
        indices = normalize(std::move(indices));
        std::vector<float> output;
        output.reserve(indices.size());
        for (int index : indices)
          if (index >= 0 && static_cast<std::size_t>(index) < scans_.size())
            output.push_back(scans_[static_cast<std::size_t>(index)].*member);
        return output;
      }

      std::vector<float> selected_double(std::vector<int> indices, double ScanMetadata::*member) const
      {
        indices = normalize(std::move(indices));
        std::vector<float> output;
        output.reserve(indices.size());
        for (int index : indices)
          if (index >= 0 && static_cast<std::size_t>(index) < scans_.size())
            output.push_back(static_cast<float>(scans_[static_cast<std::size_t>(index)].*member));
        return output;
      }

      std::vector<float> selected_float(std::vector<int> indices, float ScanMetadata::*member) const
      {
        indices = normalize(std::move(indices));
        std::vector<float> output;
        output.reserve(indices.size());
        for (int index : indices)
          if (index >= 0 && static_cast<std::size_t>(index) < scans_.size())
            output.push_back(scans_[static_cast<std::size_t>(index)].*member);
        return output;
      }

      std::string path_;
      std::string timestamp_;
      std::vector<ScanMetadata> scans_;
    };
  }

  bool is_thermo_raw(const std::string &path)
  {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value)
                   { return static_cast<char>(std::tolower(value)); });
    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".raw")
      return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
      return false;
    std::vector<std::uint8_t> header(4096);
    input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(input.gcount()));
    const std::vector<std::uint8_t> finnigan = {'F', 0, 'i', 0, 'n', 0, 'n', 0, 'i', 0, 'g', 0, 'a', 0, 'n', 0};
    return std::search(header.begin(), header.end(), finnigan.begin(), finnigan.end()) != header.end();
  }

  std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &path)
  {
    return std::make_unique<detail::ThermoReader>(path);
  }
}
