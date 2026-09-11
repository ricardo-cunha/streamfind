#ifndef STREAMFIND_MASS_SPEC_READER_AGILENT_CHEMSTATION_HPP
#define STREAMFIND_MASS_SPEC_READER_AGILENT_CHEMSTATION_HPP

#include "streamfind/mass_spec/reader.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mass_spec::reader::agilent_chemstation
{
struct IndexEntry
{
  std::uint64_t offset = 0;
  std::int32_t retention_time_ms = 0;
  std::int32_t total_signal_raw = 0;
};

struct Spectrum
{
  std::int32_t retention_time_ms = 0;
  std::int32_t status_word = 0;
  std::vector<float> mz;
  std::vector<float> intensity;
};

struct DataFile
{
  std::string path;
  std::string file_type;
  std::string data_name;
  std::string operator_name;
  std::string acquisition_date;
  std::string instrument_model;
  std::string inlet;
  std::string method_file;
  std::int32_t record_count = 0;
  std::int32_t retention_time_start_ms = 0;
  std::int32_t retention_time_end_ms = 0;
  std::vector<IndexEntry> index;
};

struct Chromatogram
{
  std::string id;
  std::string detector;
  std::string channel;
  std::string units;
  float wavelength_nm = 0.0f;
  std::vector<float> time_minutes;
  std::vector<float> intensity;
};

bool is_chemstation_directory(const std::string &path);
DataFile read_data_file(const std::string &path);
Spectrum read_spectrum(const DataFile &data_file, std::size_t index);
std::vector<Chromatogram> read_chromatograms(const std::string &directory);
std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &path);
}

#endif
