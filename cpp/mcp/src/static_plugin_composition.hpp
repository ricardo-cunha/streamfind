#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "streamfind/project.hpp"
#include "streamfind/sdk/dynamic_plugin_manager.hpp"

namespace streamfind::static_plugins {

class DynamicPluginRuntime {
public:
    DynamicPluginRuntime() = default;
    DynamicPluginRuntime(const DynamicPluginRuntime &) = delete;
    DynamicPluginRuntime &operator=(const DynamicPluginRuntime &) = delete;

    void load_and_register(const std::filesystem::path &configuration_path,
                           MethodRegistry &methods,
                           OperationRegistry &operations);

private:
    struct LoadedPlugin {
        std::filesystem::path package_root;
        sdk::DynamicPluginLoadResult plugin;
        streamfind_plugin_host_api host{};
        Json catalogue;
    };
    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
};

}  // namespace streamfind::static_plugins
