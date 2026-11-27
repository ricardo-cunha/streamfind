#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "streamfind/export.hpp"

namespace streamfind {

struct STREAMFIND_CORE_API PluginConfiguration {
    std::vector<std::filesystem::path> plugin_development_roots;
    std::vector<std::filesystem::path> plugin_deployment_roots;
    std::vector<std::filesystem::path> viewer_development_roots;
    std::vector<std::filesystem::path> viewer_deployment_roots;
    std::vector<std::string> enabled_plugins;
    std::vector<std::string> enabled_viewers;
    std::filesystem::path registry_path;
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
