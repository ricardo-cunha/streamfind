#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "streamfind/export.hpp"

namespace streamfind {

struct STREAMFIND_CORE_API PluginConfiguration {
    std::vector<std::filesystem::path> plugin_roots;
    std::vector<std::string> enabled_plugins;
};

struct STREAMFIND_CORE_API PluginConfigurationResult {
    bool valid{false};
    PluginConfiguration configuration;
    std::string diagnostics;
};

STREAMFIND_CORE_API PluginConfigurationResult
load_plugin_configuration(const std::string &json_text,
                          const std::filesystem::path &configuration_path);

STREAMFIND_CORE_API PluginConfigurationResult
load_plugin_configuration_file(const std::filesystem::path &configuration_path);

}  // namespace streamfind
