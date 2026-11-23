#include "streamfind/plugin_lockfile.hpp"
#include "streamfind/plugin_abi.h"

#include <fstream>
#include <set>
#include <stdexcept>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace streamfind::detail {

std::string lock_string(const nlohmann::json &object, const char *field) {
    if (!object.contains(field) || !object.at(field).is_string() || object.at(field).get<std::string>().empty())
        throw std::invalid_argument(std::string("plugin lockfile field must be a non-empty string: ") + field);
    return object.at(field).get<std::string>();
}

void validate_lockfile(const PluginLockfile &lockfile) {
    if (lockfile.schema_version != 1)
        throw std::invalid_argument("unsupported plugin lockfile schema_version: " +
                                    std::to_string(lockfile.schema_version));
    std::set<std::string> plugin_ids;
    for (const auto &plugin : lockfile.plugins) {
        if (plugin.plugin_id.empty() || plugin.version.empty() || plugin.package_digest.empty())
            throw std::invalid_argument("plugin lockfile entries must contain plugin identity, version and digest");
        if (!plugin_ids.insert(plugin.plugin_id).second)
            throw std::invalid_argument("duplicate plugin lockfile entry: " + plugin.plugin_id);
        if (plugin.abi_major < 0 || plugin.abi_minor < 0)
            throw std::invalid_argument("plugin lockfile ABI version cannot be negative");
        std::set<std::string> operation_ids;
        for (const auto &operation_id : plugin.operation_ids) {
            if (operation_id.empty() || !operation_ids.insert(operation_id).second)
                throw std::invalid_argument("plugin lockfile contains a duplicate or empty operation ID: " + plugin.plugin_id);
        }
        for (const auto &[operation_id, version] : plugin.result_contract_versions) {
            if (operation_id.empty() || version.empty())
                throw std::invalid_argument("plugin lockfile result contracts must contain operation and version");
        }
    }
}

void replace_atomically(const std::filesystem::path &temporary,
                        const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("plugin lockfile atomic replace failed: " + std::to_string(GetLastError()));
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) throw std::runtime_error("plugin lockfile atomic replace failed: " + error.message());
#endif
}

nlohmann::json serialize(const PluginLockfile &lockfile) {
    nlohmann::json document{{"schema_version", lockfile.schema_version}, {"plugins", nlohmann::json::array()}};
    for (const auto &plugin : lockfile.plugins) {
        document["plugins"].push_back({
            {"plugin_id", plugin.plugin_id},
            {"version", plugin.version},
            {"package_digest", plugin.package_digest},
            {"abi_version", {{"major", plugin.abi_major}, {"minor", plugin.abi_minor}}},
            {"operation_ids", plugin.operation_ids},
            {"result_contract_versions", plugin.result_contract_versions},
        });
    }
    return document;
}

}  // namespace streamfind::detail

namespace streamfind {

void validate_plugin_lockfile_against_registry(const PluginLockfile &lockfile,
                                               const PluginRegistry &registry) {
    for (const auto &locked : lockfile.plugins) {
        const auto active = find_active_plugin(registry, locked.plugin_id);
        if (!active)
            throw std::invalid_argument("plugin lockfile requires an active installed plugin: " + locked.plugin_id);
        if (active->version != locked.version || active->package_digest != locked.package_digest)
            throw std::invalid_argument("plugin lockfile does not match the active installed package: " + locked.plugin_id);
        if (locked.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
            locked.abi_minor > STREAMFIND_PLUGIN_ABI_MINOR)
            throw std::invalid_argument("plugin lockfile ABI is incompatible with this runtime: " + locked.plugin_id);
    }
}

PluginLockfileResult parse_plugin_lockfile(const std::string &json_text) {
    PluginLockfileResult result;
    try {
        const auto document = nlohmann::json::parse(json_text);
        if (!document.is_object()) throw std::invalid_argument("plugin lockfile root must be an object");
        if (!document.contains("schema_version") || !document.at("schema_version").is_number_integer())
            throw std::invalid_argument("plugin lockfile schema_version must be an integer");
        if (!document.contains("plugins") || !document.at("plugins").is_array())
            throw std::invalid_argument("plugin lockfile plugins must be an array");

        PluginLockfile lockfile;
        lockfile.schema_version = document.at("schema_version").get<int>();
        for (const auto &value : document.at("plugins")) {
            if (!value.is_object()) throw std::invalid_argument("plugin lockfile entries must be objects");
            PluginLockEntry plugin;
            plugin.plugin_id = detail::lock_string(value, "plugin_id");
            plugin.version = detail::lock_string(value, "version");
            plugin.package_digest = detail::lock_string(value, "package_digest");
            const auto &abi = value.at("abi_version");
            if (!abi.is_object() || !abi.contains("major") || !abi.at("major").is_number_integer() ||
                !abi.contains("minor") || !abi.at("minor").is_number_integer())
                throw std::invalid_argument("plugin lockfile abi_version must contain integer major and minor fields");
            plugin.abi_major = abi.at("major").get<int>();
            plugin.abi_minor = abi.at("minor").get<int>();
            if (!value.contains("operation_ids") || !value.at("operation_ids").is_array())
                throw std::invalid_argument("plugin lockfile operation_ids must be an array");
            for (const auto &operation_id : value.at("operation_ids")) {
                if (!operation_id.is_string()) throw std::invalid_argument("plugin lockfile operation IDs must be strings");
                plugin.operation_ids.push_back(operation_id.get<std::string>());
            }
            if (!value.contains("result_contract_versions") || !value.at("result_contract_versions").is_object())
                throw std::invalid_argument("plugin lockfile result_contract_versions must be an object");
            for (const auto &[operation_id, version] : value.at("result_contract_versions").items()) {
                if (!version.is_string()) throw std::invalid_argument("plugin lockfile result contract versions must be strings");
                plugin.result_contract_versions.emplace(operation_id, version.get<std::string>());
            }
            lockfile.plugins.push_back(std::move(plugin));
        }
        detail::validate_lockfile(lockfile);
        result.valid = true;
        result.lockfile = std::move(lockfile);
    } catch (const std::exception &error) {
        result.diagnostics = error.what();
    }
    return result;
}

PluginLockfileResult load_plugin_lockfile(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) return {false, {}, "unable to open plugin lockfile: " + path.string()};
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse_plugin_lockfile(contents.str());
}

void write_plugin_lockfile_atomic(const std::filesystem::path &path, const PluginLockfile &lockfile) {
    detail::validate_lockfile(lockfile);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to create temporary plugin lockfile: " + temporary.string());
        output << detail::serialize(lockfile).dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("unable to write temporary plugin lockfile: " + temporary.string());
    }
    detail::replace_atomically(temporary, path);
}

}  // namespace streamfind
