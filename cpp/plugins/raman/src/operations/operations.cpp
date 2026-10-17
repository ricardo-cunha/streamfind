#include "operations.hpp"

#include "readers/reader.hpp"
#include "operations/spectral_savgol.hpp"
#include "operations/spectral_normalization.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::raman::operations {

namespace detail {

using Json = nlohmann::json;
using RowBuffer = std::vector<Json>;

struct SpectrumFrameText {
    std::string spectrum_id;
    std::string frame_time_unit;
    std::string time_kind;
    std::string axis_kind;
    std::string axis_unit;
    std::string axis_order;
    std::string signal_kind;
    std::string signal_unit;
};

extern const std::vector<std::string> point_columns;

class SpectrumPointBatch {
  public:
    static constexpr std::size_t capacity = 65536;

    SpectrumPointBatch() {
        for (std::size_t column = 0; column < point_columns.size(); ++column) {
            if (is_string_column(column)) strings_[column].reserve(capacity);
            else if (is_integer_column(column)) integers_[column].reserve(capacity);
            else reals_[column].reserve(capacity);
        }
        frame_time_validity_.reserve((capacity + 7) / 8);
        time_validity_.reserve((capacity + 7) / 8);
        intensity_validity_.reserve((capacity + 7) / 8);
    }

    bool empty() const noexcept { return row_count_ == 0; }
    bool full() const noexcept { return row_count_ >= capacity; }
    std::size_t size() const noexcept { return row_count_; }

    void append(const std::string &analysis_id, const std::string &source_path,
                const SpectrumFrameText &frame, std::size_t frame_index,
                std::size_t point_index, std::size_t source_index,
                const std::optional<double> &frame_time,
                const std::optional<double> &time_seconds, double coordinate,
                const std::optional<double> &intensity) {
        append_string(0, analysis_id);
        append_string(1, frame.spectrum_id);
        append_integer(2, static_cast<std::int64_t>(frame_index));
        append_integer(3, static_cast<std::int64_t>(frame_index));
        append_string(4, source_path);
        append_integer(5, static_cast<std::int64_t>(point_index));
        append_integer(6, static_cast<std::int64_t>(source_index));
        append_real(7, frame_time);
        append_string(8, frame.frame_time_unit);
        append_real(9, time_seconds);
        append_string(10, frame.time_kind);
        append_string(11, frame.axis_kind);
        append_string(12, frame.axis_unit);
        append_string(13, frame.axis_kind);
        append_string(14, frame.axis_unit);
        append_string(15, frame.axis_order);
        append_string(16, frame.signal_kind);
        append_string(17, frame.signal_unit);
        append_real(18, coordinate);
        append_real(19, coordinate);
        append_real(20, intensity);
        ++row_count_;
    }

    void emit(sdk::PluginProjectAccess &access) {
        if (empty()) return;
        std::array<streamfind_plugin_batch_column, 21> columns{};
        for (std::size_t column = 0; column < columns.size(); ++column) {
            auto &descriptor = columns[column];
            descriptor.name = point_columns[column].data();
            descriptor.name_size = static_cast<std::uint32_t>(point_columns[column].size());
            descriptor.flags = 0;
            descriptor.row_count = row_count_;
            if (is_string_column(column)) {
                descriptor.type = STREAMFIND_PLUGIN_COLUMN_UTF8;
                descriptor.data = strings_[column].data();
                descriptor.element_size = sizeof(streamfind_plugin_string_view);
            } else if (is_integer_column(column)) {
                descriptor.type = STREAMFIND_PLUGIN_COLUMN_INT64;
                descriptor.data = integers_[column].data();
                descriptor.element_size = sizeof(std::int64_t);
            } else {
                descriptor.type = STREAMFIND_PLUGIN_COLUMN_FLOAT64;
                descriptor.data = reals_[column].data();
                descriptor.element_size = sizeof(double);
            }
            if (column == 7) descriptor.validity_bitmap = frame_time_validity_.data();
            else if (column == 9) descriptor.validity_bitmap = time_validity_.data();
            else if (column == 20) descriptor.validity_bitmap = intensity_validity_.data();
            else descriptor.validity_bitmap = nullptr;
        }
        access.emit_table_batch("spectraTable", std::vector<streamfind_plugin_batch_column>(
                                                     columns.begin(), columns.end()),
                                row_count_);
        clear();
    }

  private:
    static bool is_string_column(std::size_t column) {
        return column == 0 || column == 1 || column == 4 || column == 8 ||
               (column >= 10 && column <= 17);
    }

    static bool is_integer_column(std::size_t column) {
        return column == 2 || column == 3 || column == 5 || column == 6;
    }

    void append_string(std::size_t column, const std::string &value) {
        strings_[column].push_back({value.data(), static_cast<std::uint32_t>(value.size())});
    }

    void append_integer(std::size_t column, std::int64_t value) {
        integers_[column].push_back(value);
    }

    void append_real(std::size_t column, double value) {
        reals_[column].push_back(value);
        if (column == 7) set_validity(frame_time_validity_, value);
        else if (column == 9) set_validity(time_validity_, value);
        else if (column == 20) set_validity(intensity_validity_, value);
    }

    void append_real(std::size_t column, const std::optional<double> &value) {
        reals_[column].push_back(value.value_or(0.0));
        if (column == 7) set_validity(frame_time_validity_, value.has_value());
        else if (column == 9) set_validity(time_validity_, value.has_value());
        else if (column == 20) set_validity(intensity_validity_, value.has_value());
    }

    void set_validity(std::vector<std::uint8_t> &bitmap, double) {
        set_validity(bitmap, true);
    }

    void set_validity(std::vector<std::uint8_t> &bitmap, bool valid) {
        const auto byte_index = row_count_ / 8;
        if (byte_index == bitmap.size()) bitmap.push_back(0);
        if (valid) bitmap[byte_index] |= static_cast<std::uint8_t>(1u << (row_count_ % 8));
    }

    void clear() {
        for (auto &column : strings_) column.clear();
        for (auto &column : integers_) column.clear();
        for (auto &column : reals_) column.clear();
        frame_time_validity_.clear();
        time_validity_.clear();
        intensity_validity_.clear();
        row_count_ = 0;
    }

