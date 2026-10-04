#include "utils/base.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace streamfind::mass_spec::base::utils
{

nlohmann::json summarize_eic(const nlohmann::json &rows)
{
    std::map<std::tuple<std::string, int, std::string, double>, nlohmann::json> grouped;
    for (const auto &row : rows)
    {
        const auto key = std::make_tuple(row.value("analysis", std::string{}), row.value("polarity", 0),
                                         row.value("name", std::string{}), row.value("rt", 0.0));
        auto &output = grouped[key];
        if (output.empty()) output = row;
        output["intensity"] = std::max(output.value("intensity", 0.0), row.value("intensity", 0.0));
    }
    nlohmann::json result = nlohmann::json::array();
    for (auto &[key, row] : grouped) result.push_back(std::move(row));
    return result;
}

nlohmann::json merge_ms_rows(const nlohmann::json &rows, double mz_cluster, double presence)
{
    std::map<std::tuple<std::string, std::string, int, int, double, double>, std::vector<nlohmann::json>> groups;
    for (const auto &row : rows)
    groups[{row.value("analysis", std::string{}), row.value("name", std::string{}), row.value("polarity", 0),
            row.value("level", 0), row.value("rt", 0.0), row.value("precursor_mz", 0.0)}]
        .push_back(row);
    nlohmann::json result = nlohmann::json::array();
    for (auto &[key, values] : groups)
    {
        std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) { return left.value("mz", 0.0) < right.value("mz", 0.0); });
        std::set<double> all_rt;
        for (const auto &row : values) all_rt.insert(row.value("rt", 0.0));
        for (std::size_t start = 0; start < values.size();)
        {
            std::size_t end = start + 1;
            while (end < values.size() && values[end].value("mz", 0.0) - values[end - 1].value("mz", 0.0) <= mz_cluster) ++end;
            std::set<double> cluster_rt;
            for (std::size_t i = start; i < end; ++i) cluster_rt.insert(values[i].value("rt", 0.0));
            if (presence > 0.0 && static_cast<double>(cluster_rt.size()) < presence * static_cast<double>(all_rt.size())) { start = end; continue; }
            nlohmann::json row = values[start];
            double intensity = 0.0, weighted_mz = 0.0, rt = 0.0;
            for (std::size_t i = start; i < end; ++i)
            {
                const double value = values[i].value("intensity", 0.0);
                intensity = std::max(intensity, value);
                weighted_mz += values[i].value("mz", 0.0) * value;
                rt += values[i].value("rt", 0.0);
            }
            row["mz"] = weighted_mz == 0.0 ? row.value("mz", 0.0) : weighted_mz / std::max(intensity, 1e-12);
            row["intensity"] = intensity;
            row["rt"] = rt / static_cast<double>(end - start);
            result.push_back(std::move(row));
            start = end;
        }
    }
    return result;
}

std::vector<TargetRange> normalize_targets(const nlohmann::json &parameters)
{
    std::vector<TargetRange> result;
    const auto sources = parameters.value("targets", nlohmann::json::array({nlohmann::json::object()}));
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        const auto &source = sources[index];
        TargetRange target;
        target.id = source.value("name", "target" + std::to_string(index));
        const auto analyses = source.contains("analysis") ? source.at("analysis") : parameters.value("analysis_names", nlohmann::json::array());
        if (analyses.is_string())
            target.analyses.push_back(analyses.get<std::string>());
        else if (analyses.is_number_integer())
            target.analysis_indices.push_back(analyses.get<int>());
        else if (analyses.is_array())
            for (const auto &analysis : analyses)
            {
                if (analysis.is_string()) target.analyses.push_back(analysis.get<std::string>());
                else if (analysis.is_number_integer()) target.analysis_indices.push_back(analysis.get<int>());
            }
        // Polarity is an optional per-target constraint.  Do not inherit a
        // default from the operation: an omitted polarity must mean that
        // both positive and negative feature rows are eligible.
        if (source.contains("polarity") && !source.at("polarity").is_null())
        {
            const auto &polarity = source.at("polarity");
            if (polarity.is_array())
                for (const auto &value : polarity)
                    if (value.is_number_integer()) target.polarities.push_back(value.get<int>());
                    else if (value.is_string()) target.polarities.push_back(std::stoi(value.get<std::string>()));
            else if (polarity.is_number_integer())
                target.polarities.push_back(polarity.get<int>());
            else if (polarity.is_string())
                target.polarities.push_back(std::stoi(polarity.get<std::string>()));
        }
        const auto levels = source.contains("level") ? source.at("level") : parameters.value("levels", nlohmann::json::array());
        if (levels.is_number_integer())
            target.levels.push_back(levels.get<int>());
        else if (levels.is_array())
            target.levels = levels.get<std::vector<int>>();
        const double ppm = parameters.value("ppm", 20.0);
        const double mass = source.value("mass", 0.0), mz = source.value("mz", 0.0), rt = source.value("rt", 0.0);
        target.has_mass = source.contains("mass") || source.contains("mass_min") || source.contains("mass_max");
        target.mass_min = source.value("mass_min", mass == 0.0 ? -std::numeric_limits<double>::infinity() : mass - mass * ppm / 1e6);
        target.mass_max = source.value("mass_max", mass == 0.0 ? std::numeric_limits<double>::infinity() : mass + mass * ppm / 1e6);
        target.mz_min = source.value("mz_min", mz == 0.0 ? -std::numeric_limits<double>::infinity() : mz - mz * ppm / 1e6);
        target.mz_max = source.value("mz_max", mz == 0.0 ? std::numeric_limits<double>::infinity() : mz + mz * ppm / 1e6);
        const double tolerance = parameters.value("rt_tolerance", 60.0);
        target.rt_min = source.value("rt_min", rt == 0.0 ? -std::numeric_limits<double>::infinity() : rt - tolerance);
        target.rt_max = source.value("rt_max", rt == 0.0 ? std::numeric_limits<double>::infinity() : rt + tolerance);
        result.push_back(std::move(target));
    }
    return result;
}

