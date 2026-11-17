#include "operations/nta/operations.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"
#include "utils/nta.hpp"

#include "readers/reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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
    std::vector<double> masses;
    std::vector<double> rts;
    std::map<int, std::vector<Peak>> peaks_by_polarity;
    std::map<int, int> polarities;
    std::map<std::string, int> names;
    std::map<std::string, int> formulas;
    std::map<std::string, int> smiles_values;
    std::map<std::string, int> inchi_values;
    std::map<std::string, int> inchikey_values;
    std::vector<double> xlogp_values;
};

struct SuspectRow
{
    std::string analysis, name, formula, smiles, inchi, inchikey, db_ms2_formula;
    double db_mass = std::numeric_limits<double>::quiet_NaN();
    double error_mass = std::numeric_limits<double>::quiet_NaN();
    double exp_rt = std::numeric_limits<double>::quiet_NaN();
    double intensity = std::numeric_limits<double>::quiet_NaN();
    double area = std::numeric_limits<double>::quiet_NaN();
    double cosine_similarity = std::numeric_limits<double>::quiet_NaN();
    double xlogp = std::numeric_limits<double>::quiet_NaN();
    int polarity = 0, id_level = 0, candidate_rank = 0, shared_fragments = 0, db_ms2_size = 0;
    std::vector<double> db_ms2_mz, db_ms2_intensity;
};

std::string text(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "analysis") == 0) return row.analysis;
    if (std::strcmp(column, "name") == 0) return row.name;
    if (std::strcmp(column, "formula") == 0) return row.formula;
    if (std::strcmp(column, "SMILES") == 0) return row.smiles;
    if (std::strcmp(column, "InChI") == 0) return row.inchi;
    if (std::strcmp(column, "InChIKey") == 0) return row.inchikey;
    if (std::strcmp(column, "db_ms2_formula") == 0) return row.db_ms2_formula;
    return {};
}

double number(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "db_mass") == 0) return row.db_mass;
    if (std::strcmp(column, "error_mass") == 0) return row.error_mass;
    if (std::strcmp(column, "exp_rt") == 0) return row.exp_rt;
    if (std::strcmp(column, "intensity") == 0) return row.intensity;
    if (std::strcmp(column, "area") == 0) return row.area;
    if (std::strcmp(column, "cosine_similarity") == 0) return row.cosine_similarity;
    if (std::strcmp(column, "xLogP") == 0) return row.xlogp;
    return std::numeric_limits<double>::quiet_NaN();
}

int integer(const SuspectRow &row, const char *column)
{
    if (std::strcmp(column, "polarity") == 0) return row.polarity;
    if (std::strcmp(column, "id_level") == 0) return row.id_level;
    if (std::strcmp(column, "candidate_rank") == 0) return row.candidate_rank;
    if (std::strcmp(column, "shared_fragments") == 0) return row.shared_fragments;
    if (std::strcmp(column, "db_ms2_size") == 0) return row.db_ms2_size;
    return 0;
}

SuspectRow read_row(const sdk::PluginProjectAccess::BatchView &batch, std::uint64_t row,
                    const std::map<std::string, std::uint32_t> &columns)
{
    SuspectRow result;
    const auto get_text = [&](const char *name) {
        const auto column = columns.at(name);
        return batch.is_null(column, row) ? std::string{} : std::string(batch.text(column, row));
    };
    const auto get_real = [&](const char *name) {
        const auto column = columns.at(name);
        return batch.is_null(column, row) ? std::numeric_limits<double>::quiet_NaN() : batch.real(column, row);
    };
    const auto get_integer = [&](const char *name) {
        const auto column = columns.at(name);
        return batch.is_null(column, row) ? 0 : static_cast<int>(batch.integer(column, row));
    };
    const auto get_list = [&](const char *name) {
        const auto column = columns.at(name);
        std::vector<double> values;
        if (batch.is_null(column, row)) return values;
        const auto size = batch.list_size(column, row);
        values.reserve(static_cast<std::size_t>(size));
        for (std::uint64_t index = 0; index < size; ++index)
            values.push_back(batch.list_is_null(column, row, index)
                ? std::numeric_limits<double>::quiet_NaN() : batch.list_real(column, row, index));
        return values;
    };
    result.analysis = get_text("analysis"); result.name = get_text("name");
    result.formula = get_text("formula"); result.smiles = get_text("SMILES");
    result.inchi = get_text("InChI"); result.inchikey = get_text("InChIKey");
    result.db_ms2_formula = get_text("db_ms2_formula"); result.db_mass = get_real("db_mass");
    result.error_mass = get_real("error_mass"); result.exp_rt = get_real("exp_rt");
    result.intensity = get_real("intensity"); result.area = get_real("area");
    result.cosine_similarity = get_real("cosine_similarity"); result.xlogp = get_real("xLogP");
    result.polarity = get_integer("polarity"); result.id_level = get_integer("id_level");
    result.candidate_rank = get_integer("candidate_rank"); result.shared_fragments = get_integer("shared_fragments");
    result.db_ms2_size = get_integer("db_ms2_size");
    result.db_ms2_mz = get_list("db_ms2_mz"); result.db_ms2_intensity = get_list("db_ms2_intensity");
    return result;
}

