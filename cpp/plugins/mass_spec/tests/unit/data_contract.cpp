#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifndef STREAMFIND_DATA_ROOT
#error STREAMFIND_DATA_ROOT is required for official mass-spec tests
#endif

int main() {
    const std::filesystem::path root = STREAMFIND_DATA_ROOT;
    const auto wastewater = root / "data" / "mass_spec" / "wastewater";
    const auto standards = wastewater / "internal_standards.csv";
    const auto suspects = wastewater / "suspects.csv";
    if (!std::filesystem::is_directory(wastewater) ||
        !std::filesystem::is_regular_file(standards) ||
        !std::filesystem::is_regular_file(suspects))
        throw std::runtime_error("official mass-spec tests require sibling streamfind.data wastewater fixtures");
    std::ifstream input(standards);
    std::string line;
    if (!std::getline(input, line) || line.empty())
        throw std::runtime_error("streamfind.data internal_standards.csv is empty");
    bool has_mzml = false;
    for (const auto &entry : std::filesystem::directory_iterator(wastewater))
        has_mzml = has_mzml || entry.path().extension() == ".mzML";
    if (!has_mzml)
        throw std::runtime_error("streamfind.data wastewater fixture has no mzML input");
    return 0;
}
