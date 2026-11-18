#include "operations.hpp"

#include "streamfind/sdk/plugin_project_access.hpp"

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations {

namespace detail {

nlohmann::json explore_opcua_server(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json inspect_opcua_node(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json collect_opcua_data(sdk::PluginProjectAccess &, const nlohmann::json &);

}  // namespace detail

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"sensors.explore_opcua_server", sdk::CapabilityKind::Operation,
         &detail::explore_opcua_server},
        {"sensors.inspect_opcua_node", sdk::CapabilityKind::Operation,
         &detail::inspect_opcua_node},
        {"sensors.collect_opcua_data", sdk::CapabilityKind::Operation,
         &detail::collect_opcua_data},
    };
    return registry;
}

}  // namespace streamfind::sensors::operations
