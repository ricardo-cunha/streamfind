#include "streamfind/service/service_plugin_runtime.hpp"

#include "streamfind/catalogue.hpp"
#include "streamfind/core_operations.hpp"
#include "streamfind/plugin_configuration.hpp"
#include "streamfind/plugin_registry.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

#include <cstdlib>
#include <optional>
#include <set>
#include <stdexcept>

namespace streamfind::service {
namespace detail {

void report_error(const char *message, uint32_t size, void *user_data) {
    if (user_data == nullptr) return;
    auto *diagnostics = static_cast<std::string *>(user_data);
    const auto detail = message == nullptr ? std::string{"plugin reported an unknown error"}
                                           : std::string(message, size);
    if (diagnostics->empty()) diagnostics->assign(detail);
    else if (diagnostics->find(detail) == std::string::npos) *diagnostics += " | " + detail;
}
void *allocate(uint64_t size, uint64_t, void *) { return std::malloc(static_cast<std::size_t>(size)); }
void deallocate(void *pointer, uint64_t, uint64_t, void *) { std::free(pointer); }

}  // namespace detail

void ServicePluginRuntime::load(const std::filesystem::path &configuration_path,
                                MethodRegistry &methods,
                                OperationRegistry &operations) {
    configuration_path_ = configuration_path;
    const auto configuration = load_plugin_configuration_file(configuration_path);
    if (!configuration.valid) throw std::runtime_error(configuration.diagnostics);
    const auto core_path = catalogue::find_core_path();
    if (!core_path) throw std::runtime_error("installed core catalogue not found");
    auto merged = catalogue::load_document(*core_path);
    if (!merged) throw std::runtime_error("installed core catalogue could not be loaded");
    core_operations::register_operations(merged->at("entries"), operations);
    std::optional<PluginRegistry> registry;
    if (!configuration.configuration.registry_path.empty()) {
        const auto loaded_registry = load_plugin_registry(configuration.configuration.registry_path);
        if (!loaded_registry.valid) throw std::runtime_error(loaded_registry.diagnostics);
        registry = loaded_registry.registry;
    }
    std::vector<std::string> plugin_ids = configuration.configuration.enabled_plugins;
    if (registry) {
        for (const auto &installed : registry->installed) {
            if (!installed.active || installed.platform != native_plugin_platform()) continue;
            if (std::find(plugin_ids.begin(), plugin_ids.end(), installed.plugin_id) == plugin_ids.end())
                plugin_ids.push_back(installed.plugin_id);
        }
    }
    for (const auto &plugin_id : plugin_ids) {
        std::filesystem::path package_root;
        if (registry) {
            const auto active = find_active_plugin(*registry, plugin_id);
            if (!active) throw std::runtime_error("plugin is not active in the registry: " + plugin_id);
            package_root = active->package_path;
        } else {
            for (const auto &root : configuration.configuration.plugin_deployment_roots) {
                const auto candidate = root / plugin_id;
                if (std::filesystem::exists(candidate / "plugin.json")) {
                    package_root = candidate;
                    break;
                }
                for (const auto &configuration_name : {std::string("Debug"), std::string("Release"), std::string("RelWithDebInfo"), std::string("MinSizeRel")}) {
                    const auto configured_candidate = candidate / configuration_name;
                    if (std::filesystem::exists(configured_candidate / "plugin.json")) {
                        package_root = configured_candidate;
                        break;
                    }
                }
                if (!package_root.empty()) break;
            }
        }
        if (package_root.empty()) throw std::runtime_error("enabled dynamic plugin package not found: " + plugin_id);
        auto loaded = std::make_unique<LoadedPlugin>();
        loaded->package_path = package_root;
        loaded->plugin = sdk::load_dynamic_plugin_package(package_root);
        if (!loaded->plugin.loaded || loaded->plugin.manifest.plugin_id != plugin_id)
            throw std::runtime_error("dynamic plugin load failed for " + plugin_id + ": " + loaded->plugin.diagnostics);
        const auto &plugin = loaded->plugin.manifest.plugin;
        auto &host = loaded->host;
        host.struct_size = sizeof(host);
        host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
        host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
        host.report_error = &detail::report_error;
        host.user_data = &loaded->plugin.runtime_diagnostics;
        host.allocate = &detail::allocate;
        host.deallocate = &detail::deallocate;
        host.has_table = &sdk::plugin_has_table;
        host.read_batch = &sdk::plugin_read_batch;
        host.count_rows = &sdk::plugin_count_rows;
        host.emit_table_batch = &sdk::plugin_emit_table_batch;
        host.emit_result = &sdk::plugin_emit_result;
        host.report_progress = &sdk::plugin_report_progress;
        host.is_cancelled = &sdk::plugin_is_cancelled;
        if (sdk::register_dynamic_plugin(loaded->plugin, host) != STREAMFIND_PLUGIN_OK)
            throw std::runtime_error("dynamic plugin registration failed for " + plugin_id + ": " + loaded->plugin.diagnostics);
        const auto catalogue_path = package_root / loaded->plugin.manifest.semantic_catalogue;
        const auto catalogue = catalogue::load_document(catalogue_path.string());
        if (!catalogue) throw std::runtime_error("dynamic plugin catalogue could not be loaded for " + plugin_id);
        loaded->catalogue = *catalogue;
        std::set<std::string> modules;
        for (const auto &entry : loaded->catalogue.at("entries"))
            if (entry.value("plugin", "") == plugin) modules.insert(entry.value("module_id", ""));
        modules.erase("");
        if (modules.empty()) modules.insert(plugin + ".base");
        merged = catalogue::import_plugin_catalogue(
            *merged, loaded->catalogue, plugin,
            std::vector<std::string>(modules.begin(), modules.end()),
            loaded->plugin.manifest.ontology_namespace);
        plugins_.push_back(std::move(loaded));
    }
    merged_catalogue_ = *merged;
    catalogue::set_runtime_document(merged_catalogue_);
    generation_ = registry ? registry->generation : 0;
    for (auto &loaded : plugins_)
        sdk::register_dynamic_plugin_capabilities(loaded->plugin, loaded->catalogue.at("entries"), methods, operations);
}

Json ServicePluginRuntime::activate(const std::string &plugin_id,
                                    MethodRegistry &methods,
                                    OperationRegistry &operations) {
    if (configuration_path_.empty()) throw std::runtime_error("plugin runtime has no configuration path");
    const auto configuration = load_plugin_configuration_file(configuration_path_);
    if (!configuration.valid) throw std::runtime_error(configuration.diagnostics);
    const auto loaded_registry = load_plugin_registry(configuration.configuration.registry_path);
    if (!loaded_registry.valid) throw std::runtime_error(loaded_registry.diagnostics);
    const auto active = find_active_plugin(loaded_registry.registry, plugin_id);
    if (!active) throw std::runtime_error("plugin is not active in the registry: " + plugin_id);
    for (const auto &loaded : plugins_)
        if (loaded->plugin.manifest.plugin_id == plugin_id && loaded->plugin.manifest.version == active->version)
            return Json{{"plugin_id", plugin_id}, {"version", active->version}, {"generation", generation_}, {"already_loaded", true}};

    const auto core_path = catalogue::find_core_path();
    if (!core_path) throw std::runtime_error("installed core catalogue not found");
    auto merged = merged_catalogue_.empty() ? catalogue::load_document(*core_path) : std::optional<Json>(merged_catalogue_);
    if (!merged) throw std::runtime_error("runtime catalogue could not be loaded");
    auto loaded = std::make_unique<LoadedPlugin>();
    loaded->package_path = active->package_path;
    loaded->plugin = sdk::load_dynamic_plugin_package(active->package_path);
    if (!loaded->plugin.loaded || loaded->plugin.manifest.plugin_id != plugin_id)
        throw std::runtime_error("dynamic plugin load failed for " + plugin_id + ": " + loaded->plugin.diagnostics);
    const auto &plugin = loaded->plugin.manifest.plugin;
    auto &host = loaded->host;
    host.struct_size = sizeof(host); host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR; host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    host.report_error = &detail::report_error; host.user_data = &loaded->plugin.runtime_diagnostics;
    host.allocate = &detail::allocate; host.deallocate = &detail::deallocate;
    host.has_table = &sdk::plugin_has_table; host.read_batch = &sdk::plugin_read_batch;
    host.count_rows = &sdk::plugin_count_rows; host.emit_table_batch = &sdk::plugin_emit_table_batch;
    host.emit_result = &sdk::plugin_emit_result; host.report_progress = &sdk::plugin_report_progress;
    host.is_cancelled = &sdk::plugin_is_cancelled;
    if (sdk::register_dynamic_plugin(loaded->plugin, host) != STREAMFIND_PLUGIN_OK)
        throw std::runtime_error("dynamic plugin registration failed for " + plugin_id + ": " + loaded->plugin.diagnostics);
    const auto catalogue_path = active->package_path / loaded->plugin.manifest.semantic_catalogue;
    const auto plugin_catalogue = catalogue::load_document(catalogue_path.string());
    if (!plugin_catalogue) throw std::runtime_error("dynamic plugin catalogue could not be loaded for " + plugin_id);
    loaded->catalogue = *plugin_catalogue;
    std::set<std::string> modules;
    for (const auto &entry : loaded->catalogue.at("entries"))
        if (entry.value("plugin", "") == plugin) modules.insert(entry.value("module_id", ""));
    modules.erase(""); if (modules.empty()) modules.insert(plugin + ".base");
    *merged = catalogue::import_plugin_catalogue(*merged, loaded->catalogue, plugin,
        std::vector<std::string>(modules.begin(), modules.end()), loaded->plugin.manifest.ontology_namespace);
    sdk::register_dynamic_plugin_capabilities(loaded->plugin, loaded->catalogue.at("entries"), methods, operations);
    plugins_.push_back(std::move(loaded));
    merged_catalogue_ = *merged; catalogue::set_runtime_document(merged_catalogue_); generation_ = loaded_registry.registry.generation;
    return Json{{"plugin_id", plugin_id}, {"version", active->version}, {"generation", generation_}, {"already_loaded", false}};
}

Json ServicePluginRuntime::status() const {
    Json plugins = Json::array();
    for (const auto &loaded : plugins_)
        plugins.push_back({{"plugin_id", loaded->plugin.manifest.plugin_id}, {"version", loaded->plugin.manifest.version}, {"package_path", loaded->package_path.string()}});
    return Json{{"generation", generation_}, {"plugins", plugins}};
}

Json ServicePluginRuntime::dependencies() const {
    Json result = Json::array();
    for (const auto &loaded : plugins_) {
        if (loaded->plugin.plugin.describe_dependencies == nullptr) continue;
        streamfind_plugin_buffer buffer{};
        if (loaded->plugin.plugin.describe_dependencies(&buffer, loaded->plugin.plugin.user_data) != STREAMFIND_PLUGIN_OK)
            throw std::runtime_error("plugin dependency discovery failed for " + loaded->plugin.manifest.plugin_id);
        try {
            const auto values = Json::parse(std::string(buffer.data, buffer.size));
            if (values.is_array()) for (const auto &value : values) result.push_back(value);
        } catch (...) {
            loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
            throw;
        }
        loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
    }
    return result;
}

Json ServicePluginRuntime::install_dependencies(const Json &request) const {
    if (!request.is_object() || !request.contains("dependency_ids") || !request.at("dependency_ids").is_array())
        throw std::invalid_argument("dependency_ids must be an array");
    Json result = Json::array();
    for (const auto &loaded : plugins_) {
        if (loaded->plugin.plugin.install_dependencies == nullptr) continue;
        streamfind_plugin_buffer buffer{};
        const auto text = request.dump();
        const auto status = loaded->plugin.plugin.install_dependencies(
            text.data(), static_cast<uint32_t>(text.size()), &buffer, loaded->plugin.plugin.user_data);
        if (status == STREAMFIND_PLUGIN_INVALID_ARGUMENT) continue;
        if (status != STREAMFIND_PLUGIN_OK)
            throw std::runtime_error("plugin dependency installation failed for " + loaded->plugin.manifest.plugin_id +
                                     ": " + loaded->plugin.runtime_diagnostics);
        try {
            const auto values = Json::parse(std::string(buffer.data, buffer.size));
            if (values.contains("results") && values.at("results").is_array())
                for (const auto &value : values.at("results")) result.push_back(value);
        } catch (...) {
            loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
            throw;
        }
        loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
    }
    return Json{{"results", result}};
}

}  // namespace streamfind::service
