#include <filesystem>

#include "streamfind/plugin_configuration.hpp"

int main() {
    const auto defaults = streamfind::load_plugin_configuration(
        "{\"enabled_plugins\":[\"sensors\"]}", "C:/StreamFind/streamfind.json");
    if (!defaults.valid || defaults.configuration.plugin_roots.size() != 1 ||
        defaults.configuration.plugin_roots.front() != "C:/StreamFind/plugins" ||
        defaults.configuration.enabled_plugins.front() != "sensors") {
        return 1;
    }

    const auto explicit_roots = streamfind::load_plugin_configuration(
        "{\"plugin_roots\":[\"addons\"],\"enabled_plugins\":[\"sensors\",\"raman\"]}",
        "C:/StreamFind/streamfind.json");
    if (!explicit_roots.valid || explicit_roots.configuration.plugin_roots.front() != "C:/StreamFind/addons" ||
        explicit_roots.configuration.enabled_plugins.size() != 2) {
        return 2;
    }

    const auto duplicate = streamfind::load_plugin_configuration(
        "{\"enabled_plugins\":[\"sensors\",\"sensors\"]}", "streamfind.json");
    if (duplicate.valid || duplicate.diagnostics.find("duplicate") == std::string::npos) {
        return 3;
    }

    return 0;
}
