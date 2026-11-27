#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {

namespace internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

std::string string_parameter(const Json &parameters, const char *name,
                             const std::string &default_value = {}) {
    const auto found = parameters.find(name);
    if (found == parameters.end()) return default_value;
    if (!found->is_string()) throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

std::vector<std::string> string_array_parameter(const Json &parameters, const char *name) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return {};
    if (!found->is_array()) throw std::invalid_argument(std::string(name) + " must be an array");
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto &item : *found) {
        if (!item.is_string() || item.get<std::string>().empty())
            throw std::invalid_argument(std::string(name) + " must contain non-empty strings");
        if (std::find(values.begin(), values.end(), item.get<std::string>()) != values.end())
            throw std::invalid_argument(std::string(name) + " must not contain duplicate columns");
        values.push_back(item.get<std::string>());
    }
    return values;
}

const Json &required_table_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() || !inputs->contains("tableInput"))
        throw std::invalid_argument("sensor normalization requires a tableInput connection");
    const auto &input = inputs->at("tableInput");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() || input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument("tableInput must identify a persisted table");
    return input;
}

void add_column(std::vector<std::string> &columns, const std::string &name) {
    if (name.empty()) return;
    if (std::find(columns.begin(), columns.end(), name) == columns.end())
        columns.push_back(name);
}

std::string scalar_text(const sdk::PluginProjectAccess::BatchView &batch,
                        std::uint32_t column, std::uint64_t row,
                        const std::string &name) {
    if (batch.is_null(column, row))
        throw std::invalid_argument("input column " + name + " is null");
    switch (batch.type(column)) {
    case STREAMFIND_PLUGIN_COLUMN_UTF8:
    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
        return std::string(batch.text(column, row));
    case STREAMFIND_PLUGIN_COLUMN_INT64:
        return std::to_string(batch.integer(column, row));
    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
        return std::to_string(batch.real(column, row));
    case STREAMFIND_PLUGIN_COLUMN_BOOL:
        return batch.boolean(column, row) ? "true" : "false";
    default:
        throw std::invalid_argument("input column " + name + " must be a scalar value");
    }
}

std::optional<std::string> optional_scalar_text(
    const sdk::PluginProjectAccess::BatchView &batch,
    const std::optional<std::uint32_t> &column, std::uint64_t row,
    const std::string &name) {
    if (!column.has_value() || batch.is_null(*column, row)) return std::nullopt;
    return scalar_text(batch, *column, row, name);
}

std::string capture_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(micros);
    const auto fraction = micros - seconds;
    const auto time = static_cast<std::time_t>(seconds.count());
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) throw std::runtime_error("failed to format capture timestamp");
#else
    if (gmtime_r(&time, &utc) == nullptr) throw std::runtime_error("failed to format capture timestamp");
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << fraction.count() << 'Z';
    return output.str();
}

