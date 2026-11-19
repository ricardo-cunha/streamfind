#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace project_internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

const Json &required_aligned_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorAlignedTable"))
        throw std::invalid_argument(
            "project_aligned_timeseries requires a sensorAlignedTable connection");
    const auto &input = inputs->at("sensorAlignedTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorAlignedTable must identify a persisted table");
    return input;
}

std::string string_parameter(const Json &parameters, const char *name,
                             const std::string &default_value) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

sdk::TableBatchBuilder make_output_batch() {
    return sdk::TableBatchBuilder({
        {"source_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"signal_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_time", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"received_at", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
        {"time_origin", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_type", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_double", STREAMFIND_PLUGIN_COLUMN_FLOAT64},
        {"value_int", STREAMFIND_PLUGIN_COLUMN_INT64},
        {"value_bool", STREAMFIND_PLUGIN_COLUMN_BOOL},
        {"value_text", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"value_json", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"unit", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"quality", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"status_code", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"capture_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"event_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
    });
}

std::string text(const sdk::PluginProjectAccess::BatchView &batch,
                 std::uint32_t column, std::uint64_t row, const char *name) {
    if (batch.is_null(column, row))
        throw std::invalid_argument(std::string("SENSOR_ALIGNED column is null: ") + name);
    return std::string(batch.text(column, row));
}

Cell optional_text(const sdk::PluginProjectAccess::BatchView &batch,
                   std::uint32_t column, std::uint64_t row) {
    return batch.is_null(column, row) ? Cell(std::monostate{})
                                      : Cell(std::string(batch.text(column, row)));
}

}  // namespace project_internal

nlohmann::json project_aligned_timeseries(sdk::PluginProjectAccess &access,
                                          const nlohmann::json &parameters) {
    using namespace project_internal;
    const auto &input = required_aligned_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto time_axis = string_parameter(parameters, "time_axis", "reference");
    if (time_axis != "reference" && time_axis != "observed")
        throw std::invalid_argument("time_axis must be reference or observed");

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    const std::vector<std::string> columns{
        "source_id", "reference_signal", "reference_event_time", "signal_id",
        "event_time", "received_at", "time_origin", "value_type", "value_double",
        "value_int", "value_bool", "value_text", "value_json", "unit", "quality",
        "status_code", "capture_id", "event_id", "alignment_status",
        "time_delta_microseconds"};
    auto output = make_output_batch();
    std::uint64_t rows_seen = 0;
    std::uint64_t rows_emitted = 0;
    std::uint64_t missing_rows = 0;
    std::uint64_t observed_axis_fallback_rows = 0;
    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        std::vector<std::uint32_t> index;
        index.reserve(columns.size());
        for (const auto &column : columns) index.push_back(batch.column_index(column));
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++rows_seen) {
            if (access.is_cancelled())
                throw std::runtime_error("aligned time-series projection cancelled");
            const auto reference_time = text(batch, index[2], row, "reference_event_time");
            const bool observed_time_available = !batch.is_null(index[4], row);
            const bool missing = text(batch, index[18], row, "alignment_status") == "missing";
            const auto event_time = time_axis == "reference" || !observed_time_available
                                        ? reference_time
                                        : std::string(batch.text(index[4], row));
            if (missing) ++missing_rows;
            if (time_axis == "observed" && !observed_time_available)
                ++observed_axis_fallback_rows;

            output.append_row({
                text(batch, index[0], row, "source_id"),
                text(batch, index[3], row, "signal_id"),
                event_time,
                observed_time_available ? Cell(std::string(batch.text(index[5], row)))
                                        : Cell(reference_time),
                observed_time_available ? Cell(std::string(batch.text(index[6], row)))
                                        : Cell(std::string("receiver")),
                text(batch, index[7], row, "value_type"),
                batch.is_null(index[8], row) ? Cell(std::monostate{}) : Cell(batch.real(index[8], row)),
                batch.is_null(index[9], row) ? Cell(std::monostate{}) : Cell(batch.integer(index[9], row)),
                batch.is_null(index[10], row) ? Cell(std::monostate{}) : Cell(batch.boolean(index[10], row)),
                optional_text(batch, index[11], row),
                optional_text(batch, index[12], row),
                optional_text(batch, index[13], row),
                text(batch, index[14], row, "quality"),
                batch.is_null(index[15], row) ? Cell(std::string("alignment_missing"))
                                               : Cell(std::string(batch.text(index[15], row))),
                text(batch, index[16], row, "capture_id"),
                text(batch, index[17], row, "event_id") + ":projected:" + time_axis,
            });
            ++rows_emitted;
            if (output.row_count() == 1024) {
                output.emit(access, "sensorTimeseriesTable");
                output = make_output_batch();
            }
            access.report_progress(
                input_rows == 0 ? 1.0
                                : static_cast<double>(rows_seen + 1) /
                                      static_cast<double>(input_rows),
                "Projecting aligned observations");
        }
    });
    if (output.row_count() > 0)
        output.emit(access, "sensorTimeseriesTable");
    access.report_progress(1.0, "Aligned sensor time-series projection completed");
    return Json{{"contract", "SENSOR_TIMESERIES"},
                {"input_contract", "SENSOR_ALIGNED_V1"},
                {"result_ref", result_ref},
                {"time_axis", time_axis},
                {"input_rows", input_rows},
                {"rows_emitted", rows_emitted},
                {"missing_rows", missing_rows},
                {"observed_axis_fallback_rows", observed_axis_fallback_rows}};
}

}  // namespace streamfind::sensors::operations::detail
