#include "operations/fragmentation/expand_fragmentation_tree.hpp"

#include "streamfind/core/vendors/openbabel.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace streamfind::mass_spec::fragmentation::expand_fragmentation_tree
{
using Json = nlohmann::json;
namespace detail
{
std::string scalar_string(const Json &row, const char *key)
{
    const auto value = row.find(key);
    if (value == row.end() || value->is_null()) return {};
    return value->is_string() ? value->get<std::string>() : value->dump();
}

std::string suffix_after_last_underscore(const std::string &value)
{
    const auto position = value.find_last_of('_');
    return position == std::string::npos ? value : value.substr(position + 1);
}

} // namespace detail

Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto inputs = parameters.value("_inputs", Json::object());
    const auto input = inputs.find("fragmentationTreeTable");
    if (input == inputs.end() || !input->is_object() || !input->contains("physical_table") ||
        !input->at("physical_table").is_string())
        throw std::invalid_argument("Connect a fragmentationTreeTable input.");

    const auto table = input->at("physical_table").get<std::string>();
    const int max_candidates = parameters.value("max_inverse_candidates", 500);
    if (max_candidates < 1) throw std::invalid_argument("max_inverse_candidates must be at least 1.");
    const int max_attachment_variants = parameters.value("max_attachment_variants_per_fragment", 8);
    if (max_attachment_variants < 1)
        throw std::invalid_argument("max_attachment_variants_per_fragment must be at least 1.");
    const int max_fragment_units = parameters.value("max_fragment_units_per_candidate", 2);
    if (max_fragment_units < 1 || max_fragment_units > 2)
        throw std::invalid_argument("max_fragment_units_per_candidate must be 1 or 2.");
    const int max_local_edits = parameters.value("max_local_edit_candidates", 64);
    if (max_local_edits < 1)
        throw std::invalid_argument("max_local_edit_candidates must be at least 1.");

    const std::vector<std::string> columns = {
        "name", "fragment_id", "parent_id", "SMILES", "formula", "mass", "depth",
        "atom_indices", "broken_bond_indices", "neutral_losses", "fragment_data"};
    const std::vector<std::string> types = {
        "string", "string", "string", "string", "string", "real", "integer",
        "array", "array", "array", "object"};
    const auto source_rows = access.read(table, columns, "name");
    if (source_rows.empty()) throw std::invalid_argument("fragmentationTreeTable contains no rows.");

    Json rows = Json::array();
    for (const auto &source : source_rows)
    {
        Json row = Json::object();
        for (const auto &column : columns)
            row[column] = source.contains(column) ? source.at(column) : Json(nullptr);
        rows.push_back(std::move(row));
    }

    std::vector<Json> roots;
    for (const auto &source : source_rows)
    {
        if (detail::scalar_string(source, "parent_id").empty()) roots.push_back(source);
    }
    if (roots.empty()) throw std::invalid_argument("fragmentationTreeTable has no MOL-derived main compound row without parent_id.");
    if (roots.size() != 1)
        throw std::invalid_argument(
            "mass_spec.expand_fragmentation_tree requires exactly one MOL-derived root row; found " +
            std::to_string(roots.size()) + ". Run one compound per fragmentation tree.");

    std::size_t generated = 0;
    std::size_t skipped = 0;
    for (const auto &root : roots)
    {
        const auto root_id = detail::scalar_string(root, "fragment_id");
        if (root_id.empty()) { ++skipped; continue; }
        const auto root_smiles = detail::scalar_string(root, "SMILES");
        if (root_smiles.empty()) { ++skipped; continue; }
        const auto root_name = detail::scalar_string(root, "name");
        std::unordered_set<std::string> generated_smiles;
        generated_smiles.insert(root_smiles);
        std::unordered_set<std::string> observed_fragments;
        std::size_t fragment_index = 0;

        struct FragmentUnit
        {
            std::string id;
            std::string smiles;
        };
        std::vector<FragmentUnit> fragment_units;
        for (const auto &source : source_rows)
        {
            const auto source_id = detail::scalar_string(source, "fragment_id");
            const auto source_parent = detail::scalar_string(source, "parent_id");
            const auto fragment_smiles = detail::scalar_string(source, "SMILES");
            if (source_id.empty() || source_id == root_id || source_parent.empty() || fragment_smiles.empty() ||
                !observed_fragments.insert(fragment_smiles).second)
                continue;
            fragment_units.push_back({source_id, fragment_smiles});
        }

        const auto append_candidate = [&](const auto &candidate, const std::string &label,
                                          const std::string &display_label, const std::string &kind,
                                          Json provenance) {
            if (generated >= static_cast<std::size_t>(max_candidates) ||
                candidate.smiles.empty() || !generated_smiles.insert(candidate.smiles).second)
                return;
            Json row = Json::object();
            for (const auto &column : columns) row[column] = nullptr;
            row["name"] = root_name + " [inv-" + display_label + "]";
            row["fragment_id"] = "inverse:" + root_id + ":" + label;
            row["parent_id"] = root_id;
            row["SMILES"] = candidate.smiles;
            row["formula"] = candidate.formula;
            row["mass"] = candidate.exact_mass;
            row["depth"] = -1;
            provenance["edit_label"] = label;
            provenance["display_label"] = display_label;
            row["fragment_data"] = {
                {"direction", "inverse"}, {"kind", kind}, {"edit", label},
                {"structure_status", "generated_structure"}, {"provenance", provenance}};
            rows.push_back(std::move(row));
            ++generated;
        };

        const std::size_t attachment_budget = std::max<std::size_t>(
            1, static_cast<std::size_t>(max_candidates) / 2);
        const std::size_t multi_attachment_budget = std::max<std::size_t>(
            1, static_cast<std::size_t>(max_candidates) / 5);
        const std::size_t local_budget = std::max<std::size_t>(
            1, static_cast<std::size_t>(max_candidates) / 5);

        // Apply the small, high-value inverse rules first. These are bounded
        // structure hypotheses; their full provenance is retained so later
        // screening can distinguish them from observed fragment attachments.
        const auto loss_candidates = streamfind::core::vendors::openbabel::generate_dealkylation_variants(
            root_smiles, static_cast<int>(std::max<std::size_t>(1, local_budget / 2)));
        for (const auto &candidate : loss_candidates)
        {
            const auto label = "loss_" + candidate.label;
            append_candidate(candidate, label, "loss-" + detail::suffix_after_last_underscore(candidate.label),
                             "inverse_dealkylation",
                             Json{{"source_root_id", root_id}, {"transformation", candidate.label},
                                  {"rule_order", 1}});
        }

        // A methane attachment gives a net +CH2 homologation after the two
        // attachment hydrogens are consumed. A C4H8O unsaturated alcohol
        // gives a net +C4H6O oxygenated/acyl-like unit by the same rule.
        const std::vector<std::pair<std::string, std::string>> built_in_units = {
            {"C", "methylene_homologation"},
            {"CC=CCO", "oxygenated_C4_unit"}};
        for (const auto &[unit_smiles, transformation] : built_in_units)
        {
            if (generated >= static_cast<std::size_t>(max_candidates)) break;
            const auto candidates = streamfind::core::vendors::openbabel::generate_fragment_attachment_variants(
                root_smiles, unit_smiles, std::max(1, std::min(max_attachment_variants, 2)));
            for (const auto &candidate : candidates)
            {
                const auto label = transformation + "_" + candidate.label;
                append_candidate(candidate, label, transformation == "methylene_homologation" ? "CH2" : "C4H6O",
                                 "inverse_builtin_transformation",
                                 Json{{"source_root_id", root_id}, {"transformation", transformation},
                                      {"unit_smiles", unit_smiles}, {"rule_order", transformation == "methylene_homologation" ? 2 : 3},
                                      {"openbabel_edit", candidate.label}});
            }
        }

        // Generic oxygenation and halogenation follow the specific unit rules.
        // Open Babel validates valence and derives the resulting formula.
        const auto local_candidates = streamfind::core::vendors::openbabel::generate_local_transformation_variants(
            root_smiles, std::min(max_local_edits, static_cast<int>(local_budget)));
        for (const auto &candidate : local_candidates)
        {
            if (generated >= attachment_budget + multi_attachment_budget + local_budget)
                break;
            const auto label = "local_edit_" + candidate.label;
            std::string display_label = "edit" + std::to_string(fragment_index);
            if (candidate.label.rfind("deuterium_exchange_", 0) == 0)
                display_label = "D" + candidate.label.substr(std::string("deuterium_exchange_").size());
            else if (candidate.label.rfind("oxygenation_atom_", 0) == 0)
                display_label = "O" + candidate.label.substr(std::string("oxygenation_atom_").size());
            else if (candidate.label.rfind("fluorination_atom_", 0) == 0)
                display_label = "F" + candidate.label.substr(std::string("fluorination_atom_").size());
            else if (candidate.label.rfind("chlorination_atom_", 0) == 0)
                display_label = "Cl" + candidate.label.substr(std::string("chlorination_atom_").size());
            else if (candidate.label.rfind("bromination_atom_", 0) == 0)
                display_label = "Br" + candidate.label.substr(std::string("bromination_atom_").size());
            append_candidate(candidate, label, display_label, "local_structure_transformation",
                             Json{{"source_root_id", root_id}, {"transformation", candidate.label},
                                  {"rule_order", 5}});
            ++fragment_index;
        }

        struct AttachedCandidate
        {
            streamfind::core::vendors::openbabel::GraphEditCandidate candidate;
            std::string source_id;
            std::string fragment_smiles;
        };
        std::vector<AttachedCandidate> first_level_candidates;

        // Preserve experimental fragment evidence first, but enumerate a
        // bounded number of chemically valid attachment positions per unit.
        for (const auto &fragment : fragment_units)
        {
            if (generated >= attachment_budget)
                break;
            const auto candidates = streamfind::core::vendors::openbabel::generate_fragment_attachment_variants(
                root_smiles, fragment.smiles, max_attachment_variants);
            for (const auto &candidate : candidates)
            {
                if (generated >= attachment_budget)
                    break;
                const auto label = "fragment_unit_" + std::to_string(fragment_index) + "_" + candidate.label;
                append_candidate(candidate, label,
                                 "f" + std::to_string(fragment_index) + "-a" + detail::suffix_after_last_underscore(candidate.label),
                                 "observed_fragment_attachment",
                                 Json{{"source_fragment_id", fragment.id},
                                      {"fragment_smiles", fragment.smiles},
                                      {"attachment_level", 1},
                                      {"openbabel_edit", candidate.label},
                                      {"rule_order", 6}});
                first_level_candidates.push_back({candidate, fragment.id, fragment.smiles});
                ++fragment_index;
            }
        }

        // A second observed fragment unit models a bounded multi-substituent
        // impurity while keeping the combinatorial search under control.
        if (max_fragment_units >= 2)
        {
            for (const auto &first : first_level_candidates)
            {
                if (generated >= attachment_budget + multi_attachment_budget)
                    break;
                for (const auto &fragment : fragment_units)
                {
                    if (generated >= attachment_budget + multi_attachment_budget)
                        break;
                    const auto candidates = streamfind::core::vendors::openbabel::generate_fragment_attachment_variants(
                        first.candidate.smiles, fragment.smiles,
                        std::max(1, std::min(max_attachment_variants, 2)));
                    for (const auto &candidate : candidates)
                    {
                        if (generated >= attachment_budget + multi_attachment_budget)
                            break;
                        const auto label = "fragment_units_2_" + std::to_string(fragment_index) + "_" + candidate.label;
                        append_candidate(candidate, label, "2f" + std::to_string(fragment_index),
                                         "observed_multi_fragment_attachment",
                                         Json{{"source_fragment_id", first.source_id},
                                              {"source_fragment_smiles", first.fragment_smiles},
                                              {"second_fragment_id", fragment.id},
                                              {"second_fragment_smiles", fragment.smiles},
                                              {"attachment_level", 2},
                                              {"openbabel_edit", candidate.label},
                                              {"rule_order", 7}});
                        ++fragment_index;
                    }
                }
            }
        }

        const auto bond_candidates = streamfind::core::vendors::openbabel::generate_bond_order_variants(
            root_smiles, std::max(0, max_candidates - static_cast<int>(generated)));
        for (const auto &candidate : bond_candidates)
        {
            const auto label = "bond_order_isomer_" + candidate.label;
            append_candidate(candidate, label, "b" + std::to_string(fragment_index),
                             "bond_order_isomer", Json{{"openbabel_edit", candidate.label}});
            ++fragment_index;
        }
    }

    access.emit_table_rows("fragmentationTreeTable", columns, types, rows);
    return {{"status", "finished"}, {"input_rows", source_rows.size()},
            {"root_rows", roots.size()}, {"generated_inverse_rows", generated},
            {"skipped_roots", skipped}, {"artifact", "fragmentationTreeTable"}};
}
} // namespace streamfind::mass_spec::fragmentation::expand_fragmentation_tree