std::string normalized_quality(const std::optional<std::string> &value,
                               bool value_available) {
    if (!value.has_value() || value->empty()) return value_available ? "good" : "missing";
    auto quality = *value;
    std::transform(quality.begin(), quality.end(), quality.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (quality != "good" && quality != "uncertain" && quality != "bad" && quality != "missing")
        throw std::invalid_argument("quality must be good, uncertain, bad, or missing");
    return quality;
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

}  // namespace internal

nlohmann::json normalize_sensor_table(sdk::PluginProjectAccess &access,
                                      const nlohmann::json &parameters) {
    using internal::Cell;
    using internal::add_column;
    using internal::capture_timestamp;
    using internal::make_output_batch;
    using internal::normalized_quality;
    using internal::optional_scalar_text;
    using internal::required_table_input;
    using internal::scalar_text;
    using internal::string_array_parameter;
    using internal::string_parameter;
    const auto &input = required_table_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto layout = string_parameter(parameters, "layout", "long");
    if (layout != "long" && layout != "wide")
        throw std::invalid_argument("layout must be long or wide");
    const bool wide_layout = layout == "wide";
    const auto source_column = string_parameter(parameters, "source_id_column", "source_id");
    const auto signal_column = string_parameter(parameters, "signal_id_column", "signal_id");
    const auto event_column = string_parameter(parameters, "event_time_column", "event_time");
    const auto value_column = string_parameter(parameters, "value_column", "value");
    const auto variable_columns = string_array_parameter(parameters, "variable_columns");
    const auto received_column = string_parameter(parameters, "received_at_column");
    const auto unit_column = string_parameter(parameters, "unit_column");
    const auto quality_column = string_parameter(parameters, "quality_column");
    const auto status_column = string_parameter(parameters, "status_code_column");
    const auto time_origin = string_parameter(parameters, "time_origin", "source");
    const auto capture_id = string_parameter(parameters, "capture_id",
                                             std::string(access.operation_instance()));
    if (source_column.empty() || signal_column.empty() || event_column.empty() || value_column.empty())
        if (!wide_layout)
            throw std::invalid_argument("source, signal, event-time, and value column names are required for long layout");
    if (source_column.empty() || event_column.empty())
        throw std::invalid_argument("source and event-time column names are required");
    if (wide_layout && variable_columns.empty())
        throw std::invalid_argument("variable_columns must contain at least one column for wide layout");
    if (time_origin != "source" && time_origin != "server" &&
        time_origin != "broker" && time_origin != "receiver")
        throw std::invalid_argument("time_origin must be source, server, broker, or receiver");

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    if (input_rows == 0) throw std::invalid_argument("sensor normalization input table contains no rows");

    std::vector<std::string> columns;
    add_column(columns, source_column);
    add_column(columns, event_column);
    if (wide_layout) {
        for (const auto &column : variable_columns) add_column(columns, column);
    } else {
        add_column(columns, signal_column);
        add_column(columns, value_column);
    }
    add_column(columns, received_column);
    add_column(columns, unit_column);
    add_column(columns, quality_column);
    add_column(columns, status_column);

    const auto fallback_received_at = capture_timestamp();
    auto output = make_output_batch();
    constexpr std::uint64_t rows_per_batch = 1024;
    std::uint64_t rows_emitted = 0;
    std::uint64_t source_row = 0;
    bool configured = false;
    std::uint32_t source_index = 0;
    std::uint32_t signal_index = 0;
    std::uint32_t event_index = 0;
    std::uint32_t value_index = 0;
    std::vector<std::pair<std::string, std::uint32_t>> variable_indices;
    std::optional<std::uint32_t> received_index;
    std::optional<std::uint32_t> unit_index;
    std::optional<std::uint32_t> quality_index;
    std::optional<std::uint32_t> status_index;
    auto flush = [&]() {
        if (output.row_count() == 0) return;
        rows_emitted += output.row_count();
        output.emit(access, "sensorTimeseriesTable");
        output = make_output_batch();
    };

    access.read_typed_batches(result_ref, columns, [&](const sdk::PluginProjectAccess::BatchView &batch) {
        if (!configured) {
            source_index = batch.column_index(source_column);
            event_index = batch.column_index(event_column);
            if (!wide_layout) {
                signal_index = batch.column_index(signal_column);
                value_index = batch.column_index(value_column);
            } else {
                for (const auto &column : variable_columns)
                    variable_indices.emplace_back(column, batch.column_index(column));
            }
            if (!received_column.empty()) received_index = batch.column_index(received_column);
            if (!unit_column.empty()) unit_index = batch.column_index(unit_column);
            if (!quality_column.empty()) quality_index = batch.column_index(quality_column);
            if (!status_column.empty()) status_index = batch.column_index(status_column);
            configured = true;
        }
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++source_row) {
            if (access.is_cancelled()) throw std::runtime_error("sensor normalization cancelled");
            const auto source_id = scalar_text(batch, source_index, row, source_column);
            const auto event_time = scalar_text(batch, event_index, row, event_column);
            const auto received_at = optional_scalar_text(batch, received_index, row, received_column)
                .value_or(fallback_received_at);
            const auto unit = optional_scalar_text(batch, unit_index, row, unit_column);
            const auto status = optional_scalar_text(batch, status_index, row, status_column);
            const auto append_observation = [&](const std::string &signal_id, std::uint32_t observation_index) {
                std::string value_type = "missing";
                Cell value_double = std::monostate{};
                Cell value_int = std::monostate{};
                Cell value_bool = std::monostate{};
                Cell value_text = std::monostate{};
                const bool value_available = !batch.is_null(observation_index, row);
                if (value_available) {
                    switch (batch.type(observation_index)) {
                    case STREAMFIND_PLUGIN_COLUMN_INT64:
                        value_type = "integer";
                        value_int = batch.integer(observation_index, row);
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_FLOAT64:
                    case STREAMFIND_PLUGIN_COLUMN_DECIMAL:
                        value_type = "real";
                        value_double = batch.type(observation_index) == STREAMFIND_PLUGIN_COLUMN_FLOAT64
                            ? Cell(batch.real(observation_index, row))
                            : Cell(std::stod(std::string(batch.text(observation_index, row))));
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_BOOL:
                        value_type = "boolean";
                        value_bool = batch.boolean(observation_index, row);
                        break;
                    case STREAMFIND_PLUGIN_COLUMN_UTF8:
                    case STREAMFIND_PLUGIN_COLUMN_TIMESTAMP:
                        value_type = "string";
                        value_text = std::string(batch.text(observation_index, row));
                        break;
                    default:
                        throw std::invalid_argument("value column must use a scalar supported by SENSOR_TIMESERIES");
                    }
                }
                const auto quality = normalized_quality(
                    optional_scalar_text(batch, quality_index, row, quality_column), value_available);
                const auto event_id = capture_id + ":" + signal_id + ":" + event_time + ":" +
                    std::to_string(source_row);
                output.append_row({source_id, signal_id, event_time, received_at, time_origin,
                                   value_type, value_double, value_int, value_bool, value_text,
                                   Cell(std::monostate{}), unit.has_value() ? Cell(*unit) : Cell(std::monostate{}),
                                   quality, status.has_value() ? Cell(*status) : Cell(std::monostate{}),
                                   capture_id, event_id});
                if (output.row_count() == rows_per_batch) flush();
            };
            if (wide_layout) {
                for (const auto &[signal_id, observation_index] : variable_indices)
                    append_observation(signal_id, observation_index);
            } else {
                const auto signal_id = scalar_text(batch, signal_index, row, signal_column);
                append_observation(signal_id, value_index);
            }
            access.report_progress(static_cast<double>(source_row + 1) /
                                   static_cast<double>(input_rows),
                                   "Normalizing sensor observations");
        }
    });
    flush();
    return nlohmann::json{{"input_rows", input_rows}, {"rows_emitted", rows_emitted},
                          {"result_ref", result_ref}, {"contract", "SENSOR_TIMESERIES"}};
}

}  // namespace streamfind::sensors::operations::detail
