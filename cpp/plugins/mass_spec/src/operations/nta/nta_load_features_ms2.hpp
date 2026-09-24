#pragma once
#include "streamfind/sdk/plugin_project_access.hpp"
#include <nlohmann/json.hpp>
namespace streamfind::mass_spec::nta::load_features_ms2 { STREAMFIND_DOMAIN_API nlohmann::json run(sdk::PluginProjectAccess &, const nlohmann::json &); }
