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
static constexpr double proton_mass = 1.007276466621;

static bool neutral_mass_matches(const TargetRange &target, int polarity, double mz)
{
    const double neutral = (target.mass_min + target.mass_max) / 2.0;
    const double tolerance = std::max(target.mass_max - target.mass_min, 0.0) /
                             (2.0 * static_cast<double>(target.charge));
    const auto matches = [&](double offset) {
        const double expected = (neutral + offset) / static_cast<double>(target.charge);
        return mz >= expected - tolerance && mz <= expected + tolerance;
    };
    if (polarity < 0) return matches(-proton_mass);
    if (polarity > 0) return matches(proton_mass);
    return matches(proton_mass) || matches(-proton_mass);
}

static bool neutral_mass_overlaps(const TargetRange &target, int polarity, double low_mz, double high_mz)
{
    const double neutral = (target.mass_min + target.mass_max) / 2.0;
    const double tolerance = std::max(target.mass_max - target.mass_min, 0.0) /
                             (2.0 * static_cast<double>(target.charge));
    const auto overlaps = [&](double offset) {
        const double expected = (neutral + offset) / static_cast<double>(target.charge);
        return high_mz >= expected - tolerance && low_mz <= expected + tolerance;
    };
    if (polarity < 0) return overlaps(-proton_mass);
    if (polarity > 0) return overlaps(proton_mass);
    return overlaps(proton_mass) || overlaps(-proton_mass);
}

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

static double row_real_or(const nlohmann::json &row, const char *column, double fallback);
static bool row_has_value(const nlohmann::json &row, const char *column);
static double row_real_or_any(const nlohmann::json &row, const char *primary,
                              const char *alternate, double fallback);

std::vector<TargetRange> normalize_targets(const nlohmann::json &parameters)
{
    std::vector<TargetRange> result;
    const auto sources = parameters.value("targets", nlohmann::json::array());
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        const auto &source = sources[index];
        TargetRange target;
        target.id = source.value("name", "target" + std::to_string(index));
        const auto analyses = source.contains("analysis") ? source.at("analysis") : parameters.value("analysis_names", nlohmann::json::array());
        if (analyses.is_string())
        {
            const auto value = analyses.get<std::string>();
            if (!value.empty()) target.analyses.push_back(value);
        }
        else if (analyses.is_number_integer())
            target.analysis_indices.push_back(analyses.get<int>());
        else if (analyses.is_array())
            for (const auto &analysis : analyses)
            {
                if (analysis.is_string() && !analysis.get<std::string>().empty()) target.analyses.push_back(analysis.get<std::string>());
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
        const double mass = row_real_or(source, "mass", 0.0), mz = row_real_or(source, "mz", 0.0), rt = row_real_or(source, "rt", 0.0);
        const bool has_mass_min = row_has_value(source, "mass_min");
        const bool has_mass_max = row_has_value(source, "mass_max");
        target.has_mass = (source.contains("mass") && !source.at("mass").is_null()) || has_mass_min || has_mass_max;
        target.mass_min = row_real_or(source, "mass_min", mass == 0.0 ? -std::numeric_limits<double>::infinity() : mass - mass * ppm / 1e6);
        target.mass_max = row_real_or(source, "mass_max", mass == 0.0 ? std::numeric_limits<double>::infinity() : mass + mass * ppm / 1e6);
        target.mz_min = row_real_or_any(source, "mz_min", "mzmin", mz == 0.0 ? -std::numeric_limits<double>::infinity() : mz - mz * ppm / 1e6);
        target.mz_max = row_real_or_any(source, "mz_max", "mzmax", mz == 0.0 ? std::numeric_limits<double>::infinity() : mz + mz * ppm / 1e6);
        target.has_mz = mz != 0.0 ||
            row_has_value(source, "mz_min") || row_has_value(source, "mz_max") ||
            row_has_value(source, "mzmin") || row_has_value(source, "mzmax");
        if (target.mass_min > target.mass_max) std::swap(target.mass_min, target.mass_max);
        if (target.mz_min > target.mz_max) std::swap(target.mz_min, target.mz_max);
        const double tolerance = parameters.value("rt_tolerance", 60.0);
        target.rt_min = row_real_or_any(source, "rt_min", "rtmin", rt == 0.0 ? -std::numeric_limits<double>::infinity() : rt - tolerance);
        target.rt_max = row_real_or_any(source, "rt_max", "rtmax", rt == 0.0 ? std::numeric_limits<double>::infinity() : rt + tolerance);
        target.charge = source.contains("charge") && !source.at("charge").is_null()
            ? std::max(1, static_cast<int>(row_real_or(source, "charge", 1.0)))
            : std::max(1, parameters.value("charge", 1));
        if (!source.contains("polarity") && parameters.value("polarity", 0) != 0)
            target.polarities.push_back(parameters.value("polarity", 0));
        if (!target.has_mass && !std::isfinite(target.mz_min) && !std::isfinite(target.mz_max))
            continue;
        result.push_back(std::move(target));
    }
    return result;
}

bool target_matches(const TargetRange &target, const std::string &analysis, int polarity, int level, float rt, float mz)
{
    if (!((target.analyses.empty() || std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end()) &&
           (target.polarities.empty() || std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
            std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end()) &&
           (target.levels.empty() || std::find(target.levels.begin(), target.levels.end(), level) != target.levels.end()) &&
           rt >= target.rt_min && rt <= target.rt_max)) return false;
    if (target.has_mass && !target.has_mz)
    {
        // Neutral-mass targets are converted to the ion m/z implied by the
        // observed polarity.  This prevents a mass-only target from becoming
        // an accidental all-m/z match.
        return neutral_mass_matches(target, polarity, mz);
    }
    return mz >= target.mz_min && mz <= target.mz_max;
}

bool target_may_match_spectrum(const TargetRange &target, const std::string &analysis, int polarity,
                               int level, float rt, float low_mz, float high_mz)
{
    if (!((target.analyses.empty() || std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end()) &&
          (target.polarities.empty() || std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
           std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end()) &&
          (target.levels.empty() || std::find(target.levels.begin(), target.levels.end(), level) != target.levels.end()) &&
          rt >= target.rt_min && rt <= target.rt_max)) return false;
    if (target.has_mass && !target.has_mz)
    {
        return neutral_mass_overlaps(target, polarity, low_mz, high_mz);
    }
    return high_mz >= target.mz_min && low_mz <= target.mz_max;
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

static double row_real_or(const nlohmann::json &row, const char *column, double fallback)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return fallback;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) return it->get<std::string>().empty() ? fallback : std::stod(it->get<std::string>());
    return fallback;
}

static bool row_has_value(const nlohmann::json &row, const char *column)
{
    const auto it = row.find(column);
    return it != row.end() && !it->is_null() &&
           !(it->is_string() && it->get<std::string>().empty());
}

static double row_real_or_any(const nlohmann::json &row, const char *primary,
                              const char *alternate, double fallback)
{
    if (row_has_value(row, primary)) return row_real_or(row, primary, fallback);
    if (row_has_value(row, alternate)) return row_real_or(row, alternate, fallback);
    return fallback;
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
