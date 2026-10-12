#include "operations/fragmentation/filter_fragmentation_tree.hpp"

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
    const std::vector<std::string> types = {
        "string", "string", "string", "string", "string", "real", "integer",
        "array", "array", "array", "object"};
    const auto source_rows = access.read(table, columns, "depth");
    Json rows = Json::array();
    std::unordered_map<std::string, std::size_t> formula_counts;
    std::size_t skipped_mass = 0;
    std::size_t skipped_duplicate = 0;
    for (const auto &source : source_rows)
    {
        const auto mass_value = source.find("mass");
        if (mass_value == source.end() || mass_value->is_null() || !mass_value->is_number())
        {
            ++skipped_mass;
            continue;
        }
        const double mass = mass_value->get<double>();
        if (mass < minimum_mass || mass > maximum_mass)
        {
            ++skipped_mass;
            continue;
        }
        const auto formula_value = source.find("formula");
        const std::string formula = formula_value != source.end() && formula_value->is_string()
                                        ? formula_value->get<std::string>()
                                        : std::string{};
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
        Json row = Json::object();
        for (const auto &column : columns)
            row[column] = source.contains(column) ? source.at(column) : Json(nullptr);
        rows.push_back(std::move(row));
    }
    access.emit_table_rows("fragmentationTreeTable", columns, types, rows);
    return {{"status", "finished"}, {"input_rows", source_rows.size()},
            {"output_rows", rows.size()}, {"skipped_mass", skipped_mass},
            {"skipped_duplicate_formulas", skipped_duplicate},
            {"artifact", "fragmentationTreeTable"}};
}
} // namespace streamfind::mass_spec::fragmentation::filter_fragmentation_tree
