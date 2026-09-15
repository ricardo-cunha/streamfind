#include "static_plugin_composition.hpp"

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "streamfind/catalogue.hpp"
#include "streamfind/plugin_configuration.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

namespace streamfind::static_plugins {

namespace detail {

}  // namespace detail

namespace dynamic_detail {

void report_error(const char *message, uint32_t size, void *) {
    if (message != nullptr && size != 0)
        std::cerr.write(message, static_cast<std::streamsize>(size)) << '\n';
}

void *allocate(uint64_t size, uint64_t, void *) {
    return std::malloc(static_cast<std::size_t>(size));
}

void deallocate(void *pointer, uint64_t, uint64_t, void *) {
    std::free(pointer);
}

}  // namespace dynamic_detail

void DynamicPluginRuntime::load_and_register(
    const std::filesystem::path &configuration_path,
    MethodRegistry &methods,
    OperationRegistry &operations) {
    const auto configuration = streamfind::load_plugin_configuration_file(configuration_path);
    if (!configuration.valid) {
        throw std::runtime_error(configuration.diagnostics);
    }
    const auto core_path = catalogue::find_core_path();
    if (!core_path) throw std::runtime_error("installed core catalogue not found");
    auto merged = catalogue::load_document(*core_path);
    if (!merged) throw std::runtime_error("installed core catalogue could not be loaded");
    std::set<std::string> loaded_ids;
    for (const auto &plugin_id : configuration.configuration.enabled_plugins) {
        std::filesystem::path package_root;
        for (const auto &root : configuration.configuration.plugin_roots) {
            const auto candidate = root / plugin_id;
            if (std::filesystem::exists(candidate / "plugin.json")) {
                package_root = candidate;
                break;
            }
        }
        if (package_root.empty()) {
            throw std::runtime_error("enabled dynamic plugin package not found: " + plugin_id);
        }
        if (!loaded_ids.insert(plugin_id).second) {
            throw std::runtime_error("duplicate enabled dynamic plugin: " + plugin_id);
        }

        auto loaded = std::make_unique<LoadedPlugin>();
        loaded->package_root = package_root;
        loaded->plugin = sdk::load_dynamic_plugin_package(package_root);
        if (!loaded->plugin.loaded || loaded->plugin.manifest.plugin_id != plugin_id) {
            throw std::runtime_error("dynamic plugin load failed for " + plugin_id + ": " +
                                     loaded->plugin.diagnostics);
        }
        loaded->host.struct_size = sizeof(loaded->host);
        loaded->host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
        loaded->host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
        loaded->host.report_error = &dynamic_detail::report_error;
        loaded->host.allocate = &dynamic_detail::allocate;
        loaded->host.deallocate = &dynamic_detail::deallocate;
        loaded->host.has_table = &sdk::plugin_has_table;
        loaded->host.clear_table = &sdk::plugin_clear_table;
        loaded->host.read_batch = &sdk::plugin_read_batch;
        loaded->host.append_batch = &sdk::plugin_append_batch;
        loaded->host.update_batch = &sdk::plugin_update_batch;
        loaded->host.update_composite_batch = &sdk::plugin_update_composite_batch;
        loaded->host.delete_batch = &sdk::plugin_delete_batch;
        loaded->host.report_progress = &sdk::plugin_report_progress;
        loaded->host.is_cancelled = &sdk::plugin_is_cancelled;
        if (sdk::register_dynamic_plugin(loaded->plugin, loaded->host) != STREAMFIND_PLUGIN_OK) {
            throw std::runtime_error("dynamic plugin registration failed for " + plugin_id + ": " +
                                     loaded->plugin.diagnostics);
        }

        const auto catalogue_path = package_root / loaded->plugin.manifest.semantic_catalogue;
        const auto plugin_document = catalogue::load_document(catalogue_path.string());
        if (!plugin_document) {
            throw std::runtime_error("dynamic plugin catalogue could not be loaded for " + plugin_id);
        }
        loaded->catalogue = std::move(*plugin_document);
        std::set<std::string> modules;
        for (const auto &entry : loaded->catalogue.at("entries"))
            if (entry.value("domain", "") == plugin_id && entry.contains("module_id"))
                modules.insert(entry.at("module_id").get<std::string>());
        if (modules.empty()) {
            const auto module_id = loaded->plugin.plugin.module_id == nullptr
                                       ? std::string{}
                                       : loaded->plugin.plugin.module_id;
            const auto expected_module = plugin_id + ".base";
            if (module_id != expected_module)
                throw std::runtime_error("dynamic plugin catalogue has no owned modules for " + plugin_id);
            modules.insert(expected_module);
        }
        try {
            merged = catalogue::import_plugin_catalogue(
                *merged, loaded->catalogue, plugin_id, std::vector<std::string>(modules.begin(), modules.end()));
        } catch (const std::exception &error) {
            throw std::runtime_error("dynamic catalogue merge failed for " + plugin_id + ": " + error.what());
        }
        plugins_.push_back(std::move(loaded));
    }
    catalogue::set_runtime_document(std::move(*merged));
    for (auto &loaded : plugins_) {
        sdk::register_dynamic_plugin_capabilities(
            loaded->plugin, loaded->catalogue.at("entries"), methods, operations);
    }
}

}  // namespace streamfind::static_plugins
