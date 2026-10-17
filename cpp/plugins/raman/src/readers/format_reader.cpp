#include "readers/reader.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace streamfind::raman::reader {

SpectrumFile read_spectrum_file(const std::filesystem::path &path,
                                const SpectrumCallback &spectrum_callback) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".asc") return read_asc_file(path, spectrum_callback);
    if (extension == ".sif") return read_sif_file(path, spectrum_callback);
    if (extension == ".txt") return read_text_file(path, spectrum_callback);
    if (extension == ".wip") return read_wip_file(path, spectrum_callback);
    if (extension == ".smf") return read_smf_file(path, spectrum_callback);
    throw std::invalid_argument("unsupported Raman/IR source extension: " + extension);
}

}  // namespace streamfind::raman::reader
