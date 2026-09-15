#include "streamfind/plugin_configuration.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace streamfind::detail {

std::filesystem::path configuration_directory(const std::filesystem::path &path) {
    const auto parent = path.parent_path();
    return parent.empty() ? std::filesystem::path{} : parent;
}

std::string required_string(const nlohmann::json &value, const char *field) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        throw std::invalid_argument(std::string("configuration field must contain non-empty strings: ") + field);
    }
    return value.get<std::string>();
}

}  // namespace streamfind::detail

namespace streamfind {

PluginConfigurationResult load_plugin_configuration(
    const std::string &json_text,
    const std::filesystem::path &configuration_path) {
    PluginConfigurationResult result;
    try {
        const auto document = nlohmann::json::parse(json_text);
        if (!document.is_object()) {
            throw std::invalid_argument("plugin configuration root must be an object");
        }

        PluginConfiguration configuration;
        const auto base = detail::configuration_directory(configuration_path);
        if (document.contains("plugin_roots")) {
            const auto &roots = document.at("plugin_roots");
            if (!roots.is_array()) {
                throw std::invalid_argument("plugin_roots must be an array");
            }
            for (const auto &root : roots) {
                const auto root_text = detail::required_string(root, "plugin_roots");
                const std::filesystem::path root_path(root_text);
                if (!root_path.is_absolute() && root_text.find("..") != std::string::npos) {
                    throw std::invalid_argument("relative plugin roots cannot escape the configuration directory");
                }
                configuration.plugin_roots.push_back(
                    root_path.is_absolute() ? root_path : base / root_path);
            }
        }
        if (configuration.plugin_roots.empty()) {
            configuration.plugin_roots.push_back(base / "plugins");
        }

        if (document.contains("enabled_plugins")) {
            const auto &enabled = document.at("enabled_plugins");
            if (!enabled.is_array()) {
                throw std::invalid_argument("enabled_plugins must be an array");
            }
            std::unordered_set<std::string> seen;
            for (const auto &plugin : enabled) {
                const auto plugin_id = detail::required_string(plugin, "enabled_plugins");
                if (!seen.insert(plugin_id).second) {
                    throw std::invalid_argument("duplicate enabled plugin: " + plugin_id);
                }
                configuration.enabled_plugins.push_back(plugin_id);
            }
        }

        result.valid = true;
        result.configuration = std::move(configuration);
    } catch (const std::exception &error) {
        result.diagnostics = error.what();
    }
    return result;
}

PluginConfigurationResult load_plugin_configuration_file(
    const std::filesystem::path &configuration_path) {
    std::ifstream input(configuration_path);
    if (!input) {
        return PluginConfigurationResult{false, {}, "unable to open streamfind.json: " + configuration_path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return load_plugin_configuration(contents.str(), configuration_path);
}

}  // namespace streamfind
