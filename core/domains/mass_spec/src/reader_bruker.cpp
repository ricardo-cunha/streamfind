#include "streamfind/mass_spec/reader_bruker.hpp"

#include <duckdb.h>

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <limits>
#include <cstring>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <zstd.h>

namespace mass_spec::reader::bruker
{
namespace detail
{
class SqliteScan
{
public:
  explicit SqliteScan(const std::string &database_path)
  {
    if (duckdb_open(nullptr, &database_) != DuckDBSuccess)
      throw std::runtime_error("Unable to open DuckDB for Bruker SQLite metadata.");
    if (duckdb_connect(database_, &connection_) != DuckDBSuccess)
    {
      duckdb_close(&database_);
      throw std::runtime_error("Unable to connect DuckDB for Bruker SQLite metadata.");
    }
    query("LOAD sqlite");
    database_path_ = database_path;
  }

  ~SqliteScan()
  {
    duckdb_disconnect(&connection_);
    duckdb_close(&database_);
  }

  duckdb_result query(const std::string &sql)
  {
    duckdb_result result{};
    if (duckdb_query(connection_, sql.c_str(), &result) != DuckDBSuccess)
    {
      const std::string error = duckdb_result_error(&result) ? duckdb_result_error(&result) : "unknown DuckDB error";
      duckdb_destroy_result(&result);
      throw std::runtime_error(error);
    }
    return result;
  }

