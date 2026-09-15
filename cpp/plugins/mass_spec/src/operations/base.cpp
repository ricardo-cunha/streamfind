#include "operations/operations.hpp"
#include "utils/openbabel_adapter.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <limits>
#include <set>
#include <vector>
#include <map>
#include "streamfind/sdk/plugin_host_access.hpp"
#include <string>
#include "readers/reader.hpp"
#include <optional>

namespace streamfind::mass_spec::detail
{
  std::string sql(const std::string &value)
  {
    std::string out = "'";
    for (const char c : value)
      out += c == '\'' ? "''" : std::string(1, c);
    return out + "'";
  }
  std::string lower(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return value;
  }
  std::vector<std::string> names(const Json &parameters, const char *key)
  {
    std::vector<std::string> out;
    for (const auto &value : parameters.value(key, Json::array()))
      out.push_back(value.get<std::string>());
    return out;
  }
  std::vector<int> levels(const Json &parameters)
  {
    std::vector<int> out;
    for (const auto &value : parameters.value("levels", Json::array()))
      out.push_back(value.get<int>());
    return out;
  }
  std::vector<int> indices(const Json &parameters)
  {
    std::vector<int> out;
    for (const auto &value : parameters.value("indices", Json::array()))
      out.push_back(value.get<int>());
    return out;
  }
  std::vector<std::string> analysis_names(const Json &parameters)
  {
    std::vector<std::string> result;
    for (const auto &value : parameters.value("analysis_names", Json::array()))
      result.push_back(value.get<std::string>());
    return result;
  }
  bool selected(const std::vector<std::string> &wanted, const std::string &analysis)
  {
    return wanted.empty() || std::find(wanted.begin(), wanted.end(), analysis) != wanted.end();
  }
  int analysis_index(const Json &row)
  {
    const auto value = row.value("analysis_index", "0");
    return value.empty() ? 0 : std::stoi(value);
  }
  // Project::query_json stringifies every column (duckdb_value_varchar), so a
  // nullable integer column must be parsed from its text form. Returns the
  // default when the column is absent or null.
  int integer_column(const Json &row, const char *key, int fallback = 0)
  {
    auto it = row.find(key);
    if (it == row.end() || it->is_null())
      return fallback;
    if (it->is_number())
      return it->get<int>();
    const auto &value = it->get_ref<const std::string &>();
    return value.empty() ? fallback : std::stoi(value);
  }
  using streamfind::mass_spec::TargetRange;
  std::vector<TargetRange> normalize_targets_for_operation(const Json &p)
  {
    constexpr double proton = 1.007276;
    const double ppm = p.value("ppm", 20.0);
    const double rt_tolerance = p.value("rt_tolerance", 60.0);
    const int charge = std::max(1, std::abs(p.value("charge", 1)));
    const auto sources = p.contains("targets") ? p.at("targets") : Json::array({Json::object()});
    std::vector<TargetRange> out;
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
      const auto &source = sources[i];
      TargetRange target;
      target.id = source.value("id", "target" + std::to_string(i));
      const auto analysis = source.value("analyses", Json());
      if (!analysis.is_null())
        target.analyses = analysis.is_array() ? analysis.get<std::vector<std::string>>() : std::vector<std::string>{analysis.get<std::string>()};
      else
        target.analyses = names(p, "analysis_names");
      const auto polarity = source.contains("polarity") ? source.at("polarity") : p.value("polarity", Json());
      target.polarities = polarity.is_null() ? std::vector<int>{0} : polarity.is_array() ? polarity.get<std::vector<int>>()
                                                                                         : std::vector<int>{polarity.get<int>()};
      target.levels = source.value("levels", p.value("levels", Json::array())).get<std::vector<int>>();
      double chemical_mass = 0.0;
      const bool chemical = !source.contains("mass") && !source.contains("mass_min") && !source.contains("mass_max") &&
                            !source.contains("mz") && !source.contains("mz_min") && !source.contains("mz_max") &&
                            (source.contains("SMILES") || source.contains("InChI"));
      if (chemical)
      {
        const auto normalized = sf::obabel::normalize_structure(source.value("SMILES", ""), source.value("InChI", ""));
        if (normalized.ok && normalized.exact_mass > 0.0)
          chemical_mass = normalized.exact_mass;
      }
      const bool unspecified_polarity = target.polarities.size() == 1 && target.polarities[0] == 0;
      std::vector<int> signs;
      if (chemical_mass > 0.0 && unspecified_polarity)
        signs = {-1, 1}; // query both [M-H]- and [M+H]+ so the analysis polarity selects the hit
      else
        signs = {target.polarities.front() < 0 ? -1 : 1};
      for (const int sign : signs)
      {
        const double mass = source.value("mass", chemical_mass);
        const bool mass_based = chemical_mass > 0.0 || mass != 0.0 || source.contains("mass_min") || source.contains("mass_max");
        const double mass_min = source.value("mass_min", mass_based ? mass : 0.0);
        const double mass_max = source.value("mass_max", mass_based ? mass : 0.0);
        double mz_min = source.value("mz_min", p.value("mz_min", 0.0));
        double mz_max = source.value("mz_max", p.value("mz_max", 0.0));
        const double exact_mz = source.value("mz", 0.0);
        if (mz_min == 0.0 && mz_max == 0.0 && exact_mz != 0.0)
          mz_min = mz_max = exact_mz;
        if (mz_min == 0.0 && mz_max == 0.0 && mass_based)
        {
          mz_min = source.value("mass_min", mass) + sign * proton / charge;
          mz_max = source.value("mass_max", mass) + sign * proton / charge;
        }
        if (mz_min != 0.0 || mz_max != 0.0)
        {
          const double mz = mz_min != 0.0 ? mz_min : mz_max;
          const double delta = mz * ppm / 1e6;
          if ((mass_based || exact_mz != 0.0) && mz_min == mz_max)
            mz_min = mz - delta, mz_max = mz + delta;
          else
          {
            if (mz_min == 0.0)
              mz_min = mz - delta;
            if (mz_max == 0.0)
              mz_max = mz + delta;
          }
        }
        const double isolation_window = p.value("isolation_window", 0.0);
        if (isolation_window > 0.0)
        {
          mz_min -= isolation_window / 2.0;
          mz_max += isolation_window / 2.0;
        }
        const double rt = source.value("rt", 0.0);
        TargetRange emitted = target;
        emitted.polarities = {sign};
        emitted.has_mass = mass_based;
        emitted.mass_min = mass_min;
        emitted.mass_max = mass_max;
        emitted.mz_min = static_cast<float>(mz_min == 0.0 ? -std::numeric_limits<float>::infinity() : mz_min);
        emitted.mz_max = static_cast<float>(mz_max == 0.0 ? std::numeric_limits<float>::infinity() : mz_max);
        emitted.rt_min = static_cast<float>(source.value("rt_min", p.value("rt_min", rt == 0.0 ? -std::numeric_limits<double>::infinity() : rt - rt_tolerance)));
        emitted.rt_max = static_cast<float>(source.value("rt_max", p.value("rt_max", rt == 0.0 ? std::numeric_limits<double>::infinity() : rt + rt_tolerance)));
        out.push_back(std::move(emitted));
      }
    }
    return out;
  }
  std::vector<std::string> target_sql_conditions(const TargetRange &target,
                                                 const std::vector<const char *> &mass_columns,
                                                 const std::vector<const char *> &mz_columns,
                                                 const std::vector<const char *> &rt_columns,
                                                 bool has_polarity)
  {
    auto number = [](double value)
    { return std::to_string(value); };
    auto add_range = [&](std::vector<std::string> &conditions, const std::vector<const char *> &columns, double minimum, double maximum)
    {
      std::vector<std::string> column_conditions;
      for (const auto *column : columns)
      {
        std::string condition;
        if (std::isfinite(minimum))
          condition = std::string(column) + " >= " + number(minimum);
        if (std::isfinite(maximum))
          condition += (condition.empty() ? std::string(column) : " AND " + std::string(column)) + " <= " + number(maximum);
        if (!condition.empty())
          column_conditions.push_back(std::move(condition));
      }
      if (column_conditions.size() == 1)
        conditions.push_back(std::move(column_conditions.front()));
      else if (!column_conditions.empty())
      {
        std::string expression = "(";
        for (std::size_t i = 0; i < column_conditions.size(); ++i)
          expression += (i ? " OR " : "") + column_conditions[i];
        conditions.push_back(expression + ")");
      }
    };
    std::vector<std::string> conditions;
    if (target.has_mass)
      add_range(conditions, mass_columns, target.mass_min, target.mass_max);
    if (std::isfinite(target.mz_min) || std::isfinite(target.mz_max))
      add_range(conditions, mz_columns, target.mz_min, target.mz_max);
    if (std::isfinite(target.rt_min) || std::isfinite(target.rt_max))
      add_range(conditions, rt_columns, target.rt_min, target.rt_max);
    if (has_polarity && std::find(target.polarities.begin(), target.polarities.end(), 0) == target.polarities.end())
    {
      std::vector<std::string> polarity_conditions;
      for (const auto polarity : target.polarities)
        polarity_conditions.push_back("polarity = " + std::to_string(polarity));
      if (!polarity_conditions.empty())
      {
        std::string expression = "(";
        for (std::size_t i = 0; i < polarity_conditions.size(); ++i)
          expression += (i ? " OR " : "") + polarity_conditions[i];
        conditions.push_back(expression + ")");
      }
    }
    return conditions;
  }
  bool target_matches(const TargetRange &t, const std::string &analysis, int polarity, int level, float rt, float mz) { return (t.analyses.empty() || std::find(t.analyses.begin(), t.analyses.end(), analysis) != t.analyses.end()) && (std::find(t.polarities.begin(), t.polarities.end(), 0) != t.polarities.end() || std::find(t.polarities.begin(), t.polarities.end(), polarity) != t.polarities.end()) && (t.levels.empty() || std::find(t.levels.begin(), t.levels.end(), level) != t.levels.end()) && mz >= t.mz_min && mz <= t.mz_max && rt >= t.rt_min && rt <= t.rt_max; }
  Json summarize_eic(const Json &rows)
  {
    struct Value
    {
      Json row;
      double mz = 0.0;
      double mobility = 0.0;
      std::size_t count = 0;
    };
    std::map<std::tuple<std::string, int, std::string, std::string, float>, Value> grouped;
    for (const auto &row : rows)
    {
      const auto key = std::make_tuple(row.at("analysis").get<std::string>(), row.at("polarity").get<int>(), row.value("target_id", ""), row.at("id").get<std::string>(), row.at("rt").get<float>());
      auto &value = grouped[key];
      if (value.count == 0)
        value.row = row;
      value.mz += row.at("mz").get<double>();
      value.mobility += row.at("mobility").get<double>();
      ++value.count;
      value.row["intensity"] = std::max(value.row.value("intensity", 0.0), row.value("intensity", 0.0));
    }
    Json out = Json::array();
    for (auto &[key, value] : grouped)
    {
      if (value.count == 0)
        continue;
      value.row["level"] = 1;
      value.row["mz"] = value.mz / value.count;
      value.row["mobility"] = value.mobility / value.count;
      out.push_back(std::move(value.row));
    }
    return out;
  }
  Json merge_ms_rows(const Json &rows, double mz_clust, double presence)
  {
    using Key = std::tuple<std::string, std::string, int>;
    std::map<Key, std::vector<Json>> groups;
    for (const auto &row : rows)
      groups[{row.value("analysis", ""), row.value("id", ""), row.value("polarity", 0)}].push_back(row);
    Json out = Json::array();
    const double tolerance = std::max(0.0, mz_clust);
    const double threshold = std::clamp(presence, 0.0, 1.0);
    for (auto &[key, values] : groups)
    {
      std::sort(values.begin(), values.end(), [](const Json &left, const Json &right)
                { return left.value("mz", 0.0) < right.value("mz", 0.0); });
      std::set<double> all_rt;
      for (const auto &row : values)
        all_rt.insert(row.value("rt", 0.0));
      std::size_t start = 0;
      while (start < values.size())
      {
        std::size_t end = start + 1;
        while (end < values.size() && values[end].value("mz", 0.0) - values[end - 1].value("mz", 0.0) <= tolerance)
          ++end;
        std::set<double> cluster_rt;
        for (std::size_t i = start; i < end; ++i)
          cluster_rt.insert(values[i].value("rt", 0.0));
        if (threshold > 0.0 && !all_rt.empty() && static_cast<double>(cluster_rt.size()) < threshold * static_cast<double>(all_rt.size()))
        {
          start = end;
          continue;
        }
        double intensity_sum = 0.0, weighted_mz = 0.0, rt_sum = 0.0, mobility_sum = 0.0, max_intensity = 0.0;
        Json row = values[start];
        for (std::size_t i = start; i < end; ++i)
        {
          const double intensity = values[i].value("intensity", 0.0);
          intensity_sum += intensity;
          weighted_mz += values[i].value("mz", 0.0) * intensity;
          rt_sum += values[i].value("rt", 0.0);
          mobility_sum += values[i].value("mobility", 0.0);
          max_intensity = std::max(max_intensity, intensity);
        }
        if (intensity_sum > 0.0)
        {
          row["level"] = row.value("level", 1);
          row["mz"] = weighted_mz / intensity_sum;
          row["intensity"] = max_intensity;
          row["rt"] = rt_sum / static_cast<double>(end - start);
          row["mobility"] = mobility_sum / static_cast<double>(end - start);
          out.push_back(std::move(row));
        }
        start = end;
      }
    }
    std::sort(out.begin(), out.end(), [](const Json &left, const Json &right)
              { return std::tie(left["analysis"], left["id"], left["mz"]) < std::tie(right["analysis"], right["id"], right["mz"]); });
    return out;
  }
}

