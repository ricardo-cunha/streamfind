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
    Json activate(const std::string &plugin_id, MethodRegistry &methods, OperationRegistry &operations);
    Json status() const;
    Json dependencies() const;
    Json install_dependencies(const Json &request) const;

private:
    struct LoadedPlugin {
        sdk::DynamicPluginLoadResult plugin;
        Json catalogue;
        streamfind_plugin_host_api host{};
        std::filesystem::path package_path;
    };
    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
    std::filesystem::path configuration_path_;
    Json merged_catalogue_;
    std::uint64_t generation_{0};
};

}  // namespace streamfind::service
