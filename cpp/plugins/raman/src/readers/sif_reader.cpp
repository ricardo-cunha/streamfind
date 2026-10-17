#include "readers/reader.hpp"

#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::reader::sif_detail {

std::vector<std::string> split_words(const std::string &line) {
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    return words;
}

std::optional<double> number(const std::string &text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stod(text, &consumed);
        while (consumed < text.size() &&
               std::isspace(static_cast<unsigned char>(text[consumed])) != 0)
            ++consumed;
        if (consumed == text.size() && std::isfinite(value)) return value;
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

std::vector<double> numeric_words(const std::string &line) {
    std::vector<double> values;
    for (const auto &word : split_words(line)) {
        const auto value = number(word);
        if (!value) return {};
        values.push_back(*value);
    }
    return values;
}

std::size_t positive_integer(const std::string &word, const std::string &label) {
    const auto value = number(word);
    if (!value || *value < 1.0 || std::floor(*value) != *value ||
        *value > static_cast<double>(std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error("invalid Andor SIF " + label);
    return static_cast<std::size_t>(*value);
}

}  // namespace streamfind::raman::reader::sif_detail

namespace streamfind::raman::reader {

SpectrumFile read_sif_file(const std::filesystem::path &path,
                           const SpectrumCallback &spectrum_callback) {
    using namespace sif_detail;
    if (!spectrum_callback) throw std::invalid_argument("SIF spectrum callback is required");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open Andor SIF file: " + path.string());

    SpectrumFile result;
    result.path = path;
    result.format = "andor_sif";
    result.axis_kind = "raman_shift";
    result.axis_unit = "cm^-1";
    result.signal_kind = "counts";
    result.signal_unit = "counts";

    std::string line;
    if (!std::getline(input, line) || line != "Andor Technology Multi-Channel File")
        throw std::runtime_error("unrecognized Andor SIF signature: " + path.string());
    result.metadata.push_back({"SIF signature", line, 0});

    std::size_t point_count = 0;
    std::size_t frame_count = 0;
    std::size_t pixel_count = 0;
    std::vector<double> calibration;
    std::optional<double> excitation_nm;
    bool after_calibration = false;
    bool saw_pixel_marker = false;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.rfind("Pixel number", 0) == 0) {
            const auto fields = split_words(line);
            if (fields.size() < 9)
                throw std::runtime_error("Andor SIF pixel descriptor is incomplete");
            point_count = positive_integer(fields[4], "spectral width");
            frame_count = positive_integer(fields[6], "frame count");
            pixel_count = positive_integer(fields[8], "pixel count");
            if (point_count > std::numeric_limits<std::size_t>::max() / frame_count ||
                point_count * frame_count != pixel_count)
                throw std::runtime_error("Andor SIF pixel dimensions do not match its frame count");
            saw_pixel_marker = true;
            break;
        }
        if (line.rfind("65540", 0) == 0) after_calibration = true;
        if (line.find("Raman shift") != std::string::npos) {
            result.axis_kind = "raman_shift";
            result.metadata.push_back({"Axis label", "Raman shift", 0});
        }
        if (line.find("Counts") != std::string::npos) {
            result.signal_kind = "counts";
            result.signal_unit = "counts";
            result.metadata.push_back({"Data Type", "Counts", 0});
        }
        const auto values = numeric_words(line);
        if (after_calibration && calibration.empty() && values.size() == 4 &&
            values.front() > 200.0 && values.front() < 1000.0)
            calibration = values;
        else if (after_calibration && !calibration.empty() && !excitation_nm &&
                 values.size() == 1 && values.front() > 200.0 && values.front() < 1000.0)
            excitation_nm = values.front();
    }
    if (!saw_pixel_marker)
        throw std::runtime_error("Andor SIF has no pixel descriptor: " + path.string());
    if (!calibration.empty()) {
        std::ostringstream encoded;
        encoded << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (std::size_t i = 0; i < calibration.size(); ++i) {
            if (i != 0) encoded << ',';
            encoded << calibration[i];
        }
        result.metadata.push_back({"Calibration_data", encoded.str(), 0});
    }
    if (excitation_nm) {
        result.metadata.push_back({"RamanExWavelength", std::to_string(*excitation_nm), 0});
    } else {
        result.axis_kind = "pixel";
        result.axis_unit = "pixel";
    }

    // The line after the pixel descriptor describes the detector ROI. The next
    // frame_count scalar lines are acquisition timestamps in microseconds.
    if (!std::getline(input, line))
        throw std::runtime_error("Andor SIF is missing its ROI descriptor");
    std::vector<double> timestamps;
    timestamps.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (!std::getline(input, line))
            throw std::runtime_error("Andor SIF timestamp table is truncated");
        const auto timestamp = number(line);
        if (!timestamp)
            throw std::runtime_error("Andor SIF timestamp row is invalid at frame " +
                                     std::to_string(frame));
        timestamps.push_back(*timestamp * 1e-6);
    }
    result.has_frame_time = frame_count > 1;
    if (frame_count > 1) {
        const auto cycle = timestamps[1] - timestamps[0];
        if (cycle > 0.0) result.kinetic_cycle_seconds = cycle;
        result.metadata.push_back({"Timestamp unit", "microseconds in source; normalized to seconds", 0});
        if (result.kinetic_cycle_seconds)
            result.metadata.push_back({"Kinetic Cycle Time (secs)",
                                       std::to_string(*result.kinetic_cycle_seconds), 0});
    }

    const auto file_size = std::filesystem::file_size(path);
    constexpr std::uintmax_t footer_size = 8;
    const auto payload_size = static_cast<std::uintmax_t>(pixel_count) * sizeof(float);
    if (file_size < footer_size || payload_size > file_size - footer_size)
        throw std::runtime_error("Andor SIF float payload exceeds the source file size");
    const auto payload_offset = file_size - footer_size - payload_size;
    std::ifstream pixels(path, std::ios::binary);
    pixels.seekg(static_cast<std::streamoff>(payload_offset));
    if (!pixels) throw std::runtime_error("cannot seek to Andor SIF pixel payload");

    std::vector<float> intensities(point_count);
    std::size_t missing_intensity_count = 0;
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        for (std::size_t detector_index = 0; detector_index < intensities.size(); ++detector_index) {
            auto &intensity = intensities[detector_index];
            std::array<unsigned char, 4> bytes{};
            pixels.read(reinterpret_cast<char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            if (!pixels) throw std::runtime_error("Andor SIF pixel payload is truncated");
            const auto bits = static_cast<std::uint32_t>(bytes[0]) |
                              (static_cast<std::uint32_t>(bytes[1]) << 8) |
                              (static_cast<std::uint32_t>(bytes[2]) << 16) |
                              (static_cast<std::uint32_t>(bytes[3]) << 24);
            intensity = std::bit_cast<float>(bits);
            if (!std::isfinite(intensity)) ++missing_intensity_count;
        }
        SpectrumFrame spectrum;
        spectrum.index = frame_index;
        spectrum.time_value = result.has_frame_time
                                  ? std::optional<double>(timestamps[frame_index])
                                  : std::nullopt;
        spectrum.points.reserve(point_count);
        for (std::size_t point = 0; point < point_count; ++point) {
            const auto detector_index = point_count - point - 1;
            double coordinate = static_cast<double>(point + 1);
            if (!calibration.empty() && excitation_nm) {
                const auto pixel = static_cast<double>(point + 1);
                const auto wavelength = calibration[0] + calibration[1] * pixel +
                                        calibration[2] * pixel * pixel +
                                        calibration[3] * pixel * pixel * pixel;
                if (!(wavelength > 0.0))
                    throw std::runtime_error("Andor SIF calibration produced a non-positive wavelength");
                coordinate = (1.0 / *excitation_nm - 1.0 / wavelength) * 1e7;
            }
            const auto intensity = std::isfinite(intensities[detector_index])
                                       ? std::optional<double>(intensities[detector_index])
                                       : std::nullopt;
            spectrum.points.push_back({coordinate, intensity, detector_index});
        }
        result.point_count += spectrum.points.size();
        spectrum_callback(result, spectrum);
        ++result.spectrum_count;
    }
    if (missing_intensity_count != 0)
        result.metadata.push_back({"Missing detector values",
                                   std::to_string(missing_intensity_count) +
                                       " non-finite source values stored as null intensities", 0});
    return result;
}

}  // namespace streamfind::raman::reader
