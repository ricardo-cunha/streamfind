#include "readers/reader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace streamfind::raman::reader::detail {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    if (first == value.end()) return {};
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::vector<double>> parse_numeric_row(const std::string &line,
                                                     std::size_t line_number) {
    std::vector<double> values;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto separator = line.find(';', begin);
        const auto end = separator == std::string::npos ? line.size() : separator;
        const auto token = trim(line.substr(begin, end - begin));
        if (token.empty()) {
            if (separator == std::string::npos || end + 1 == line.size()) break;
            if (values.empty()) return std::nullopt;
            throw std::runtime_error("ASC line " + std::to_string(line_number) +
                                     " contains an empty numeric field");
        }
        std::size_t consumed = 0;
        double value = 0.0;
        try {
            value = std::stod(token, &consumed);
        } catch (const std::exception &) {
            if (values.empty()) return std::nullopt;
            throw std::runtime_error("ASC line " + std::to_string(line_number) +
                                     " contains a non-numeric field: " + token);
        }
        if (consumed != token.size() || !std::isfinite(value)) {
            throw std::runtime_error("ASC line " + std::to_string(line_number) +
                                     " contains an invalid finite number: " + token);
        }
        values.push_back(value);
        if (separator == std::string::npos) break;
        begin = separator + 1;
        if (begin == line.size()) break;
    }
    if (values.empty()) return std::nullopt;
    return values;
}

std::optional<double> positive_metadata_number(
    const std::vector<MetadataField> &metadata, const std::string &key) {
    const auto wanted = lower(key);
    for (auto field = metadata.rbegin(); field != metadata.rend(); ++field) {
        if (lower(field->key) != wanted) continue;
        try {
            std::size_t consumed = 0;
            const auto value = std::stod(trim(field->value), &consumed);
            if (consumed == trim(field->value).size() &&
                std::isfinite(value) && value > 0.0)
                return value;
        } catch (const std::exception &) {
            return std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool begins_new_frame(const SpectrumFrame &frame, double coordinate,
                      std::optional<double> time_value) {
    if (frame.points.empty()) return false;
    if (time_value != frame.time_value) return true;
    if (frame.points.size() < 2) return false;
    const auto first_step = frame.points[1].coordinate - frame.points[0].coordinate;
    const auto last = frame.points.back().coordinate;
    const bool ascending = first_step > 0.0;
    const bool reset = ascending ? coordinate < last : coordinate > last;
    if (!reset) return false;
    const auto first = frame.points.front().coordinate;
    const auto tolerance = std::max(1.0, std::abs(first)) * 1e-10;
    if (std::abs(coordinate - first) > tolerance)
        throw std::runtime_error("ASC spectral coordinate changes direction within a frame");
    return true;
}

}  // namespace streamfind::raman::reader::detail

namespace streamfind::raman::reader {

std::string metadata_value(const std::vector<MetadataField> &metadata,
                           const std::string &key) {
    const auto wanted = detail::lower(key);
    for (auto field = metadata.rbegin(); field != metadata.rend(); ++field)
        if (detail::lower(field->key) == wanted) return detail::trim(field->value);
    return {};
}

SpectrumFile read_asc_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback) {
    if (!spectrum_callback) throw std::invalid_argument("ASC spectrum callback is required");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open Raman ASC file: " + path.string());

    SpectrumFile result;
    result.path = path;
    result.format = "andor_asc";
    result.signal_kind = "counts";
    std::string line;
    std::size_t line_number = 0;
    std::size_t first_numeric_line = 0;
    std::optional<std::vector<double>> first_row;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        const auto trimmed = detail::trim(line);
        if (trimmed.empty()) continue;
        first_row = detail::parse_numeric_row(trimmed, line_number);
        if (first_row) {
            first_numeric_line = line_number;
            break;
        }
        const auto separator = line.find(':');
        if (separator != std::string::npos) {
            auto key = detail::trim(line.substr(0, separator));
            auto value = detail::trim(line.substr(separator + 1));
            if (!key.empty()) {
                const auto occurrence = static_cast<std::size_t>(std::count_if(
                    result.metadata.begin(), result.metadata.end(),
                    [&](const auto &entry) { return entry.key == key; }));
                result.metadata.push_back({std::move(key), std::move(value), occurrence});
            }
        }
    }
    if (!first_row) throw std::runtime_error("Raman ASC file has no numeric spectrum: " + path.string());
    if (first_row->size() != 2 && first_row->size() != 3)
        throw std::runtime_error("ASC line " + std::to_string(first_numeric_line) +
                                 " must contain coordinate/intensity or time/coordinate/intensity");

    const auto columns = first_row->size();
    result.has_frame_time = columns == 3;
    if (result.has_frame_time) {
        result.kinetic_cycle_seconds =
            detail::positive_metadata_number(result.metadata, "Kinetic Cycle Time (secs)");
        if (!result.kinetic_cycle_seconds)
            result.kinetic_cycle_seconds =
                detail::positive_metadata_number(result.metadata, "Accumulate Cycle Time (secs)");
    }

    SpectrumFrame frame;
    frame.index = 0;
    frame.points.reserve(2048);
    auto consume_row = [&](const std::vector<double> &row, std::size_t row_line) {
        if (row.size() != columns)
            throw std::runtime_error("ASC line " + std::to_string(row_line) +
                                     " has " + std::to_string(row.size()) +
                                     " numeric fields; expected " + std::to_string(columns));
        const auto time = columns == 3 ? std::optional<double>(row[0]) : std::nullopt;
        const auto coordinate = row[columns - 2];
        if (detail::begins_new_frame(frame, coordinate, time)) {
            if (frame.points.size() < 2)
                throw std::runtime_error("ASC spectrum frame contains fewer than two points");
            result.point_count += frame.points.size();
            spectrum_callback(result, frame);
            ++result.spectrum_count;
            frame = SpectrumFrame{};
            frame.index = result.spectrum_count;
            frame.time_value = time;
            frame.points.reserve(2048);
        } else if (frame.points.empty()) {
            frame.time_value = time;
        }
        if (!frame.points.empty() && coordinate == frame.points.back().coordinate)
            throw std::runtime_error("ASC line " + std::to_string(row_line) +
                                     " repeats a spectral coordinate within one frame");
        frame.points.push_back({coordinate, row.back(), frame.points.size()});
    };

    consume_row(*first_row, first_numeric_line);
    while (std::getline(input, line)) {
        ++line_number;
        const auto trimmed = detail::trim(line);
        if (trimmed.empty()) continue;
        const auto row = detail::parse_numeric_row(trimmed, line_number);
        if (!row) throw std::runtime_error("ASC line " + std::to_string(line_number) +
                                           " contains unexpected text after spectrum data began");
        consume_row(*row, line_number);
    }
    if (input.bad()) throw std::runtime_error("error while reading Raman ASC file: " + path.string());
    if (frame.points.size() < 2)
        throw std::runtime_error("ASC spectrum frame contains fewer than two points");
    result.point_count += frame.points.size();
    spectrum_callback(result, frame);
    ++result.spectrum_count;
    return result;
}

}  // namespace streamfind::raman::reader
