#pragma once

#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/export.hpp"

namespace streamfind::mass_spec {

struct TargetRange {
    std::string id;
    std::vector<std::string> analyses;
    std::vector<int> polarities;
    std::vector<int> levels;
    bool has_mass = false;
    double mass_min = 0.0;
    double mass_max = 0.0;
    double mz_min = 0.0;
    double mz_max = 0.0;
    double rt_min = 0.0;
    double rt_max = 0.0;
};

STREAMFIND_DOMAIN_API void register_methods(MethodRegistry &registry);

namespace detail {
std::string sql(const std::string &);
std::vector<TargetRange> normalize_targets_for_operation(const Json &);
std::vector<std::string> target_sql_conditions(const TargetRange &, const std::vector<const char *> &, const std::vector<const char *> &, const std::vector<const char *> &, bool);
bool target_matches(const TargetRange &, const std::string &, int, int, float, float);
Json summarize_eic(const Json &);
Json merge_ms_rows(const Json &, double, double);
}
namespace operations {
STREAMFIND_DOMAIN_API Json add_analyses(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_analyses_info(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json remove_analyses(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_features(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_internal_standards(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_suspects(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_transformation_products(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_analysis_names(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_replicate_names(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_blank_names(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_concentrations(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json set_replicate_names(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json set_blank_names(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json set_concentrations(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_spectra_headers(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_chromatograms_headers(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_spectra_tic(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_raw_spectra(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_raw_spectra_eic(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_raw_spectra_ms1(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_raw_spectra_ms2(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_chromatograms(sdk::PluginProjectAccess &, const Json &);
STREAMFIND_DOMAIN_API Json get_raw_chromatograms(sdk::PluginProjectAccess &, const Json &);
}

}