  std::string scan(const std::string &table) const
  {
    std::string quoted;
    quoted.reserve(database_path_.size() + 2);
    for (const char value : database_path_)
    {
      if (value == '\'') quoted.push_back('\'');
      quoted.push_back(value);
    }
    return "sqlite_scan('" + quoted + "','" + table + "')";
  }

private:
  duckdb_database database_ = nullptr;
  duckdb_connection connection_ = nullptr;
  std::string database_path_;
};

std::string value_string(duckdb_result &result, idx_t column, idx_t row)
{
  char *value = duckdb_value_varchar(&result, column, row);
  if (!value) return {};
  std::string output(value);
  duckdb_free(value);
  return output;
}
}

Family detect_family(const std::string &path)
{
  const std::filesystem::path root(path);
  if (!std::filesystem::is_directory(root)) return Family::Unknown;
  const auto tsf = std::filesystem::is_regular_file(root / "analysis.tsf") &&
                   std::filesystem::is_regular_file(root / "analysis.tsf_bin");
  if (tsf) return Family::Tsf;
  const auto baf = std::filesystem::is_regular_file(root / "analysis.baf") &&
                   std::filesystem::is_regular_file(root / "analysis.baf_idx") &&
                   std::filesystem::is_regular_file(root / "analysis.sqlite");
  if (baf) return Family::Baf;
  return Family::Unknown;
}

TsfLineSpectrum read_tsf_line_spectrum(const std::string &path, const TsfFrame &frame)
{
  if (detect_family(path) != Family::Tsf)
    throw std::runtime_error("Not a Bruker TSF directory: " + path);
  if (frame.tims_id < 0 || frame.num_peaks < 0)
    throw std::runtime_error("Invalid TSF frame locator or peak count.");

  const auto binary_path = std::filesystem::path(path) / "analysis.tsf_bin";
  std::ifstream input(binary_path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to open TSF binary payload: " + binary_path.string());
  input.seekg(0, std::ios::end);
  const auto file_size = input.tellg();
  const auto offset = static_cast<std::uint64_t>(frame.tims_id);
  if (file_size < 0 || offset > static_cast<std::uint64_t>(file_size) ||
      static_cast<std::uint64_t>(file_size) - offset < 8)
    throw std::runtime_error("TSF frame offset is outside analysis.tsf_bin.");
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::uint32_t block_size = 0, compressed_size = 0;
  input.read(reinterpret_cast<char *>(&block_size), sizeof(block_size));
  input.read(reinterpret_cast<char *>(&compressed_size), sizeof(compressed_size));
  if (!input || block_size < 8 || compressed_size > block_size - 8)
    throw std::runtime_error("Invalid TSF frame block header.");
  if (offset + 8ull + compressed_size > static_cast<std::uint64_t>(file_size))
    throw std::runtime_error("TSF compressed frame exceeds analysis.tsf_bin.");

  std::vector<std::uint8_t> compressed(compressed_size);
  input.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
  const auto expected = static_cast<std::uint64_t>(frame.num_peaks) * 16ull;
  if (expected > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    throw std::runtime_error("TSF frame is too large.");
  const auto content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (content_size != expected)
    throw std::runtime_error("Unexpected TSF type-3 decompressed size.");
  std::vector<std::uint8_t> decoded(static_cast<std::size_t>(expected));
  const auto result = ZSTD_decompress(decoded.data(), decoded.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(result) || result != decoded.size())
    throw std::runtime_error(std::string("TSF Zstandard decompression failed: ") + ZSTD_getErrorName(result));

  TsfLineSpectrum spectrum;
  spectrum.tof.resize(static_cast<std::size_t>(frame.num_peaks));
  spectrum.intensity.resize(static_cast<std::size_t>(frame.num_peaks));
  std::memcpy(spectrum.tof.data(), decoded.data(), spectrum.tof.size() * sizeof(double));
  const auto intensity_offset = spectrum.tof.size() * sizeof(double);
  for (std::size_t index = 0; index < spectrum.intensity.size(); ++index)
  {
    float intensity = 0.0f;
    std::memcpy(&intensity, decoded.data() + intensity_offset + index * sizeof(float), sizeof(float));
    spectrum.intensity[index] = intensity;
  }
  return spectrum;
}

TsfCalibration read_tsf_calibration(const std::string &path, const TsfFrame &frame)
{
  if (detect_family(path) != Family::Tsf)
    throw std::runtime_error("Not a Bruker TSF directory: " + path);
  detail::SqliteScan database((std::filesystem::path(path) / "analysis.tsf").string());
  const auto calibration_query = "SELECT CAST(Id AS VARCHAR),CAST(ModelType AS VARCHAR),CAST(DigitizerTimebase AS VARCHAR),CAST(DigitizerDelay AS VARCHAR),CAST(T1 AS VARCHAR),CAST(T2 AS VARCHAR),CAST(dC1 AS VARCHAR),CAST(dC2 AS VARCHAR),CAST(C0 AS VARCHAR),CAST(C1 AS VARCHAR),CAST(C2 AS VARCHAR),CAST(C3 AS VARCHAR),CAST(C4 AS VARCHAR) FROM " + database.scan("MzCalibration") + " WHERE Id=" + std::to_string(frame.mz_calibration);
  auto calibration_result = database.query(calibration_query);
  if (duckdb_row_count(&calibration_result) != 1)
  {
    duckdb_destroy_result(&calibration_result);
    throw std::runtime_error("Missing TSF MzCalibration row.");
  }
  TsfCalibration calibration;
  calibration.id = static_cast<std::int32_t>(std::stoi(detail::value_string(calibration_result, 0, 0)));
  calibration.model_type = static_cast<std::int32_t>(std::stoi(detail::value_string(calibration_result, 1, 0)));
  calibration.digitizer_timebase = std::stod(detail::value_string(calibration_result, 2, 0));
  calibration.digitizer_delay = std::stod(detail::value_string(calibration_result, 3, 0));
  calibration.t1 = std::stod(detail::value_string(calibration_result, 4, 0));
  calibration.t2 = std::stod(detail::value_string(calibration_result, 5, 0));
  calibration.dc1 = std::stod(detail::value_string(calibration_result, 6, 0));
  calibration.dc2 = std::stod(detail::value_string(calibration_result, 7, 0));
  calibration.c0 = std::stod(detail::value_string(calibration_result, 8, 0));
  calibration.c1 = std::stod(detail::value_string(calibration_result, 9, 0));
  calibration.c2 = std::stod(detail::value_string(calibration_result, 10, 0));
  calibration.c3 = std::stod(detail::value_string(calibration_result, 11, 0));
  calibration.c4 = std::stod(detail::value_string(calibration_result, 12, 0));
  duckdb_destroy_result(&calibration_result);
  auto metadata_result = database.query("SELECT Key,Value FROM " + database.scan("GlobalMetadata") + " WHERE Key IN ('MzAcqRangeLower','MzAcqRangeUpper','DigitizerNumSamples','AcquisitionSoftware')");
  for (idx_t row = 0; row < duckdb_row_count(&metadata_result); ++row)
  {
    const auto key = detail::value_string(metadata_result, 0, row);
    const auto value = detail::value_string(metadata_result, 1, row);
    if (key == "MzAcqRangeLower") calibration.mz_min = std::stod(value);
    else if (key == "MzAcqRangeUpper") calibration.mz_max = std::stod(value);
    else if (key == "DigitizerNumSamples") calibration.tof_max = static_cast<std::uint32_t>(std::stoul(value));
    else if (key == "AcquisitionSoftware") calibration.otof_control = value == "Bruker otofControl";
  }
  duckdb_destroy_result(&metadata_result);
  if (!(calibration.mz_min > 0.0 && calibration.mz_max > calibration.mz_min && calibration.tof_max > 0))
    throw std::runtime_error("Incomplete TSF calibration metadata.");
  return calibration;
}

std::vector<double> tsf_tof_to_mz(const TsfCalibration &calibration, const std::vector<double> &tof)
{
  double mz_min = calibration.mz_min;
  double mz_max = calibration.mz_max;
  if (calibration.otof_control) { mz_min -= 5.0; mz_max += 5.0; }
  const double intercept = std::sqrt(mz_min);
  const double slope = (std::sqrt(mz_max) - intercept) / static_cast<double>(calibration.tof_max);
  std::vector<double> mz;
  mz.reserve(tof.size());
  for (const auto value : tof) mz.push_back(std::pow(intercept + slope * value, 2.0));
  return mz;
}

std::vector<TsfFrame> read_tsf_frames(const std::string &path)
{
  if (detect_family(path) != Family::Tsf)
    throw std::runtime_error("Not a Bruker TSF directory: " + path);
  detail::SqliteScan database((std::filesystem::path(path) / "analysis.tsf").string());
  auto result = database.query("SELECT Id,Time,Polarity,ScanMode,MsMsType,TimsId,MaxIntensity,SummedIntensities,NumPeaks,MzCalibration,T1,T2,PropertyGroup FROM " + database.scan("Frames") + " ORDER BY Id");
  std::vector<TsfFrame> frames;
  frames.reserve(duckdb_row_count(&result));
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
  {
    TsfFrame frame;
    frame.id = duckdb_value_int64(&result, 0, row);
    frame.retention_time = duckdb_value_double(&result, 1, row);
    frame.polarity = detail::value_string(result, 2, row);
    frame.scan_mode = static_cast<std::int32_t>(duckdb_value_int64(&result, 3, row));
    frame.msms_type = static_cast<std::int32_t>(duckdb_value_int64(&result, 4, row));
    frame.tims_id = duckdb_value_int64(&result, 5, row);
    frame.max_intensity = duckdb_value_double(&result, 6, row);
    frame.summed_intensities = duckdb_value_double(&result, 7, row);
    frame.num_peaks = static_cast<std::int32_t>(duckdb_value_int64(&result, 8, row));
    frame.mz_calibration = static_cast<std::int32_t>(duckdb_value_int64(&result, 9, row));
    frame.t1 = duckdb_value_double(&result, 10, row);
    frame.t2 = duckdb_value_double(&result, 11, row);
    frame.property_group = static_cast<std::int32_t>(duckdb_value_int64(&result, 12, row));
    frames.push_back(std::move(frame));
  }
  duckdb_destroy_result(&result);
  return frames;
}

std::vector<TsfMsMsInfo> read_tsf_msms_info(const std::string &path)
{
  if (detect_family(path) != Family::Tsf)
    throw std::runtime_error("Not a Bruker TSF directory: " + path);
  detail::SqliteScan database((std::filesystem::path(path) / "analysis.tsf").string());
  auto result = database.query("SELECT Frame,Parent,TriggerMass,IsolationWidth,PrecursorCharge,CollisionEnergy FROM " + database.scan("FrameMsMsInfo") + " ORDER BY Frame");
  std::vector<TsfMsMsInfo> infos;
  infos.reserve(duckdb_row_count(&result));
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
  {
    infos.push_back({duckdb_value_int64(&result, 0, row), duckdb_value_int64(&result, 1, row),
                     duckdb_value_double(&result, 2, row), duckdb_value_double(&result, 3, row),
                     static_cast<std::int32_t>(duckdb_value_int64(&result, 4, row)),
                     duckdb_value_double(&result, 5, row)});
  }
  duckdb_destroy_result(&result);
  return infos;
}

BafLineSpectrum read_baf_line_spectrum(const std::string &path, std::uint64_t line_array_id)
{
  if (detect_family(path) != Family::Baf)
    throw std::runtime_error("Not a Bruker BAF directory: " + path);
  const auto type = static_cast<std::uint8_t>(line_array_id >> 56);
  if (type != 0x11 && type != 0x16)
    throw std::runtime_error("BAF array ID is not a line-spectrum array.");
  const auto offset = line_array_id & 0x00FFFFFFFFFFFFFFull;
  const auto binary_path = std::filesystem::path(path) / "analysis.baf";
  std::ifstream input(binary_path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to open BAF payload: " + binary_path.string());
  input.seekg(0, std::ios::end);
  const auto file_size = input.tellg();
  if (file_size < 0 || offset > static_cast<std::uint64_t>(file_size) || static_cast<std::uint64_t>(file_size) - offset < 92)
    throw std::runtime_error("BAF line-array offset is outside analysis.baf.");
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::uint32_t block_size = 0, block_type = 0;
  input.read(reinterpret_cast<char *>(&block_size), sizeof(block_size));
  input.read(reinterpret_cast<char *>(&block_type), sizeof(block_type));
  if (!input || block_type != 0xBFA01002u || block_size < 92 || offset + block_size > static_cast<std::uint64_t>(file_size))
    throw std::runtime_error("Invalid BAF LineSpectrumBlock.");
  input.seekg(static_cast<std::streamoff>(offset + 24), std::ios::beg);
  std::uint32_t count = 0;
  input.read(reinterpret_cast<char *>(&count), sizeof(count));
  const auto expected = 92ull + static_cast<std::uint64_t>(count) * 16ull;
  if (!input || expected != block_size)
    throw std::runtime_error("BAF line block size/count mismatch.");
  std::vector<std::uint8_t> payload(static_cast<std::size_t>(expected - 92ull));
  input.seekg(static_cast<std::streamoff>(offset + 92), std::ios::beg);
  input.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
  if (!input) throw std::runtime_error("Truncated BAF line block.");
  BafLineSpectrum spectrum;
  spectrum.coordinate.resize(count); spectrum.intensity.resize(count); spectrum.width.resize(count);
  std::memcpy(spectrum.coordinate.data(), payload.data(), count * sizeof(double));
  for (std::size_t i = 0; i < count; ++i)
  {
    float value = 0.0f;
    std::memcpy(&value, payload.data() + count * 8ull + i * 4ull, sizeof(value)); spectrum.intensity[i] = value;
    std::memcpy(&value, payload.data() + count * 12ull + i * 4ull, sizeof(value)); spectrum.width[i] = value;
  }
  return spectrum;
}

std::vector<BafSpectrumMetadata> read_baf_spectra_metadata(const std::string &path)
{
  if (detect_family(path) != Family::Baf)
    throw std::runtime_error("Not a Bruker BAF directory: " + path);
  detail::SqliteScan database((std::filesystem::path(path) / "analysis.sqlite").string());
  const auto query = "SELECT s.Id,s.Rt,s.AcquisitionKey,COALESCE(s.Parent,0),s.MzAcqRangeLower,s.MzAcqRangeUpper,s.SumIntensity,s.MaxIntensity,s.TransformatorId,s.ProfileMzId,s.ProfileIntensityId,s.LineMzId,s.LineIntensityId,COALESCE(a.Polarity,0),COALESCE(a.ScanMode,0),COALESCE(a.AcquisitionMode,0),COALESCE(a.MsLevel,0) FROM " + database.scan("Spectra") + " s LEFT JOIN " + database.scan("AcquisitionKeys") + " a ON a.Id=s.AcquisitionKey ORDER BY s.Id";
  auto result = database.query(query);
  std::vector<BafSpectrumMetadata> spectra;
  spectra.reserve(duckdb_row_count(&result));
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
  {
    BafSpectrumMetadata spectrum;
    spectrum.id = duckdb_value_int64(&result, 0, row);
    spectrum.retention_time = duckdb_value_double(&result, 1, row);
    spectrum.acquisition_key = static_cast<std::int32_t>(duckdb_value_int64(&result, 2, row));
    spectrum.parent = duckdb_value_int64(&result, 3, row);
    spectrum.mz_lower = static_cast<std::int32_t>(duckdb_value_int64(&result, 4, row));
    spectrum.mz_upper = static_cast<std::int32_t>(duckdb_value_int64(&result, 5, row));
    spectrum.summed_intensity = duckdb_value_double(&result, 6, row);
    spectrum.maximum_intensity = duckdb_value_double(&result, 7, row);
    spectrum.transformator_id = static_cast<std::int32_t>(duckdb_value_int64(&result, 8, row));
    spectrum.profile_mz_id = std::stoull(detail::value_string(result, 9, row));
    spectrum.profile_intensity_id = std::stoull(detail::value_string(result, 10, row));
    spectrum.line_mz_id = std::stoull(detail::value_string(result, 11, row));
    spectrum.line_intensity_id = std::stoull(detail::value_string(result, 12, row));
    spectrum.polarity = static_cast<std::int32_t>(duckdb_value_int64(&result, 13, row));
    spectrum.scan_mode = static_cast<std::int32_t>(duckdb_value_int64(&result, 14, row));
    spectrum.acquisition_mode = static_cast<std::int32_t>(duckdb_value_int64(&result, 15, row));
    spectrum.ms_level = static_cast<std::int32_t>(duckdb_value_int64(&result, 16, row));
    spectra.push_back(std::move(spectrum));
  }
  duckdb_destroy_result(&result);
  return spectra;
}

namespace detail
{
class BafBitReader
{
public:
  BafBitReader(const std::vector<std::uint8_t> &bytes, std::size_t position)
      : bytes_(bytes), position_(position) {}

  std::uint32_t read(const unsigned count)
  {
    if (count == 0 || count > 32)
      throw std::runtime_error("Invalid BAF profile bit count.");
    std::uint32_t value = 0;
    unsigned remaining = count;
    while (remaining != 0)
    {
      if (bits_ == 0)
      {
        if (position_ + 4 > bytes_.size())
          throw std::runtime_error("BAF profile bitstream ended during refill.");
        buffer_ = (static_cast<std::uint32_t>(bytes_[position_]) << 24) |
                  (static_cast<std::uint32_t>(bytes_[position_ + 1]) << 16) |
                  (static_cast<std::uint32_t>(bytes_[position_ + 2]) << 8) |
                  static_cast<std::uint32_t>(bytes_[position_ + 3]);
        position_ += 4;
        bits_ = 32;
      }
      const auto take = std::min<unsigned>(remaining, bits_);
      value = (value << take) | (buffer_ >> (32 - take));
      buffer_ <<= take;
      bits_ -= take;
      remaining -= take;
    }
    return value;
  }

private:
  const std::vector<std::uint8_t> &bytes_;
  std::size_t position_ = 0;
  std::uint32_t buffer_ = 0;
  unsigned bits_ = 0;
};

std::uint32_t read_be32(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
  if (offset + 4 > bytes.size())
    throw std::runtime_error("BAF profile preamble is truncated.");
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_le32(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
  if (offset + 4 > bytes.size())
    throw std::runtime_error("BAF profile block header is truncated.");
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}
}

BafProfileSpectrum read_baf_profile_spectrum_variant(const std::string &path, const std::uint64_t profile_array_id, const bool base_inside_sign)
{
  if (detect_family(path) != Family::Baf)
    throw std::runtime_error("Not a Bruker BAF directory: " + path);
  if (static_cast<std::uint8_t>(profile_array_id >> 56) != 0x42)
    throw std::runtime_error("BAF array ID is not a ProfileIntensityId.");
  const auto offset = profile_array_id & 0x00FFFFFFFFFFFFFFull;
  std::ifstream input(std::filesystem::path(path) / "analysis.baf", std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open BAF payload: " + path);
  input.seekg(0, std::ios::end);
  const auto file_size = input.tellg();
  if (file_size < 0 || offset > static_cast<std::uint64_t>(file_size) ||
      static_cast<std::uint64_t>(file_size) - offset < 0x34)
    throw std::runtime_error("BAF profile-array offset is outside analysis.baf.");
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::uint32_t block_size = 0, block_type = 0;
  input.read(reinterpret_cast<char *>(&block_size), sizeof(block_size));
  input.read(reinterpret_cast<char *>(&block_type), sizeof(block_type));
  if (!input || block_type != 0xBFA01001u || block_size < 0x52 ||
      offset + block_size > static_cast<std::uint64_t>(file_size))
    throw std::runtime_error("Invalid BAF DataVectorBlock.");
  std::vector<std::uint8_t> block(block_size);
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  input.read(reinterpret_cast<char *>(block.data()), static_cast<std::streamsize>(block.size()));
  if (!input || detail::read_le32(block, 0x2c) != 0xEE77u)
    throw std::runtime_error("Invalid BAF profile decoder header.");
  const auto count = static_cast<std::size_t>(detail::read_le32(block, 0x30));
  const auto table_size = static_cast<std::size_t>(detail::read_be32(block, 0x34));
  if (table_size != 0x1a || 0x38 + table_size > block.size())
    throw std::runtime_error("Unsupported BAF profile decoder table header.");
  const std::vector<std::uint8_t> table(block.begin() + 0x38, block.begin() + 0x38 + table_size);
  const std::vector<std::uint32_t> widths{0, 4, 5, 6, 7, 9, 10, 11, 32};
  const std::vector<std::uint32_t> bases{0, 1, 0x11, 0x31, 0x71, 0xf1, 0x2f1, 0x6f1, 0};
  const std::vector<std::uint32_t> slots{0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
  if (!std::equal(widths.begin(), widths.end(), table.begin()) || table.back() != 10)
    throw std::runtime_error("Unsupported BAF profile decoder table values.");
  detail::BafBitReader reader(block, 0x34 + 4 + table_size);
  BafProfileSpectrum spectrum;
  spectrum.intensity.assign(count, 0);
  std::size_t position = 0;
  std::uint32_t previous = 0;
  while (position < count)
  {
    unsigned control = 0;
    bool emitted = false;
    while (!emitted)
    {
      const auto marker = reader.read(1);
      ++control;
      if (marker == 0)
        continue;
      if (control == 16)
        throw std::runtime_error("Invalid BAF profile control terminator.");
      if (control == 10)
      {
        const auto run = reader.read(32);
        if (run == 0)
          continue;
        if (run > count - position)
          throw std::runtime_error("BAF profile zero run exceeds output length.");
        position += run;
        emitted = true;
        continue;
      }
      if (control >= slots.size())
        throw std::runtime_error("BAF profile control index is outside decoder tables.");
      const auto slot = slots[control];
      const auto width = widths[slot];
      if (width == 0)
      {
        spectrum.intensity[position++] = previous;
      }
      else if (width < 32)
      {
        const auto raw = reader.read(width + 1);
        const auto magnitude = (raw >> 1) + bases[slot];
        const auto delta = base_inside_sign
                               ? ((raw & 1u) == 0 ? static_cast<std::int64_t>(magnitude) : -static_cast<std::int64_t>(magnitude))
                               : ((raw & 1u) == 0 ? static_cast<std::int64_t>(raw >> 1) : -static_cast<std::int64_t>(raw >> 1)) + static_cast<std::int64_t>(bases[slot]);
        const auto value = static_cast<std::int64_t>(previous) + delta;
        if (value < 0 || value > std::numeric_limits<std::uint32_t>::max())
          throw std::runtime_error("BAF profile delta exceeds uint32 range.");
        previous = static_cast<std::uint32_t>(value);
        spectrum.intensity[position++] = previous;
      }
      else
      {
        previous = reader.read(32);
        spectrum.intensity[position++] = previous;
      }
      emitted = true;
    }
  }
  return spectrum;
}

BafProfileSpectrum read_baf_profile_spectrum(const std::string &path, const std::uint64_t profile_array_id)
{
  try
  {
    return read_baf_profile_spectrum_variant(path, profile_array_id, true);
  }
  catch (const std::runtime_error &first)
  {
    try
    {
      return read_baf_profile_spectrum_variant(path, profile_array_id, false);
    }
    catch (const std::runtime_error &second)
    {
      throw std::runtime_error(std::string("BAF profile decode failed with both native delta encodings: ") + first.what() + "; alternate: " + second.what());
    }
  }
}

class TsfReader final : public MASS_SPEC_READER
{
public:
  explicit TsfReader(const std::string &file) : MASS_SPEC_READER(file), frames_(read_tsf_frames(file)), msms_(read_tsf_msms_info(file)) {}
  int get_number_spectra() override { return static_cast<int>(frames_.size()); }
  int get_number_chromatograms() override { return 0; }
  int get_number_spectra_binary_arrays() override { return static_cast<int>(frames_.size() * 2); }
  std::string get_format() override { return "BrukerTSF"; }
  std::string get_type() override { return "MS"; }
  std::string get_time_stamp() override { return {}; }
  std::vector<int> get_polarity() override { return metadata_int([](const TsfFrame &f) { return f.polarity == "+" ? 1 : f.polarity == "-" ? -1 : 0; }); }
  std::vector<int> get_mode() override { return metadata_int([](const TsfFrame &f) { return f.scan_mode; }); }
  std::vector<int> get_level() override { return metadata_int([](const TsfFrame &f) { return f.msms_type == 0 ? 1 : 2; }); }
  std::vector<int> get_configuration() override { return std::vector<int>(frames_.size(), 0); }
  float get_min_mz() override { return 95.0f; }
  float get_max_mz() override { return 2505.0f; }
  float get_start_rt() override { return frames_.empty() ? 0.0f : static_cast<float>(frames_.front().retention_time); }
  float get_end_rt() override { return frames_.empty() ? 0.0f : static_cast<float>(frames_.back().retention_time); }
  bool has_ion_mobility() override { return false; }
  MASS_SPEC_SUMMARY get_summary() override
  {
    MASS_SPEC_SUMMARY s{}; s.number_spectra = get_number_spectra(); s.number_chromatograms = 0;
    s.number_spectra_binary_arrays = get_number_spectra_binary_arrays(); s.format = get_format(); s.type = get_type();
    s.min_mz = get_min_mz(); s.max_mz = get_max_mz(); s.start_rt = get_start_rt(); s.end_rt = get_end_rt();
    s.has_ion_mobility = false; s.polarity = get_polarity(); s.mode = get_mode(); s.level = get_level(); s.configuration = get_configuration(); return s;
  }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { return normalize(indices); }
  std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { return values(indices, [](const TsfFrame &f) { return static_cast<int>(f.id); }); }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override { return values(indices, [](const TsfFrame &f) { return f.num_peaks; }); }
  std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { return values(indices, [](const TsfFrame &f) { return f.msms_type == 0 ? 1 : 2; }); }
  std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return std::vector<int>(normalize(std::move(indices)).size(), 0); }
  std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return values(indices, [](const TsfFrame &f) { return f.scan_mode; }); }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { return values(indices, [](const TsfFrame &f) { return f.polarity == "+" ? 1 : f.polarity == "-" ? -1 : 0; }); }
  std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return values_float(indices, [](const TsfFrame &) { return 95.0f; }); }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return values_float(indices, [](const TsfFrame &) { return 2505.0f; }); }
  std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { return values_float(indices, [this](const TsfFrame &f) { return base_peak_mz(f); }); }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { return values_float(indices, [](const TsfFrame &f) { return static_cast<float>(f.max_intensity); }); }
  std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { return values_float(indices, [](const TsfFrame &f) { return static_cast<float>(f.summed_intensities); }); }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { return values_float(indices, [](const TsfFrame &f) { return static_cast<float>(f.retention_time); }); }
  std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { return std::vector<float>(normalize(std::move(indices)).size(), 0.0f); }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return values(indices, [this](const TsfFrame &f) { const auto *i = find_msms(f.id); return i ? static_cast<int>(i->parent) : 0; }); }
  std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return values_float(indices, [this](const TsfFrame &f) { const auto *i = find_msms(f.id); return i ? static_cast<float>(i->trigger_mass) : 0.0f; }); }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(indices); }
  std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override { return values_float(indices, [this](const TsfFrame &f) { const auto *i = find_msms(f.id); return i ? static_cast<float>(i->trigger_mass - i->isolation_width / 2.0) : 0.0f; }); }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override { return values_float(indices, [this](const TsfFrame &f) { const auto *i = find_msms(f.id); return i ? static_cast<float>(i->trigger_mass + i->isolation_width / 2.0) : 0.0f; }); }
  std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return values_float(indices, [this](const TsfFrame &f) { const auto *i = find_msms(f.id); return i ? static_cast<float>(i->collision_energy) : 0.0f; }); }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
  {
    const auto selected = normalize(indices); MASS_SPEC_SPECTRA_HEADERS out; out.resize_all(selected.size());
    for (std::size_t n = 0; n < selected.size(); ++n) { const auto &f = frames_.at(selected[n]); out.index[n] = static_cast<int>(selected[n]); out.scan[n] = static_cast<int>(f.id); out.array_length[n] = f.num_peaks; out.level[n] = f.msms_type == 0 ? 1 : 2; out.mode[n] = f.scan_mode; out.polarity[n] = f.polarity == "+" ? 1 : f.polarity == "-" ? -1 : 0; out.lowmz[n] = 95.0f; out.highmz[n] = 2505.0f; out.bpint[n] = static_cast<float>(f.max_intensity); out.tic[n] = static_cast<float>(f.summed_intensities); out.rt[n] = static_cast<float>(f.retention_time); const auto *i = find_msms(f.id); if (i) { out.precursor_mz[n] = static_cast<float>(i->trigger_mass); out.window_mz[n] = static_cast<float>(i->trigger_mass); out.window_mzlow[n] = static_cast<float>(i->trigger_mass - i->isolation_width / 2.0); out.window_mzhigh[n] = static_cast<float>(i->trigger_mass + i->isolation_width / 2.0); out.activation_ce[n] = static_cast<float>(i->collision_energy); out.precursor_charge[n] = i->precursor_charge; } }
    return out;
  }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> = {}) override { return {}; }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> out; for (const auto i : normalize(indices)) out.push_back(get_spectrum(i).binary_data); return out; }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> = {}) override { return {}; }
  std::vector<std::vector<std::string>> get_software() override { return {}; }
  std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
  {
    trace_spectrum_decode(index);
    const auto &frame = frames_.at(static_cast<std::size_t>(index)); const auto raw = read_tsf_line_spectrum(file_, frame); const auto calibration = read_tsf_calibration(file_, frame); const auto mz = tsf_tof_to_mz(calibration, raw.tof); const auto *info = find_msms(frame.id); MASS_SPEC_SPECTRUM out{}; out.index = index; out.scan = static_cast<int>(frame.id); out.array_length = static_cast<int>(mz.size()); out.level = frame.msms_type == 0 ? 1 : 2; out.mode = frame.scan_mode; out.polarity = frame.polarity == "+" ? 1 : frame.polarity == "-" ? -1 : 0; out.lowmz = 95.0f; out.highmz = 2505.0f; out.bpint = static_cast<float>(frame.max_intensity); out.tic = static_cast<float>(frame.summed_intensities); out.rt = static_cast<float>(frame.retention_time); if (info) { out.window_mz = static_cast<float>(info->trigger_mass); out.window_mzlow = static_cast<float>(info->trigger_mass - info->isolation_width / 2.0); out.window_mzhigh = static_cast<float>(info->trigger_mass + info->isolation_width / 2.0); out.precursor_mz = static_cast<float>(info->trigger_mass); out.precursor_charge = info->precursor_charge; out.activation_ce = static_cast<float>(info->collision_energy); } out.binary_arrays_count = 2; out.binary_names = {"m/z", "intensity"}; out.binary_data.resize(2); out.binary_data[0].reserve(mz.size()); out.binary_data[1].reserve(raw.intensity.size()); for (std::size_t n = 0; n < mz.size(); ++n) { out.binary_data[0].push_back(static_cast<float>(mz[n])); out.binary_data[1].push_back(static_cast<float>(raw.intensity[n])); } return out;
  }
