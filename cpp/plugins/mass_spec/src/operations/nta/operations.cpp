#include "operations/nta/operations.hpp"
#include "operations/nta/nta_load_features_ms1.hpp"
#include "operations/nta/nta_load_features_ms2.hpp"
#include "operations/nta/nta_blank_subtraction.hpp"
#include "operations/nta/nta_filters.hpp"
#include "operations/nta/nta_alignment.hpp"
#include "operations/nta/nta_gap_filling.hpp"
#include "operations/nta/nta_componentization.hpp"
#include "operations/nta/nta_annotation.hpp"
#include "operations/nta/nta_suspect_screening.hpp"
#include "operations/nta/nta_correction_algorithms.hpp"
#include "operations/nta/nta_assign_transformation_products.hpp"
#include "operations/nta/nta_metfrag_runner.hpp"


#include "utils/base.hpp"
#include "utils/nta.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cmath>

namespace streamfind::mass_spec::nta
{
using Json = nlohmann::json;

Json get_features(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto &columns = utils::feature_columns();
    const auto targets = base::utils::normalize_targets(access, parameters);
    const auto requested_targets = parameters.value("targets", Json::array());
    const bool rename_matches = requested_targets.is_array() && !requested_targets.empty();
    const bool include_filtered = parameters.value("filtered", false);
    const auto table = base::utils::input_table(parameters, "featuresTable");
    auto output = utils::make_feature_batch_builder();
    std::size_t source_index = 0;
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    const auto flush = [&]() {
        if (output.row_count() == 0) return;
        output_count += static_cast<std::size_t>(output.row_count());
        output.emit(access, "featuresTable");
        output.clear();
    };
    access.read_typed_batches(table, columns,
        [&](const sdk::PluginProjectAccess::BatchView &batch) {
            const auto analysis_column = batch.column_index("analysis");
            const auto filtered_column = batch.column_index("filtered");
            const auto polarity_column = batch.column_index("polarity");
            const auto mass_column = batch.column_index("mass");
            const auto mz_column = batch.column_index("mz");
            const auto rt_column = batch.column_index("rt");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++source_index) {
                ++input_count;
                if (!include_filtered && !batch.is_null(filtered_column, row)) {
                    const auto is_filtered = batch.type(filtered_column) == STREAMFIND_PLUGIN_COLUMN_BOOL
                        ? batch.boolean(filtered_column, row) : batch.integer(filtered_column, row) != 0;
                    if (is_filtered) continue;
                }
                const auto analysis = batch.is_null(analysis_column, row)
                    ? std::string{} : std::string(batch.text(analysis_column, row));
                const auto polarity = batch.is_null(polarity_column, row)
                    ? 0 : static_cast<int>(batch.integer(polarity_column, row));
                const auto mass = batch.is_null(mass_column, row) ? 0.0 : batch.real(mass_column, row);
                const auto mz = batch.is_null(mz_column, row) ? 0.0 : batch.real(mz_column, row);
                const auto rt = batch.is_null(rt_column, row) ? 0.0 : batch.real(rt_column, row);
                const base::utils::TargetRange *matched = nullptr;
                for (const auto &target : targets) {
                    const bool analysis_ok = (target.analyses.empty() && target.analysis_indices.empty()) ||
                        std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end() ||
                        std::find(target.analysis_indices.begin(), target.analysis_indices.end(),
                                  static_cast<int>(source_index)) != target.analysis_indices.end();
                    const bool polarity_ok = target.polarities.empty() ||
                        std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
                        std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end();
                    if (analysis_ok && polarity_ok && (!target.has_mass ||
                        (mass >= target.mass_min && mass <= target.mass_max)) &&
                        mz >= target.mz_min && mz <= target.mz_max && rt >= target.rt_min && rt <= target.rt_max) {
                        matched = &target;
                        break;
                    }
                }
                if (matched == nullptr) continue;
                if (rename_matches)
                    output.append_row(batch, row, {{"name", matched->id}});
                else
                    output.append_row(batch, row);
                if (output.row_count() >= 1024) flush();
            }
        });
    flush();
    if (output_count == 0) output.emit(access, "featuresTable");
    return Json{{"status", "finished"}, {"features", output_count}};
}

namespace drop_filtered_features
{
Json run(sdk::PluginProjectAccess &access, const Json &parameters)
{
    auto output = utils::make_feature_batch_builder();
    const auto &columns = utils::feature_columns();
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    const auto flush = [&]() {
        if (output.row_count() == 0) return;
        output_count += static_cast<std::size_t>(output.row_count());
        output.emit(access, "featuresTable");
        output.clear();
    };
    access.read_typed_batches(base::utils::input_table(parameters, "featuresTable"), columns,
        [&](const sdk::PluginProjectAccess::BatchView &batch) {
            const auto filtered_column = batch.column_index("filtered");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
                ++input_count;
                bool filtered = false;
                if (!batch.is_null(filtered_column, row)) {
                    if (batch.type(filtered_column) == STREAMFIND_PLUGIN_COLUMN_BOOL)
                        filtered = batch.boolean(filtered_column, row);
                    else filtered = batch.integer(filtered_column, row) != 0;
                }
                if (!filtered) output.append_row(batch, row);
                if (output.row_count() >= 1024) flush();
            }
        });
    flush();
    if (input_count == 0) output.emit(access, "featuresTable");
    return Json{{"status", "finished"}, {"input_features", input_count}, {"output_features", output_count},
                {"dropped_features", input_count - output_count}};
}
}