    std::array<std::vector<streamfind_plugin_string_view>, 21> strings_;
    std::array<std::vector<std::int64_t>, 21> integers_;
    std::array<std::vector<double>, 21> reals_;
    std::vector<std::uint8_t> frame_time_validity_;
    std::vector<std::uint8_t> time_validity_;
    std::vector<std::uint8_t> intensity_validity_;
    std::size_t row_count_ = 0;
};

const std::vector<std::string> analysis_columns = {
    "analysis_id", "analysis", "file_path", "file_name", "format", "technique",
    "acquisition_kind", "number_spectra", "number_points", "has_time", "time_kind",
    "axis_kind", "axis_unit", "signal_kind", "signal_unit", "start_time_seconds",
    "end_time_seconds", "metadata_json"};
const std::vector<std::string> analysis_types = {
    "string", "string", "string", "string", "string", "string", "string", "integer",
    "integer", "boolean", "string", "string", "string", "string", "string", "real",
    "real", "object"};

const std::vector<std::string> summary_columns = {
    "analysis_id", "spectrum_id", "spectrum_index", "source_frame_index", "source_path",
    "point_count", "frame_time_value", "frame_time_unit", "time_seconds", "time_kind",
    "axis_kind", "axis_unit", "source_axis_kind", "source_axis_unit", "axis_order",
    "signal_kind", "signal_unit"};
const std::vector<std::string> summary_types = {
    "string", "string", "integer", "integer", "string", "integer", "real", "string",
    "real", "string", "string", "string", "string", "string", "string", "string", "string"};

const std::vector<std::string> point_columns = {
    "analysis_id", "spectrum_id", "spectrum_index", "source_frame_index", "source_path",
    "point_index", "source_point_index", "frame_time_value", "frame_time_unit",
    "time_seconds", "time_kind", "axis_kind", "axis_unit", "source_axis_kind",
    "source_axis_unit", "axis_order", "signal_kind", "signal_unit",
    "source_spectral_coordinate", "spectral_coordinate", "intensity"};
const std::vector<std::string> point_types = {
    "string", "string", "integer", "integer", "string", "integer", "integer", "real",
    "string", "real", "string", "string", "string", "string", "string", "string",
    "string", "string", "real", "real", "real"};
const std::vector<std::string> baseline_columns = {
    "analysis_id", "spectrum_id", "point_index", "baseline_intensity"};
const std::vector<std::string> baseline_types = {"string", "string", "integer", "real"};

const std::vector<std::string> metadata_columns = {
    "analysis_id", "source_path", "metadata_key", "occurrence", "metadata_value"};
const std::vector<std::string> metadata_types = {
    "string", "string", "string", "integer", "string"};

std::string option_string(const Json &parameters, const char *name,
                          const std::string &default_value) {
    const auto value = parameters.find(name);
    if (value == parameters.end()) return default_value;
    if (!value->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return value->get<std::string>();
}

std::string axis_unit_for(const std::string &kind, const std::string &override_unit) {
    if (!override_unit.empty()) return override_unit;
    if (kind == "raman_shift" || kind == "wavenumber") return "cm^-1";
    if (kind == "wavelength") return "nm";
    if (kind == "pixel") return "pixel";
    return "unknown";
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string infer_technique(const reader::SpectrumFile &source) {
    const auto format = lower_copy(source.format);
    if (format.find("smf") != std::string::npos ||
        lower_copy(source.signal_kind).find("transmittance") != std::string::npos)
        return "infrared";
    for (const auto &field : source.metadata) {
        const auto key = lower_copy(field.key);
        if (key.find("technique") == std::string::npos &&
            key.find("application") == std::string::npos &&
            key.find("measurement type") == std::string::npos)
            continue;
        const auto value = lower_copy(field.value);
        if (value.find("infrared") != std::string::npos || value.find("ftir") != std::string::npos)
            return "infrared";
        if (value.find("raman") != std::string::npos) return "raman";
    }
    return "raman";
}

std::string infer_axis_kind(const reader::SpectrumFile &source,
                            const reader::SpectrumFrame &frame,
                            const std::string &technique) {
    if (!source.axis_kind.empty()) return source.axis_kind;
    if (source.axis_unit == "cm^-1")
        return technique == "infrared" ? "wavenumber" : "raman_shift";
    if (source.axis_unit == "nm" || source.axis_unit == "um" || source.axis_unit == "µm")
        return "wavelength";
    if (source.axis_unit == "pixel" || source.axis_unit == "point") return "pixel";
    if (frame.points.size() > 1) {
        bool one_based_pixel_axis = true;
        for (std::size_t index = 0; index < frame.points.size(); ++index) {
            if (frame.points[index].coordinate != static_cast<double>(index + 1)) {
                one_based_pixel_axis = false;
                break;
            }
        }
        if (one_based_pixel_axis) return "pixel";
    }
    if (technique == "raman" && !frame.points.empty() &&
        frame.points.front().coordinate < 0.0)
        return "raman_shift";
    return "unknown";
}

void emit_batches(sdk::PluginProjectAccess &access,
                  const std::string &contract,
                  const std::vector<std::string> &columns,
                  const std::vector<std::string> &types,
                  const RowBuffer &rows,
                  std::size_t begin,
                  std::size_t count) {
    if (count == 0) return;
    Json batch = Json::array();
    for (std::size_t index = begin; index < begin + count; ++index)
        batch.push_back(rows[index]);
    access.emit_table_rows(contract, columns, types, batch);
}

Json read_raman_files(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto source_paths = parameters.find("source_paths");
    if (source_paths == parameters.end() || !source_paths->is_array() || source_paths->empty())
        throw std::invalid_argument("source_paths must be a non-empty array of supported Raman/IR file paths");

    std::vector<std::filesystem::path> paths;
    paths.reserve(source_paths->size());
    for (const auto &value : *source_paths) {
        if (!value.is_string() || value.get<std::string>().empty())
            throw std::invalid_argument("source_paths must contain non-empty strings");
        std::filesystem::path path(value.get<std::string>());
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            throw std::invalid_argument("Raman source path is not a readable file: " + path.string());
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension != ".asc" && extension != ".sif" && extension != ".txt" &&
            extension != ".wip" && extension != ".smf")
            throw std::invalid_argument("Raman reader does not support file extension " + extension +
                                        ": " + path.string());
        paths.push_back(std::move(path));
    }

    RowBuffer analyses;
    RowBuffer metadata_rows;
    SpectrumPointBatch points;
    std::size_t total_spectra = 0;
    std::size_t total_points = 0;
    std::size_t total_metadata = 0;
    std::size_t emitted_points = 0;

    for (std::size_t file_index = 0; file_index < paths.size(); ++file_index) {
        if (access.is_cancelled()) throw std::runtime_error("Raman file import cancelled");
        const auto &path = paths[file_index];
        const auto analysis_id = path.lexically_normal().generic_string();
        const auto analysis_name = path.stem().string();
        access.report_progress(static_cast<double>(file_index) / static_cast<double>(paths.size()),
                               "Reading Raman spectrum file " + path.filename().string());

        reader::SpectrumFile file;
        std::size_t file_point_count = 0;
        std::optional<double> first_time_seconds;
        std::optional<double> last_time_seconds;
        std::string file_axis_kind;
        std::string file_axis_unit;
        std::string frame_time_unit = "unknown";
        std::string resolved_time_kind = "unknown";
        const auto source_path = path.generic_string();
        std::deque<SpectrumFrameText> frame_text;

        file = reader::read_spectrum_file(path, [&](const reader::SpectrumFile &source,
                                               const reader::SpectrumFrame &frame) {
            if (access.is_cancelled()) throw std::runtime_error("Raman file import cancelled");
            if (frame.points.empty()) throw std::runtime_error("spectrum reader emitted an empty spectrum");
            const auto technique = infer_technique(source);
            const auto axis_kind = infer_axis_kind(source, frame, technique);
            const auto axis_unit = !source.axis_unit.empty()
                                       ? source.axis_unit : axis_unit_for(axis_kind, "");
            if (file_axis_kind.empty()) {
                file_axis_kind = axis_kind;
                file_axis_unit = axis_unit;
            } else if (file_axis_kind != axis_kind || file_axis_unit != axis_unit) {
                throw std::runtime_error("ASC file changes spectral axis kind or unit between frames: " +
                                         path.string());
            }

            if (frame.time_value && source.kinetic_cycle_seconds) {
                resolved_time_kind = "elapsed_time";
                frame_time_unit = "seconds";
            }

            std::optional<double> time_seconds;
            if (frame.time_value && source.kinetic_cycle_seconds) {
                const auto seconds = *frame.time_value;
                time_seconds = seconds;
                if (!first_time_seconds) first_time_seconds = seconds;
                last_time_seconds = seconds;
            }

            const auto axis_order = frame.points.size() < 2 ||
                                    frame.points[1].coordinate > frame.points[0].coordinate
                                    ? "ascending" : "descending";
            frame_text.push_back({analysis_id + "#spectrum=" + std::to_string(frame.index),
                                  frame.time_value ? frame_time_unit : "unknown",
                                  frame.time_value ? resolved_time_kind : "none",
                                  axis_kind, axis_unit, axis_order,
                                  source.signal_kind.empty() ? "unknown" : source.signal_kind,
                                  source.signal_unit.empty() ? "unknown" : source.signal_unit});
            const auto &frame_strings = frame_text.back();

            for (std::size_t point_index = 0; point_index < frame.points.size(); ++point_index) {
                const auto &point = frame.points[point_index];
                points.append(analysis_id, source_path, frame_strings, frame.index,
                              point_index, point.source_index, frame.time_value, time_seconds,
                              point.coordinate, point.intensity);
                ++file_point_count;
                if (points.full()) {
                    emitted_points += points.size();
                    points.emit(access);
                }
            }
            if (frame.index % 32 == 0) {
                const auto within_file = static_cast<double>(frame.index) /
                                         static_cast<double>(frame.index + 1024);
                const auto fraction = (static_cast<double>(file_index) + within_file) /
                                      static_cast<double>(paths.size());
                access.report_progress(fraction, "Reading " + path.filename().string() +
                                                   " (spectrum " + std::to_string(frame.index + 1) + ")");
            }
        });

        if (file.spectrum_count == 0 || file.point_count != file_point_count)
            throw std::runtime_error("spectrum reader returned inconsistent totals: " + path.string());
        Json metadata = Json::object();
        for (const auto &field : file.metadata) {
            if (!metadata.contains(field.key)) metadata[field.key] = field.value;
            else if (metadata[field.key].is_array()) metadata[field.key].push_back(field.value);
            else metadata[field.key] = Json::array({metadata[field.key], field.value});
            metadata_rows.push_back({{"analysis_id", analysis_id},
                                     {"source_path", path.generic_string()},
                                     {"metadata_key", field.key},
                                     {"occurrence", field.occurrence},
                                     {"metadata_value", field.value}});
        }
        const auto acquisition_kind = file.has_frame_time
                                          ? (file.kinetic_cycle_seconds ? "kinetic_sequence" : "timed_sequence_unknown_units")
                                          : "single_spectrum";
        analyses.push_back({
            {"analysis_id", analysis_id}, {"analysis", analysis_name},
            {"file_path", path.generic_string()}, {"file_name", path.filename().string()},
            {"format", file.format}, {"technique", infer_technique(file)},
            {"acquisition_kind", acquisition_kind}, {"number_spectra", file.spectrum_count},
            {"number_points", file.point_count}, {"has_time", file.has_frame_time},
            {"time_kind", file.has_frame_time ? resolved_time_kind : "none"},
            {"axis_kind", file_axis_kind}, {"axis_unit", file_axis_unit},
            {"signal_kind", file.signal_kind.empty()
                                ? (reader::metadata_value(file.metadata, "Data Type").empty()
                                       ? "unknown" : reader::metadata_value(file.metadata, "Data Type"))
                                : file.signal_kind},
            {"signal_unit", file.signal_unit.empty()
                                ? (reader::metadata_value(file.metadata, "Data Type").empty()
                                       ? "unknown" : reader::metadata_value(file.metadata, "Data Type"))
                                : file.signal_unit},
            {"start_time_seconds", first_time_seconds ? Json(*first_time_seconds) : Json(nullptr)},
            {"end_time_seconds", last_time_seconds ? Json(*last_time_seconds) : Json(nullptr)},
            {"metadata_json", metadata}});

        total_spectra += file.spectrum_count;
        total_points += file.point_count;
        total_metadata += file.metadata.size();
        if (!points.empty()) {
            emitted_points += points.size();
            points.emit(access);
        }
        if (!metadata_rows.empty()) {
            emit_batches(access, "sourceMetadataTable", metadata_columns, metadata_types,
                         metadata_rows, 0, metadata_rows.size());
            metadata_rows.clear();
        }
        if (access.is_cancelled()) throw std::runtime_error("Raman file import cancelled");
        access.report_progress(static_cast<double>(file_index + 1) / static_cast<double>(paths.size()),
                               "Imported Raman spectra from " + path.filename().string());
    }

    if (!analyses.empty())
        access.emit_table_rows("analysesTable", analysis_columns, analysis_types, analyses);
    if (emitted_points != total_points)
        throw std::runtime_error("Raman point batch count does not match parsed point count");
    return {{"sources", paths.size()}, {"spectra", total_spectra},
            {"points", total_points}, {"metadata_fields", total_metadata}};
}

void filter_analysis_artifact(sdk::PluginProjectAccess &access, const Json &inputs,
                              const std::string &port, const std::string &output,
                              const std::vector<std::string> &columns,
                              const std::vector<std::string> &types,
                              const std::unordered_set<std::string> &removed_ids) {
    const auto &input = inputs.at(port);
    const auto physical_table = input.at("physical_table").get<std::string>();
    RowBuffer retained;
    constexpr std::size_t batch_size = 8192;
    access.read_batches(physical_table, columns, "analysis_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            const auto id = row.at("analysis_id").get<std::string>();
            if (removed_ids.contains(id)) continue;
            retained.push_back(row);
            if (retained.size() == batch_size) {
                access.emit_table_rows(output, columns, types, retained);
                retained.clear();
            }
        }
    });
    if (!retained.empty()) access.emit_table_rows(output, columns, types, retained);
}

