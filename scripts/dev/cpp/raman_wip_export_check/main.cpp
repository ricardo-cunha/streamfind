#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::dev_detail {

struct DecodedSpectrum {
    std::string caption;
    std::vector<streamfind::raman::reader::SpectrumPoint> points;
};

struct Match {
    std::size_t index = 0;
    double max_axis_error = std::numeric_limits<double>::infinity();
    double max_signal_error = std::numeric_limits<double>::infinity();
};

std::vector<DecodedSpectrum> read_spectra(const std::filesystem::path &path) {
    std::vector<DecodedSpectrum> spectra;
    const auto callback = [&](const auto &source, const auto &frame) {
            if (spectra.size() <= frame.index) spectra.resize(frame.index + 1);
            auto &spectrum = spectra[frame.index];
            spectrum.points = frame.points;
            if (source.format == "witec_wip") {
                const auto caption = std::find_if(source.metadata.rbegin(), source.metadata.rend(),
                    [&](const auto &field) {
                        return field.key == "Graph Caption" && field.occurrence == frame.index;
                    });
                if (caption != source.metadata.rend()) spectrum.caption = caption->value;
            }
        };
    const auto extension = path.extension().string();
    const auto file = extension == ".wip"
        ? streamfind::raman::reader::read_wip_file(path, callback)
        : streamfind::raman::reader::read_text_file(path, callback);
    if (file.spectrum_count != spectra.size())
        throw std::runtime_error("Reader spectrum count did not match emitted frames: " + path.string());
    return spectra;
}

Match compare(const DecodedSpectrum &exported, const std::vector<DecodedSpectrum> &graphs) {
    Match best;
    for (std::size_t i = 0; i < graphs.size(); ++i) {
        const auto &candidate = graphs[i];
        if (candidate.points.size() != exported.points.size()) continue;
        double axis_error = 0.0;
        double signal_error = 0.0;
        bool valid = true;
        for (std::size_t p = 0; p < candidate.points.size(); ++p) {
            if (!candidate.points[p].intensity || !exported.points[p].intensity) {
                valid = false;
                break;
            }
            axis_error = std::max(axis_error, std::abs(candidate.points[p].coordinate -
                                                        exported.points[p].coordinate));
            signal_error = std::max(signal_error, std::abs(*candidate.points[p].intensity -
                                                           *exported.points[p].intensity));
        }
        if (valid && (signal_error < best.max_signal_error ||
            (signal_error == best.max_signal_error && axis_error < best.max_axis_error)))
            best = {i, axis_error, signal_error};
    }
    return best;
}

}  // namespace streamfind::raman::dev_detail

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: raman_wip_export_check <project.wip> <export1.txt> [export2.txt ...]\n";
        return 2;
    }
    try {
        const auto graphs = streamfind::raman::dev_detail::read_spectra(argv[1]);
        std::size_t checked = 0;
        constexpr double axis_tolerance = 0.006;
        constexpr double signal_tolerance = 0.001;
        const bool is_reference_export = std::filesystem::path(argv[2]).filename() ==
                                          "Spektren-Referenz.txt";
        const std::vector<std::size_t> expected_reference_graphs{7, 10, 13, 16};
        for (int arg = 2; arg < argc; ++arg) {
            const auto exports = streamfind::raman::dev_detail::read_spectra(argv[arg]);
            for (std::size_t i = 0; i < exports.size(); ++i) {
                const auto match = streamfind::raman::dev_detail::compare(exports[i], graphs);
                if (!std::isfinite(match.max_signal_error) ||
                    match.max_axis_error > axis_tolerance ||
                    match.max_signal_error > signal_tolerance) {
                    throw std::runtime_error("No WIP graph matches " + std::string(argv[arg]) +
                        " spectrum " + std::to_string(i) + " within axis/signal tolerances " +
                        std::to_string(axis_tolerance) + "/" + std::to_string(signal_tolerance) +
                        "; closest graph " + std::to_string(match.index) + " ('" +
                        graphs[match.index].caption + "') had errors " +
                        std::to_string(match.max_axis_error) + "/" +
                        std::to_string(match.max_signal_error));
                }
                if (arg == 2 && is_reference_export &&
                    (i >= expected_reference_graphs.size() || match.index != expected_reference_graphs[i]))
                    throw std::runtime_error("Reference export spectrum " + std::to_string(i) +
                        " matched unexpected WIP graph " + std::to_string(match.index));
                std::cout << std::filesystem::path(argv[arg]).filename().string()
                          << " spectrum=" << i << " -> WIP graph=" << match.index
                          << " caption=\"" << graphs[match.index].caption << "\""
                          << " max_axis_error=" << match.max_axis_error
                          << " max_signal_error=" << match.max_signal_error << '\n';
                ++checked;
            }
        }
        std::cout << "Validated " << checked << " exported spectra against "
                  << graphs.size() << " WIP graph records.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "WIP/export validation failed: " << error.what() << '\n';
        return 1;
    }
}
