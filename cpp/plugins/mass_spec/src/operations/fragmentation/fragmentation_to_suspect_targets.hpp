#pragma once

#include "streamfind/sdk/plugin_host_access.hpp"
#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets
{
nlohmann::json run(sdk::PluginProjectAccess &access, const nlohmann::json &parameters);
}