namespace streamfind::mass_spec::operations
{
  Json analysis_values(sdk::PluginProjectAccess &access, const char *column, bool numeric)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    const auto rows = access.read("MASS_SPEC_ANALYSES", {column}, "analysis");
    Json result = Json::array();
    for (const auto &row : rows)
    {
      const auto value = row.value(column, "");
      result.push_back(numeric && !value.empty() ? Json(std::stod(value)) : Json(value));
    }
    return result;
  }

  Json update_values(sdk::PluginProjectAccess &access, const Json &parameters,
                     const char *key, const char *column, bool numeric)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    const auto names = access.read("MASS_SPEC_ANALYSES", {"analysis"}, "analysis");
    const auto values = parameters.at(key);
    if (values.size() != names.size())
      throw std::invalid_argument(std::string(key) + " length must match analyses");
    std::vector<std::vector<std::optional<std::string>>> rows;
    rows.reserve(names.size());
    for (std::size_t index = 0; index < names.size(); ++index)
    {
      const auto analysis = names[index].at("analysis").get<std::string>();
      const auto value = numeric ? std::to_string(values[index].get<double>())
                                 : values[index].get<std::string>();
      rows.push_back({analysis, value});
    }
    if (!rows.empty())
      access.update_composite("MASS_SPEC_ANALYSES", {"analysis"}, {column}, rows);
    return Json{{"updated", rows.size()}};
  }

  Json get_analysis_names(sdk::PluginProjectAccess &access, const Json &)
  {
    return analysis_values(access, "analysis", false);
  }
  Json get_replicate_names(sdk::PluginProjectAccess &access, const Json &)
  {
    return analysis_values(access, "replicate", false);
  }
  Json get_blank_names(sdk::PluginProjectAccess &access, const Json &)
  {
    return analysis_values(access, "blank", false);
  }
  Json get_concentrations(sdk::PluginProjectAccess &access, const Json &)
  {
    return analysis_values(access, "concentration", true);
  }
  Json set_replicate_names(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    return update_values(access, parameters, "replicate_names", "replicate", false);
  }
  Json set_blank_names(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    return update_values(access, parameters, "blank_names", "blank", false);
  }
  Json set_concentrations(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    return update_values(access, parameters, "concentrations", "concentration", true);
  }

  Json get_spectra_headers(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_SPECTRA_HEADERS");
    Json output = Json::array();
    const auto wanted = detail::analysis_names(parameters);
    for (const auto &row : access.query("SELECT analysis, file_path, analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
    {
      const auto analysis = row.at("analysis").get<std::string>();
      if (!detail::selected(wanted, analysis))
        continue;
      ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
      file.select_analysis(detail::analysis_index(row));
      const auto headers = file.get_spectra_headers();
      for (std::size_t index = 0; index < headers.index.size(); ++index)
        output.push_back({{"analysis", row.at("analysis")}, {"index", headers.index[index]}, {"scan", headers.scan[index]}, {"array_length", headers.array_length[index]}, {"level", headers.level[index]}, {"mode", headers.mode[index]}, {"polarity", headers.polarity[index]}, {"configuration", headers.configuration[index]}, {"lowmz", headers.lowmz[index]}, {"highmz", headers.highmz[index]}, {"bpmz", headers.bpmz[index]}, {"bpint", headers.bpint[index]}, {"tic", headers.tic[index]}, {"rt", headers.rt[index]}, {"mobility", headers.mobility[index]}, {"window_mz", headers.window_mz[index]}, {"window_mzlow", headers.window_mzlow[index]}, {"window_mzhigh", headers.window_mzhigh[index]}, {"precursor_mz", headers.precursor_mz[index] == headers.precursor_mz[index] ? headers.precursor_mz[index] : 0.0f}, {"precursor_intensity", headers.precursor_intensity[index]}, {"precursor_charge", headers.precursor_charge[index]}, {"activation_ce", headers.activation_ce[index] == headers.activation_ce[index] ? headers.activation_ce[index] : 0.0f}});
    }
    return output;
  }

  Json get_chromatograms_headers(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_CHROMATOGRAMS_HEADERS");
    Json output = Json::array();
    const auto wanted = detail::analysis_names(parameters);
    for (const auto &row : access.query("SELECT analysis, file_path, analysis_index FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
    {
      const auto analysis = row.at("analysis").get<std::string>();
      if (!detail::selected(wanted, analysis))
        continue;
      ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
      file.select_analysis(detail::analysis_index(row));
      const auto headers = file.get_chromatograms_headers();
      for (std::size_t index = 0; index < headers.index.size(); ++index)
        output.push_back({{"analysis", row.at("analysis")}, {"index", headers.index[index]}, {"chromatogram_id", headers.chromatogram_id[index]}, {"array_length", headers.array_length[index]}, {"polarity", headers.polarity[index]}, {"precursor_mz", headers.precursor_mz[index] == headers.precursor_mz[index] ? headers.precursor_mz[index] : 0.0f}, {"activation_ce", headers.activation_ce[index] == headers.activation_ce[index] ? headers.activation_ce[index] : 0.0f}, {"product_mz", headers.product_mz[index] == headers.product_mz[index] ? headers.product_mz[index] : 0.0f}, {"signal_type", headers.signal_type[index]}, {"chromatogram_type", headers.chromatogram_type[index]}, {"detector", headers.detector[index]}, {"channel", headers.channel[index]}, {"units", headers.units[index]}, {"wavelength_nm", headers.wavelength_nm[index] == headers.wavelength_nm[index] ? headers.wavelength_nm[index] : 0.0f}, {"interval_ms", headers.interval_ms[index]}, {"start_time", headers.start_time[index]}, {"end_time", headers.end_time[index]}, {"intensity_multiplier", headers.intensity_multiplier[index]}});
    }
    return output;
  }

  Json get_spectra_tic(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    const auto headers = get_spectra_headers(access, parameters);
    std::map<std::string, std::string> replicates;
    for (const auto &row : access.query("SELECT analysis, replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
      replicates[row.at("analysis").get<std::string>()] = row.value("replicate", "");
    std::vector<int> levels;
    for (const auto &value : parameters.value("levels", Json::array()))
      levels.push_back(value.get<int>());
    Json output = Json::array();
    for (const auto &header : headers)
    {
      const auto level = header.at("level").get<int>();
      const auto rt = header.at("rt").get<float>();
      if ((!levels.empty() && std::find(levels.begin(), levels.end(), level) == levels.end()) ||
          (parameters.contains("rt_min") && rt < parameters.at("rt_min").get<float>()) ||
          (parameters.contains("rt_max") && rt > parameters.at("rt_max").get<float>()))
        continue;
      const auto analysis = header.at("analysis").get<std::string>();
      output.push_back({{"analysis", header.at("analysis")}, {"replicate", replicates[analysis]}, {"polarity", header.at("polarity")}, {"level", header.at("level")}, {"rt", header.at("rt")}, {"mobility", header.at("mobility")}, {"tic", header.at("tic")}, {"bpmz", header.at("bpmz")}, {"bpint", header.at("bpint")}});
    }
    return output;
  }

  Json get_raw_spectra(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    Json output = Json::array();
    const auto targets = streamfind::mass_spec::detail::normalize_targets_for_operation(parameters);
    std::vector<int> indices;
    for (const auto &value : parameters.value("indices", Json::array()))
      indices.push_back(value.get<int>());
    const bool indexed = !indices.empty();
    for (const auto &row : access.query("SELECT analysis, file_path, analysis_index, replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
    {
      const auto analysis = row.at("analysis").get<std::string>();
      if (!targets.empty() && std::all_of(targets.begin(), targets.end(), [](const auto &target)
                                          { return !target.analyses.empty(); }) &&
          std::none_of(targets.begin(), targets.end(), [&](const auto &target)
                       { return std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end(); }))
        continue;
      ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
      file.select_analysis(std::stoi(row.value("analysis_index", "0")));
      if (indexed)
      {
        const auto all_headers = file.get_spectra_headers();
        for (const auto index : indices)
          if (index < 0 || static_cast<std::size_t>(index) >= all_headers.index.size())
            throw std::out_of_range("mass spectrometry spectrum index is out of range: " + std::to_string(index));
      }
      const auto headers = indexed ? file.get_spectra_headers(indices) : file.get_spectra_headers();
      const auto spectra = indexed ? file.get_spectra(indices) : file.get_spectra();
      for (std::size_t i = 0; i < spectra.size() && i < headers.index.size(); ++i)
      {
        if (spectra[i].size() < 2)
          continue;
        for (std::size_t j = 0; j < spectra[i][0].size() && j < spectra[i][1].size(); ++j)
        {
          const double intensity = spectra[i][1][j];
          if ((headers.level[i] == 1 && intensity < parameters.value("min_intensity_ms1", 0.0)) ||
              (headers.level[i] >= 2 && intensity < parameters.value("min_intensity_ms2", 0.0)))
            continue;
          const auto base = Json{{"analysis", analysis}, {"replicate", row.value("replicate", "")}, {"id", analysis + ":" + std::to_string(headers.index[i])}, {"polarity", headers.polarity[i]}, {"level", headers.level[i]}, {"pre_mz", headers.precursor_mz[i]}, {"pre_mzlow", headers.window_mzlow[i]}, {"pre_mzhigh", headers.window_mzhigh[i]}, {"pre_ce", headers.activation_ce[i]}, {"rt", headers.rt[i]}, {"mobility", headers.mobility[i]}, {"mz", spectra[i][0][j]}, {"intensity", spectra[i][1][j]}};
          if (indexed)
          {
            auto result = base;
            result["target_id"] = "spectrum:" + std::to_string(headers.index[i]);
            output.push_back(std::move(result));
          }
          else
          {
            for (const auto &target : targets)
              if (streamfind::mass_spec::detail::target_matches(target, analysis, headers.polarity[i], headers.level[i], headers.rt[i], spectra[i][0][j]))
              {
                auto result = base;
                result["target_id"] = target.id;
                output.push_back(std::move(result));
              }
          }
        }
      }
    }
    return output;
  }

  Json get_raw_spectra_eic(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    auto copy = parameters;
    copy["levels"] = Json::array({1});
    return streamfind::mass_spec::detail::summarize_eic(get_raw_spectra(access, copy));
  }

  Json get_raw_spectra_ms1(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    auto copy = parameters;
    copy["levels"] = Json::array({1});
    return streamfind::mass_spec::detail::merge_ms_rows(get_raw_spectra(access, copy), copy.value("mz_clust", 0.003), copy.value("presence", 0.8));
  }

  Json get_raw_spectra_ms2(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    auto copy = parameters;
    copy["levels"] = Json::array({2});
    return streamfind::mass_spec::detail::merge_ms_rows(get_raw_spectra(access, copy), copy.value("mz_clust", 0.005), copy.value("presence", 0.0));
  }

  Json get_raw_chromatograms(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    std::vector<int> indices;
    for (const auto &value : parameters.value("indices", Json::array()))
      indices.push_back(value.get<int>());
    Json output = Json::array();
    const auto wanted = parameters.value("analysis_names", Json::array());
    for (const auto &row : access.query("SELECT analysis, file_path, analysis_index, replicate FROM MASS_SPEC_ANALYSES ORDER BY analysis"))
    {
      const auto analysis = row.at("analysis").get<std::string>();
      if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), analysis) == wanted.end())
        continue;
      ::mass_spec::reader::MASS_SPEC_FILE file(row.at("file_path").get<std::string>());
      file.select_analysis(std::stoi(row.value("analysis_index", "0")));
      const auto headers = file.get_chromatograms_headers(indices);
      const auto arrays = file.get_chromatograms(indices);
      const auto replicate = row.at("replicate").is_null() ? Json("") : row.at("replicate");
      for (std::size_t i = 0; i < arrays.size() && i < headers.chromatogram_id.size(); ++i)
      {
        if (arrays[i].size() < 2)
          continue;
        const auto count = std::min(arrays[i][0].size(), arrays[i][1].size());
        const auto precursor = std::isfinite(headers.precursor_mz[i]) ? Json(headers.precursor_mz[i]) : Json(nullptr);
        const auto activation = std::isfinite(headers.activation_ce[i]) ? Json(headers.activation_ce[i]) : Json(nullptr);
        const auto product = std::isfinite(headers.product_mz[i]) ? Json(headers.product_mz[i]) : Json(nullptr);
        for (std::size_t j = 0; j < count; ++j)
          output.push_back({{"analysis", analysis}, {"replicate", replicate}, {"index", headers.index[i]}, {"chromatogram_id", headers.chromatogram_id[i]}, {"polarity", headers.polarity[i]}, {"precursor_mz", precursor.is_null() ? Json(0.0f) : precursor}, {"activation_ce", activation.is_null() ? Json(0.0f) : activation}, {"product_mz", product.is_null() ? Json(0.0f) : product}, {"wavelength_nm", headers.wavelength_nm[i] == headers.wavelength_nm[i] ? headers.wavelength_nm[i] : 0.0f}, {"rt", arrays[i][0][j]}, {"raw_intensity", arrays[i][1][j]}, {"baseline", 0.0}, {"intensity", arrays[i][1][j]}});
      }
    }
    return output;
  }

  Json add_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    Json added = Json::array();
    std::vector<std::vector<std::optional<std::string>>> rows;
    for (const auto &item : parameters.at("analyses"))
    {
      const std::filesystem::path path = item.at("path").get<std::string>();
      const auto extension = detail::lower(path.extension().string());
      ::mass_spec::reader::MASS_SPEC_FILE file(path.string());
      const auto replicate = item.value("replicate_name", "");
      const auto blank = item.value("blank_name", "");
      for (const auto &descriptor : file.get_analysis_catalog())
      {
        file.select_analysis(descriptor.analysis_index);
        const auto summary = file.get_summary();
        const auto analysis = path.stem().string();
        for (const auto &existing : access.read("MASS_SPEC_ANALYSES", {"analysis"}, "analysis"))
          if (existing.value("analysis", "") == analysis)
            throw std::invalid_argument("analysis already exists in project: " + analysis);
        rows.push_back({analysis, std::to_string(descriptor.analysis_index),
                        std::to_string(descriptor.source_analysis_number), std::to_string(descriptor.analysis_count),
                        replicate, blank, path.filename().string(), path.string(), path.parent_path().string(), extension,
                        summary.format, "MS", summary.time_stamp, std::to_string(summary.number_spectra),
                        std::to_string(summary.number_chromatograms), std::to_string(summary.number_spectra_binary_arrays),
                        std::to_string(summary.min_mz), std::to_string(summary.max_mz), std::to_string(summary.start_rt),
                        std::to_string(summary.end_rt), summary.has_ion_mobility ? "true" : "false", std::nullopt});
        added.push_back({{"analysis", analysis}, {"file_path", path.string()}, {"analysis_index", descriptor.analysis_index}, {"source_analysis_number", descriptor.source_analysis_number}, {"analysis_count", descriptor.analysis_count}, {"replicate", replicate}, {"blank", blank}});
      }
    }
    if (!rows.empty())
      access.append("MASS_SPEC_ANALYSES",
                    {"analysis", "analysis_index", "source_analysis_number", "analysis_count", "replicate", "blank",
                     "file_name", "file_path", "file_dir", "file_extension", "format", "type", "time_stamp",
                     "number_spectra", "number_chromatograms", "number_spectra_binary_arrays", "min_mz", "max_mz",
                     "start_rt", "end_rt", "has_ion_mobility", "concentration"},
                    rows);
    return added;
  }

  Json get_analyses_info(sdk::PluginProjectAccess &access, const Json &)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    return access.read("MASS_SPEC_ANALYSES",
                       {"analysis", "analysis_index", "source_analysis_number", "analysis_count", "replicate", "blank",
                        "file_path", "format", "number_spectra", "number_chromatograms"},
                       "analysis");
  }

  Json remove_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
  {
    access.require_table("MASS_SPEC_ANALYSES");
    std::vector<std::vector<std::optional<std::string>>> rows;
    Json removed = Json::array();
    for (const auto &value : parameters.at("analysis_names"))
    {
      const auto name = value.get<std::string>();
      rows.push_back({name});
      removed.push_back(name);
    }
    access.delete_rows("MASS_SPEC_ANALYSES", "analysis", rows);
    return removed;
  }

} // namespace streamfind::mass_spec::operations
