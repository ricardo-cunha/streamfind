#include "streamfind/sdk/dynamic_plugin_manager.hpp"

#include <atomic>
#include <cstring>
#include <set>

#include "streamfind/project_table_store.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

namespace streamfind::sdk::detail {

std::string dynamic_platform_tag() {
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__linux__)
    return "linux-x86_64";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

bool supports_platform(const PluginManifest &manifest, const std::string &platform) {
    for (const auto &candidate : manifest.platforms) {
        if (candidate == platform) {
            return true;
        }
    }
    return false;
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

namespace detail {

std::vector<std::string> entry_tables(const Json &entry) {
    std::set<std::string> unique;
    const auto effects = entry.contains("effects") && entry.at("effects").is_object()
                             ? entry.at("effects")
                             : Json::object();
    for (const char *key : {"reads", "writes"}) {
        const auto values = effects.value(key, Json::array());
        if (!values.is_array()) {
            continue;
        }
        for (const auto &value : values) {
            if (value.is_string()) {
                unique.insert(value.get<std::string>());
            }
        }
    }
    const auto domain = entry.value("domain", std::string{});
    if (!domain.empty()) {
        if (const auto manifest = catalogue::table_manifest_json(domain, "")) {
            for (const auto &table : *manifest) {
                const auto name = table.value("table_name", std::string{});
                if (!name.empty()) unique.insert(name);
            }
        }
    }
    return {unique.begin(), unique.end()};
}

Json invoke_dynamic(
    DynamicPluginLoadResult &plugin,
    Project &project,
    const std::string &capability_id,
    const Json &parameters,
    const std::vector<std::string> &owned_tables) {
    if (plugin.plugin.invoke == nullptr || plugin.plugin.release_buffer == nullptr) {
        throw Error(ErrorCode::MethodExecution, "dynamic plugin has no JSON invocation contract");
    }
    Json request = {{"capability_id", capability_id}, {"parameters", parameters}};
    Json response;
    if (!owned_tables.empty() && !plugin.manifest.domain.empty() && plugin.plugin.module_id != nullptr)
        ProjectTableStore::install_manifest_schema(project, plugin.manifest.domain, "");
    ProjectTableStore::transaction(project, owned_tables, [&](ProjectTableStore &tables) {
        if (!owned_tables.empty() && !plugin.manifest.domain.empty() && plugin.plugin.module_id != nullptr) {
            tables.require_installed_manifest(plugin.manifest.domain, plugin.plugin.module_id, 1);
        }
        PluginDataServiceContext context;
        std::atomic_bool cancelled{false};
        context.tables = &tables;
        context.cancelled = &cancelled;
        context.allowed_tables = owned_tables;
        if (!owned_tables.empty() && !plugin.manifest.domain.empty() && plugin.plugin.module_id != nullptr) {
            const auto manifest = catalogue::table_manifest_json(
                plugin.manifest.domain, "");
            if (!manifest) throw Error(ErrorCode::SchemaMismatch, "dynamic table manifest unavailable");
            for (const auto &table : *manifest) {
                const auto table_name = table.value("table_name", std::string{});
                if (std::find(owned_tables.begin(), owned_tables.end(), table_name) == owned_tables.end()) continue;
                for (const auto &column : table.value("columns", Json::array())) {
                    const auto column_name = column.value("name", std::string{});
                    if (column_name.empty()) continue;
                    context.readable_columns[table_name].insert(column_name);
                    context.writable_columns[table_name].insert(column_name);
                }
            }
        }
        streamfind_plugin_buffer buffer{};
        const auto text = request.dump();
        const auto status = plugin.plugin.invoke(
            &context, text.data(), static_cast<uint32_t>(text.size()), &buffer, plugin.plugin.user_data);
        if (status != STREAMFIND_PLUGIN_OK) {
            throw Error(ErrorCode::MethodExecution,
                        "dynamic plugin invocation failed with status " + std::to_string(status));
        }
        if ((buffer.data == nullptr) != (buffer.size == 0)) {
            plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
            throw Error(ErrorCode::MethodExecution,
                        "dynamic plugin returned an invalid response buffer");
        }
        try {
            response = Json::parse(std::string(buffer.data, buffer.size));
        } catch (const std::exception &error) {
            plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
            throw Error(ErrorCode::MethodExecution,
                        std::string("dynamic plugin returned invalid JSON: ") + error.what());
        }
        plugin.plugin.release_buffer(&buffer, plugin.plugin.user_data);
    });
    return response;
}

}  // namespace detail

DynamicPluginLoadResult load_dynamic_plugin_package(const std::filesystem::path &package_root) {
    DynamicPluginLoadResult result;
    const auto manifest_path = package_root / "plugin.json";
    const auto manifest_result = load_plugin_manifest(manifest_path);
    if (!manifest_result.valid) {
        result.diagnostics = manifest_result.diagnostics;
        return result;
    }
    result.manifest = manifest_result.manifest;

    if (result.manifest.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR) {
        result.diagnostics = "plugin ABI major is incompatible with the host";
        return result;
    }
    if (!detail::supports_platform(result.manifest, detail::dynamic_platform_tag())) {
        result.diagnostics = "plugin manifest does not support the current platform";
        return result;
    }

    const auto platform = detail::dynamic_platform_tag();
    const auto *library_name = result.manifest.library_for_platform(platform);
    if (library_name == nullptr) {
        result.diagnostics = "plugin manifest has no library for the current platform";
        return result;
    }

    const auto package_path = std::filesystem::weakly_canonical(package_root);
    const auto library_path = std::filesystem::weakly_canonical(package_path / *library_name);
    const auto relative = library_path.lexically_relative(package_path);
    if (relative.empty() || relative == ".." || relative.string().starts_with("..")) {
        result.diagnostics = "plugin library resolves outside its package root";
        return result;
    }

    auto library_result = DynamicLibrary::load(library_path);
    if (!library_result.loaded || !library_result.library) {
        result.diagnostics = library_result.diagnostics;
        return result;
    }

    std::string symbol_diagnostics;
    const auto symbol = library_result.library->symbol(
        "streamfind_plugin_get_descriptor", symbol_diagnostics);
    if (symbol == nullptr) {
        result.diagnostics = symbol_diagnostics;
        return result;
    }

    streamfind_plugin_get_descriptor_fn get_descriptor = nullptr;
    static_assert(sizeof(get_descriptor) == sizeof(symbol));
    std::memcpy(&get_descriptor, &symbol, sizeof(get_descriptor));
    if (get_descriptor == nullptr) {
        result.diagnostics = "plugin descriptor symbol is invalid";
        return result;
    }

    streamfind_plugin_descriptor descriptor{};
    const auto status = get_descriptor(
        STREAMFIND_PLUGIN_ABI_MAJOR,
        STREAMFIND_PLUGIN_ABI_MINOR,
        &descriptor);
    if (status != STREAMFIND_PLUGIN_OK) {
        result.diagnostics = "plugin descriptor negotiation failed with status " +
                             std::to_string(status);
        return result;
    }
    if (descriptor.struct_size < sizeof(streamfind_plugin_descriptor) ||
        descriptor.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR ||
        descriptor.plugin_id == nullptr || descriptor.plugin_version == nullptr ||
        descriptor.register_plugin == nullptr) {
        result.diagnostics = "plugin descriptor is incomplete or incompatible";
        return result;
    }
    if (result.manifest.plugin_id != descriptor.plugin_id) {
        result.diagnostics = "plugin descriptor ID does not match its manifest";
        return result;
    }
    if (result.manifest.version != descriptor.plugin_version) {
        result.diagnostics = "plugin descriptor version does not match its manifest";
        return result;
    }

    result.descriptor = descriptor;
    result.library = std::move(library_result.library);
    result.loaded = true;
    return result;
}

streamfind_plugin_status register_dynamic_plugin(
    DynamicPluginLoadResult &plugin,
    const streamfind_plugin_host_api &host) {
    if (!plugin.loaded || !plugin.library || plugin.descriptor.register_plugin == nullptr) {
        plugin.diagnostics = "cannot register an unloaded dynamic plugin";
        return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    }
    if (host.struct_size < sizeof(streamfind_plugin_host_api) ||
        host.abi_major != STREAMFIND_PLUGIN_ABI_MAJOR) {
        plugin.diagnostics = "host ABI is incompatible with the dynamic plugin";
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    }

    plugin.plugin = {};
    plugin.plugin.struct_size = sizeof(streamfind_plugin_api);
    plugin.plugin.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    plugin.plugin.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    const auto status = plugin.descriptor.register_plugin(
        &host, &plugin.plugin, plugin.descriptor.user_data);
    if (status != STREAMFIND_PLUGIN_OK) {
        plugin.diagnostics = "dynamic plugin registration failed with status " +
                             std::to_string(status);
    }
    return status;
}

void register_dynamic_plugin_capabilities(
    DynamicPluginLoadResult &plugin,
    const Json &catalogue_entries,
    MethodRegistry &methods,
    OperationRegistry &operations) {
    if (!plugin.loaded || !catalogue_entries.is_array()) {
        throw Error(ErrorCode::InvalidArgument,
                    "dynamic plugin registration requires a loaded plugin and catalogue entries");
    }

    const auto parameter_schema = [](const Json &entry) {
        Json parameters = Json::array();
        const auto source = entry.contains("parameters") && entry.at("parameters").is_array()
                                ? entry.at("parameters")
                                : Json::array();
        for (const auto &item : source) {
            if (!item.is_object()) continue;
            auto definition = item;
            if (definition.contains("schema")) {
                definition["type"] = definition.at("schema");
                definition.erase("schema");
            }
            parameters.push_back(std::move(definition));
        }
        return parameters;
    };
    for (const auto &entry : catalogue_entries) {
        if (!entry.is_object() || !entry.value("executable", false)) {
            continue;
        }
        const auto id = entry.value("canonical_id", std::string{});
        if (id.empty()) {
            throw Error(ErrorCode::InvalidArgument, "dynamic catalogue entry has no canonical_id");
        }
        const auto tables = detail::entry_tables(entry);
        if (entry.value("kind", std::string{}) == "method") {
            auto definition_document = entry;
            definition_document["id"] = id;
            definition_document["parameters"] = parameter_schema(entry);
            const auto effects = entry.contains("effects") && entry.at("effects").is_object()
                                     ? entry.at("effects")
                                     : Json::object();
            definition_document["writes"] = effects.value("writes", Json::array());
            auto definition = Method::definition_from_json(definition_document);
            methods.register_method(Method(
                std::move(definition),
                [&plugin, id, tables](Project &project, const Json &parameters) {
                    return detail::invoke_dynamic(plugin, project, id, parameters, tables);
                }));
        } else if (entry.value("kind", std::string{}) == "operation") {
            OperationDefinition definition;
            definition.id = id;
            definition.name = entry.value("label", id);
            definition.description = entry.value("definition", "");
            definition.domain = entry.value("domain", "");
            definition.parameters = ParameterSchema::from_json(
                parameter_schema(entry));
            operations.register_operation(Operation(
                std::move(definition),
                [&plugin, id, tables](Project &project, const Json &parameters) {
                    return detail::invoke_dynamic(plugin, project, id, parameters, tables);
                }));
        }
    }
}

}  // namespace streamfind::sdk
