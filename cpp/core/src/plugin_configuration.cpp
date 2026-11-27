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

std::vector<std::filesystem::path> path_array(const nlohmann::json &document,
                                              const char *field,
                                              const std::filesystem::path &base) {
    std::vector<std::filesystem::path> result;
    if (!document.contains(field)) return result;
    const auto &roots = document.at(field);
    if (!roots.is_array()) throw std::invalid_argument(std::string(field) + " must be an array");
    for (const auto &root : roots) {
        const auto root_text = required_string(root, field);
        const std::filesystem::path root_path(root_text);
        result.push_back(root_path.is_absolute() ? root_path : base / root_path);
    }
    return result;
}

std::vector<std::filesystem::path> extension_path_array(const nlohmann::json &document,
                                                         const char *root_field,
                                                         const char *path_kind,
                                                         const std::filesystem::path &base) {
    if (!document.contains(root_field)) return {};
    const auto &section = document.at(root_field);
    if (!section.is_object()) throw std::invalid_argument(std::string(root_field) + " must be an object");
    return path_array(section, path_kind, base);
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
        configuration.plugin_development_roots = detail::extension_path_array(
            document, "plugin_roots", "development", base);
        configuration.plugin_deployment_roots = detail::extension_path_array(
            document, "plugin_roots", "deployment", base);
        configuration.viewer_development_roots = detail::extension_path_array(
            document, "viewer_roots", "development", base);
        configuration.viewer_deployment_roots = detail::extension_path_array(
            document, "viewer_roots", "deployment", base);
        if (configuration.plugin_deployment_roots.empty())
            configuration.plugin_deployment_roots.push_back(base / "plugins");
        if (configuration.viewer_deployment_roots.empty())
            configuration.viewer_deployment_roots.push_back(base / "viewers" / "native");

        if (document.contains("registry_path")) {
            const auto registry_text = detail::required_string(document.at("registry_path"), "registry_path");
            const std::filesystem::path registry(registry_text);
            configuration.registry_path = registry.is_absolute() ? registry : base / registry;
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

        if (document.contains("enabled_viewers")) {
            const auto &enabled = document.at("enabled_viewers");
            if (!enabled.is_array()) {
                throw std::invalid_argument("enabled_viewers must be an array");
            }
            std::unordered_set<std::string> seen;
            for (const auto &viewer : enabled) {
                const auto viewer_id = detail::required_string(viewer, "enabled_viewers");
                if (!seen.insert(viewer_id).second) {
                    throw std::invalid_argument("duplicate enabled viewer: " + viewer_id);
                }
                configuration.enabled_viewers.push_back(viewer_id);
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
