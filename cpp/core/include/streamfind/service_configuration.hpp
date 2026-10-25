#pragma once

#include "streamfind/export.hpp"

#include <cstdint>
#include <filesystem>

namespace streamfind {

struct STREAMFIND_CORE_API ServiceConfiguration {
    std::uint16_t port{8787};
    bool tray_enabled{true};
    bool tray_start_with_app{true};
    bool tray_start_with_service{true};
    std::uint32_t tray_status_interval_ms{2000};
    bool tray_notifications{true};
};

STREAMFIND_CORE_API ServiceConfiguration load_service_configuration(
    const std::filesystem::path &configuration_path);

}  // namespace streamfind
