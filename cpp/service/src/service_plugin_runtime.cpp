#include "streamfind/service/service_plugin_runtime.hpp"

#include "streamfind/catalogue.hpp"
#include "streamfind/plugin_configuration.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"

#include <cstdlib>
#include <set>
#include <stdexcept>

namespace streamfind::service {
namespace detail {

void report_error(const char *message, uint32_t size, void *user_data) {
    if (user_data == nullptr) return;
    auto *diagnostics = static_cast<std::string *>(user_data);
    diagnostics->assign(message == nullptr ? "plugin reported an unknown error" : std::string(message, size));
}
void *allocate(uint64_t size, uint64_t, void *) { return std::malloc(static_cast<std::size_t>(size)); }
void deallocate(void *pointer, uint64_t, uint64_t, void *) { std::free(pointer); }

}  // namespace detail

void ServicePluginRuntime::load(const std::filesystem::path &configuration_path,
                                MethodRegistry &methods,
                                OperationRegistry &operations) {
    const auto configuration = load_plugin_configuration_file(configuration_path);
    if (!configuration.valid) throw std::runtime_error(configuration.diagnostics);
    const auto core_path = catalogue::find_core_path();
    if (!core_path) throw std::runtime_error("installed core catalogue not found");
    auto merged = catalogue::load_document(*core_path);
    if (!merged) throw std::runtime_error("installed core catalogue could not be loaded");
    for (const auto &plugin_id : configuration.configuration.enabled_plugins) {
        std::filesystem::path package_root;
        for (const auto &root : configuration.configuration.plugin_roots) {
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
        if (package_root.empty()) throw std::runtime_error("enabled dynamic plugin package not found: " + plugin_id);
        auto loaded = std::make_unique<LoadedPlugin>();
        loaded->plugin = sdk::load_dynamic_plugin_package(package_root);
        if (!loaded->plugin.loaded || loaded->plugin.manifest.plugin_id != plugin_id)
            throw std::runtime_error("dynamic plugin load failed for " + plugin_id + ": " + loaded->plugin.diagnostics);
        auto &host = loaded->host;
        host.struct_size = sizeof(host);
        host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
        host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
        host.report_error = &detail::report_error;
        host.user_data = &loaded->plugin.runtime_diagnostics;
        host.allocate = &detail::allocate;
        host.deallocate = &detail::deallocate;
        host.has_table = &sdk::plugin_has_table;
        host.clear_table = &sdk::plugin_clear_table;
        host.read_batch = &sdk::plugin_read_batch;
        host.append_batch = &sdk::plugin_append_batch;
        host.emit_table_batch = &sdk::plugin_emit_table_batch;
        host.emit_result = &sdk::plugin_emit_result;
        host.update_batch = &sdk::plugin_update_batch;
        host.update_composite_batch = &sdk::plugin_update_composite_batch;
        host.delete_batch = &sdk::plugin_delete_batch;
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
            if (entry.value("domain", "") == plugin_id) modules.insert(entry.value("module_id", ""));
        modules.erase("");
        if (modules.empty()) modules.insert(plugin_id + ".base");
        merged = catalogue::import_plugin_catalogue(
            *merged, loaded->catalogue, plugin_id,
            std::vector<std::string>(modules.begin(), modules.end()));
        plugins_.push_back(std::move(loaded));
    }
    catalogue::set_runtime_document(std::move(*merged));
    for (auto &loaded : plugins_)
        sdk::register_dynamic_plugin_capabilities(loaded->plugin, loaded->catalogue.at("entries"), methods, operations);
}

}  // namespace streamfind::service
