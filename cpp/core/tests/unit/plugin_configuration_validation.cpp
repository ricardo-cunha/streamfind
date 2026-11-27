#include <filesystem>

#include "streamfind/plugin_configuration.hpp"

int main() {
    const auto defaults = streamfind::load_plugin_configuration(
        "{\"enabled_plugins\":[\"sensors\"]}", "C:/StreamFind/streamfind.json");
    if (!defaults.valid || defaults.configuration.plugin_deployment_roots.size() != 1 ||
        defaults.configuration.plugin_deployment_roots.front() != "C:/StreamFind/plugins" ||
        defaults.configuration.enabled_plugins.front() != "sensors") {
        return 1;
    }

    const auto explicit_roots = streamfind::load_plugin_configuration(
        "{\"plugin_roots\":{\"development\":[\"source\"],\"deployment\":[\"addons\"]},\"viewer_roots\":{\"development\":[\"viewer-source\"],\"deployment\":[\"viewer-deploy\"]},\"enabled_plugins\":[\"sensors\",\"raman\"],\"enabled_viewers\":[\"custom.viewer.example\"]}",
        "C:/StreamFind/streamfind.json");
    if (!explicit_roots.valid || explicit_roots.configuration.plugin_development_roots.front() != "C:/StreamFind/source" ||
        explicit_roots.configuration.plugin_deployment_roots.front() != "C:/StreamFind/addons" ||
        explicit_roots.configuration.viewer_development_roots.front() != "C:/StreamFind/viewer-source" ||
        explicit_roots.configuration.viewer_deployment_roots.front() != "C:/StreamFind/viewer-deploy" ||
        explicit_roots.configuration.enabled_plugins.size() != 2 ||
        explicit_roots.configuration.enabled_viewers.size() != 1 ||
        explicit_roots.configuration.enabled_viewers.front() != "custom.viewer.example") {
        return 2;
    }

    const auto registry = streamfind::load_plugin_configuration(
        "{\"registry_path\":\"extensions/registry.json\",\"enabled_plugins\":[\"native.plugin.sensors\"]}",
        "C:/StreamFind/streamfind.json");
    if (!registry.valid || registry.configuration.registry_path != "C:/StreamFind/extensions/registry.json" ||
        registry.configuration.enabled_plugins.front() != "native.plugin.sensors") {
        return 3;
    }

    const auto duplicate = streamfind::load_plugin_configuration(
        "{\"enabled_plugins\":[\"sensors\",\"sensors\"]}", "streamfind.json");
    if (duplicate.valid || duplicate.diagnostics.find("duplicate") == std::string::npos) {
        return 4;
    }

    return 0;
}
