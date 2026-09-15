#include "operations.hpp"

#include "streamfind/sdk/plugin_host_access.hpp"

#include <nlohmann/json.hpp>

namespace streamfind::raman::operations {

namespace detail {

nlohmann::json operation(sdk::PluginProjectAccess &, const nlohmann::json &parameters) {
    return nlohmann::json{{"status", "ok"}, {"plugin", "raman"},
                          {"operation", parameters.at("capability_id")}};
}

}  // namespace detail

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"raman.add_analyses", sdk::CapabilityKind::Operation, &detail::operation},
        {"raman.remove_analyses", sdk::CapabilityKind::Operation, &detail::operation},
    };
    return registry;
}

}  // namespace streamfind::raman::operations
