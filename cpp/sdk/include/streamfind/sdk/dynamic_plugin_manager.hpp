#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "streamfind/plugin_abi.h"
#include "streamfind/plugin_loader.hpp"
#include "streamfind/project.hpp"
#include "streamfind/sdk/plugin_manifest.hpp"

namespace streamfind::sdk {

struct STREAMFIND_SDK_API DynamicPluginLoadResult {
    bool loaded{false};
    std::string diagnostics;
    PluginManifest manifest;
    std::unique_ptr<DynamicLibrary> library;
    streamfind_plugin_descriptor descriptor{};
    streamfind_plugin_api plugin{};
};

STREAMFIND_SDK_API DynamicPluginLoadResult
load_dynamic_plugin_package(const std::filesystem::path &package_root);

STREAMFIND_SDK_API streamfind_plugin_status
register_dynamic_plugin(DynamicPluginLoadResult &plugin,
                        const streamfind_plugin_host_api &host);

STREAMFIND_SDK_API void
register_dynamic_plugin_capabilities(
    DynamicPluginLoadResult &plugin,
    const Json &catalogue_entries,
    MethodRegistry &methods,
    OperationRegistry &operations);

}  // namespace streamfind::sdk
