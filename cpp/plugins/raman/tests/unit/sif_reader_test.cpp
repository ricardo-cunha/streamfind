#include "readers/reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::tests {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_float(std::ofstream &output, float value, bool big_endian) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int shift : big_endian ? std::array<int, 4>{24, 16, 8, 0}
                                : std::array<int, 4>{0, 8, 16, 24})
        output.put(static_cast<char>((bits >> shift) & 0xff));
}

void test_sif_payload_byte_order_and_calibration(bool big_endian) {
    const auto root = std::filesystem::path(RAMAN_TEST_SOURCE_DIR) /
                      "../../../../tmp/projects";
    std::filesystem::create_directories(root);
    const auto path = root / (big_endian ? "raman_sif_reader_big_endian_test.sif"
                                         : "raman_sif_reader_little_endian_test.sif");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "could not create temporary SIF test file");
        output << "Andor Technology Multi-Channel File\n"
               << "65540 binary calibration marker\n"
               << "532 0 0 0\n"
               << "532.14\n"
               << "Raman shift6\n"
               << "Counts12\n"
               << "Pixel number65541 1 127 4 1 2 1 8 4\n"
               << "65538 1 67 4 66 2 1 0\n"
               << "0\n239670\n";
        for (float value : std::array<float, 8>{100.0F, 200.0F, 300.0F,
                                                400.0F, 500.0F, 600.0F,
                                                700.0F, 1.0e30F})
            write_float(output, value, big_endian);
        for (int i = 0; i < 8; ++i) output.put('\0');
    }

    std::vector<reader::SpectrumFrame> frames;
    const auto parsed = reader::read_sif_file(path,
        [&](const reader::SpectrumFile &, const reader::SpectrumFrame &frame) {
            frames.push_back(frame);
        });
    require(frames.size() == 2, "SIF frame count was not preserved");
    require(parsed.axis_kind == "raman_shift" && parsed.axis_unit == "cm^-1",
            "SIF Raman-shift axis metadata was not preserved");
    require(frames[0].points.size() == 4, "SIF spectral width was not preserved");
    require(frames[0].points[0].intensity.has_value() &&
                std::abs(*frames[0].points[0].intensity - 400.0) < 1e-6,
            "SIF counts were not decoded or detector order was not preserved");
    require(frames[1].points.front().intensity == std::nullopt,
            "out-of-range SIF value should be represented as missing");
    require(frames[0].time_value.has_value() && *frames[0].time_value == 0.0,
            "SIF frame acquisition time was not converted to seconds");
    std::filesystem::remove(path);
}

}  // namespace streamfind::raman::tests

int main(int argc, char **argv) {
    try {
        streamfind::raman::tests::test_sif_payload_byte_order_and_calibration(true);
        streamfind::raman::tests::test_sif_payload_byte_order_and_calibration(false);
        if (argc > 1) {
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            std::uint64_t missing = 0;
            const auto parsed = streamfind::raman::reader::read_sif_file(argv[1],
                [&](const streamfind::raman::reader::SpectrumFile &,
                    const streamfind::raman::reader::SpectrumFrame &frame) {
                    for (const auto &point : frame.points) {
                        if (!point.intensity) {
                            ++missing;
                            continue;
                        }
                        minimum = std::min(minimum, *point.intensity);
                        maximum = std::max(maximum, *point.intensity);
                    }
                });
            std::cout << "SIF sample: spectra=" << parsed.spectrum_count
                      << " points=" << parsed.point_count
                      << " axis=" << parsed.axis_kind << ' ' << parsed.axis_unit
                      << " intensity-range=" << minimum << ".." << maximum
                      << " missing=" << missing << '\n';
        }
        std::cout << "SIF reader tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "SIF reader test failed: " << error.what() << '\n';
        return 1;
    }
}
