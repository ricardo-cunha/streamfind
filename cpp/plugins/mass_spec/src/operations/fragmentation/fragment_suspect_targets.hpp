#pragma once

#include "streamfind/export.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"
#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::fragmentation::fragment_suspect_targets
{
STREAMFIND_DOMAIN_API nlohmann::json run(
    sdk::PluginProjectAccess &access,
    const nlohmann::json &parameters);
}