namespace {

std::vector<sdk::TableBatchBuilder::Column> batch_schema(
    const sdk::PluginProjectAccess::BatchView &batch,
    const std::vector<std::string> &names) {
    std::vector<sdk::TableBatchBuilder::Column> schema;
    schema.reserve(batch.column_count());
    for (std::uint32_t column = 0; column < batch.column_count(); ++column) {
        const auto type = batch.type(column);
        schema.push_back({names.at(column), type, batch.is_list(column),
                          batch.is_list(column) ? batch.list_child_type(column)
                                                : STREAMFIND_PLUGIN_COLUMN_UTF8});
    }
    return schema;
}

double numeric_cell(const sdk::PluginProjectAccess::BatchView &batch,
                    std::uint32_t column, std::uint64_t row) {
    if (batch.is_null(column, row)) return 0.0;
    switch (batch.type(column)) {
    case STREAMFIND_PLUGIN_COLUMN_INT64: return static_cast<double>(batch.integer(column, row));
    case STREAMFIND_PLUGIN_COLUMN_FLOAT64: return batch.real(column, row);
    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
    case STREAMFIND_PLUGIN_COLUMN_UTF8: return std::stod(std::string(batch.text(column, row)));
    default: throw std::invalid_argument("target filter column must be numeric");
    }
}

std::size_t emit_target_matches(sdk::PluginProjectAccess &access, const Json &parameters,
                                const char *input_port, const char *output_port,
                                const std::vector<std::string> &columns,
                                const char *mass_name, const char *rt_name,
                                const char *polarity_name) {
    const auto targets = base::utils::normalize_targets(access, parameters);
    const auto requested = parameters.value("targets", Json::array());
    const bool rename = requested.is_array() && !requested.empty();
    const auto table = base::utils::input_table(parameters, input_port);
    std::size_t source_index = 0;
    std::size_t emitted = 0;
    access.read_typed_batches(table, columns, [&](const auto &batch) {
        sdk::TableBatchBuilder output(batch_schema(batch, columns));
        const auto analysis_col = batch.column_index("analysis");
        const auto mass_col = mass_name == nullptr ? UINT32_MAX : batch.column_index(mass_name);
        const auto rt_col = rt_name == nullptr ? UINT32_MAX : batch.column_index(rt_name);
        const auto polarity_col = polarity_name == nullptr ? UINT32_MAX : batch.column_index(polarity_name);
        const auto name_col = [&]() -> std::uint32_t {
            try { return batch.column_index("name"); }
            catch (const std::out_of_range &) { return UINT32_MAX; }
        }();
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++source_index) {
            const auto analysis = batch.is_null(analysis_col, row)
                ? std::string{} : std::string(batch.text(analysis_col, row));
            const auto mass = mass_col == UINT32_MAX ? 0.0 : numeric_cell(batch, mass_col, row);
            const auto rt = rt_col == UINT32_MAX ? 0.0 : numeric_cell(batch, rt_col, row);
            const auto polarity = polarity_col == UINT32_MAX || batch.is_null(polarity_col, row)
                ? 0 : static_cast<int>(numeric_cell(batch, polarity_col, row));
            const base::utils::TargetRange *matched = nullptr;
            for (const auto &target : targets) {
                const bool analysis_ok = (target.analyses.empty() && target.analysis_indices.empty()) ||
                    std::find(target.analyses.begin(), target.analyses.end(), analysis) != target.analyses.end() ||
                    std::find(target.analysis_indices.begin(), target.analysis_indices.end(),
                              static_cast<int>(source_index)) != target.analysis_indices.end();
                const bool polarity_ok = target.polarities.empty() ||
                    std::find(target.polarities.begin(), target.polarities.end(), 0) != target.polarities.end() ||
                    std::find(target.polarities.begin(), target.polarities.end(), polarity) != target.polarities.end();
                const bool mass_ok = mass_name == nullptr || !target.has_mass ||
                    (mass >= target.mass_min && mass <= target.mass_max);
                const bool rt_ok = rt_name == nullptr || (rt >= target.rt_min && rt <= target.rt_max);
                if (analysis_ok && polarity_ok && mass_ok && rt_ok) { matched = &target; break; }
            }
            if (matched == nullptr) continue;
            if (rename && name_col != UINT32_MAX)
                output.append_row(batch, row, {{"name", matched->id}});
            else output.append_row(batch, row);
            if (output.row_count() >= 1024) {
                emitted += static_cast<std::size_t>(output.row_count());
                output.emit(access, output_port);
                output.clear();
            }
        }
        if (output.row_count() != 0) {
            emitted += static_cast<std::size_t>(output.row_count());
            output.emit(access, output_port);
        }
    });
    return emitted;
}

}  // namespace

Json get_suspects(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto count = emit_target_matches(access, parameters, "suspectsTable", "suspectsTable",
        utils::suspects_columns(), "exp_mass", "exp_rt", "polarity");
    return {{"status", "finished"}, {"rows", count}};
}

Json get_internal_standards(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto count = emit_target_matches(access, parameters, "internalStandardsTable", "internalStandardsTable",
        utils::internal_standards_columns(), "exp_mass", "exp_rt", "polarity");
    return {{"status", "finished"}, {"rows", count}};
}

Json get_transformation_products(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto count = emit_target_matches(access, parameters, "transformationProductsTable", "transformationProductsTable",
        utils::transformation_products_columns(), "mass", nullptr, nullptr);
    return {{"status", "finished"}, {"rows", count}};
}
}