Json remove_analyses(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto &names = parameters.at("analysis_names");
    if (!names.is_array()) throw std::invalid_argument("analysis_names must be an array of strings");
    std::vector<std::string> exact_names;
    std::vector<std::regex> patterns;
    for (const auto &value : names) {
        if (!value.is_string() || value.get<std::string>().empty())
            throw std::invalid_argument("analysis_names must contain non-empty strings or regex patterns");
        const auto pattern = value.get<std::string>();
        if (pattern.find_first_of(R"(\^$.*+?()[]{}|)") == std::string::npos) {
            exact_names.push_back(pattern);
        } else {
            try {
                patterns.emplace_back(pattern, std::regex::ECMAScript);
            } catch (const std::regex_error &error) {
                throw std::invalid_argument("invalid analysis_names regex: " + pattern +
                                            " (" + error.what() + ")");
            }
        }
    }

    const std::vector<std::string> names_columns = {"analysis_id", "analysis"};
    const auto analyses_input = inputs.at("analysesTable");
    const auto analyses_table = analyses_input.at("physical_table").get<std::string>();
    std::unordered_set<std::string> removed_ids;
    Json removed_names = Json::array();
    access.read_batches(analyses_table, names_columns, "analysis", [&](const Json &rows) {
        for (const auto &row : rows) {
            const auto name = row.at("analysis").get<std::string>();
            const bool match = std::find(exact_names.begin(), exact_names.end(), name) != exact_names.end() ||
                std::any_of(patterns.begin(), patterns.end(), [&](const auto &pattern) {
                    return std::regex_search(name, pattern);
                });
            if (!match) continue;
            removed_ids.insert(row.at("analysis_id").get<std::string>());
            removed_names.push_back(name);
        }
    });

    filter_analysis_artifact(access, inputs, "analysesTable", "analysesTable",
                             analysis_columns, analysis_types, removed_ids);
    filter_analysis_artifact(access, inputs, "spectraTable", "spectraTable",
                             point_columns, point_types, removed_ids);
    filter_analysis_artifact(access, inputs, "sourceMetadataTable", "sourceMetadataTable",
                             metadata_columns, metadata_types, removed_ids);
    return {{"removed", std::move(removed_names)}, {"count", removed_ids.size()}};
}

double json_number(const Json &value, const std::string &field) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        std::size_t parsed = 0;
        const auto number = std::stod(value.get<std::string>(), &parsed);
        if (parsed != value.get<std::string>().size())
            throw std::invalid_argument(field + " is not a numeric value");
        return number;
    }
    throw std::invalid_argument(field + " is not a numeric value");
}

std::vector<double> solve_banded(std::vector<std::vector<double>> matrix,
                                 std::vector<double> rhs, std::size_t bandwidth) {
    const auto size = rhs.size();
    const auto slot = [bandwidth](std::size_t row, std::size_t col) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(col) -
            static_cast<std::ptrdiff_t>(row) + static_cast<std::ptrdiff_t>(bandwidth));
    };
    for (std::size_t pivot = 0; pivot < size; ++pivot) {
        const auto diagonal = matrix[pivot][slot(pivot, pivot)];
        if (!(diagonal > 0.0) || !std::isfinite(diagonal))
            throw std::runtime_error("airpls smoothing system is singular");
        const auto last_row = std::min(size - 1, pivot + bandwidth);
        for (std::size_t row = pivot + 1; row <= last_row; ++row) {
            const auto factor = matrix[row][slot(row, pivot)] / diagonal;
            if (factor == 0.0) continue;
            const auto last_col = std::min(size - 1, pivot + bandwidth);
            for (std::size_t col = pivot; col <= last_col; ++col)
                matrix[row][slot(row, col)] -= factor * matrix[pivot][slot(pivot, col)];
            rhs[row] -= factor * rhs[pivot];
        }
    }
    std::vector<double> result(size, 0.0);
    for (std::size_t reverse = size; reverse-- > 0;) {
        double value = rhs[reverse];
        const auto last_col = std::min(size - 1, reverse + bandwidth);
        for (std::size_t col = reverse + 1; col <= last_col; ++col)
            value -= matrix[reverse][slot(reverse, col)] * result[col];
        result[reverse] = value / matrix[reverse][slot(reverse, reverse)];
    }
    return result;
}

