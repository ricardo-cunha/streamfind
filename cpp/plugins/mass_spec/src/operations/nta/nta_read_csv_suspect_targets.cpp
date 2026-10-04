#include "operations/nta/operations.hpp"

#include "utils/target_csv.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::mass_spec::nta::read_csv_suspect_targets
{
    Json run(sdk::PluginProjectAccess &access, const Json &parameters)
    {
        const auto path = parameters.value("suspect_targets_csv_path", std::string{});
        if (path.empty()) throw std::invalid_argument("suspect_targets_csv_path must be a non-empty CSV file path");
        const auto parsed = target_csv::read_targets_csv(path, true);
        const std::vector<std::string> columns = {
            "name", "mass", "polarity", "mz", "rt", "formula", "SMILES", "InChI", "InChIKey", "xLogP",
            "database_id", "fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"};
        const std::vector<std::string> types = {
            "string", "real", "integer", "real", "real", "string", "string", "string", "string", "real",
            "string", "array", "array", "array", "array"};
        Json rows = Json::array();
        for (const auto &source : parsed)
        {
            Json row = Json::object();
            for (const auto &column : columns) row[column] = nullptr;
            for (const auto &key : {"name", "formula", "SMILES", "InChI", "InChIKey", "database_id"})
                if (source.contains(key)) row[key] = source.at(key);
            for (const auto &key : {"mass", "polarity", "mz", "rt", "xLogP"})
                if (source.contains(key)) row[key] = source.at(key);
            for (const auto &key : {"fragments_mz_pos", "fragments_intensity_pos", "fragments_mz_neg", "fragments_intensity_neg"})
                if (source.contains(key)) row[key] = source.at(key);
            rows.push_back(std::move(row));
        }
        access.emit_table_rows("suspectTargetsTable", columns, types, rows);
        return {{"rows", rows.size()}, {"path", path}};
    }
}
