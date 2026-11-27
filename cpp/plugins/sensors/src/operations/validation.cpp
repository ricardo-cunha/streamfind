#include "streamfind/sdk/plugin_project_access.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace streamfind::sensors::operations::detail {
namespace validation_internal {

using Json = nlohmann::json;

const Json &required_timeseries_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorTimeseriesTable"))
        throw std::invalid_argument(
            "validate_timeseries requires a sensorTimeseriesTable connection");
    const auto &input = inputs->at("sensorTimeseriesTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorTimeseriesTable must identify a persisted table");
    return input;
}

bool is_allowed_quality(std::string_view quality) {
    return quality == "good" || quality == "uncertain" || quality == "bad" ||
           quality == "missing";
}

bool is_allowed_time_origin(std::string_view origin) {
    return origin == "source" || origin == "server" || origin == "broker" ||
           origin == "receiver";
}

bool is_null(const sdk::PluginProjectAccess::BatchView &batch,
             std::uint32_t column, std::uint64_t row) {
    return batch.is_null(column, row);
}

std::string text(const sdk::PluginProjectAccess::BatchView &batch,
                 std::uint32_t column, std::uint64_t row) {
    return std::string(batch.text(column, row));
}

}  // namespace validation_internal

nlohmann::json validate_timeseries(sdk::PluginProjectAccess &access,
                                   const nlohmann::json &parameters) {
    using namespace validation_internal;

    const auto &input = required_timeseries_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    access.require_table(result_ref);

    const std::vector<std::string> columns{
        "source_id",       "signal_id",       "event_time", "received_at",
        "time_origin",     "value_type",      "value_double", "value_int",
        "value_bool",      "value_text",      "value_json", "unit",
        "quality",         "status_code",     "capture_id", "event_id"};

    std::uint64_t rows = 0;
    std::uint64_t invalid_rows = 0;
    std::uint64_t missing_value_rows = 0;
    std::uint64_t good_rows = 0;
    std::uint64_t uncertain_rows = 0;
    std::uint64_t bad_rows = 0;
    std::uint64_t missing_quality_rows = 0;
    std::uint64_t duplicate_timestamp_rows = 0;
    std::uint64_t out_of_order_rows = 0;
    std::uint64_t empty_identity_rows = 0;
    std::uint64_t invalid_quality_rows = 0;
    std::uint64_t invalid_time_origin_rows = 0;
    std::uint64_t invalid_typed_value_rows = 0;
    std::set<std::string> signals;
    std::map<std::string, std::string> last_event_time;

    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        const auto source_index = batch.column_index("source_id");
        const auto signal_index = batch.column_index("signal_id");
        const auto event_index = batch.column_index("event_time");
        const auto received_index = batch.column_index("received_at");
        const auto origin_index = batch.column_index("time_origin");
        const auto value_type_index = batch.column_index("value_type");
        const auto value_double_index = batch.column_index("value_double");
        const auto value_int_index = batch.column_index("value_int");
        const auto value_bool_index = batch.column_index("value_bool");
        const auto value_text_index = batch.column_index("value_text");
        const auto value_json_index = batch.column_index("value_json");
        const auto quality_index = batch.column_index("quality");

        for (std::uint64_t row = 0; row < batch.row_count(); ++row) {
            ++rows;
            if (access.is_cancelled())
                throw std::runtime_error("sensor time-series validation cancelled");

            bool invalid = false;
            if (is_null(batch, source_index, row) ||
                is_null(batch, signal_index, row) ||
                text(batch, source_index, row).empty() ||
                text(batch, signal_index, row).empty()) {
                ++empty_identity_rows;
                invalid = true;
            }

            const auto signal = is_null(batch, signal_index, row)
                                    ? std::string{}
                                    : text(batch, signal_index, row);
            const auto event_time = is_null(batch, event_index, row)
                                        ? std::string{}
                                        : text(batch, event_index, row);
            if (is_null(batch, event_index, row) || event_time.empty() ||
                is_null(batch, received_index, row))
                invalid = true;

            if (!signal.empty()) {
                signals.insert(signal);
                const auto previous = last_event_time.find(signal);
                if (previous != last_event_time.end()) {
                    if (event_time == previous->second) {
                        ++duplicate_timestamp_rows;
                        invalid = true;
                    } else if (event_time < previous->second) {
                        ++out_of_order_rows;
                        invalid = true;
                    }
                }
                last_event_time[signal] = event_time;
            }

            if (is_null(batch, origin_index, row) ||
                !is_allowed_time_origin(text(batch, origin_index, row))) {
                ++invalid_time_origin_rows;
                invalid = true;
            }

            const auto quality = is_null(batch, quality_index, row)
                                     ? std::string{}
                                     : text(batch, quality_index, row);
            if (quality == "good")
                ++good_rows;
            else if (quality == "uncertain")
                ++uncertain_rows;
            else if (quality == "bad")
                ++bad_rows;
            else if (quality == "missing")
                ++missing_quality_rows;
            else {
                ++invalid_quality_rows;
                invalid = true;
            }

            const auto value_type = is_null(batch, value_type_index, row)
                                        ? std::string{}
                                        : text(batch, value_type_index, row);
            const bool has_double = !is_null(batch, value_double_index, row);
            const bool has_int = !is_null(batch, value_int_index, row);
            const bool has_bool = !is_null(batch, value_bool_index, row);
            const bool has_text = !is_null(batch, value_text_index, row);
            const bool has_json = !is_null(batch, value_json_index, row);
            const auto typed_values = static_cast<int>(has_double) +
                                      static_cast<int>(has_int) +
                                      static_cast<int>(has_bool) +
                                      static_cast<int>(has_text) +
                                      static_cast<int>(has_json);
            bool value_valid = true;
            if (value_type == "missing") {
                if (typed_values != 0) value_valid = false;
                ++missing_value_rows;
            } else if (value_type == "real") {
                value_valid = has_double && typed_values == 1;
            } else if (value_type == "integer") {
                value_valid = has_int && typed_values == 1;
            } else if (value_type == "boolean") {
                value_valid = has_bool && typed_values == 1;
            } else if (value_type == "string") {
                value_valid = has_text && typed_values == 1;
            } else if (value_type == "json") {
                value_valid = has_json && typed_values == 1;
            } else {
                value_valid = false;
            }
            if (!value_valid) {
                ++invalid_typed_value_rows;
                invalid = true;
            }
            if ((quality == "missing") != (value_type == "missing"))
                invalid = true;
            if (invalid) ++invalid_rows;
            access.report_progress(
                batch.row_count() == 0
                    ? 1.0
                    : static_cast<double>(row + 1) /
                          static_cast<double>(batch.row_count()),
                "Validating sensor observations");
        }
    });

    const auto report = Json{
        {"contract", "SENSOR_TIMESERIES_V1"},
        {"result_ref", result_ref},
        {"valid", invalid_rows == 0},
        {"rows", rows},
        {"signals", signals.size()},
        {"missing_value_rows", missing_value_rows},
        {"quality_rows",
         {{"good", good_rows},
          {"uncertain", uncertain_rows},
          {"bad", bad_rows},
          {"missing", missing_quality_rows}}},
        {"duplicate_timestamp_rows", duplicate_timestamp_rows},
        {"out_of_order_rows", out_of_order_rows},
        {"invalid_rows", invalid_rows},
        {"issues",
         {{"empty_identity_rows", empty_identity_rows},
          {"invalid_time_origin_rows", invalid_time_origin_rows},
          {"invalid_quality_rows", invalid_quality_rows},
          {"invalid_typed_value_rows", invalid_typed_value_rows}}}};
    access.emit_result("sensorValidationReport", report.dump());
    access.report_progress(1.0, "Sensor time-series validation completed");
    return report;
}

}  // namespace streamfind::sensors::operations::detail