std::vector<double> airpls_baseline(const std::vector<double> &signal, double lambda,
                                   std::size_t differences, std::size_t iterations) {
    const auto size = signal.size();
    if (size < 2 || size <= differences)
        throw std::invalid_argument("airpls requires more points than the difference order");
    if (!(lambda > 0.0) || !std::isfinite(lambda))
        throw std::invalid_argument("airpls lambda must be finite and greater than zero");
    if (differences < 1 || differences > 3)
        throw std::invalid_argument("airpls differences must be between 1 and 3");
    if (iterations < 1 || iterations > 1000)
        throw std::invalid_argument("airpls itermax must be between 1 and 1000");
    const auto bandwidth = differences;
    const auto slot = [bandwidth](std::size_t row, std::size_t col) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(col) -
            static_cast<std::ptrdiff_t>(row) + static_cast<std::ptrdiff_t>(bandwidth));
    };
    std::vector<std::vector<double>> penalty(size, std::vector<double>(2 * bandwidth + 1, 0.0));
    std::vector<double> coefficients(differences + 1, 1.0);
    for (std::size_t index = 0; index <= differences; ++index) {
        std::size_t choose = 1;
        for (std::size_t factor = 1; factor <= index; ++factor)
            choose = choose * (differences + 1 - factor) / factor;
        coefficients[index] = (index % 2 == differences % 2 ? 1.0 : -1.0) *
            static_cast<double>(choose);
    }
    for (std::size_t row = 0; row + differences < size; ++row)
        for (std::size_t left = 0; left <= differences; ++left)
            for (std::size_t right = 0; right <= differences; ++right) {
                const auto matrix_row = row + left;
                const auto matrix_col = row + right;
                penalty[matrix_row][slot(matrix_row, matrix_col)] +=
                    lambda * coefficients[left] * coefficients[right];
            }

    std::vector<double> weights(size, 1.0);
    std::vector<double> baseline(size, 0.0);
    for (std::size_t iteration = 1; iteration <= iterations; ++iteration) {
        auto system = penalty;
        std::vector<double> rhs(size, 0.0);
        for (std::size_t index = 0; index < size; ++index) {
            system[index][slot(index, index)] += weights[index];
            rhs[index] = weights[index] * signal[index];
        }
        baseline = solve_banded(std::move(system), std::move(rhs), bandwidth);
        std::vector<double> negative_residuals;
        double absolute_signal = 0.0;
        for (std::size_t index = 0; index < size; ++index) {
            absolute_signal += std::abs(signal[index]);
            if (signal[index] - baseline[index] < 0.0)
                negative_residuals.push_back(signal[index] - baseline[index]);
        }
        double sum_smaller = 0.0;
        for (const auto value : negative_residuals) sum_smaller += std::abs(value);
        if (sum_smaller < 0.001 * absolute_signal || iteration == iterations) break;
        if (negative_residuals.empty()) break;
        const auto maximum_negative = *std::max_element(
            negative_residuals.begin(), negative_residuals.end());
        for (std::size_t index = 0; index < size; ++index) {
            const auto residual = signal[index] - baseline[index];
            weights[index] = residual >= 0.0 ? 0.0 :
                std::exp(static_cast<double>(iteration) * std::abs(residual) / sum_smaller);
        }
        weights.front() = std::exp(static_cast<double>(iteration) * maximum_negative / sum_smaller);
        weights.back() = weights.front();
    }
    return baseline;
}

std::string required_table_name(const Json &inputs, const char *port) {
    const auto &input = inputs.at(port);
    if (!input.is_object() || !input.contains("physical_table") ||
        !input.at("physical_table").is_string())
        throw std::invalid_argument(std::string("missing physical table for input port ") + port);
    return input.at("physical_table").get<std::string>();
}

Json normalize_spectra(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto algorithm = option_string(parameters, "normalization_algorithm", "minmax");
    const auto method = operations::parse_normalization_method(algorithm);
    const auto lift_parameter = parameters.find("lift_to_zero");
    if (lift_parameter != parameters.end() && !lift_parameter->is_boolean())
        throw std::invalid_argument("lift_to_zero must be a boolean");
    const bool lift_to_zero = lift_parameter != parameters.end() && lift_parameter->get<bool>();
    if (lift_to_zero && method != NormalizationMethod::standard_normal_variate)
        throw std::invalid_argument("lift_to_zero is only valid with the snv algorithm");

    const auto points_table = required_table_name(inputs, "spectraTable");
    constexpr std::size_t output_batch_size = 4096;
    const auto point_count = access.count_rows(points_table);
    if (point_count == 0)
        throw std::invalid_argument("normalize_spectra requires non-empty spectral points");

    RowBuffer output_rows;
    output_rows.reserve(output_batch_size);
    std::vector<Json> spectrum_rows;
    std::unordered_set<std::string> completed_spectra;
    std::string current_spectrum_id;
    std::uint64_t transformed_spectra = 0;
    std::uint64_t transformed_points = 0;
    const auto flush_spectrum = [&]() {
        if (spectrum_rows.empty()) return;
        std::sort(spectrum_rows.begin(), spectrum_rows.end(), [](const Json &left, const Json &right) {
            return left.at("point_index").get<std::size_t>() < right.at("point_index").get<std::size_t>();
        });
        std::vector<std::optional<double>> intensities;
        intensities.reserve(spectrum_rows.size());
        for (const auto &row : spectrum_rows) {
            const auto &value = row.at("intensity");
            intensities.push_back(value.is_null()
                ? std::nullopt : std::optional<double>(json_number(value, "intensity")));
        }
        const auto normalized = operations::normalize_spectrum(intensities, method, lift_to_zero);
        for (std::size_t index = 0; index < spectrum_rows.size(); ++index) {
            spectrum_rows[index]["intensity"] = normalized[index] ? Json(*normalized[index]) : Json(nullptr);
            output_rows.push_back(std::move(spectrum_rows[index]));
            if (output_rows.size() == output_batch_size) {
                emit_batches(access, "spectraTable", point_columns, point_types,
                             output_rows, 0, output_rows.size());
                output_rows.clear();
            }
        }
        transformed_points += intensities.size();
        ++transformed_spectra;
        completed_spectra.insert(current_spectrum_id);
        spectrum_rows.clear();
    };

    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("normalize_spectra cancelled");
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            if (current_spectrum_id.empty()) current_spectrum_id = spectrum_id;
            if (spectrum_id != current_spectrum_id) {
                flush_spectrum();
                if (completed_spectra.contains(spectrum_id))
                    throw std::runtime_error("spectraTable rows are not grouped by spectrum_id");
                current_spectrum_id = spectrum_id;
            }
            spectrum_rows.push_back(row);
        }
    });
    flush_spectrum();
    if (!output_rows.empty())
        emit_batches(access, "spectraTable", point_columns, point_types,
                     output_rows, 0, output_rows.size());
    if (transformed_points != point_count)
        throw std::runtime_error("normalize_spectra emitted a different number of points than it read");
    access.report_progress(1.0, "Normalized " + std::to_string(transformed_spectra) + " Raman/IR spectra.");
    return {{"algorithm", algorithm}, {"spectra", transformed_spectra}, {"points", transformed_points}};
}