private:
  std::vector<int> normalize(std::vector<int> indices) const { if (indices.empty()) { indices.resize(frames_.size()); std::iota(indices.begin(), indices.end(), 0); } return indices; }
  template <typename F> std::vector<int> values(const std::vector<int> &indices, F f) const { std::vector<int> out; for (const auto i : normalize(indices)) out.push_back(f(frames_.at(i))); return out; }
  template <typename F> std::vector<float> values_float(const std::vector<int> &indices, F f) const { std::vector<float> out; for (const auto i : normalize(indices)) out.push_back(f(frames_.at(i))); return out; }
  template <typename F> std::vector<int> metadata_int(F f) const { return values({}, f); }
  const TsfMsMsInfo *find_msms(std::int64_t frame) const { for (const auto &i : msms_) if (i.frame == frame) return &i; return nullptr; }
  float base_peak_mz(const TsfFrame &frame) const { const auto raw = read_tsf_line_spectrum(file_, frame); const auto cal = read_tsf_calibration(file_, frame); const auto mz = tsf_tof_to_mz(cal, raw.tof); const auto it = std::max_element(raw.intensity.begin(), raw.intensity.end()); return it == raw.intensity.end() ? 0.0f : static_cast<float>(mz[static_cast<std::size_t>(std::distance(raw.intensity.begin(), it))]); }
  std::vector<TsfFrame> frames_; std::vector<TsfMsMsInfo> msms_;
};

