#include "operations/fragmentation/filter_fragmentation_tree.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace streamfind::mass_spec::fragmentation::filter_fragmentation_tree
{
using Json = nlohmann::json;

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("fragmentationTreeTable");
    if (input == inputs.end() || !input->is_object() || !input->contains("physical_table") ||
        !input->at("physical_table").is_string())
        throw std::invalid_argument("Connect a fragmentationTreeTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const auto minimum_value = parameters.value("minimum_mass", Json(nullptr));
    const auto maximum_value = parameters.value("maximum_mass", Json(nullptr));
    if (!minimum_value.is_null() && (!minimum_value.is_number() || minimum_value.get<double>() < 0.0))
        throw std::invalid_argument("minimum_mass must be null or non-negative.");
    if (!maximum_value.is_null() && (!maximum_value.is_number() || maximum_value.get<double>() < 0.0))
        throw std::invalid_argument("maximum_mass must be null or non-negative.");
    const double minimum_mass = minimum_value.is_null() ? -INFINITY : minimum_value.get<double>();
    const double maximum_mass = maximum_value.is_null() ? INFINITY : maximum_value.get<double>();
    if (minimum_mass > maximum_mass)
        throw std::invalid_argument("minimum_mass cannot exceed maximum_mass.");
    const auto duplicate_value = parameters.value("number_duplicate_formulas", Json(2));
    if (!duplicate_value.is_number_integer() || duplicate_value.get<int>() < 0)
        throw std::invalid_argument("number_duplicate_formulas must be a non-negative integer.");
    const auto duplicate_limit = duplicate_value.get<std::size_t>();

    const std::vector<std::string> columns = {
        "name", "fragment_id", "parent_id", "SMILES", "formula", "mass", "depth",
        "atom_indices", "broken_bond_indices", "neutral_losses", "fragment_data"};
    sdk::TableBatchBuilder output({
        {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"fragment_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"parent_id", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"SMILES", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"formula", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"depth", STREAMFIND_PLUGIN_COLUMN_INT64},
        {"atom_indices", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_INT64},
        {"broken_bond_indices", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_INT64},
        {"neutral_losses", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"fragment_data", STREAMFIND_PLUGIN_COLUMN_UTF8}});
    std::unordered_map<std::string, std::size_t> formula_counts;
    std::size_t skipped_mass = 0;
    std::size_t skipped_duplicate = 0;
    std::size_t input_rows = 0;
    std::size_t output_rows = 0;
    access.read_typed_batches(table, columns, "depth",
        [&](const sdk::PluginProjectAccess::BatchView &batch) {
            const auto mass_column = batch.column_index("mass");
            const auto formula_column = batch.column_index("formula");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row)
            {
                ++input_rows;
                if (batch.is_null(mass_column, row))
                {
                    ++skipped_mass;
                    continue;
                }
                const double mass = batch.real(mass_column, row);
                if (mass < minimum_mass || mass > maximum_mass)
                {
                    ++skipped_mass;
                    continue;
                }
                const std::string formula = batch.is_null(formula_column, row)
                    ? std::string{} : std::string(batch.text(formula_column, row));
                if (duplicate_limit > 0 && !formula.empty())
                {
                    auto &count = formula_counts[formula];
                    if (count >= duplicate_limit)
                    {
                        ++skipped_duplicate;
                        continue;
                    }
                    ++count;
                }
                output.append_row(batch, row);
                ++output_rows;
            }
        });
    output.emit(access, "fragmentationTreeTable");
    return {{"status", "finished"}, {"input_rows", input_rows},
            {"output_rows", output_rows}, {"skipped_mass", skipped_mass},
            {"skipped_duplicate_formulas", skipped_duplicate},
            {"artifact", "fragmentationTreeTable"}};
}
} // namespace streamfind::mass_spec::fragmentation::filter_fragmentation_tree
