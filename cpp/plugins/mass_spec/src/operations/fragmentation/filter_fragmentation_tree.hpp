#pragma once

#include "streamfind/export.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"
#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::fragmentation::filter_fragmentation_tree
{
STREAMFIND_DOMAIN_API nlohmann::json run(
    sdk::PluginProjectAccess &access,
    const nlohmann::json &parameters);
}