bool target_matches(const TargetRange &target, const std::string &analysis, int polarity, int level, float rt, float mz)
{
    return (target.analyses.empty() || std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end()) &&
           (target.polarities.empty() || std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
            std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end()) &&
           (target.levels.empty() || std::find(target.levels.begin(), target.levels.end(), level) != target.levels.end()) &&
           mz >= target.mz_min && mz <= target.mz_max && rt >= target.rt_min && rt <= target.rt_max;
}

static std::string row_text(const nlohmann::json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

static double row_real(const nlohmann::json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return 0.0;
    return it->is_number() ? it->get<double>() : std::stod(it->get<std::string>());
}

static int row_integer(const nlohmann::json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return 0;
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_number()) return it->get<int>();
    const auto value = it->get<std::string>();
    if (value == "true" || value == "TRUE") return 1;
    if (value == "false" || value == "FALSE") return 0;
    return value.empty() ? 0 : std::stoi(value);
}

nlohmann::json filter_target_rows(const nlohmann::json &rows, const nlohmann::json &parameters,
                                  const char *mass_column, const char *mz_column, const char *rt_column, const char *polarity_column)
{
    const auto targets = normalize_targets(parameters);
    const auto requested_targets = parameters.value("targets", nlohmann::json::array());
    const bool has_requested_targets = requested_targets.is_array() && !requested_targets.empty();
    nlohmann::json result = nlohmann::json::array();
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
    {
        const auto &row = rows[row_index];
        for (const auto &target : targets)
        {
            const auto analysis = row_text(row, "analysis");
            const int polarity = polarity_column == nullptr ? 0 : row_integer(row, polarity_column);
            const double mass = mass_column == nullptr ? 0.0 : row_real(row, mass_column);
            const double mz = mz_column == nullptr ? 0.0 : row_real(row, mz_column);
            const double rt = rt_column == nullptr ? 0.0 : row_real(row, rt_column);
            const bool analysis_ok = (target.analyses.empty() && target.analysis_indices.empty()) ||
                                     std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end() ||
                                     std::find(target.analysis_indices.begin(), target.analysis_indices.end(), static_cast<int>(row_index)) != target.analysis_indices.end();
            const bool polarity_ok = target.polarities.empty() || std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
                                     std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end();
            if (analysis_ok && polarity_ok && (mass_column == nullptr || !target.has_mass || (mass >= target.mass_min && mass <= target.mass_max)) &&
                (mz_column == nullptr || (mz >= target.mz_min && mz <= target.mz_max)) && (rt_column == nullptr || (rt >= target.rt_min && rt <= target.rt_max)))
            {
                auto matched = row;
                if (has_requested_targets) matched["name"] = target.id;
                result.push_back(std::move(matched));
                break;
            }
        }
    }
    return result;
}

const std::string &input_table(const nlohmann::json &parameters, const char *port)
{
    return parameters.at("_inputs").at(port).at("physical_table").get_ref<const std::string &>();
}

nlohmann::json input_rows(sdk::PluginProjectAccess &access, const nlohmann::json &parameters, const char *port,
                          const std::vector<std::string> &columns, const char *order)
{
    return access.read(input_table(parameters, port), columns, order == nullptr ? std::string{} : std::string(order));
}
}
