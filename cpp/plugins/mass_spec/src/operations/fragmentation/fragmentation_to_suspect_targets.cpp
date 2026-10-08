#include "operations/fragmentation/fragmentation_to_suspect_targets.hpp"
#include "streamfind/core/vendors/openbabel.hpp"

#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets
{
using Json = nlohmann::json;

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("fragmentationTreeTable");
    if (input == inputs.end() || !input->is_object() ||
        !input->contains("physical_table") || !input->at("physical_table").is_string())
        throw std::invalid_argument("Connect a fragmentationTreeTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const auto duplicate_formula_limit = parameters.value("number_duplicate_formulas", Json(0));
    if (!duplicate_formula_limit.is_number_integer() || duplicate_formula_limit.get<int>() < 0)
        throw std::invalid_argument("number_duplicate_formulas must be a non-negative integer");
    const auto max_duplicate_formulas = duplicate_formula_limit.get<std::size_t>();
    const auto total_row_count = access.count_rows(table);
    access.report_progress(0.0, "Reading " + std::to_string(total_row_count) + " fragmentation tree rows.");

    const std::vector<std::string> columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    const std::vector<std::string> types = {
        "string", "real", "integer", "real", "real", "string", "string", "string", "string", "real",
        "array", "array", "array", "array"};

    Json rows = Json::array();
    constexpr std::size_t output_batch_size = 4096;
    std::size_t skipped = 0;
    std::size_t skipped_duplicate_formulas = 0;
    std::size_t emitted_rows = 0;
    std::size_t source_row_count = 0;
    std::size_t normalized_row_count = 0;
    std::size_t cache_hit_count = 0;
    std::unordered_set<std::string> unique_smiles;
    std::unordered_map<std::string, std::size_t> formula_counts;
    std::unordered_map<std::string, streamfind::core::vendors::openbabel::NormalizedStructure> normalized_smiles;
    normalized_smiles.reserve(100000);
    const bool use_openbabel = streamfind::core::vendors::openbabel::openbabel_available();
    const auto process_batch = [&](const Json &source_rows) {
        for (const auto &source : source_rows)
        {
            if (access.is_cancelled())
                throw std::runtime_error("Operation cancelled");
            ++source_row_count;
            const auto smiles_value = source.find("SMILES");
            if (smiles_value == source.end() || !smiles_value->is_string() || smiles_value->get<std::string>().empty())
            {
                ++skipped;
                continue;
            }

            const auto name_value = source.find("name");
            std::string name = name_value == source.end() || !name_value->is_string()
                ? std::string("Fragment") : name_value->get<std::string>();
            const auto id_value = source.find("fragment_id");
            const std::string fragment_id = id_value == source.end() || id_value->is_null()
                ? std::to_string(source_row_count)
                : id_value->is_string() ? id_value->get<std::string>() : id_value->dump();
            name += " [fragment " + fragment_id + "]";

            Json row = Json::object();
            for (const auto &column : columns) row[column] = nullptr;
            row["name"] = std::move(name);
            row["SMILES"] = *smiles_value;
            bool has_valid_formula = false;
            bool has_valid_mass = false;
            if (const auto formula = source.find("formula"); formula != source.end() && formula->is_string() &&
                !formula->get<std::string>().empty())
            {
                row["formula"] = *formula;
                has_valid_formula = true;
            }
            if (const auto mass = source.find("mass"); mass != source.end())
            {
                if (mass->is_number() && std::isfinite(mass->get<double>()) && mass->get<double>() > 0.0)
                {
                    row["mass"] = *mass;
                    has_valid_mass = true;
                }
                else if (mass->is_string() && !mass->get<std::string>().empty())
                {
                    const auto parsed_mass = std::stod(mass->get<std::string>());
                    if (std::isfinite(parsed_mass) && parsed_mass > 0.0)
                    {
                        row["mass"] = parsed_mass;
                        has_valid_mass = true;
                    }
                }
            }

            const auto smiles = smiles_value->get<std::string>();
            unique_smiles.insert(smiles);
            if (use_openbabel && (!has_valid_formula || !has_valid_mass))
            {
                auto cached = normalized_smiles.find(smiles);
                const bool was_cached = cached != normalized_smiles.end();
                if (cached == normalized_smiles.end())
                {
                    access.report_progress(
                        total_row_count == 0 ? 0.0 : static_cast<double>(source_row_count) / total_row_count,
                        "Normalizing fragment " + std::to_string(source_row_count) + " of " +
                            std::to_string(total_row_count) + " with Open Babel.");
                    const auto structure = streamfind::core::vendors::openbabel::normalize_structure(smiles, {});
                    ++normalized_row_count;
                    access.report_progress(
                        total_row_count == 0 ? 0.0 : static_cast<double>(source_row_count) / total_row_count,
                        "Open Babel processed fragment " + std::to_string(source_row_count) + " of " +
                            std::to_string(total_row_count) + ".");
                    if (normalized_smiles.size() < 100000)
                        cached = normalized_smiles.emplace(smiles, structure).first;
                    else
                    {
                        if (structure.ok)
                        {
                            if (!structure.canonical_smiles.empty()) row["SMILES"] = structure.canonical_smiles;
                            if (!structure.formula.empty()) row["formula"] = structure.formula;
                            row["mass"] = structure.exact_mass;
                            row["InChI"] = structure.inchi;
                            row["InChIKey"] = structure.inchikey;
                            if (structure.has_xlogp) row["xLogP"] = structure.xlogp;
                        }
                        cached = normalized_smiles.end();
                    }
                }
                if (cached != normalized_smiles.end())
                {
                    if (was_cached) ++cache_hit_count;
                    const auto &structure = cached->second;
                    if (structure.ok)
                    {
                        if (!structure.canonical_smiles.empty()) row["SMILES"] = structure.canonical_smiles;
                        if (!structure.formula.empty()) row["formula"] = structure.formula;
                        row["mass"] = structure.exact_mass;
                        row["InChI"] = structure.inchi;
                        row["InChIKey"] = structure.inchikey;
                        if (structure.has_xlogp) row["xLogP"] = structure.xlogp;
                    }
                }
            }

            const auto formula = row.at("formula").is_string() ? row.at("formula").get<std::string>() : std::string{};
            if (max_duplicate_formulas > 0 && !formula.empty())
            {
                auto &count = formula_counts[formula];
                if (count >= max_duplicate_formulas)
                {
                    ++skipped_duplicate_formulas;
                    continue;
                }
                ++count;
            }
            rows.push_back(std::move(row));
            if (rows.size() >= output_batch_size)
            {
                access.emit_table_rows("suspectTargetsTable", columns, types, rows);
                emitted_rows += rows.size();
                rows.clear();
            }
            if ((emitted_rows + rows.size()) % output_batch_size == 0)
                access.report_progress(
                    total_row_count == 0 ? 1.0 : static_cast<double>(source_row_count) / total_row_count,
                    "Processed " + std::to_string(source_row_count) + " of " +
                        std::to_string(total_row_count) + " fragmentation rows (" +
                        std::to_string(unique_smiles.size()) + " unique SMILES).");
        }
    };
    access.read_batches(table, {"name", "fragment_id", "SMILES", "formula", "mass"}, "name", process_batch);
    if (access.is_cancelled())
        throw std::runtime_error("Operation cancelled");

    if (source_row_count == 0)
        throw std::invalid_argument("fragmentationTreeTable contains no fragment rows.");
    if (rows.empty() && emitted_rows == 0)
        throw std::invalid_argument("fragmentationTreeTable has no fragments with SMILES.");

    if (!rows.empty())
    {
        emitted_rows += rows.size();
        access.emit_table_rows("suspectTargetsTable", columns, types, rows);
    }
    access.report_progress(1.0, "Converted fragmentation rows to suspect targets.");
    return {{"status", "finished"}, {"fragment_rows", source_row_count},
            {"suspect_target_rows", emitted_rows}, {"skipped_without_smiles", skipped},
            {"skipped_duplicate_formulas", skipped_duplicate_formulas},
            {"unique_smiles", unique_smiles.size()}, {"normalized_smiles", normalized_row_count},
            {"normalization_cache_hits", cache_hit_count},
            {"artifact", "suspectTargetsTable"}};
}
} // namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets
