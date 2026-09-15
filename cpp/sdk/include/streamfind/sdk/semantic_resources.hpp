#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "streamfind/export.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API SemanticResourceSet {
    std::filesystem::path core_directory;
    std::vector<std::filesystem::path> plugin_directories;
    std::string domain_id;
};

STREAMFIND_SDK_API std::vector<std::filesystem::path>
semantic_files(const std::filesystem::path &directory);

STREAMFIND_SDK_API std::filesystem::path streamfind_home();
STREAMFIND_SDK_API std::filesystem::path jena_home();

}  // namespace streamfind::sdk
