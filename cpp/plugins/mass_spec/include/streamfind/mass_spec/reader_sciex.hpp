#ifndef STREAMFIND_MASS_SPEC_READER_SCIEX_HPP
#define STREAMFIND_MASS_SPEC_READER_SCIEX_HPP

#include "streamfind/mass_spec/reader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mass_spec::reader::sciex
{
struct ScanBlock
{
  std::uint32_t sample_number = 0;
  std::size_t offset = 0;
  std::vector<std::uint8_t> bytes;
};

struct IdxRecord
{
  std::size_t source_index = 0;
  std::uint32_t sample_number = 0;
  std::uint32_t scan_offset = 0;
  std::uint32_t scan_size = 0;
  float retention_time_minutes = 0.0f;
  std::uint8_t ms_level_flag = 0;
  double tic = 0.0;
  double grid_field = 0.0;
};

struct IndexedFloatRecord
{
  IdxRecord index;
  std::vector<float> fields;
};

struct CompactMrmPair
{
  float first_intensity = 0.0f;
  float second_intensity = 0.0f;
};

struct ScanPoint
{
  std::uint32_t raw_mz_bin = 0;
  std::uint32_t raw_intensity = 0;
};

struct TracePair
{
  std::vector<float> tic;
  std::vector<float> bpc;
};

struct EventRecord
{
  std::size_t ordinal = 0;
  float retention_time_minutes = 0.0f;
  std::vector<float> fields;
};

struct IntensityGroup
{
  int field_code = 0;
  std::vector<float> intensities;
};

struct Transition
{
  std::string name;
  float precursor_mz = 0.0f;
  float product_mz = 0.0f;
  float start_time = 0.0f;
  float end_time = 0.0f;
  float collision_energy = 0.0f;
};

struct MrmExperimentSeries
{
  int experiment_index = 0;
  std::vector<Transition> transitions;
  std::vector<std::vector<float>> retention_times;
  std::vector<std::vector<float>> intensities;
};

// Metadata retained by SciexReader while an MRM file is open. Payload records
// are decoded by get_chromatograms() only for requested public indices.
struct MrmExperimentMetadata
{
  int experiment_index = 0;
  std::vector<Transition> transitions;
};

struct MrmMetadata
{
  std::vector<MrmExperimentMetadata> experiments;
  bool sparse_tagged = false;
  float record_marker = 0.0f;
};

std::string scan_path_for_wiff(const std::string &wiff_path);
std::vector<ScanBlock> read_scan_blocks(const std::string &wiff_path);
std::vector<IdxRecord> read_idx_records(const std::string &wiff_path, int source_analysis_number);
std::vector<IndexedFloatRecord> read_idx_float_records(const std::string &wiff_path,
                                                        int source_analysis_number);
std::vector<EventRecord> read_idx_event_records(const std::string &wiff_path,
                                                int source_analysis_number);
std::vector<CompactMrmPair> read_compact_mrm_pairs(const std::string &wiff_path,
                                                   int source_analysis_number);
std::vector<MrmExperimentSeries> read_compact_mrm_experiments(const std::string &wiff_path,
                                                              int source_analysis_number);
MrmExperimentSeries read_sparse_tagged_mrm_series(const std::string &wiff_path,
                                                 int source_analysis_number,
                                                 float record_marker,
                                                 int transition_count);
MrmExperimentSeries build_compact_mrm_series(const std::string &wiff_path,
                                             int source_analysis_number,
                                             int experiment_index,
                                             const std::vector<Transition> &transitions,
                                             const std::vector<CompactMrmPair> &pairs);
std::vector<ScanPoint> decode_scan_payload(const std::vector<std::uint8_t> &payload);
std::vector<ScanPoint> read_scan_points(const std::string &wiff_path, const IdxRecord &record,
                                        const IdxRecord *next_record = nullptr);
std::vector<MASS_SPEC_SPECTRUM> read_tof_spectra(const std::string &wiff_path,
                                                 int source_analysis_number);
std::unique_ptr<MASS_SPEC_READER> create_reader(const std::string &file, int source_analysis_number = 1);
std::vector<MASS_SPEC_ANALYSIS> read_analysis_catalog(const std::string &wiff_path);
std::vector<EventRecord> read_event_records(const ScanBlock &block);
std::vector<IntensityGroup> decode_intensity_groups(const EventRecord &record);
TracePair decode_tic_bpc(const ScanBlock &block);
std::vector<Transition> read_transitions(const std::string &wiff_path, int source_analysis_number = 1);
std::vector<Transition> read_transitions_for_experiment(const std::string &wiff_path,
                                                        int source_analysis_number,
                                                        int period,
                                                        int experiment);
}

#endif
