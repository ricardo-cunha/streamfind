#include "operations/nta/operations.hpp"

#include "utils/nta.hpp"
#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::mass_spec::nta::update_suspect_targets
{
using Json = nlohmann::json;

namespace detail
{
struct Peak { double mz = 0.0; double intensity = 0.0; };

struct Evidence
{
    std::vector<double> masses;
    std::vector<double> rts;
    std::vector<double> xlogp;
    std::vector<Peak> peaks;
};

std::string text(const Json &row, const char *column)
{
    const auto it = row.find(column);
    if (it == row.end() || it->is_null()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
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

std::string identity(const Json &row)
{
    for (const auto *column : {"InChIKey", "InChI", "SMILES"})
    {
        const auto value = text(row, column);
        if (!value.empty()) return std::string(column) + ":" + value;
    }
    const auto formula = text(row, "formula");
    const auto mass = number(row, "mass");
    if (!formula.empty() && std::isfinite(mass))
        return "formula:" + formula + ":" + std::to_string(std::round(mass * 10000.0) / 10000.0);
    return {};
}

std::string suspect_identity(const Json &row)
{
    Json normalized = row;
    if (!normalized.contains("mass") && normalized.contains("db_mass")) normalized["mass"] = normalized["db_mass"];
    return identity(normalized);
}

double quality_number(const Json &row, const char *column)
{
    const auto value = number(row, column);
    return std::isfinite(value) ? value : -std::numeric_limits<double>::infinity();
}

bool better_representative(const Json &candidate, const Json &current, const double target_rt)
{
    if (std::isfinite(target_rt))
    {
        const auto candidate_rt = number(candidate, "exp_rt");
        const auto current_rt = number(current, "exp_rt");
        const auto candidate_distance = std::isfinite(candidate_rt) ? std::abs(candidate_rt - target_rt) : std::numeric_limits<double>::infinity();
        const auto current_distance = std::isfinite(current_rt) ? std::abs(current_rt - target_rt) : std::numeric_limits<double>::infinity();
        if (candidate_distance != current_distance) return candidate_distance < current_distance;
    }

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

void add_spectrum(Evidence &evidence, const Json &row)
{
    const auto mz = decode(text(row, "db_ms2_mz"));
    const auto intensity = decode(text(row, "db_ms2_intensity"));
    const auto formulas = fragment_formulas(text(row, "db_ms2_formula"));
    if (mz.empty() || mz.size() != intensity.size() || formulas.size() != mz.size()) return;
    const auto size = integer(row, "db_ms2_size");
    const auto count = size > 0 ? std::min(static_cast<std::size_t>(size), mz.size()) : mz.size();
    for (std::size_t index = 0; index < count; ++index)
        if (!formulas[index].empty() && std::isfinite(mz[index]) && std::isfinite(intensity[index]) && mz[index] > 0.0 && intensity[index] >= 0.0)
            evidence.peaks.push_back({mz[index], intensity[index]});
}

std::vector<Peak> harmonize(std::vector<Peak> peaks, double tolerance)
{
    std::sort(peaks.begin(), peaks.end(), [](const Peak &left, const Peak &right) { return left.mz < right.mz; });
    std::vector<Peak> result;
    for (const auto &peak : peaks)
    {
        if (result.empty() || std::abs(peak.mz - result.back().mz) > tolerance)
            result.push_back(peak);
        else
        {
            auto &cluster = result.back();
            cluster.mz = (cluster.mz + peak.mz) / 2.0;
            cluster.intensity += peak.intensity;
        }
    }
    const auto maximum = result.empty() ? 0.0 : std::max_element(result.begin(), result.end(),
        [](const Peak &left, const Peak &right) { return left.intensity < right.intensity; })->intensity;
    for (auto &peak : result)
    {
        peak.mz = std::round(peak.mz * 10000.0) / 10000.0;
        peak.intensity = maximum > 0.0 ? std::round(100.0 * peak.intensity / maximum * 10.0) / 10.0 : 0.0;
    }
    return result;
}

Json array(const std::vector<Peak> &peaks, bool mz)
{
    Json output = Json::array();
    for (const auto &peak : peaks) output.push_back(mz ? peak.mz : peak.intensity);
    return output;
}

void set_if_present(Json &target, const char *target_column, const Json &source, const char *source_column)
{
    const auto value = source.find(source_column);
    if (value != source.end() && !value->is_null()) target[target_column] = *value;
}

void set_array_if_present(Json &target, const char *target_column, const Json &source)
{
    const auto value = source.find(target_column);
    if (value == source.end() || value->is_null()) return;
    if (value->is_array())
    {
        target[target_column] = *value;
        return;
    }
    if (!value->is_string()) return;
    try
    {
        const auto parsed = Json::parse(value->get<std::string>());
        if (parsed.is_array()) target[target_column] = parsed;
    }
    catch (...) {}
}

Json target_row(const Json &source)
{
    static const std::vector<std::string> columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    Json row = Json::object();
    for (const auto &column : columns) row[column] = nullptr;
    for (const auto *column : {"name", "formula", "SMILES", "InChI", "InChIKey"}) set_if_present(row, column, source, column);
    for (const auto *column : {"mass", "polarity", "mz", "rt", "xLogP"}) set_if_present(row, column, source, column);
    for (const auto *column : {"fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"})
        set_array_if_present(row, column, source);
    return row;
}

void merge_metadata(Json &target, const Evidence &evidence)
{
    if (!evidence.masses.empty())
        target["mass"] = std::accumulate(evidence.masses.begin(), evidence.masses.end(), 0.0) / evidence.masses.size();
    if (!evidence.rts.empty())
        target["rt"] = std::accumulate(evidence.rts.begin(), evidence.rts.end(), 0.0) / evidence.rts.size();
    if (!evidence.xlogp.empty())
        target["xLogP"] = std::accumulate(evidence.xlogp.begin(), evidence.xlogp.end(), 0.0) / evidence.xlogp.size();
}

void merge_spectrum(Json &target, const Evidence &evidence, int polarity, double tolerance)
{
    const auto peaks = harmonize(evidence.peaks, tolerance);
    if (peaks.empty()) return;
    if (polarity > 0)
    {
        target["fragments_mz_pos"] = array(peaks, true);
        target["fragments_intensity_pos"] = array(peaks, false);
    }
    else if (polarity < 0)
    {
        target["fragments_mz_neg"] = array(peaks, true);
        target["fragments_intensity_neg"] = array(peaks, false);
    }
}
}

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto target_input = inputs.find("suspectTargetsTable");
    const auto suspects_input = inputs.find("suspectsTable");
    if (target_input == inputs.end() || !target_input->is_object() || !target_input->contains("physical_table"))
        throw std::invalid_argument("Connect a suspectTargetsTable input.");
    if (suspects_input == inputs.end() || !suspects_input->is_object() || !suspects_input->contains("physical_table"))
        throw std::invalid_argument("Connect a suspectsTable input.");

    const double tolerance = parameters.value("mz_clust", 0.003);
    const double minimum_cosine = parameters.value("min_cosine_similarity", 0.7);
    const int minimum_shared = parameters.value("min_shared_fragments", 3);
    if (!std::isfinite(tolerance) || tolerance <= 0.0)
        throw std::invalid_argument("mz_clust must be greater than zero");
    if (!std::isfinite(minimum_cosine) || minimum_cosine < 0.0 || minimum_cosine > 1.0)
        throw std::invalid_argument("min_cosine_similarity must be between zero and one");
    if (minimum_shared < 0)
        throw std::invalid_argument("min_shared_fragments must be non-negative");

    const std::vector<std::string> target_columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    const auto target_rows = access.read(target_input->at("physical_table").get<std::string>(), target_columns, "name");

    const auto suspects = access.read(suspects_input->at("physical_table").get<std::string>(),
                                      utils::suspects_columns(), "analysis");
    std::map<std::string, Json> targets;
    for (const auto &row : target_rows)
    {
        const auto key = detail::identity(row);
        if (key.empty()) throw std::invalid_argument("Suspect target CSV contains a row without chemical identity");
        targets[key] = detail::target_row(row);
    }
    std::set<std::string> original_keys;
    for (const auto &[key, row] : targets) { (void)row; original_keys.insert(key); }

    std::map<std::string, std::map<int, detail::Evidence>> evidence;
    std::map<std::string, Json> representatives;
    for (const auto &suspect : suspects)
    {
        const auto shared = detail::integer(suspect, "shared_fragments");
        const auto cosine = detail::number(suspect, "cosine_similarity");
        const auto db_ms2_size = detail::integer(suspect, "db_ms2_size");
        if (db_ms2_size <= 0 || shared < minimum_shared || !std::isfinite(cosine) || cosine < minimum_cosine)
            continue;
        const auto key = detail::suspect_identity(suspect);
        if (key.empty()) continue;
        const auto polarity = detail::integer(suspect, "polarity");
        const auto target = targets.find(key);
        const auto target_rt = target != targets.end() ? detail::number(target->second, "rt") : std::numeric_limits<double>::quiet_NaN();
        const auto representative_key = detail::text(suspect, "analysis") + "|" + key + "|polarity:" + std::to_string(polarity);
        const auto representative = representatives.find(representative_key);
        if (representative == representatives.end() || detail::better_representative(suspect, representative->second, target_rt))
            representatives[representative_key] = suspect;
    }

    std::vector<Json> accepted_suspects;
    accepted_suspects.reserve(representatives.size());
    for (const auto &[representative_key, suspect] : representatives)
    {
        (void)representative_key;
        accepted_suspects.push_back(suspect);
        const auto key = detail::suspect_identity(suspect);
        const auto polarity = detail::integer(suspect, "polarity");
        auto &entry = evidence[key][polarity];
        const auto mass = detail::number(suspect, "db_mass");
        const auto rt = detail::number(suspect, "exp_rt");
        const auto xlogp = detail::number(suspect, "xLogP");
        if (std::isfinite(mass) && mass > 0.0) entry.masses.push_back(mass);
        if (std::isfinite(rt)) entry.rts.push_back(rt);
        if (std::isfinite(xlogp)) entry.xlogp.push_back(xlogp);
        detail::add_spectrum(entry, suspect);
    }

    std::size_t matched = 0;
    std::size_t added = 0;
    for (const auto &suspect : accepted_suspects)
    {
        const auto key = detail::suspect_identity(suspect);
        if (key.empty() || targets.contains(key)) continue;
        const auto name = detail::text(suspect, "name");
        const auto formula = detail::text(suspect, "formula");
        const auto smiles = detail::text(suspect, "SMILES");
        const auto inchi = detail::text(suspect, "InChI");
        const auto mass = detail::number(suspect, "db_mass");
        if (name.empty() || formula.empty() || (smiles.empty() && inchi.empty()) || !std::isfinite(mass) || mass <= 0.0) continue;
        Json source = {{"name", name}, {"formula", formula}};
        if (!smiles.empty()) source["SMILES"] = smiles;
        if (!inchi.empty()) source["InChI"] = inchi;
        const auto inchikey = detail::text(suspect, "InChIKey");
        if (!inchikey.empty()) source["InChIKey"] = inchikey;
        const auto rt = detail::number(suspect, "exp_rt");
        const auto xlogp = detail::number(suspect, "xLogP");
        if (std::isfinite(mass)) source["mass"] = mass;
        if (std::isfinite(rt)) source["rt"] = rt;
        if (std::isfinite(xlogp)) source["xLogP"] = xlogp;
        source["polarity"] = detail::integer(suspect, "polarity");
        Json row = detail::target_row(source);
        targets.emplace(key, std::move(row));
        ++added;
    }

    for (const auto &[key, modes] : evidence)
    {
        const auto target = targets.find(key);
        if (target == targets.end()) continue;
        if (original_keys.contains(key)) ++matched;
        detail::Evidence combined;
        for (const auto &[polarity, mode] : modes)
        {
            combined.masses.insert(combined.masses.end(), mode.masses.begin(), mode.masses.end());
            combined.rts.insert(combined.rts.end(), mode.rts.begin(), mode.rts.end());
            combined.xlogp.insert(combined.xlogp.end(), mode.xlogp.begin(), mode.xlogp.end());
            detail::merge_spectrum(target->second, mode, polarity, tolerance);
        }
        detail::merge_metadata(target->second, combined);
    }

    const std::vector<std::string> columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    const std::vector<std::string> types = {
        "string", "real", "integer", "real", "real", "string", "string", "string", "string", "real",
        "array", "array", "array", "array"};
    Json output = Json::array();
    for (const auto &[key, row] : targets) { (void)key; output.push_back(row); }
    access.emit_table_rows("suspectTargetsTable", columns, types, output);
    access.report_progress(1.0, "Updated suspect targets with connected suspect evidence.");
    return Json{{"status", "finished"}, {"target_rows_before", target_rows.size()}, {"suspect_rows", suspects.size()},
                {"validated_suspect_rows", accepted_suspects.size()}, {"matched_targets", matched},
                {"added_targets", added}, {"target_rows", output.size()}};
}
}
