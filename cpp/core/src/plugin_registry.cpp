#include "streamfind/plugin_registry.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace streamfind::detail {

std::string registry_string(const nlohmann::json &object, const char *field) {
    if (!object.contains(field) || !object.at(field).is_string() || object.at(field).get<std::string>().empty())
        throw std::invalid_argument(std::string("plugin registry field must be a non-empty string: ") + field);
    return object.at(field).get<std::string>();
}

void validate_registry(const PluginRegistry &registry) {
    if (registry.schema_version != 1)
        throw std::invalid_argument("unsupported plugin registry schema_version: " +
                                    std::to_string(registry.schema_version));
    std::set<std::string> identities;
    std::set<std::string> active_plugins;
    for (const auto &entry : registry.installed) {
        if (entry.plugin_id.empty() || entry.version.empty() || entry.platform.empty() ||
            entry.package_path.empty() || entry.package_digest.empty() || entry.origin.empty() || entry.trust.empty())
            throw std::invalid_argument("plugin registry entries must contain identity, package and trust metadata");
        const auto identity = entry.plugin_id + "\n" + entry.version + "\n" + entry.platform;
        if (!identities.insert(identity).second)
            throw std::invalid_argument("duplicate installed plugin identity: " + entry.plugin_id);
        if (entry.active && !active_plugins.insert(entry.plugin_id).second)
            throw std::invalid_argument("multiple active versions for plugin: " + entry.plugin_id);
    }
}

nlohmann::json to_json(const PluginRegistry &registry) {
    nlohmann::json document{{"schema_version", registry.schema_version}, {"generation", registry.generation},
                            {"installed", nlohmann::json::array()}};
    for (const auto &entry : registry.installed) {
        document["installed"].push_back({
            {"plugin_id", entry.plugin_id},
            {"version", entry.version},
            {"platform", entry.platform},
            {"package_path", entry.package_path.generic_string()},
            {"package_digest", entry.package_digest},
            {"origin", entry.origin},
            {"trust", entry.trust},
            {"compatibility", entry.compatibility},
            {"diagnostics", entry.diagnostics},
            {"source_path", entry.source_path.generic_string()},
            {"active", entry.active},
        });
    }
    return document;
}

void replace_file_atomically(const std::filesystem::path &temporary,
                             const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("plugin registry atomic replace failed: " + std::to_string(GetLastError()));
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) throw std::runtime_error("plugin registry atomic replace failed: " + error.message());
#endif
}

}  // namespace streamfind::detail

namespace streamfind {

std::string native_plugin_platform() {
#ifdef _WIN32
    return "windows-x86_64";
#else
    return "linux-x86_64";
#endif
}

std::optional<InstalledPlugin> find_active_plugin(const PluginRegistry &registry,
                                                   const std::string &plugin_id,
                                                   const std::string &platform) {
    const auto requested_platform = platform.empty() ? native_plugin_platform() : platform;
    for (const auto &entry : registry.installed) {
        if (entry.plugin_id == plugin_id && entry.platform == requested_platform && entry.active)
            return entry;
    }
    return std::nullopt;
}

PluginRegistryResult parse_plugin_registry(const std::string &json_text) {
    PluginRegistryResult result;
    try {
        const auto document = nlohmann::json::parse(json_text);
        if (!document.is_object()) throw std::invalid_argument("plugin registry root must be an object");
        if (!document.contains("schema_version") || !document.at("schema_version").is_number_integer())
            throw std::invalid_argument("plugin registry schema_version must be an integer");
        if (!document.contains("generation") || !document.at("generation").is_number_unsigned())
            throw std::invalid_argument("plugin registry generation must be an unsigned integer");
        if (!document.contains("installed") || !document.at("installed").is_array())
            throw std::invalid_argument("plugin registry installed must be an array");

        PluginRegistry registry;
        registry.schema_version = document.at("schema_version").get<int>();
        registry.generation = document.at("generation").get<std::uint64_t>();
        for (const auto &value : document.at("installed")) {
            if (!value.is_object()) throw std::invalid_argument("plugin registry entries must be objects");
            InstalledPlugin entry;
            entry.plugin_id = detail::registry_string(value, "plugin_id");
            entry.version = detail::registry_string(value, "version");
            entry.platform = detail::registry_string(value, "platform");
            entry.package_path = detail::registry_string(value, "package_path");
            entry.package_digest = detail::registry_string(value, "package_digest");
            entry.origin = detail::registry_string(value, "origin");
            entry.trust = detail::registry_string(value, "trust");
            if (value.contains("compatibility")) entry.compatibility = detail::registry_string(value, "compatibility");
            if (value.contains("diagnostics")) {
                if (!value.at("diagnostics").is_string()) throw std::invalid_argument("plugin registry diagnostics must be a string");
                entry.diagnostics = value.at("diagnostics").get<std::string>();
            }
            if (value.contains("source_path")) {
                if (!value.at("source_path").is_string()) throw std::invalid_argument("plugin registry source_path must be a string");
                entry.source_path = value.at("source_path").get<std::string>();
            }
            if (!value.contains("active") || !value.at("active").is_boolean())
                throw std::invalid_argument("plugin registry active must be boolean");
            entry.active = value.at("active").get<bool>();
            registry.installed.push_back(std::move(entry));
        }
        detail::validate_registry(registry);
        result.valid = true;
        result.registry = std::move(registry);
    } catch (const std::exception &error) {
        result.diagnostics = error.what();
    }
    return result;
}

PluginRegistryResult load_plugin_registry(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) return {false, {}, "unable to open plugin registry: " + path.string()};
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse_plugin_registry(contents.str());
}

void write_plugin_registry_atomic(const std::filesystem::path &path, const PluginRegistry &registry) {
    detail::validate_registry(registry);
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to create temporary plugin registry: " + temporary.string());
        output << detail::to_json(registry).dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("unable to write temporary plugin registry: " + temporary.string());
    }
    detail::replace_file_atomically(temporary, path);
}

}  // namespace streamfind
