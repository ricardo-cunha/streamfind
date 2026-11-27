#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace filter_internal {

using Json = nlohmann::json;

const Json &required_timeseries_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorTimeseriesTable"))
        throw std::invalid_argument(
            "filter_timeseries requires a sensorTimeseriesTable connection");
    const auto &input = inputs->at("sensorTimeseriesTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorTimeseriesTable must identify a persisted table");
    return input;
}

std::string string_parameter(const Json &parameters, const char *name) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return {};
    if (!found->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

std::string timestamp_key(std::string value) {
    if (value.size() >= 1 && value.back() == 'Z') value.pop_back();
    if (value.size() > 10 && value.at(10) == 'T') value.at(10) = ' ';
    return value;
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

sdk::TableBatchBuilder::Cell scalar_cell(
    const sdk::PluginProjectAccess::BatchView &batch, std::uint32_t column,
    std::uint64_t row) {
    if (batch.is_null(column, row)) return std::monostate{};
    switch (batch.type(column)) {
    case STREAMFIND_PLUGIN_COLUMN_UTF8:
    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
        return std::string(batch.text(column, row));
    case STREAMFIND_PLUGIN_COLUMN_INT64:
        return batch.integer(column, row);
    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
        return batch.real(column, row);
    case STREAMFIND_PLUGIN_COLUMN_BOOL:
        return batch.boolean(column, row);
    default:
        throw std::invalid_argument("SENSOR_TIMESERIES contains an unsupported scalar column");
    }
}

}  // namespace filter_internal

nlohmann::json filter_timeseries(sdk::PluginProjectAccess &access,
                                 const nlohmann::json &parameters) {
    using filter_internal::make_output_batch;
    using filter_internal::required_timeseries_input;
    using filter_internal::scalar_cell;
    using filter_internal::string_parameter;

    const auto &input = required_timeseries_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto start_time = string_parameter(parameters, "start_time");
    const auto end_time = string_parameter(parameters, "end_time");
    const auto start_key = filter_internal::timestamp_key(start_time);
    const auto end_key = filter_internal::timestamp_key(end_time);
    if (!start_time.empty() && !end_time.empty() && start_key >= end_key)
        throw std::invalid_argument("start_time must be earlier than end_time");

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    const std::vector<std::string> columns{
        "source_id",       "signal_id",       "event_time", "received_at",
        "time_origin",     "value_type",      "value_double", "value_int",
        "value_bool",      "value_text",      "value_json", "unit",
        "quality",         "status_code",     "capture_id", "event_id"};

    constexpr std::uint64_t rows_per_batch = 1024;
    auto output = make_output_batch();
    std::uint64_t rows_selected = 0;
    std::uint64_t rows_seen = 0;
    auto flush = [&]() {
        if (output.row_count() == 0) return;
        output.emit(access, "sensorTimeseriesTable");
        output = make_output_batch();
    };

    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        const auto event_index = batch.column_index("event_time");
        std::vector<std::uint32_t> indices;
        indices.reserve(columns.size());
        for (const auto &column : columns) indices.push_back(batch.column_index(column));
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++rows_seen) {
            if (access.is_cancelled())
                throw std::runtime_error("sensor time-series filtering cancelled");

            bool selected = true;
            if (!start_time.empty() || !end_time.empty()) {
                if (batch.is_null(event_index, row)) {
                    selected = false;
                } else {
                    const auto event_time = filter_internal::timestamp_key(
                        std::string(batch.text(event_index, row)));
                    selected = (start_time.empty() || event_time >= start_key) &&
                               (end_time.empty() || event_time < end_key);
                }
            }
            if (selected) {
                std::vector<sdk::TableBatchBuilder::Cell> values;
                values.reserve(indices.size());
                for (const auto column : indices)
                    values.push_back(scalar_cell(batch, column, row));
                output.append_row(values);
                ++rows_selected;
                if (output.row_count() == rows_per_batch) flush();
            }
            access.report_progress(
                input_rows == 0 ? 1.0
                                : static_cast<double>(rows_seen + 1) /
                                      static_cast<double>(input_rows),
                "Filtering sensor observations");
        }
    });
    flush();
    access.report_progress(1.0, "Sensor time-series filtering completed");
    return nlohmann::json{{"contract", "SENSOR_TIMESERIES"},
                          {"input_rows", input_rows},
                          {"rows_selected", rows_selected},
                          {"result_ref", result_ref},
                          {"start_time", start_time},
                          {"end_time", end_time}};
}

}  // namespace streamfind::sensors::operations::detail
