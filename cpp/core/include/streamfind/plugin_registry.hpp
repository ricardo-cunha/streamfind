#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"

namespace streamfind {

struct STREAMFIND_CORE_API InstalledPlugin {
    std::string plugin_id;
    std::string version;
    std::string platform;
    std::filesystem::path package_path;
    std::string package_digest;
    std::string origin;
    std::string trust;
    bool active{false};
    std::string compatibility{"compatible"};
    std::string diagnostics;
    std::filesystem::path source_path;
};

struct STREAMFIND_CORE_API PluginRegistry {
    int schema_version{1};
    std::uint64_t generation{0};
    std::vector<InstalledPlugin> installed;
};

struct STREAMFIND_CORE_API PluginRegistryResult {
    bool valid{false};
    PluginRegistry registry;
    std::string diagnostics;
};

STREAMFIND_CORE_API PluginRegistryResult
parse_plugin_registry(const std::string &json_text);

STREAMFIND_CORE_API PluginRegistryResult
load_plugin_registry(const std::filesystem::path &path);

STREAMFIND_CORE_API void
write_plugin_registry_atomic(const std::filesystem::path &path,
                             const PluginRegistry &registry);

STREAMFIND_CORE_API std::string native_plugin_platform();
STREAMFIND_CORE_API std::optional<InstalledPlugin>
find_active_plugin(const PluginRegistry &registry,
                   const std::string &plugin_id,
                   const std::string &platform = {});

}  // namespace streamfind
