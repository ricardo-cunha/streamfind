#include "operations/nta/operations.hpp"

#include "utils/target_csv.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

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
        std::vector<sdk::TableBatchBuilder::Column> schema = {
            {"name", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"mass", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"polarity", STREAMFIND_PLUGIN_COLUMN_INT64}, {"mz", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"rt", STREAMFIND_PLUGIN_COLUMN_FLOAT64}, {"formula", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"SMILES", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"InChI", STREAMFIND_PLUGIN_COLUMN_UTF8},
            {"InChIKey", STREAMFIND_PLUGIN_COLUMN_UTF8}, {"xLogP", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_mz_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_intensity_pos", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_mz_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64},
            {"fragments_intensity_neg", STREAMFIND_PLUGIN_COLUMN_UTF8, true, STREAMFIND_PLUGIN_COLUMN_FLOAT64}};
        sdk::TableBatchBuilder output(std::move(schema));
        for (const auto &source : parsed)
        {
            const auto text = [&source](const char *key) -> sdk::TableBatchBuilder::Cell {
                const auto it = source.find(key);
                if (it == source.end() || !it->is_string()) return std::monostate{};
                return it->get<std::string>();
            };
            const auto real = [&source](const char *key) -> sdk::TableBatchBuilder::Cell {
                const auto it = source.find(key);
                if (it == source.end() || !it->is_number()) return std::monostate{};
                return it->get<double>();
            };
            const auto integer = [&source](const char *key) -> sdk::TableBatchBuilder::Cell {
                const auto it = source.find(key);
                if (it == source.end() || !it->is_number_integer()) return std::monostate{};
                return it->get<std::int64_t>();
            };
            const auto list = [&source](const char *key) -> sdk::TableBatchBuilder::Cell {
                const auto it = source.find(key);
                if (it == source.end() || !it->is_array()) return std::monostate{};
                std::vector<sdk::TableBatchBuilder::ListCell> values;
                values.reserve(it->size());
                for (const auto &value : *it) {
                    if (!value.is_number()) values.emplace_back(std::monostate{});
                    else values.emplace_back(value.get<double>());
                }
                return values;
            };
            output.append_row({text("name"), real("mass"), integer("polarity"), real("mz"), real("rt"),
                               text("formula"), text("SMILES"), text("InChI"), text("InChIKey"), real("xLogP"),
                               list("fragments_mz_pos"), list("fragments_intensity_pos"),
                               list("fragments_mz_neg"), list("fragments_intensity_neg")});
        }
        output.emit(access, "suspectTargetsTable");
        return {{"rows", parsed.size()}, {"path", path}};
    }
}
