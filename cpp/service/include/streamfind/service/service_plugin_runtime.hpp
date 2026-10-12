#pragma once

#include "streamfind/project.hpp"
#include "streamfind/sdk/dynamic_plugin_manager.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace streamfind::service {

class ServicePluginRuntime {
public:
    ServicePluginRuntime() = default;
    ServicePluginRuntime(const ServicePluginRuntime &) = delete;
    ServicePluginRuntime &operator=(const ServicePluginRuntime &) = delete;

    void load(const std::filesystem::path &configuration_path, MethodRegistry &methods, OperationRegistry &operations);
    Json dependencies() const;
    Json install_dependencies(const Json &request) const;

private:
    struct LoadedPlugin {
        sdk::DynamicPluginLoadResult plugin;
        Json catalogue;
        streamfind_plugin_host_api host{};
    };
    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
};

}  // namespace streamfind::service
