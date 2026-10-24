#pragma once

#include "streamfind/export.hpp"

#include <cstdint>
#include <filesystem>

namespace streamfind {

struct STREAMFIND_CORE_API ServiceConfiguration {
    std::uint16_t port{8787};
};

STREAMFIND_CORE_API ServiceConfiguration load_service_configuration(
    const std::filesystem::path &configuration_path);

}  // namespace streamfind
