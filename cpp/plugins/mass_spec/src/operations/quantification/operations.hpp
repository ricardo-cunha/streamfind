#pragma once

#include "streamfind/export.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::quantification
{
STREAMFIND_DOMAIN_API Json set_quantification_method(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json set_quantification_analyses(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json extract_quantification_responses(sdk::PluginProjectAccess &, const Json &);
}
