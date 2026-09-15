#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/export.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API PluginManifest {
    std::string plugin_id;
    std::string name;
    std::string version;
    int abi_major{0};
    int abi_minor{0};
    std::string sdk_minimum;
    std::string sdk_maximum;
    std::map<std::string, std::string> libraries;
    std::string domain;
    std::string semantic_catalogue;
    bool static_composition{false};
    std::vector<std::string> platforms;

    const std::string *library_for_platform(const std::string &platform) const;
};

struct STREAMFIND_SDK_API PluginManifestResult {
    bool valid{false};
    PluginManifest manifest;
    std::string diagnostics;
};

STREAMFIND_SDK_API PluginManifestResult
parse_plugin_manifest(const std::string &json_text);

STREAMFIND_SDK_API PluginManifestResult
load_plugin_manifest(const std::filesystem::path &path);

}  // namespace streamfind::sdk
