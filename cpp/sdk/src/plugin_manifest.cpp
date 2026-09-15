#include "streamfind/sdk/plugin_manifest.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace streamfind::sdk::detail {

std::string required_string(const nlohmann::json &object, const char *name) {
    if (!object.contains(name) || !object.at(name).is_string()) {
        throw std::invalid_argument(std::string("manifest field must be a string: ") + name);
    }
    return object.at(name).get<std::string>();
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

const std::string *PluginManifest::library_for_platform(const std::string &platform) const {
    const auto it = libraries.find(platform);
    return it == libraries.end() ? nullptr : &it->second;
}

PluginManifestResult parse_plugin_manifest(const std::string &json_text) {
    PluginManifestResult result;
    try {
        const auto document = nlohmann::json::parse(json_text);
        if (!document.is_object()) {
            throw std::invalid_argument("manifest root must be an object");
        }

        PluginManifest manifest;
        manifest.plugin_id = detail::required_string(document, "plugin_id");
        manifest.name = detail::required_string(document, "name");
        manifest.version = detail::required_string(document, "version");
        manifest.domain = detail::required_string(document, "domain");
        manifest.semantic_catalogue = detail::required_string(document, "semantic_catalogue");

        if (manifest.plugin_id.empty() || manifest.domain.empty()) {
            throw std::invalid_argument("manifest plugin_id and domain must not be empty");
        }
        const auto &libraries = document.at("library");
        if (!libraries.is_object() || libraries.empty()) {
            throw std::invalid_argument("manifest library must be a non-empty platform map");
        }
        for (const auto &[platform, library] : libraries.items()) {
            if (!library.is_string() || library.get<std::string>().empty()) {
                throw std::invalid_argument("manifest library paths must be non-empty strings");
            }
            const auto library_name = library.get<std::string>();
            const std::filesystem::path library_path(library_name);
            if (library_path.is_absolute() || library_name.find("..") != std::string::npos) {
                throw std::invalid_argument("manifest library paths must be relative and remain inside their package");
            }
            manifest.libraries.emplace(platform, library_name);
        }

        const auto &abi = document.at("abi_version");
        if (!abi.is_object() || !abi.contains("major") || !abi.at("major").is_number_integer() ||
            !abi.contains("minor") || !abi.at("minor").is_number_integer()) {
            throw std::invalid_argument("manifest abi_version must contain integer major and minor fields");
        }
        manifest.abi_major = abi.at("major").get<int>();
        manifest.abi_minor = abi.at("minor").get<int>();
        if (manifest.abi_major < 0 || manifest.abi_minor < 0) {
            throw std::invalid_argument("manifest ABI version cannot be negative");
        }

        const auto &sdk = document.at("sdk_compatibility");
        if (!sdk.is_object()) {
            throw std::invalid_argument("manifest sdk_compatibility must be an object");
        }
        manifest.sdk_minimum = detail::required_string(sdk, "minimum");
        manifest.sdk_maximum = detail::required_string(sdk, "maximum");

        if (!document.contains("static_composition") || !document.at("static_composition").is_boolean()) {
            throw std::invalid_argument("manifest static_composition must be boolean");
        }
        manifest.static_composition = document.at("static_composition").get<bool>();

        const auto &platforms = document.at("platforms");
        if (!platforms.is_array() || platforms.empty()) {
            throw std::invalid_argument("manifest platforms must be a non-empty array");
        }
        for (const auto &platform : platforms) {
            if (!platform.is_string() || platform.get<std::string>().empty()) {
                throw std::invalid_argument("manifest platforms must contain non-empty strings");
            }
            manifest.platforms.push_back(platform.get<std::string>());
        }

        result.valid = true;
        result.manifest = std::move(manifest);
    } catch (const std::exception &error) {
        result.diagnostics = error.what();
    }
    return result;
}

PluginManifestResult load_plugin_manifest(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        return PluginManifestResult{false, {}, "unable to open plugin manifest: " + path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse_plugin_manifest(contents.str());
}

}  // namespace streamfind::sdk