std::unique_ptr<MASS_SPEC_READER> create_tsf_reader(const std::string &path)
{
  return std::make_unique<TsfReader>(path);
}

std::size_t baf_profile_point_count(const std::string &path, const std::uint64_t profile_array_id)
{
  const auto offset = profile_array_id & 0x00FFFFFFFFFFFFFFull;
  std::ifstream input(std::filesystem::path(path) / "analysis.baf", std::ios::binary);
  if (!input)
    throw std::runtime_error("Unable to open BAF payload: " + path);
  input.seekg(static_cast<std::streamoff>(offset + 0x30), std::ios::beg);
  std::uint32_t count = 0;
  input.read(reinterpret_cast<char *>(&count), sizeof(count));
  if (!input)
    throw std::runtime_error("BAF profile-array point count is outside analysis.baf.");
  return static_cast<std::size_t>(count);
}

class BafReader final : public MASS_SPEC_READER
{
public:
  explicit BafReader(const std::string &file) : MASS_SPEC_READER(file), spectra_(read_baf_spectra_metadata(file))
  {
    if (spectra_.empty())
      throw std::runtime_error("Bruker BAF contains no spectra.");
  }
  int get_number_spectra() override { return static_cast<int>(spectra_.size()); }
  int get_number_chromatograms() override { return 0; }
  int get_number_spectra_binary_arrays() override { return static_cast<int>(spectra_.size() * 2); }
  std::string get_format() override { return "BrukerBAF"; }
  std::string get_type() override { return "MS"; }
  std::string get_time_stamp() override { return {}; }
  std::vector<int> get_polarity() override { return values([](const auto &s) { return s.polarity; }); }
  std::vector<int> get_mode() override { return values([](const auto &s) { return s.scan_mode; }); }
  std::vector<int> get_level() override { return values([](const auto &s) { return s.ms_level; }); }
  std::vector<int> get_configuration() override { return values([](const auto &s) { return s.acquisition_mode; }); }
  float get_min_mz() override { return static_cast<float>(spectra_.front().mz_lower); }
  float get_max_mz() override { return static_cast<float>(spectra_.front().mz_upper); }
  float get_start_rt() override { return static_cast<float>(spectra_.front().retention_time); }
  float get_end_rt() override { return static_cast<float>(spectra_.back().retention_time); }
  bool has_ion_mobility() override { return false; }
  MASS_SPEC_SUMMARY get_summary() override
  {
    MASS_SPEC_SUMMARY s{}; s.number_spectra = get_number_spectra(); s.number_chromatograms = 0;
    s.number_spectra_binary_arrays = get_number_spectra_binary_arrays(); s.format = get_format(); s.type = get_type();
    s.min_mz = get_min_mz(); s.max_mz = get_max_mz(); s.start_rt = get_start_rt(); s.end_rt = get_end_rt();
    s.has_ion_mobility = false; s.polarity = get_polarity(); s.mode = get_mode(); s.level = get_level(); s.configuration = get_configuration(); return s;
  }
  std::vector<int> get_spectra_index(std::vector<int> indices = {}) override { return normalize(indices); }
  std::vector<int> get_spectra_scan_number(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return static_cast<int>(s.id); }); }
  std::vector<int> get_spectra_array_length(std::vector<int> indices = {}) override { return selected(indices, [this](const auto &s) { return static_cast<int>(baf_profile_point_count(file_, s.profile_intensity_id)); }); }
  std::vector<int> get_spectra_level(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return s.ms_level; }); }
  std::vector<int> get_spectra_configuration(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return s.acquisition_mode; }); }
  std::vector<int> get_spectra_mode(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return s.scan_mode; }); }
  std::vector<int> get_spectra_polarity(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return s.polarity; }); }
  std::vector<float> get_spectra_lowmz(std::vector<int> indices = {}) override { return selected_float(indices, [](const auto &s) { return static_cast<float>(s.mz_lower); }); }
  std::vector<float> get_spectra_highmz(std::vector<int> indices = {}) override { return selected_float(indices, [](const auto &s) { return static_cast<float>(s.mz_upper); }); }
  std::vector<float> get_spectra_bpmz(std::vector<int> indices = {}) override { return selected_float(indices, [this](const auto &s) { return base_peak_mz(s); }); }
  std::vector<float> get_spectra_bpint(std::vector<int> indices = {}) override { return selected_float(indices, [](const auto &s) { return static_cast<float>(s.maximum_intensity); }); }
  std::vector<float> get_spectra_tic(std::vector<int> indices = {}) override { return selected_float(indices, [](const auto &s) { return static_cast<float>(s.summed_intensity); }); }
  std::vector<float> get_spectra_rt(std::vector<int> indices = {}) override { return selected_float(indices, [](const auto &s) { return static_cast<float>(s.retention_time); }); }
  std::vector<float> get_spectra_mobility(std::vector<int> indices = {}) override { return std::vector<float>(normalize(std::move(indices)).size(), 0.0f); }
  std::vector<int> get_spectra_precursor_scan(std::vector<int> indices = {}) override { return selected(indices, [](const auto &s) { return static_cast<int>(s.parent); }); }
  std::vector<float> get_spectra_precursor_mz(std::vector<int> indices = {}) override { return std::vector<float>(normalize(indices).size(), 0.0f); }
  std::vector<float> get_spectra_precursor_window_mz(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(indices); }
  std::vector<float> get_spectra_precursor_window_mzlow(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(indices); }
  std::vector<float> get_spectra_precursor_window_mzhigh(std::vector<int> indices = {}) override { return get_spectra_precursor_mz(indices); }
  std::vector<float> get_spectra_collision_energy(std::vector<int> indices = {}) override { return std::vector<float>(normalize(indices).size(), 0.0f); }
  MASS_SPEC_SPECTRA_HEADERS get_spectra_headers(std::vector<int> indices = {}, bool = false) override
  {
    const auto selected_indices = normalize(indices); MASS_SPEC_SPECTRA_HEADERS out; out.resize_all(selected_indices.size());
    for (std::size_t n = 0; n < selected_indices.size(); ++n)
    {
      const auto &s = spectra_.at(selected_indices[n]); const auto length = static_cast<int>(baf_profile_point_count(file_, s.profile_intensity_id));
      out.index[n] = static_cast<int>(selected_indices[n]); out.scan[n] = static_cast<int>(s.id); out.array_length[n] = length;
      out.level[n] = s.ms_level; out.mode[n] = s.scan_mode; out.polarity[n] = s.polarity; out.configuration[n] = s.acquisition_mode;
      out.lowmz[n] = static_cast<float>(s.mz_lower); out.highmz[n] = static_cast<float>(s.mz_upper); out.bpint[n] = static_cast<float>(s.maximum_intensity);
      out.tic[n] = static_cast<float>(s.summed_intensity); out.rt[n] = static_cast<float>(s.retention_time); out.precursor_charge[n] = 0;
    }
    return out;
  }
  MASS_SPEC_CHROMATOGRAMS_HEADERS get_chromatograms_headers(std::vector<int> = {}) override { return {}; }
  std::vector<std::vector<std::vector<float>>> get_spectra(std::vector<int> indices = {}) override { std::vector<std::vector<std::vector<float>>> out; for (const auto i : normalize(indices)) out.push_back(get_spectrum(static_cast<int>(i)).binary_data); return out; }
  std::vector<std::vector<std::vector<float>>> get_chromatograms(std::vector<int> = {}) override { return {}; }
  std::vector<std::vector<std::string>> get_software() override { return {}; }
  std::vector<std::vector<std::string>> get_hardware() override { return {}; }
  MASS_SPEC_SPECTRUM get_spectrum(const int &index) override
  {
    trace_spectrum_decode(index);
    const auto &s = spectra_.at(static_cast<std::size_t>(index)); const auto profile = read_baf_profile_spectrum(file_, s.profile_intensity_id);
    MASS_SPEC_SPECTRUM out{}; out.index = index; out.scan = static_cast<int>(s.id); out.array_length = static_cast<int>(profile.intensity.size()); out.level = s.ms_level;
    out.mode = s.scan_mode; out.polarity = s.polarity; out.lowmz = static_cast<float>(s.mz_lower); out.highmz = static_cast<float>(s.mz_upper);
    out.bpint = static_cast<float>(s.maximum_intensity); out.tic = static_cast<float>(s.summed_intensity); out.rt = static_cast<float>(s.retention_time);
    out.binary_arrays_count = 2; out.binary_names = {"m/z", "intensity"}; out.binary_data.resize(2); out.binary_data[0].resize(profile.intensity.size()); out.binary_data[1].resize(profile.intensity.size());
    const auto step = profile.intensity.size() > 1 ? static_cast<double>(s.mz_upper - s.mz_lower) / static_cast<double>(profile.intensity.size() - 1) : 0.0;
    double decoded_tic = 0.0; for (std::size_t n = 0; n < profile.intensity.size(); ++n) { out.binary_data[0][n] = static_cast<float>(s.mz_lower + step * static_cast<double>(n)); out.binary_data[1][n] = static_cast<float>(profile.intensity[n]); decoded_tic += profile.intensity[n]; } out.tic = static_cast<float>(decoded_tic); return out;
  }
