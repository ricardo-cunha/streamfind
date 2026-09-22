#pragma once

#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::nta
{
  STREAMFIND_DOMAIN_API Json get_features(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_suspects(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_internal_standards(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_transformation_products(sdk::PluginProjectAccess &, const Json &);
}
