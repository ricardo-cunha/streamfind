#include "static_plugin_composition.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "streamfind/catalogue.hpp"
#include "streamfind/mass_spec/register.hpp"
#include "streamfind/raman/register.hpp"
#include "streamfind/sensors/register.hpp"

namespace streamfind::static_plugins {

namespace detail {

std::optional<std::string> plugin_catalogue_path(const std::string &aggregate,
                                                 const std::string &domain) {
    const auto aggregate_path = std::filesystem::path(aggregate);
    const auto parent = aggregate_path.parent_path();
    const std::vector<std::filesystem::path> candidates = {
        parent / "plugins" / domain / "semantic_catalogue" / "catalogue.duckdb",
        parent.parent_path() / "plugins" / domain / "semantic_catalogue" / "catalogue.duckdb",
        parent / "plugins" / domain / "catalogue.duckdb"};
    for (const auto &candidate : candidates)
        if (std::filesystem::exists(candidate)) return candidate.string();
    return std::nullopt;
}

std::optional<std::string> core_catalogue_path(const std::string &aggregate) {
    const auto aggregate_path = std::filesystem::path(aggregate);
    const auto parent = aggregate_path.parent_path();
    const std::vector<std::filesystem::path> candidates = {
        parent / "semantic_catalogue" / "core" / "catalogue.duckdb",
        parent / "core" / "catalogue.duckdb"};
    for (const auto &candidate : candidates)
        if (std::filesystem::exists(candidate)) return candidate.string();
    return std::nullopt;
}

Json catalogue_document(const Json &entries) { return Json{{"version", 2}, {"entries", entries}}; }

}  // namespace detail

void register_all(const Json &entries, MethodRegistry &methods, OperationRegistry &operations) {
    mass_spec::register_plugin(entries, methods, operations);
    raman::register_plugin(entries, methods, operations);
    sensors::register_plugin(entries, methods, operations);
}

void load_and_register(const std::string &aggregate_path,
                       MethodRegistry &methods,
                       OperationRegistry &operations) {
    catalogue::set_runtime_path(aggregate_path);
    const auto core_path = detail::core_catalogue_path(aggregate_path);
    if (!core_path) throw std::runtime_error("core catalogue not found");
    const auto core_entries = catalogue::load(*core_path);
    if (!core_entries) throw std::runtime_error("core catalogue could not be loaded");
    auto merged = detail::catalogue_document(*core_entries);
    for (const auto &[domain, modules] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"mass_spec", {"mass_spec.base", "mass_spec.chromatograms", "mass_spec.nta"}},
             {"raman", {"raman.base"}},
             {"sensors", {"sensors.base"}}}) {
        const auto path = detail::plugin_catalogue_path(aggregate_path, domain);
        if (!path) throw std::runtime_error("plugin catalogue not found for domain " + domain);
        const auto entries = catalogue::load(*path);
        if (!entries) throw std::runtime_error("plugin catalogue could not be loaded for domain " + domain);
        merged = catalogue::import_plugin_catalogue(
            merged, detail::catalogue_document(*entries), domain, modules);
    }
    register_all(merged.at("entries"), methods, operations);
}

}  // namespace streamfind::static_plugins
