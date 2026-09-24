#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamfind/sdk/plugin_project_access.hpp"

namespace streamfind::mass_spec::base::utils
{
struct TargetRange
{
    std::string id;
    std::vector<std::string> analyses;
    std::vector<int> analysis_indices;
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

nlohmann::json summarize_eic(const nlohmann::json &);
nlohmann::json merge_ms_rows(const nlohmann::json &, double, double);
std::vector<TargetRange> normalize_targets(const nlohmann::json &);
bool target_matches(const TargetRange &, const std::string &, int, int, float, float);
nlohmann::json filter_target_rows(const nlohmann::json &, const nlohmann::json &, const char *, const char *, const char *, const char *);
const std::string &input_table(const nlohmann::json &, const char *);
nlohmann::json input_rows(sdk::PluginProjectAccess &, const nlohmann::json &, const char *, const std::vector<std::string> &, const char * = nullptr);
}
