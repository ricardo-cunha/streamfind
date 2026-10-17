#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::reader::text_detail {

std::string trim_field(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split_semicolon(const std::string &line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto separator = line.find(';', begin);
        const auto end = separator == std::string::npos ? line.size() : separator;
        fields.push_back(trim_field(line.substr(begin, end - begin)));
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    while (!fields.empty() && fields.back().empty()) fields.pop_back();
    return fields;
}

double parse_number(const std::string &token, std::size_t line_number) {
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(token, &consumed);
    } catch (const std::exception &) {
        throw std::runtime_error("text spectrum line " + std::to_string(line_number) +
                                 " has an invalid number: " + token);
    }
    if (consumed != token.size() || !std::isfinite(value))
        throw std::runtime_error("text spectrum line " + std::to_string(line_number) +
                                 " has a non-finite number: " + token);
    return value;
}

}  // namespace streamfind::raman::reader::text_detail

namespace streamfind::raman::reader {

SpectrumFile read_text_file(const std::filesystem::path &path,
                            const SpectrumCallback &spectrum_callback) {
    if (!spectrum_callback) throw std::invalid_argument("text spectrum callback is required");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open Raman text export: " + path.string());

    std::string channel_line;
    std::string units_line;
    if (!std::getline(input, channel_line) || !std::getline(input, units_line))
        throw std::runtime_error("Raman text export is missing its two header rows: " + path.string());
    const auto channels = text_detail::split_semicolon(channel_line);
    const auto units = text_detail::split_semicolon(units_line);
    if (channels.size() < 2 || units.size() != channels.size())
        throw std::runtime_error("Raman text export has inconsistent channel and unit headers: " +
                                 path.string());
    if (channels.front().empty() || units.front().empty())
        throw std::runtime_error("Raman text export is missing its spectral-axis header");

    SpectrumFile result;
    result.path = path;
    result.format = "witec_text";
    result.axis_unit = "cm^-1";
    result.signal_kind = units.size() > 1 ? units[1] : "unknown";
    result.signal_unit = result.signal_kind;
    result.metadata.push_back({"X-Axis", channels.front(), 0});
    result.metadata.push_back({"X-Axis Unit", units.front(), 0});
    for (std::size_t channel = 1; channel < channels.size(); ++channel) {
        result.metadata.push_back({"Channel " + std::to_string(channel), channels[channel], 0});
        result.metadata.push_back({"Channel Unit " + std::to_string(channel), units[channel], 0});
    }
    result.metadata.push_back({"Data Type", result.signal_kind, 0});

    for (std::size_t channel = 1; channel < channels.size(); ++channel) {
        std::ifstream rows(path);
        if (!rows) throw std::runtime_error("cannot reopen Raman text export: " + path.string());
        std::string line;
        std::getline(rows, line);
        std::getline(rows, line);
        SpectrumFrame frame;
        frame.index = channel - 1;
        std::size_t line_number = 2;
        while (std::getline(rows, line)) {
            ++line_number;
            if (text_detail::trim_field(line).empty()) continue;
            const auto fields = text_detail::split_semicolon(line);
            if (fields.size() != channels.size())
                throw std::runtime_error("Raman text export line " + std::to_string(line_number) +
                                         " does not match its declared column count");
            const auto coordinate = text_detail::parse_number(fields.front(), line_number);
            const auto intensity = text_detail::parse_number(fields[channel], line_number);
            frame.points.push_back({coordinate, intensity, frame.points.size()});
        }
        if (frame.points.size() < 2)
            throw std::runtime_error("Raman text export channel contains fewer than two points");
        result.point_count += frame.points.size();
        spectrum_callback(result, frame);
        ++result.spectrum_count;
    }
    result.axis_kind.clear();
    return result;
}

}  // namespace streamfind::raman::reader
