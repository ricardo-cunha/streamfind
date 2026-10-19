#include "readers/reader.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::reader::sif_detail {

float decode_ieee754_float(std::uint32_t bits) {
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

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

std::optional<std::uint64_t> unsigned_integer_line(const std::string &line) {
    const auto words = split_words(line);
    if (words.size() != 1) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(words.front(), &consumed);
        if (consumed == words.front().size()) return value;
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

float decode_float(const std::array<unsigned char, 4> &bytes) {
    // Andor SIF stores its float32 pixel block in little-endian byte order.
    // Do not infer the byte order from the values: a wrong-endian float can
    // still look finite for a subset of pixels and produce values around
    // 1e38 in the viewer.
    const auto bits = static_cast<std::uint32_t>(bytes[0]) |
                      (static_cast<std::uint32_t>(bytes[1]) << 8) |
                      (static_cast<std::uint32_t>(bytes[2]) << 16) |
                      (static_cast<std::uint32_t>(bytes[3]) << 24);
    return decode_ieee754_float(bits);
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
    std::size_t calibration_state = 0;
    bool saw_pixel_marker = false;
    std::uint64_t sif_version = 0;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 3) {
            const auto words = split_words(line);
            if (!words.empty()) {
                try {
                    sif_version = std::stoull(words.front());
                } catch (const std::exception &) {
                }
            }
        }
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
        if (calibration_state == 0) {
            const auto words = split_words(line);
            if (!words.empty()) {
                const auto version = number(words.front());
                if (version && *version == 65540.0)
                    calibration_state = 2;
            }
        } else if (calibration_state == 1) {
            calibration_state = 2;
        } else if (calibration_state == 2) {
            const auto values = numeric_words(line);
            const auto has_nonzero_tail = values.size() == 4 &&
                                          (values[1] != 0.0 || values[2] != 0.0 ||
                                           values[3] != 0.0);
            if (values.size() == 4 &&
                ((values.front() > 200.0 && values.front() < 1000.0) ||
                 (values.front() == 0.0 && has_nonzero_tail))) {
                calibration = values;
                calibration_state = 3;
            } else {
                // This was another 65540 metadata block, not the
                // spectrometer calibration record.
                calibration_state = 0;
            }
        } else if (calibration_state == 3) {
            // Calibration_data_old.
            calibration_state = 4;
        } else if (calibration_state == 4) {
            // Reserved calibration field.
            calibration_state = 5;
        } else if (calibration_state == 5 && !excitation_nm) {
            const auto values = numeric_words(line);
            if (values.size() == 1) {
                excitation_nm = values.front();
                calibration_state = 6;
            }
        }
        if (line.find("Raman shift") != std::string::npos) {
            result.axis_kind = "raman_shift";
            result.metadata.push_back({"Axis label", "Raman shift", 0});
        }
        if (line.find("Counts") != std::string::npos) {
            result.signal_kind = "counts";
            result.signal_unit = "counts";
            result.metadata.push_back({"Data Type", "Counts", 0});
        }
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

    // ORPL/sif_parser uses the excitation wavelength when Andor writes a
    // zero placeholder as the first calibration coefficient.
    if (!calibration.empty() && excitation_nm && calibration.front() == 0.0)
        calibration.front() = *excitation_nm;

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

    // The pixel block starts after the timestamp/control section.  It is not
    // safe to infer its location from EOF because SIF versions can append
    // metadata or padding after the pixel data.
    auto payload_offset = input.tellg();
    if (payload_offset < 0)
        throw std::runtime_error("cannot determine Andor SIF pixel payload offset");
    const auto control_offset = payload_offset;
    std::string control_line;
    bool complete_control_line = false;
    for (std::size_t probe = 0; probe < 64 && input.good(); ++probe) {
        char character = '\0';
        input.get(character);
        if (!input) break;
        if (character == '\n') {
            complete_control_line = true;
            break;
        }
        if (std::isdigit(static_cast<unsigned char>(character)) != 0 ||
            character == '\r' || character == ' ' || character == '\t')
            control_line.push_back(character);
        else
            break;
    }
    input.clear();
    input.seekg(control_offset);
    if (complete_control_line) {
        const auto control = unsigned_integer_line(control_line);
        if (control && *control == 0) {
            input.seekg(control_offset);
            input.seekg(static_cast<std::streamoff>(control_line.size() + 1),
                        std::ios::cur);
            payload_offset = input.tellg();
        } else if (control && *control == 1 && sif_version == 65567) {
            input.seekg(static_cast<std::streamoff>(control_line.size() + 1),
                        std::ios::cur);
            for (std::size_t frame = 0; frame < frame_count; ++frame) {
                if (!std::getline(input, control_line))
                    throw std::runtime_error("Andor SIF frame control table is truncated");
            }
            payload_offset = input.tellg();
        } else {
            payload_offset = control_offset;
        }
    } else {
        payload_offset = control_offset;
    }

    const auto file_size = std::filesystem::file_size(path);
    const auto payload_size = static_cast<std::uintmax_t>(pixel_count) * sizeof(float);
    if (payload_offset < 0 || static_cast<std::uintmax_t>(payload_offset) > file_size ||
        payload_size > file_size - static_cast<std::uintmax_t>(payload_offset))
        throw std::runtime_error("Andor SIF float payload exceeds the source file size");
    std::ifstream pixels(path, std::ios::binary);
    pixels.seekg(payload_offset);
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
            intensity = decode_float(bytes);
            if (!std::isfinite(intensity) || std::abs(intensity) > 1.0e7F) {
                intensity = std::numeric_limits<float>::quiet_NaN();
                ++missing_intensity_count;
            }
        }
        SpectrumFrame spectrum;
        spectrum.index = frame_index;
        spectrum.time_value = result.has_frame_time
                                  ? std::optional<double>(timestamps[frame_index])
                                  : std::nullopt;
        spectrum.points.reserve(point_count);
        for (std::size_t point = 0; point < point_count; ++point) {
            // sif_parser exposes the detector axis in the same order as the
            // Raman calibration polynomial.  Reversing it here mirrors the
            // spectrum and moves low-shift peaks to the wrong end of the
            // plotted axis.
            const auto detector_index = point;
            double coordinate = static_cast<double>(point + 1);
            if (!calibration.empty() && excitation_nm) {
                const auto pixel = static_cast<double>(point + 1);
                const auto wavelength = calibration[0] + calibration[1] * pixel +
                                        calibration[2] * pixel * pixel +
                                        calibration[3] * pixel * pixel * pixel;
                if (!(wavelength > 0.0))
                    throw std::runtime_error("Andor SIF calibration produced a non-positive wavelength");
                coordinate = (1.0 / *excitation_nm - 1.0 / wavelength) * 1e7;
                coordinate = std::round(coordinate);
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
                                       " non-finite or out-of-range source values stored as null intensities", 0});
    result.metadata.push_back({"Pixel byte order", "little-endian", 0});
    result.metadata.push_back({"Pixel payload offset", std::to_string(payload_offset), 0});
    return result;
}

}  // namespace streamfind::raman::reader
