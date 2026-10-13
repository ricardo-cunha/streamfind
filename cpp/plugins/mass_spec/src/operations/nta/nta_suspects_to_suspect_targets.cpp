#include "operations/nta/operations.hpp"
#include "utils/nta.hpp"

#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace streamfind::mass_spec::nta::suspects_to_suspect_targets
{
using Json = nlohmann::json;

namespace detail
{
struct Peak
{
    double mz = 0.0;
    double intensity = 0.0;
};

struct Cluster
{
    std::vector<Peak> peaks;
};

struct Compound
{
    std::string key;
    std::string name;
    std::string formula;
    std::string smiles;
    std::string inchi;
    std::string inchikey;
    int polarity = 0;
    std::vector<double> masses;
    std::vector<double> rts;
    std::vector<Peak> peaks;
    std::map<std::string, int> names;
    std::map<std::string, int> formulas;
    std::map<std::string, int> smiles_values;
    std::map<std::string, int> inchi_values;
    std::map<std::string, int> inchikey_values;
    std::vector<double> xlogp_values;
};

std::string text(const Json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

double number(const Json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return std::numeric_limits<double>::quiet_NaN();
    if (it->is_number()) return it->get<double>();
    if (it->is_string())
    {
        try { return std::stod(it->get<std::string>()); }
        catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

int integer(const Json &row, const char *column)
{
    const auto value = number(row, column);
    return std::isfinite(value) ? static_cast<int>(value) : 0;
}

void add_value(std::map<std::string, int> &counts, std::string &selected, const std::string &value)
{
    if (value.empty()) return;
    const auto count = ++counts[value];
    if (selected.empty() || count > counts[selected]) selected = value;
}

std::vector<double> decode(const std::string &encoded)
{
    if (encoded.empty()) return {};
    const auto raw = ::mass_spec::reader::utils::decode_base64(encoded);
    const auto values = ::mass_spec::reader::utils::decode_little_endian_to_float(raw, 4);
    return {values.begin(), values.end()};
}

std::vector<std::string> fragment_formulas(const std::string &encoded)
{
    std::vector<std::string> result;
    if (encoded.empty()) return result;
    std::istringstream stream(encoded);
    std::string formula;
    while (std::getline(stream, formula, ';'))
        result.push_back(formula);
    return result;
}

std::string compound_key(const Json &row, int polarity)
{
    const auto inchikey = text(row, "InChIKey");
    if (!inchikey.empty()) return "inchikey:" + inchikey + "|polarity:" + std::to_string(polarity);
    const auto smiles = text(row, "SMILES");
    if (!smiles.empty()) return "smiles:" + smiles + "|polarity:" + std::to_string(polarity);
    const auto formula = text(row, "formula");
    const auto mass = number(row, "db_mass");
    return "fallback:" + formula + "|" + std::to_string(mass) + "|" + text(row, "name") +
           "|polarity:" + std::to_string(polarity);
}

double quality_number(const Json &row, const char *column)
{
    const auto value = number(row, column);
    return std::isfinite(value) ? value : -std::numeric_limits<double>::infinity();
}

bool better_representative(const Json &candidate, const Json &current)
{
    const auto candidate_shared = integer(candidate, "shared_fragments");
    const auto current_shared = integer(current, "shared_fragments");
    if (candidate_shared != current_shared) return candidate_shared > current_shared;

    const auto candidate_error = std::abs(quality_number(candidate, "error_mass"));
    const auto current_error = std::abs(quality_number(current, "error_mass"));
    if (candidate_error != current_error) return candidate_error < current_error;

    const auto candidate_intensity = quality_number(candidate, "intensity");
    const auto current_intensity = quality_number(current, "intensity");
    if (candidate_intensity != current_intensity) return candidate_intensity > current_intensity;

    const auto candidate_area = quality_number(candidate, "area");
    const auto current_area = quality_number(current, "area");
    if (candidate_area != current_area) return candidate_area > current_area;

    const auto candidate_cosine = quality_number(candidate, "cosine_similarity");
    const auto current_cosine = quality_number(current, "cosine_similarity");
    if (candidate_cosine != current_cosine) return candidate_cosine > current_cosine;

    const auto candidate_level = integer(candidate, "id_level");
    const auto current_level = integer(current, "id_level");
    if (candidate_level != current_level) return candidate_level > current_level;

    const auto candidate_rank = integer(candidate, "candidate_rank");
    const auto current_rank = integer(current, "candidate_rank");
    if (candidate_rank != current_rank) return candidate_rank < current_rank;
    return text(candidate, "name") < text(current, "name");
}

void add_spectrum(Compound &compound, const Json &row)
{
    const auto size = integer(row, "db_ms2_size");
    const auto mz = decode(text(row, "db_ms2_mz"));
    const auto intensity = decode(text(row, "db_ms2_intensity"));
    const auto formulas = fragment_formulas(text(row, "db_ms2_formula"));
    if (size <= 0 || mz.empty() || mz.size() != intensity.size() || formulas.size() != mz.size()) return;
    const auto count = std::min(static_cast<std::size_t>(size), mz.size());
    for (std::size_t i = 0; i < count; ++i)
        if (!formulas[i].empty() && std::isfinite(mz[i]) && std::isfinite(intensity[i]) && mz[i] > 0.0 && intensity[i] >= 0.0)
            compound.peaks.push_back({mz[i], intensity[i]});
}

std::vector<Cluster> cluster(std::vector<Peak> peaks, double tolerance)
{
    std::sort(peaks.begin(), peaks.end(), [](const Peak &left, const Peak &right) { return left.mz < right.mz; });
    std::vector<Cluster> clusters;
    for (const auto &peak : peaks)
    {
        if (clusters.empty())
        {
            clusters.push_back({{peak}});
            continue;
        }
        auto &current = clusters.back();
        double mean_mz = 0.0;
        for (const auto &member : current.peaks) mean_mz += member.mz;
        mean_mz /= static_cast<double>(current.peaks.size());
        if (std::abs(peak.mz - mean_mz) <= tolerance)
            current.peaks.push_back(peak);
        else
            clusters.push_back({{peak}});
    }
    return clusters;
}

double rounded(const double value, const int decimals)
{
    const auto factor = std::pow(10.0, static_cast<double>(decimals));
    return std::round(value * factor) / factor;
}

Json numeric_array(const std::vector<double> &values, const int decimals)
{
    Json result = Json::array();
    for (const auto value : values) result.push_back(rounded(value, decimals));
    return result;
}

std::vector<double> harmonize(const Compound &compound, double tolerance)
{
    const auto clusters = cluster(compound.peaks, tolerance);
    std::vector<double> intensities;
    intensities.reserve(clusters.size());
    for (const auto &candidate : clusters)
    {
        if (candidate.peaks.empty()) continue;
        double intensity = 0.0;
        for (const auto &peak : candidate.peaks) intensity += peak.intensity;
        intensities.push_back(intensity / static_cast<double>(candidate.peaks.size()));
    }
    const auto maximum = intensities.empty() ? 0.0 : *std::max_element(intensities.begin(), intensities.end());
    if (maximum <= 0.0) return intensities;
    for (auto &intensity : intensities) intensity = 100.0 * intensity / maximum;
    return intensities;
}

std::vector<double> harmonized_mz(const Compound &compound, double tolerance)
{
    const auto clusters = cluster(compound.peaks, tolerance);
    std::vector<double> result;
    result.reserve(clusters.size());
    for (const auto &candidate : clusters)
    {
        if (candidate.peaks.empty()) continue;
        double mz = 0.0;
        for (const auto &peak : candidate.peaks) mz += peak.mz;
        result.push_back(std::round((mz / static_cast<double>(candidate.peaks.size())) * 10000.0) / 10000.0);
    }
    return result;
}

std::string mode(const std::map<std::string, int> &counts, const std::string &fallback)
{
    if (counts.empty()) return fallback;
    return std::max_element(counts.begin(), counts.end(), [](const auto &left, const auto &right) {
        return left.second < right.second;
    })->first;
}
}

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("suspectsTable");
    if (input == inputs.end() || !input->is_object() || !input->contains("physical_table"))
        throw std::invalid_argument("Connect a suspectsTable or internalStandardsTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const double tolerance = parameters.value("mz_clust", 0.003);
    const double minimum_cosine = parameters.value("min_cosine_similarity", 0.7);
    const int minimum_shared = parameters.value("min_shared_fragments", 3);
    if (!std::isfinite(tolerance) || tolerance <= 0.0)
        throw std::invalid_argument("mz_clust must be greater than zero");
    if (!std::isfinite(minimum_cosine) || minimum_cosine < 0.0 || minimum_cosine > 1.0)
        throw std::invalid_argument("min_cosine_similarity must be between zero and one");
    if (minimum_shared < 0)
        throw std::invalid_argument("min_shared_fragments must be non-negative");

    const std::vector<std::string> input_columns = {
        "analysis", "name", "polarity", "db_mass", "error_mass", "exp_rt", "intensity", "area", "id_level", "candidate_rank", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "shared_fragments", "cosine_similarity", "db_ms2_size", "db_ms2_mz", "db_ms2_intensity"};
    const auto rows = access.read(table, input_columns, "analysis");
    std::map<std::string, detail::Compound> compounds;
    std::map<std::string, Json> representatives;
    std::size_t accepted_rows = 0;
    for (const auto &row : rows)
    {
        const auto shared = detail::integer(row, "shared_fragments");
        const auto cosine = detail::number(row, "cosine_similarity");
        const auto db_ms2_size = detail::integer(row, "db_ms2_size");
        if (db_ms2_size <= 0 || shared < minimum_shared || !std::isfinite(cosine) || cosine < minimum_cosine)
            continue;

        const int polarity = detail::integer(row, "polarity");
        const auto key = detail::compound_key(row, polarity);
        const auto analysis = detail::text(row, "analysis");
        const auto representative_key = analysis + "|" + key;
        const auto representative = representatives.find(representative_key);
        if (representative == representatives.end() || detail::better_representative(row, representative->second))
            representatives[representative_key] = row;
        ++accepted_rows;
    }

    for (const auto &[representative_key, row] : representatives)
    {
        (void)representative_key;
        const int polarity = detail::integer(row, "polarity");
        const auto key = detail::compound_key(row, polarity);
        auto [it, inserted] = compounds.emplace(key, detail::Compound{});
        auto &compound = it->second;
        if (inserted)
        {
            compound.key = key;
            compound.polarity = polarity;
            compound.name = detail::text(row, "name");
            compound.formula = detail::text(row, "formula");
            compound.smiles = detail::text(row, "SMILES");
            compound.inchi = detail::text(row, "InChI");
            compound.inchikey = detail::text(row, "InChIKey");
        }
        detail::add_value(compound.names, compound.name, detail::text(row, "name"));
        detail::add_value(compound.formulas, compound.formula, detail::text(row, "formula"));
        detail::add_value(compound.smiles_values, compound.smiles, detail::text(row, "SMILES"));
        detail::add_value(compound.inchi_values, compound.inchi, detail::text(row, "InChI"));
        detail::add_value(compound.inchikey_values, compound.inchikey, detail::text(row, "InChIKey"));
        const auto mass = detail::number(row, "db_mass");
        if (std::isfinite(mass) && mass > 0.0) compound.masses.push_back(mass);
        const auto rt = detail::number(row, "exp_rt");
        if (std::isfinite(rt)) compound.rts.push_back(rt);
        const auto xlogp = detail::number(row, "xLogP");
        if (std::isfinite(xlogp)) compound.xlogp_values.push_back(xlogp);
        detail::add_spectrum(compound, row);
    }

    const std::vector<std::string> output_columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    const std::vector<std::string> output_types = {
        "string", "real", "integer", "real", "real", "string", "string", "string", "string", "real",
        "array", "array", "array", "array"};
    Json output = Json::array();
    for (const auto &[key, compound] : compounds)
    {
        const auto mz = detail::harmonized_mz(compound, tolerance);
        const auto intensity = detail::harmonize(compound, tolerance);
        if (mz.size() != intensity.size()) continue;
        Json row = Json::object();
        for (const auto &column : output_columns) row[column] = nullptr;
        row["name"] = detail::mode(compound.names, compound.name);
        if (!compound.masses.empty())
            row["mass"] = detail::rounded(std::accumulate(compound.masses.begin(), compound.masses.end(), 0.0) /
                                           compound.masses.size(), 4);
        row["polarity"] = compound.polarity;
        if (!compound.rts.empty())
            row["rt"] = detail::rounded(std::accumulate(compound.rts.begin(), compound.rts.end(), 0.0) /
                                         compound.rts.size(), 0);
        row["formula"] = detail::mode(compound.formulas, compound.formula);
        row["SMILES"] = detail::mode(compound.smiles_values, compound.smiles);
        row["InChI"] = detail::mode(compound.inchi_values, compound.inchi);
        row["InChIKey"] = detail::mode(compound.inchikey_values, compound.inchikey);
        if (!compound.xlogp_values.empty())
            row["xLogP"] = detail::rounded(std::accumulate(compound.xlogp_values.begin(), compound.xlogp_values.end(), 0.0) /
                                            compound.xlogp_values.size(), 2);
        if (compound.polarity > 0)
        {
            row["fragments_mz_pos"] = detail::numeric_array(mz, 4);
            row["fragments_intensity_pos"] = detail::numeric_array(intensity, 1);
        }
        else if (compound.polarity < 0)
        {
            row["fragments_mz_neg"] = detail::numeric_array(mz, 4);
            row["fragments_intensity_neg"] = detail::numeric_array(intensity, 1);
        }
        output.push_back(std::move(row));
    }

    access.emit_table_rows("suspectTargetsTable", output_columns, output_types, output);
    access.report_progress(1.0, "Converted and harmonized suspect MS2 spectra.");
    return Json{{"status", "finished"}, {"input_rows", rows.size()}, {"validated_rows", accepted_rows},
                {"suspect_target_rows", output.size()}, {"artifact", "suspectTargetsTable"}};
}
}

namespace streamfind::mass_spec::nta::internal_standards_to_suspects
{
using Json = nlohmann::json;

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("internalStandardsTable");
    if (input == inputs.end() || !input->is_object() || !input->contains("physical_table"))
        throw std::invalid_argument("Connect an internalStandardsTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const auto columns = ::streamfind::mass_spec::nta::utils::suspects_columns();
    const std::vector<std::string> types = {
        "string", "string", "string", "string", "string", "string", "integer", "string", "integer",
        "real", "real", "real", "real", "real", "real", "real", "real", "integer", "real", "integer",
        "real", "string", "string", "string", "string", "real", "integer", "string", "string", "string",
        "string", "integer", "string", "string", "string", "string", "string", "integer", "string",
        "string", "integer", "string", "string", "integer", "integer", "real", "boolean"};
    const auto rows = access.read(table, columns, "analysis");
    access.emit_table_rows("suspectsTable", columns, types, rows);
    access.report_progress(1.0, "Converted internal standards to the suspectsTable contract.");
    return Json{{"status", "finished"}, {"rows", rows.size()}, {"artifact", "suspectsTable"}};
}
}