Json correct_spectra_baseline(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto algorithm = option_string(parameters, "baseline_algorithm", "airpls");
    if (algorithm != "airpls")
        throw std::invalid_argument("baseline_algorithm currently supports only airpls");
    const auto lambda_value = parameters.find("lambda");
    const auto lambda = lambda_value == parameters.end() ? 10.0 :
        json_number(*lambda_value, "lambda");
    const auto read_integer = [&](const char *name, std::int64_t default_value) {
        const auto value = parameters.find(name);
        if (value == parameters.end()) return default_value;
        if (!value->is_number_integer())
            throw std::invalid_argument(std::string(name) + " must be an integer");
        return value->get<std::int64_t>();
    };
    const auto differences = read_integer("differences", 1);
    const auto itermax = read_integer("itermax", 20);
    if (differences < 1 || differences > 3)
        throw std::invalid_argument("differences must be between 1 and 3");
    if (itermax < 1 || itermax > 1000)
        throw std::invalid_argument("itermax must be between 1 and 1000");

    const auto points_table = required_table_name(inputs, "spectraTable");
    const auto point_count = access.count_rows(points_table);
    if (point_count == 0)
        throw std::invalid_argument("correct_spectra_baseline requires non-empty spectral points");
    constexpr std::size_t batch_size = 4096;

    RowBuffer corrected_rows;
    RowBuffer baseline_rows;
    corrected_rows.reserve(batch_size);
    baseline_rows.reserve(batch_size);
    std::vector<Json> spectrum_rows;
    std::unordered_set<std::string> completed;
    std::string current_spectrum;
    std::uint64_t spectra = 0;
    std::uint64_t points = 0;
    const auto flush = [&]() {
        if (spectrum_rows.empty()) return;
        std::sort(spectrum_rows.begin(), spectrum_rows.end(), [](const Json &left, const Json &right) {
            return left.at("point_index").get<std::size_t>() < right.at("point_index").get<std::size_t>();
        });
        std::vector<double> signal;
        signal.reserve(spectrum_rows.size());
        for (const auto &row : spectrum_rows) {
            const auto &value = row.at("intensity");
            if (value.is_null()) throw std::invalid_argument("airpls does not accept missing intensity values");
            const auto intensity = json_number(value, "intensity");
            if (!std::isfinite(intensity)) throw std::invalid_argument("airpls requires finite intensity values");
            signal.push_back(intensity);
        }
        const auto baseline = airpls_baseline(signal, lambda,
            static_cast<std::size_t>(differences), static_cast<std::size_t>(itermax));
        for (std::size_t index = 0; index < spectrum_rows.size(); ++index) {
            auto baseline_row = Json{{"analysis_id", spectrum_rows[index].at("analysis_id")},
                {"spectrum_id", current_spectrum}, {"point_index", spectrum_rows[index].at("point_index")},
                {"baseline_intensity", baseline[index]}};
            baseline_rows.push_back(std::move(baseline_row));
            const auto clipped_baseline = std::min(baseline[index], signal[index]);
            spectrum_rows[index]["intensity"] = signal[index] - clipped_baseline;
            corrected_rows.push_back(std::move(spectrum_rows[index]));
            if (baseline_rows.size() == batch_size) {
                emit_batches(access, "baselineTable", baseline_columns, baseline_types,
                             baseline_rows, 0, baseline_rows.size());
                baseline_rows.clear();
            }
            if (corrected_rows.size() == batch_size) {
                emit_batches(access, "spectraTable", point_columns, point_types,
                             corrected_rows, 0, corrected_rows.size());
                corrected_rows.clear();
            }
        }
        points += spectrum_rows.size();
        ++spectra;
        completed.insert(current_spectrum);
        spectrum_rows.clear();
    };
    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("correct_spectra_baseline cancelled");
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            if (current_spectrum.empty()) current_spectrum = spectrum_id;
            if (spectrum_id != current_spectrum) {
                flush();
                if (completed.contains(spectrum_id))
                    throw std::runtime_error("spectraTable rows are not grouped by spectrum_id");
                current_spectrum = spectrum_id;
            }
            spectrum_rows.push_back(row);
        }
    });
    flush();
    if (!baseline_rows.empty())
        emit_batches(access, "baselineTable", baseline_columns, baseline_types,
                     baseline_rows, 0, baseline_rows.size());
    if (!corrected_rows.empty())
        emit_batches(access, "spectraTable", point_columns, point_types,
                     corrected_rows, 0, corrected_rows.size());
    if (points != point_count)
        throw std::runtime_error("correct_spectra_baseline emitted a different number of points than it read");
    access.report_progress(1.0, "Baseline-corrected " + std::to_string(spectra) + " Raman/IR spectra.");
    return {{"algorithm", algorithm}, {"spectra", spectra}, {"points", points},
        {"lambda", lambda}, {"differences", differences}, {"itermax", itermax}};
}