private:
  std::vector<int> normalize(std::vector<int> indices) const { if (indices.empty()) { indices.resize(spectra_.size()); std::iota(indices.begin(), indices.end(), 0); } return indices; }
  template <typename F> std::vector<int> selected(const std::vector<int> &indices, F f) const { std::vector<int> out; for (const auto i : normalize(indices)) out.push_back(f(spectra_.at(static_cast<std::size_t>(i)))); return out; }
  template <typename F> std::vector<float> selected_float(const std::vector<int> &indices, F f) const { std::vector<float> out; for (const auto i : normalize(indices)) out.push_back(f(spectra_.at(static_cast<std::size_t>(i)))); return out; }
  template <typename F> std::vector<int> values(F f) const { return selected({}, f); }
  float base_peak_mz(const BafSpectrumMetadata &s) const { const auto values = read_baf_profile_spectrum(file_, s.profile_intensity_id).intensity; const auto it = std::max_element(values.begin(), values.end()); if (it == values.end()) return 0.0f; const auto index = static_cast<std::size_t>(std::distance(values.begin(), it)); return static_cast<float>(s.mz_lower + (s.mz_upper - s.mz_lower) * static_cast<double>(index) / static_cast<double>(values.size() - 1)); }
  std::vector<BafSpectrumMetadata> spectra_;
};

std::unique_ptr<MASS_SPEC_READER> create_baf_reader(const std::string &path)
{
  return std::make_unique<BafReader>(path);
}
}
