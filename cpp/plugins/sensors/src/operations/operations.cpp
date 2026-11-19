#include "operations.hpp"

#include "streamfind/sdk/plugin_project_access.hpp"

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations {

namespace detail {

nlohmann::json explore_opcua_server(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json inspect_opcua_node(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json collect_opcua_data(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json capture_opcua_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json normalize_sensor_table(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json validate_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json filter_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json resample_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json align_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json project_aligned_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);

}  // namespace detail

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"sensors.explore_opcua_server", sdk::CapabilityKind::Operation,
         &detail::explore_opcua_server},
        {"sensors.inspect_opcua_node", sdk::CapabilityKind::Operation,
         &detail::inspect_opcua_node},
        {"sensors.collect_opcua_data", sdk::CapabilityKind::Operation,
         &detail::collect_opcua_data},
        {"sensors.capture_opcua_timeseries", sdk::CapabilityKind::Operation,
         &detail::capture_opcua_timeseries},
        {"sensors.normalize_sensor_table", sdk::CapabilityKind::Operation,
         &detail::normalize_sensor_table},
        {"sensors.validate_timeseries", sdk::CapabilityKind::Operation,
         &detail::validate_timeseries},
        {"sensors.filter_timeseries", sdk::CapabilityKind::Operation,
         &detail::filter_timeseries},
        {"sensors.resample_timeseries", sdk::CapabilityKind::Operation,
         &detail::resample_timeseries},
        {"sensors.align_timeseries", sdk::CapabilityKind::Operation,
         &detail::align_timeseries},
        {"sensors.project_aligned_timeseries", sdk::CapabilityKind::Operation,
         &detail::project_aligned_timeseries},
    };
    return registry;
}

}  // namespace streamfind::sensors::operations