Json filter_spectral_range(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto read_bound = [&](const char *name) -> std::optional<double> {
        const auto value = parameters.find(name);
        if (value == parameters.end() || value->is_null()) return std::nullopt;
        if (value->is_string() && value->get<std::string>().empty()) return std::nullopt;
        const auto number = json_number(*value, name);
        if (!std::isfinite(number))
            throw std::invalid_argument(std::string(name) + " must be finite");
        return number;
    };
    const auto minimum = read_bound("minimum_coordinate");
    const auto maximum = read_bound("maximum_coordinate");
    if (!minimum && !maximum)
        throw std::invalid_argument("filter_spectral_range requires a minimum or maximum");
    if (minimum && maximum && *minimum > *maximum)
        throw std::invalid_argument("minimum_coordinate must not exceed maximum_coordinate");
    const auto mode = option_string(parameters, "mode", "include");
    if (mode != "include" && mode != "exclude")
        throw std::invalid_argument("mode must be include or exclude");

    const auto points_table = required_table_name(inputs, "spectraTable");
    const auto expected_points = access.count_rows(points_table);

    constexpr std::size_t batch_size = 4096;
    RowBuffer output_rows;
    output_rows.reserve(batch_size);
    std::unordered_set<std::string> spectra;
    std::uint64_t input_points = 0;
    std::uint64_t kept_points = 0;
    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("filter_spectral_range cancelled");
            ++input_points;
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            spectra.insert(spectrum_id);
            const auto &coordinate_value = row.at("spectral_coordinate");
            if (coordinate_value.is_null())
                throw std::invalid_argument("filter_spectral_range requires non-null spectral coordinates");
            const auto coordinate = json_number(coordinate_value, "spectral_coordinate");
            if (!std::isfinite(coordinate))
                throw std::invalid_argument("filter_spectral_range requires finite spectral coordinates");
            const bool within = (!minimum || coordinate >= *minimum) &&
                (!maximum || coordinate <= *maximum);
            if ((mode == "include" && !within) || (mode == "exclude" && within)) continue;
            output_rows.push_back(row);
            ++kept_points;
            if (output_rows.size() == batch_size) {
                emit_batches(access, "spectraTable", point_columns, point_types,
                             output_rows, 0, output_rows.size());
                output_rows.clear();
            }
        }
    });
    if (input_points != expected_points)
        throw std::runtime_error("filter_spectral_range read an inconsistent number of spectral points");
    if (!output_rows.empty())
        emit_batches(access, "spectraTable", point_columns, point_types,
                     output_rows, 0, output_rows.size());

    access.report_progress(1.0, "Retained " + std::to_string(kept_points) +
        " of " + std::to_string(input_points) + " spectral points.");
    return {{"mode", mode}, {"spectra", spectra.size()},
        {"input_points", input_points}, {"points", kept_points}};
}

Json summarize_spectra(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto points_table = required_table_name(inputs, "spectraTable");
    constexpr std::size_t batch_size = 4096;
    RowBuffer summaries;
    summaries.reserve(batch_size);
    Json current_summary;
    std::string current_spectrum_id;
    std::uint64_t point_count = 0;
    std::uint64_t spectra_count = 0;
    const std::vector<std::string> summary_source_columns = {
        "analysis_id", "spectrum_id", "spectrum_index", "source_frame_index", "source_path",
        "frame_time_value", "frame_time_unit", "time_seconds", "time_kind", "axis_kind",
        "axis_unit", "source_axis_kind", "source_axis_unit", "axis_order", "signal_kind",
        "signal_unit"};
    const auto emit_current = [&]() {
        if (current_spectrum_id.empty()) return;
        current_summary["point_count"] = point_count;
        summaries.push_back(std::move(current_summary));
        ++spectra_count;
        point_count = 0;
        if (summaries.size() == batch_size) {
            emit_batches(access, "spectraSummaryTable", summary_columns, summary_types,
                         summaries, 0, summaries.size());
            summaries.clear();
        }
    };
    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("summarize_spectra cancelled");
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            if (current_spectrum_id.empty() || spectrum_id != current_spectrum_id) {
                emit_current();
                current_spectrum_id = spectrum_id;
                current_summary = Json::object();
                for (const auto &column : summary_source_columns)
                    current_summary[column] = row.at(column);
            } else {
                for (const auto &column : summary_source_columns) {
                    if (row.at(column) != current_summary.at(column))
                        throw std::runtime_error("spectraTable contains inconsistent per-spectrum metadata");
                }
            }
            ++point_count;
        }
    });
    emit_current();
    if (!summaries.empty())
        emit_batches(access, "spectraSummaryTable", summary_columns, summary_types,
                     summaries, 0, summaries.size());
    access.report_progress(1.0, "Summarized " + std::to_string(spectra_count) + " spectra.");
    return {{"spectra", spectra_count}};
}

Json smooth_spectra(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto algorithm = option_string(parameters, "smoothing_algorithm", "moving_average");
    if (algorithm != "moving_average")
        throw std::invalid_argument("smoothing_algorithm currently supports only moving_average");
    const auto window_parameter = parameters.find("window_size");
    std::int64_t window_size = 5;
    if (window_parameter != parameters.end()) {
        if (!window_parameter->is_number_integer())
            throw std::invalid_argument("window_size must be an integer");
        window_size = window_parameter->get<std::int64_t>();
    }
    if (window_size < 1 || window_size > 1001 || window_size % 2 == 0)
        throw std::invalid_argument("window_size must be an odd integer between 1 and 1001");

    const auto points_table = required_table_name(inputs, "spectraTable");
    const auto expected_points = access.count_rows(points_table);
    if (expected_points == 0)
        throw std::invalid_argument("smooth_spectra requires non-empty spectral points");

    constexpr std::size_t batch_size = 4096;
    RowBuffer output_rows;
    output_rows.reserve(batch_size);
    std::vector<Json> spectrum_rows;
    std::unordered_set<std::string> completed_spectra;
    std::string current_spectrum;
    std::uint64_t spectra = 0;
    std::uint64_t points = 0;
    const auto flush_spectrum = [&]() {
        if (spectrum_rows.empty()) return;
        std::sort(spectrum_rows.begin(), spectrum_rows.end(), [](const Json &left, const Json &right) {
            return left.at("point_index").get<std::size_t>() < right.at("point_index").get<std::size_t>();
        });
        std::vector<double> intensities;
        intensities.reserve(spectrum_rows.size());
        for (const auto &row : spectrum_rows) {
            const auto &value = row.at("intensity");
            if (value.is_null())
                throw std::invalid_argument("moving-average smoothing does not accept missing intensity values");
            const auto intensity = json_number(value, "intensity");
            if (!std::isfinite(intensity))
                throw std::invalid_argument("moving-average smoothing requires finite intensity values");
            intensities.push_back(intensity);
        }
        const auto radius = static_cast<std::size_t>(window_size / 2);
        for (std::size_t index = 0; index < spectrum_rows.size(); ++index) {
            const auto begin = index > radius ? index - radius : 0;
            const auto end = std::min(spectrum_rows.size(), index + radius + 1);
            double sum = 0.0;
            for (auto neighbor = begin; neighbor < end; ++neighbor) sum += intensities[neighbor];
            spectrum_rows[index]["intensity"] = sum / static_cast<double>(end - begin);
            output_rows.push_back(std::move(spectrum_rows[index]));
            if (output_rows.size() == batch_size) {
                emit_batches(access, "spectraTable", point_columns, point_types,
                             output_rows, 0, output_rows.size());
                output_rows.clear();
            }
        }
        points += spectrum_rows.size();
        ++spectra;
        completed_spectra.insert(current_spectrum);
        spectrum_rows.clear();
    };
    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("smooth_spectra cancelled");
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            if (current_spectrum.empty()) current_spectrum = spectrum_id;
            if (spectrum_id != current_spectrum) {
                flush_spectrum();
                if (completed_spectra.contains(spectrum_id))
                    throw std::runtime_error("spectraTable rows are not grouped by spectrum_id");
                current_spectrum = spectrum_id;
            }
            spectrum_rows.push_back(row);
        }
    });
    flush_spectrum();
    if (!output_rows.empty())
        emit_batches(access, "spectraTable", point_columns, point_types,
                     output_rows, 0, output_rows.size());
    if (points != expected_points)
        throw std::runtime_error("smooth_spectra emitted a different number of points than it read");
    access.report_progress(1.0, "Smoothed " + std::to_string(spectra) + " Raman/IR spectra.");
    return {{"algorithm", algorithm}, {"window_size", window_size},
        {"spectra", spectra}, {"points", points}};
}

