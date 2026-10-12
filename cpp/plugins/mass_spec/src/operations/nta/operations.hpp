#pragma once

#include "streamfind/sdk/plugin_project_access.hpp"
#include <nlohmann/json.hpp>

namespace streamfind::mass_spec::nta
{
  using Json = nlohmann::json;
  STREAMFIND_DOMAIN_API Json get_features(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_suspects(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_internal_standards(sdk::PluginProjectAccess &, const Json &);
  STREAMFIND_DOMAIN_API Json get_transformation_products(sdk::PluginProjectAccess &, const Json &);

  namespace load_features_ms1 { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace load_features_ms2 { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace subtract_blank { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace filter_features { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace filter_features_ms2 { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace group_features { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace fill_features { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace create_components { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace annotate_components { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace suspect_screening { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace read_csv_suspect_targets { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace find_internal_standards { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace enrich_internal_standards { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace filter_suspects { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace filter_internal_standards { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace correct_matrix_suppression { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace assign_transformation_products { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace metfrag_screening { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
  namespace find_internal_standards_metfrag { STREAMFIND_DOMAIN_API Json run(sdk::PluginProjectAccess &, const Json &); }
}
