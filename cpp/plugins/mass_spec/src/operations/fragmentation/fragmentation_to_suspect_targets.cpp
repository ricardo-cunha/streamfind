#include "operations/fragmentation/fragmentation_to_suspect_targets.hpp"

#include "streamfind/core/vendors/openbabel.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets {
using Json = nlohmann::json;
using Builder = sdk::TableBatchBuilder;

namespace detail {
std::string text_or_empty(const sdk::PluginProjectAccess::BatchView &batch,
                          std::uint32_t column, std::uint64_t row) {
    if (batch.is_null(column, row)) return {};
    if (batch.type(column) == STREAMFIND_PLUGIN_COLUMN_UTF8)
        return std::string(batch.text(column, row));
    if (batch.type(column) == STREAMFIND_PLUGIN_COLUMN_INT64)
        return std::to_string(batch.integer(column, row));
    return {};
}

Builder make_output() {
    return Builder({
        {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"formula", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"SMILES", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"InChI", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"InChIKey", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"xLogP", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_mz_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_intensity_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_mz_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"fragments_intensity_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64}});
}
}  // namespace detail

Json run(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("fragmentationTreeTable");
    if (input == inputs.end() || !input->is_object() ||
        !input->contains("result_ref") || !input->at("result_ref").is_string())
        throw std::invalid_argument("Connect a fragmentationTreeTable input.");

    const auto table = input->at("result_ref").get<std::string>();
    const auto duplicate_formula_limit = parameters.value("number_duplicate_formulas", Json(2));
    if (!duplicate_formula_limit.is_number_integer() || duplicate_formula_limit.get<int>() < 0)
        throw std::invalid_argument("number_duplicate_formulas must be a non-negative integer");
    const auto max_duplicate_formulas = duplicate_formula_limit.get<std::size_t>();
    const auto total_row_count = access.count_rows(table);
    access.report_progress(0.0, "Reading " + std::to_string(total_row_count) + " fragmentation tree rows.");

    Builder output = detail::make_output();
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

    const auto flush = [&]() {
        if (output.row_count() == 0) return;
        emitted_rows += static_cast<std::size_t>(output.row_count());
        output.emit(access, "suspectTargetsTable");
        output.clear();
    };
    const auto process_batch = [&](const sdk::PluginProjectAccess::BatchView &batch) {
        const auto name_col = batch.column_index("name");
        const auto id_col = batch.column_index("fragment_id");
        const auto smiles_col = batch.column_index("SMILES");
        const auto formula_col = batch.column_index("formula");
        const auto mass_col = batch.column_index("mass");
        for (std::uint64_t row_index = 0; row_index < batch.row_count(); ++row_index) {
            if (access.is_cancelled()) throw std::runtime_error("Operation cancelled");
            ++source_row_count;
            std::string smiles = detail::text_or_empty(batch, smiles_col, row_index);
            if (smiles.empty()) { ++skipped; continue; }

            std::string name = detail::text_or_empty(batch, name_col, row_index);
            if (name.empty()) name = "Fragment";
            std::string fragment_id = detail::text_or_empty(batch, id_col, row_index);
            if (fragment_id.empty()) fragment_id = std::to_string(source_row_count);
            name += " [fragment " + fragment_id + "]";

            std::string formula = detail::text_or_empty(batch, formula_col, row_index);
            bool has_valid_formula = !formula.empty();
            bool has_valid_mass = !batch.is_null(mass_col, row_index) &&
                                  batch.type(mass_col) == STREAMFIND_PLUGIN_COLUMN_FLOAT64 &&
                                  std::isfinite(batch.real(mass_col, row_index)) &&
                                  batch.real(mass_col, row_index) > 0.0;
            double mass = has_valid_mass ? batch.real(mass_col, row_index) : 0.0;
            std::string inchi;
            std::string inchikey;
            double xlogp = 0.0;
            bool has_xlogp = false;

            unique_smiles.insert(smiles);
            if (use_openbabel && (!has_valid_formula || !has_valid_mass)) {
                auto cached = normalized_smiles.find(smiles);
                const bool was_cached = cached != normalized_smiles.end();
                if (!was_cached) {
                    access.report_progress(total_row_count == 0 ? 0.0 :
                        static_cast<double>(source_row_count) / static_cast<double>(total_row_count),
                        "Normalizing fragment " + std::to_string(source_row_count) + " of " +
                            std::to_string(total_row_count) + " with Open Babel.");
                    const auto normalized = streamfind::core::vendors::openbabel::normalize_structure(smiles, {});
                    ++normalized_row_count;
                    if (normalized_smiles.size() < 100000)
                        cached = normalized_smiles.emplace(smiles, normalized).first;
                    else if (normalized.ok) {
                        if (!normalized.canonical_smiles.empty()) smiles = normalized.canonical_smiles;
                        if (!normalized.formula.empty()) formula = normalized.formula;
                        mass = normalized.exact_mass;
                        inchi = normalized.inchi;
                        inchikey = normalized.inchikey;
                        if (normalized.has_xlogp) { xlogp = normalized.xlogp; has_xlogp = true; }
                    }
                }
                if (cached != normalized_smiles.end()) {
                    if (was_cached) ++cache_hit_count;
                    const auto &normalized = cached->second;
                    if (normalized.ok) {
                        if (!normalized.canonical_smiles.empty()) smiles = normalized.canonical_smiles;
                        if (!normalized.formula.empty()) formula = normalized.formula;
                        mass = normalized.exact_mass;
                        inchi = normalized.inchi;
                        inchikey = normalized.inchikey;
                        if (normalized.has_xlogp) { xlogp = normalized.xlogp; has_xlogp = true; }
                    }
                }
            }

            if (max_duplicate_formulas > 0 && !formula.empty()) {
                auto &count = formula_counts[formula];
                if (count >= max_duplicate_formulas) { ++skipped_duplicate_formulas; continue; }
                ++count;
            }

            using Cell = Builder::Cell;
            output.append_row({name,
                has_valid_mass || mass > 0.0 ? Cell{mass} : Cell{std::monostate{}},
                std::monostate{}, std::monostate{}, std::monostate{},
                formula.empty() ? Cell{std::monostate{}} : Cell{formula}, smiles,
                inchi.empty() ? Cell{std::monostate{}} : Cell{inchi},
                inchikey.empty() ? Cell{std::monostate{}} : Cell{inchikey},
                has_xlogp ? Cell{xlogp} : Cell{std::monostate{}},
                std::monostate{}, std::monostate{}, std::monostate{}, std::monostate{}});
            if (output.row_count() >= output_batch_size) flush();
            if ((emitted_rows + output.row_count()) % output_batch_size == 0)
                access.report_progress(total_row_count == 0 ? 1.0 :
                    static_cast<double>(source_row_count) / static_cast<double>(total_row_count),
                    "Processed " + std::to_string(source_row_count) + " of " +
                        std::to_string(total_row_count) + " fragmentation rows (" +
                        std::to_string(unique_smiles.size()) + " unique SMILES).");
        }
    };

    access.read_typed_batches(table, {"name", "fragment_id", "SMILES", "formula", "mass"}, process_batch);
    if (access.is_cancelled()) throw std::runtime_error("Operation cancelled");
    if (source_row_count == 0) throw std::invalid_argument("fragmentationTreeTable contains no fragment rows.");
    flush();
    if (emitted_rows == 0) throw std::invalid_argument("fragmentationTreeTable has no fragments with SMILES.");

    access.report_progress(1.0, "Converted fragmentation rows to suspect targets.");
    return {{"status", "finished"}, {"fragment_rows", source_row_count},
            {"suspect_target_rows", emitted_rows}, {"skipped_without_smiles", skipped},
            {"skipped_duplicate_formulas", skipped_duplicate_formulas},
            {"unique_smiles", unique_smiles.size()}, {"normalized_smiles", normalized_row_count},
            {"normalization_cache_hits", cache_hit_count}, {"artifact", "suspectTargetsTable"}};
}
}  // namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets
