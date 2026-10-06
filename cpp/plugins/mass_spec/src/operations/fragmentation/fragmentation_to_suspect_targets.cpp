#include "operations/fragmentation/fragmentation_to_suspect_targets.hpp"
#include "streamfind/core/vendors/openbabel.hpp"

#include <stdexcept>
#include <string>
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
    const auto source_rows = access.read(
        table, {"name", "fragment_id", "SMILES", "formula", "mass"}, "name");
    if (source_rows.empty())
        throw std::invalid_argument("fragmentationTreeTable contains no fragment rows.");

    const std::vector<std::string> columns = {
        "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
        "database_id", "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
    const std::vector<std::string> types = {
        "string", "real", "integer", "real", "real", "string", "string", "string", "string", "real",
        "string", "array", "array", "array", "array"};

    Json rows = Json::array();
    std::size_t skipped = 0;
    for (const auto &source : source_rows)
    {
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
            ? std::to_string(rows.size() + 1)
            : id_value->is_string() ? id_value->get<std::string>() : id_value->dump();
        name += " [fragment " + fragment_id + "]";

        Json row = Json::object();
        for (const auto &column : columns) row[column] = nullptr;
        row["name"] = std::move(name);
        row["SMILES"] = *smiles_value;
        if (const auto formula = source.find("formula"); formula != source.end() && formula->is_string())
            row["formula"] = *formula;
        if (const auto mass = source.find("mass"); mass != source.end())
        {
            if (mass->is_number()) row["mass"] = *mass;
            else if (mass->is_string() && !mass->get<std::string>().empty())
                row["mass"] = std::stod(mass->get<std::string>());
        }

        if (streamfind::core::vendors::openbabel::openbabel_available())
        {
            const auto structure = streamfind::core::vendors::openbabel::normalize_structure(
                smiles_value->get<std::string>(), {});
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
        rows.push_back(std::move(row));
    }

    if (rows.empty())
        throw std::invalid_argument("fragmentationTreeTable has no fragments with SMILES.");

    access.emit_table_rows("suspectTargetsTable", columns, types, rows);
    return {{"status", "finished"}, {"fragment_rows", source_rows.size()},
            {"suspect_target_rows", rows.size()}, {"skipped_without_smiles", skipped},
            {"artifact", "suspectTargetsTable"}};
}
} // namespace streamfind::mass_spec::fragmentation::fragmentation_to_suspect_targets