void add_value(std::map<std::string, int> &counts, std::string &selected, const std::string &value)
{
    if (value.empty()) return;
    const auto count = ++counts[value];
    if (selected.empty() || count > counts[selected]) selected = value;
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

std::string compound_key(const SuspectRow &row)
{
    const auto inchikey = text(row, "InChIKey");
    if (!inchikey.empty()) return "inchikey:" + inchikey;
    const auto smiles = text(row, "SMILES");
    if (!smiles.empty()) return "smiles:" + smiles;
    const auto formula = text(row, "formula");
    const auto mass = number(row, "db_mass");
    return "fallback:" + formula + "|" + std::to_string(mass) + "|" + text(row, "name");
}

double quality_number(const SuspectRow &row, const char *column)
{
    const auto value = number(row, column);
    return std::isfinite(value) ? value : -std::numeric_limits<double>::infinity();
}

bool better_representative(const SuspectRow &candidate, const SuspectRow &current)
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

void add_spectrum(Compound &compound, const SuspectRow &row, const int polarity)
{
    const auto size = integer(row, "db_ms2_size");
    const auto &mz = row.db_ms2_mz;
    const auto &intensity = row.db_ms2_intensity;
    const auto formulas = fragment_formulas(text(row, "db_ms2_formula"));
    if (size <= 0 || mz.empty() || mz.size() != intensity.size() || formulas.size() != mz.size()) return;
    const auto count = std::min(static_cast<std::size_t>(size), mz.size());
    for (std::size_t i = 0; i < count; ++i)
        if (!formulas[i].empty() && std::isfinite(mz[i]) && std::isfinite(intensity[i]) && mz[i] > 0.0 && intensity[i] >= 0.0)
            compound.peaks_by_polarity[polarity].push_back({mz[i], intensity[i]});
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

std::vector<double> harmonize(const std::vector<Peak> &peaks, double tolerance)
{
    const auto clusters = cluster(peaks, tolerance);
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

std::vector<double> harmonized_mz(const std::vector<Peak> &peaks, double tolerance)
{
    const auto clusters = cluster(peaks, tolerance);
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
        "shared_fragments", "cosine_similarity", "db_ms2_size", "db_ms2_mz", "db_ms2_intensity", "db_ms2_formula"};
    std::map<std::string, detail::Compound> compounds;
    std::map<std::string, detail::SuspectRow> representatives;
    std::size_t accepted_rows = 0;
    std::size_t input_rows = 0;
    access.read_typed_batches(table, input_columns, "analysis",
        [&](const sdk::PluginProjectAccess::BatchView &batch) {
            std::map<std::string, std::uint32_t> columns;
            for (const auto &name : input_columns) columns.emplace(name, batch.column_index(name));
            for (std::uint64_t row_index = 0; row_index < batch.row_count(); ++row_index) {
                ++input_rows;
                const auto row = detail::read_row(batch, row_index, columns);
                const auto shared = detail::integer(row, "shared_fragments");
                const auto cosine = detail::number(row, "cosine_similarity");
                if (detail::integer(row, "db_ms2_size") <= 0 || shared < minimum_shared ||
                    !std::isfinite(cosine) || cosine < minimum_cosine)
                    continue;
                const auto key = detail::compound_key(row);
                const auto representative_key = row.analysis + "|" + key + "|polarity:" +
                                                std::to_string(row.polarity);
                const auto representative = representatives.find(representative_key);
                if (representative == representatives.end() ||
                    detail::better_representative(row, representative->second))
                    representatives[representative_key] = row;
                ++accepted_rows;
            }
        });

    for (const auto &[representative_key, row] : representatives)
    {
        (void)representative_key;
        const int polarity = detail::integer(row, "polarity");
        const auto key = detail::compound_key(row);
        auto [it, inserted] = compounds.emplace(key, detail::Compound{});
        auto &compound = it->second;
        if (inserted)
        {
            compound.key = key;
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
        ++compound.polarities[polarity];
        detail::add_spectrum(compound, row, polarity);
    }

    sdk::TableBatchBuilder output({
        {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"formula", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"SMILES", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"InChI", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"InChIKey", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"xLogP", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_mz_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_intensity_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_mz_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_intensity_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
    std::size_t output_rows = 0;
    for (const auto &[key, compound] : compounds)
    {
        using Cell = sdk::TableBatchBuilder::Cell;
        using List = std::vector<sdk::TableBatchBuilder::ListCell>;
        Cell mass = std::monostate{};
        if (!compound.masses.empty())
            mass = detail::rounded(std::accumulate(compound.masses.begin(), compound.masses.end(), 0.0) /
                                   compound.masses.size(), 4);
        Cell polarity = std::monostate{};
        if (compound.polarities.size() == 1) polarity = static_cast<std::int64_t>(compound.polarities.begin()->first);
        Cell rt = std::monostate{};
        if (!compound.rts.empty())
            rt = detail::rounded(std::accumulate(compound.rts.begin(), compound.rts.end(), 0.0) /
                                 compound.rts.size(), 0);
        Cell xlogp = std::monostate{};
        if (!compound.xlogp_values.empty())
            xlogp = detail::rounded(std::accumulate(compound.xlogp_values.begin(), compound.xlogp_values.end(), 0.0) /
                                    compound.xlogp_values.size(), 2);
        Cell mz_pos = std::monostate{}, intensity_pos = std::monostate{};
        Cell mz_neg = std::monostate{}, intensity_neg = std::monostate{};
        for (const auto &[polarity, peaks] : compound.peaks_by_polarity)
        {
            const auto mz = detail::harmonized_mz(peaks, tolerance);
            const auto intensity = detail::harmonize(peaks, tolerance);
            if (mz.size() != intensity.size() || mz.empty()) continue;
            List mz_values, intensity_values;
            mz_values.reserve(mz.size()); intensity_values.reserve(intensity.size());
            for (const auto value : mz) mz_values.emplace_back(detail::rounded(value, 4));
            for (const auto value : intensity) intensity_values.emplace_back(detail::rounded(value, 1));
            if (polarity > 0)
            {
                mz_pos = std::move(mz_values); intensity_pos = std::move(intensity_values);
            }
            else if (polarity < 0)
            {
                mz_neg = std::move(mz_values); intensity_neg = std::move(intensity_values);
            }
        }
        output.append_row({detail::mode(compound.names, compound.name), mass, polarity,
            std::monostate{}, rt, detail::mode(compound.formulas, compound.formula),
            detail::mode(compound.smiles_values, compound.smiles), detail::mode(compound.inchi_values, compound.inchi),
            detail::mode(compound.inchikey_values, compound.inchikey), xlogp,
            std::move(mz_pos), std::move(intensity_pos), std::move(mz_neg), std::move(intensity_neg)});
        ++output_rows;
    }

    output.emit(access, "suspectTargetsTable");
    access.report_progress(1.0, "Converted and harmonized suspect MS2 spectra.");
    return Json{{"status", "finished"}, {"input_rows", input_rows}, {"validated_rows", accepted_rows},
                {"suspect_target_rows", output_rows}, {"artifact", "suspectTargetsTable"}};
}
}

namespace streamfind::mass_spec::nta::internal_standards_to_suspects
{
using Json = nlohmann::json;

namespace detail {
std::vector<sdk::TableBatchBuilder::Column> schema_from_types(
    const std::vector<std::string> &columns, const std::vector<std::string> &types) {
    std::vector<sdk::TableBatchBuilder::Column> schema;
    schema.reserve(columns.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto &type = types.at(index);
        if (type == "array") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8, true,
                                                STREAMFIND_PLUGIN_COLUMN_FLOAT64});
        else if (type == "integer") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_INT64});
        else if (type == "real") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_FLOAT64});
        else if (type == "boolean") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_BOOL});
        else schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8});
    }
    return schema;
}
}

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("internalStandardsTable");
    if (input == inputs.end() || !input->is_object() || !input->contains("physical_table"))
        throw std::invalid_argument("Connect an internalStandardsTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const auto columns = ::streamfind::mass_spec::nta::utils::suspects_columns();
    const auto types = ::streamfind::mass_spec::nta::utils::detail::suspects_types();
    sdk::TableBatchBuilder output(detail::schema_from_types(columns, types));
    const auto rows = access.count_rows(table);
    access.read_typed_batches(table, columns, "analysis", [&](const sdk::PluginProjectAccess::BatchView &batch) {
        for (std::uint64_t row = 0; row < batch.row_count(); ++row)
            output.append_row(batch, row);
    });
    output.emit(access, "suspectsTable");
    access.report_progress(1.0, "Converted internal standards to the suspectsTable contract.");
    return Json{{"status", "finished"}, {"rows", rows}, {"artifact", "suspectsTable"}};
}
}
