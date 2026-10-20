#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace streamfind::raman::reader {

struct MetadataField {
    std::string key;
    std::string value;
    std::size_t occurrence = 0;
};

struct SpectrumPoint {
    double coordinate = 0.0;
    std::optional<double> intensity;
    std::size_t source_index = 0;
};

struct SpectrumFrame {
    // One callback represents exactly one spectrum; time_value is the
    // acquisition coordinate for this spectrum, when known.
    std::size_t index = 0;
    std::optional<double> time_value;
    std::vector<SpectrumPoint> points;
};

struct SpectrumFile {
    std::filesystem::path path;
    std::vector<MetadataField> metadata;
    std::string format;
    std::string axis_kind;
    std::string axis_unit;
    std::string signal_kind;
    std::string signal_unit;
    std::size_t point_count = 0;
    std::size_t spectrum_count = 0;
    std::optional<double> kinetic_cycle_seconds;
    bool has_frame_time = false;
};

using SpectrumCallback = std::function<void(const SpectrumFile &, const SpectrumFrame &)>;

/** Read supported Raman/IR source formats and emit one spectrum frame at a
 * time. The frame callback gives consumers a format-independent interface.
 */
SpectrumFile read_spectrum_file(const std::filesystem::path &path,
                                const SpectrumCallback &spectrum_callback);

SpectrumFile read_asc_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback);
SpectrumFile read_sif_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback);
SpectrumFile read_text_file(const std::filesystem::path &path,
                            const SpectrumCallback &spectrum_callback);
SpectrumFile read_wip_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback);
SpectrumFile read_smf_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback);

std::string metadata_value(const std::vector<MetadataField> &metadata,
                           const std::string &key);

}  // namespace streamfind::raman::reader