Json smooth_spectra_savgol(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto &inputs = parameters.at("_inputs");
    const auto read_integer = [&](const char *name, std::int64_t default_value) {
        const auto value = parameters.find(name);
        if (value == parameters.end()) return default_value;
        if (!value->is_number_integer())
            throw std::invalid_argument(std::string(name) + " must be an integer");
        return value->get<std::int64_t>();
    };
    const auto window_length = read_integer("window_length", 11);
    const auto polynomial_order = read_integer("polynomial_order", 4);
    if (window_length < 3 || window_length > 1001 || window_length % 2 == 0)
        throw std::invalid_argument("window_length must be an odd integer between 3 and 1001");
    if (polynomial_order < 0 || polynomial_order > 6 || polynomial_order >= window_length)
        throw std::invalid_argument("polynomial_order must be between 0 and 6 and less than window_length");

    const auto points_table = required_table_name(inputs, "spectraTable");
    const auto expected_points = access.count_rows(points_table);
    if (expected_points == 0)
        throw std::invalid_argument("smooth_spectra_savgol requires non-empty spectral points");

    constexpr std::size_t batch_size = 4096;
    RowBuffer output_rows;
    output_rows.reserve(batch_size);
    std::vector<Json> spectrum_rows;
    std::unordered_set<std::string> completed_spectra;
    std::string current_spectrum;
    std::uint64_t spectra = 0;
    std::uint64_t points = 0;
    const auto flush_spectrum = [&]() {
        if (spectrum_rows.empty()) return;
        std::sort(spectrum_rows.begin(), spectrum_rows.end(), [](const Json &left, const Json &right) {
            return left.at("point_index").get<std::size_t>() < right.at("point_index").get<std::size_t>();
        });
        std::vector<double> intensities;
        std::vector<double> coordinates;
        intensities.reserve(spectrum_rows.size());
        coordinates.reserve(spectrum_rows.size());
        for (const auto &row : spectrum_rows) {
            const auto &intensity = row.at("intensity");
            const auto &coordinate = row.at("spectral_coordinate");
            if (intensity.is_null() || coordinate.is_null())
                throw std::invalid_argument("Savitzky-Golay smoothing does not accept missing signal or coordinates");
            intensities.push_back(json_number(intensity, "intensity"));
            coordinates.push_back(json_number(coordinate, "spectral_coordinate"));
        }
        const auto smoothed = operations::savitzky_golay_smooth(
            intensities, coordinates, static_cast<std::size_t>(window_length),
            static_cast<std::size_t>(polynomial_order));
        for (std::size_t index = 0; index < spectrum_rows.size(); ++index) {
            spectrum_rows[index]["intensity"] = smoothed[index];
            output_rows.push_back(std::move(spectrum_rows[index]));
            if (output_rows.size() == batch_size) {
                emit_batches(access, "spectraTable", point_columns, point_types,
                             output_rows, 0, output_rows.size());
                output_rows.clear();
            }
        }
        points += spectrum_rows.size();
        ++spectra;
        completed_spectra.insert(current_spectrum);
        spectrum_rows.clear();
    };
    access.read_batches(points_table, point_columns, "spectrum_id", [&](const Json &rows) {
        for (const auto &row : rows) {
            if (access.is_cancelled()) throw std::runtime_error("smooth_spectra_savgol cancelled");
            const auto spectrum_id = row.at("spectrum_id").get<std::string>();
            if (current_spectrum.empty()) current_spectrum = spectrum_id;
            if (spectrum_id != current_spectrum) {
                flush_spectrum();
                if (completed_spectra.contains(spectrum_id))
                    throw std::runtime_error("spectraTable rows are not grouped by spectrum_id");
                current_spectrum = spectrum_id;
            }
            spectrum_rows.push_back(row);
        }
    });
    flush_spectrum();
    if (!output_rows.empty())
        emit_batches(access, "spectraTable", point_columns, point_types,
                     output_rows, 0, output_rows.size());
    if (points != expected_points)
        throw std::runtime_error("smooth_spectra_savgol emitted a different number of points than it read");
    access.report_progress(1.0, "Savitzky-Golay-smoothed " + std::to_string(spectra) + " Raman/IR spectra.");
    return {{"algorithm", "savitzky_golay"}, {"window_length", window_length},
        {"polynomial_order", polynomial_order}, {"derivative_order", 0},
        {"spectra", spectra}, {"points", points}};
}

}  // namespace detail

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"raman.read_raman_files", sdk::CapabilityKind::Operation, &detail::read_raman_files},
        {"raman.remove_analyses", sdk::CapabilityKind::Operation, &detail::remove_analyses},
        {"raman.normalize_spectra", sdk::CapabilityKind::Operation, &detail::normalize_spectra},
        {"raman.correct_spectra_baseline", sdk::CapabilityKind::Operation, &detail::correct_spectra_baseline},
        {"raman.filter_spectral_range", sdk::CapabilityKind::Operation, &detail::filter_spectral_range},
        {"raman.summarize_spectra", sdk::CapabilityKind::Operation, &detail::summarize_spectra},
        {"raman.smooth_spectra", sdk::CapabilityKind::Operation, &detail::smooth_spectra},
        {"raman.smooth_spectra_savgol", sdk::CapabilityKind::Operation, &detail::smooth_spectra_savgol},
    };
    return registry;
}

}  // namespace streamfind::raman::operations
