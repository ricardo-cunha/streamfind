#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace analysis_internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

const Json &required_timeseries_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorTimeseriesTable"))
        throw std::invalid_argument(
            "sensor analysis requires a sensorTimeseriesTable connection");
    const auto &input = inputs->at("sensorTimeseriesTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorTimeseriesTable must identify a persisted table");
    return input;
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
                 std::uint32_t column, std::uint64_t row,
                 const char *name) {
    if (batch.is_null(column, row))
        throw std::invalid_argument(std::string("SENSOR_TIMESERIES column is null: ") + name);
    return std::string(batch.text(column, row));
}

struct Observation {
    std::string source_id;
    std::string signal_id;
    std::string event_time;
    std::int64_t event_micros = 0;
    std::string received_at;
    std::string time_origin;
    std::string value_type;
    std::optional<double> value_double;
    std::optional<std::int64_t> value_int;
    std::optional<bool> value_bool;
    std::optional<std::string> value_text;
    std::optional<std::string> value_json;
    std::optional<std::string> unit;
    std::string quality;
    std::optional<std::string> status_code;
    std::string capture_id;
    std::string event_id;
};

std::int64_t number(const std::string &value, std::size_t begin,
                   std::size_t count) {
    if (begin + count > value.size()) throw std::invalid_argument("invalid timestamp");
    std::int64_t result = 0;
    for (std::size_t index = begin; index < begin + count; ++index) {
        if (value[index] < '0' || value[index] > '9')
            throw std::invalid_argument("invalid timestamp");
        result = result * 10 + (value[index] - '0');
    }
    return result;
}

std::int64_t parse_timestamp_microseconds(const std::string &value) {
    if (value.size() < 19 || (value[10] != 'T' && value[10] != ' ') ||
        value[4] != '-' || value[7] != '-' || value[13] != ':' ||
        value[16] != ':')
        throw std::invalid_argument("SENSOR_TIMESERIES event_time is not ISO-8601");
    const auto year = number(value, 0, 4);
    const auto month = number(value, 5, 2);
    const auto day = number(value, 8, 2);
    const auto hour = number(value, 11, 2);
    const auto minute = number(value, 14, 2);
    const auto second = number(value, 17, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 59)
        throw std::invalid_argument("SENSOR_TIMESERIES event_time is invalid");
    std::int64_t fraction = 0;
    std::size_t position = 19;
    std::size_t digits = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size() && value[position] >= '0' &&
               value[position] <= '9') {
            if (digits == 6)
                throw std::invalid_argument("event_time precision exceeds microseconds");
            fraction = fraction * 10 + (value[position++] - '0');
            ++digits;
        }
        if (digits == 0) throw std::invalid_argument("event_time fraction is empty");
        while (digits++ < 6) fraction *= 10;
    }
    if (position < value.size() && value[position] == 'Z') ++position;
    if (position != value.size())
        throw std::invalid_argument("SENSOR_TIMESERIES event_time must be UTC");
    const auto adjusted_year = year - (month <= 2);
    const auto era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const auto year_of_era = static_cast<std::uint32_t>(adjusted_year - era * 400);
    const auto day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto day_of_era = year_of_era * 365 + year_of_era / 4 -
                            year_of_era / 100 + day_of_year;
    const auto days = era * 146097 + static_cast<std::int64_t>(day_of_era) -
                      719468;
    return ((days * 24 + hour) * 60 + minute) * 60 * 1000000 +
           second * 1000000 + fraction;
}

Observation read_observation(const sdk::PluginProjectAccess::BatchView &batch,
                             const std::vector<std::uint32_t> &index,
                             std::uint64_t row) {
    Observation value;
    value.source_id = text(batch, index[0], row, "source_id");
    value.signal_id = text(batch, index[1], row, "signal_id");
    value.event_time = text(batch, index[2], row, "event_time");
    value.event_micros = parse_timestamp_microseconds(value.event_time);
    value.received_at = text(batch, index[3], row, "received_at");
    value.time_origin = text(batch, index[4], row, "time_origin");
    value.value_type = text(batch, index[5], row, "value_type");
    if (value.value_type != "real" && value.value_type != "integer" &&
        value.value_type != "boolean" && value.value_type != "string" &&
        value.value_type != "json" && value.value_type != "missing")
        throw std::invalid_argument("SENSOR_TIMESERIES value_type is invalid");
    if (!batch.is_null(index[6], row)) value.value_double = batch.real(index[6], row);
    if (!batch.is_null(index[7], row)) value.value_int = batch.integer(index[7], row);
    if (!batch.is_null(index[8], row)) value.value_bool = batch.boolean(index[8], row);
    if (!batch.is_null(index[9], row)) value.value_text = std::string(batch.text(index[9], row));
    if (!batch.is_null(index[10], row)) value.value_json = std::string(batch.text(index[10], row));
    if (!batch.is_null(index[11], row)) value.unit = std::string(batch.text(index[11], row));
    value.quality = text(batch, index[12], row, "quality");
    if (value.quality != "good" && value.quality != "uncertain" &&
        value.quality != "bad" && value.quality != "missing")
        throw std::invalid_argument("SENSOR_TIMESERIES quality is invalid");
    if (!batch.is_null(index[13], row)) value.status_code = std::string(batch.text(index[13], row));
    value.capture_id = text(batch, index[14], row, "capture_id");
    value.event_id = text(batch, index[15], row, "event_id");
    return value;
}

std::optional<double> numeric_value(const Observation &observation) {
    if (observation.quality == "bad" || observation.quality == "missing")
        return std::nullopt;
    if (observation.value_type == "real" && observation.value_double.has_value())
        return observation.value_double;
    if (observation.value_type == "integer" && observation.value_int.has_value())
        return static_cast<double>(*observation.value_int);
    return std::nullopt;
}

std::vector<Cell> output_row(const Observation &observation,
                             const std::string &signal_id,
                             const std::string &value_type,
                             const std::optional<double> &value_double,
                             const std::optional<std::string> &value_json,
                             const std::string &quality,
                             const std::string &status_code,
                             const std::string &event_id) {
    return {observation.source_id,
            signal_id,
            observation.event_time,
            observation.received_at,
            observation.time_origin,
            value_type,
            value_double.has_value() ? Cell(*value_double) : Cell(std::monostate{}),
            Cell(std::monostate{}),
            Cell(std::monostate{}),
            Cell(std::monostate{}),
            value_json.has_value() ? Cell(*value_json) : Cell(std::monostate{}),
            observation.unit.has_value() ? Cell(*observation.unit) : Cell(std::monostate{}),
            quality,
            status_code,
            observation.capture_id,
            event_id};
}

std::vector<std::string> input_columns() {
    return {"source_id", "signal_id", "event_time", "received_at",
            "time_origin", "value_type", "value_double", "value_int",
            "value_bool", "value_text", "value_json", "unit", "quality",
            "status_code", "capture_id", "event_id"};
}

void read_series(sdk::PluginProjectAccess &access, const std::string &result_ref,
                 std::map<std::pair<std::string, std::string>,
                          std::vector<Observation>> &series,
                 std::uint64_t input_rows, const std::string &progress_message) {
    std::uint64_t rows_seen = 0;
    const auto columns = input_columns();
    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        std::vector<std::uint32_t> indices;
        indices.reserve(columns.size());
        for (const auto &column : columns) indices.push_back(batch.column_index(column));
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++rows_seen) {
            if (access.is_cancelled())
                throw std::runtime_error("sensor analysis cancelled");
            auto observation = read_observation(batch, indices, row);
            series[{observation.source_id, observation.signal_id}].push_back(
                std::move(observation));
            access.report_progress(
                input_rows == 0 ? 1.0
                                : static_cast<double>(rows_seen + 1) /
                                      static_cast<double>(input_rows),
                progress_message);
        }
    });
    for (auto &[key, observations] : series) {
        std::sort(observations.begin(), observations.end(),
                  [](const Observation &left, const Observation &right) {
                      return std::tie(left.event_micros, left.event_id) <
                             std::tie(right.event_micros, right.event_id);
                  });
    }
}

double required_number(const Json &parameters, const char *name) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || !found->is_number() ||
        !std::isfinite(found->get<double>()))
        throw std::invalid_argument(std::string(name) + " must be a finite number");
    return found->get<double>();
}

double optional_number(const Json &parameters, const char *name, double default_value) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_number() || !std::isfinite(found->get<double>()))
        throw std::invalid_argument(std::string(name) + " must be a finite number");
    return found->get<double>();
}

std::string optional_string(const Json &parameters, const char *name,
                            const std::string &default_value) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

}  // namespace analysis_internal

nlohmann::json rolling_statistics(sdk::PluginProjectAccess &access,
                                  const nlohmann::json &parameters) {
    using namespace analysis_internal;
    const auto &input = required_timeseries_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto window_seconds =
        optional_number(parameters, "rolling_window_seconds", 60.0);
    if (window_seconds <= 0.0)
        throw std::invalid_argument("rolling_window_seconds must be positive");
    const auto statistic =
        optional_string(parameters, "rolling_statistic", "mean");
    if (statistic != "mean" && statistic != "min" && statistic != "max" &&
        statistic != "stddev")
        throw std::invalid_argument(
            "rolling_statistic must be mean, min, max, or stddev");
    const auto window_micros = static_cast<std::int64_t>(
        std::llround(window_seconds * 1000000.0));
    if (window_micros <= 0)
        throw std::invalid_argument(
            "rolling_window_seconds is below microsecond precision");

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    std::map<std::pair<std::string, std::string>, std::vector<Observation>> series;
    read_series(access, result_ref, series, input_rows,
                "Reading sensor observations for rolling statistics");

    auto output = make_output_batch();
    std::uint64_t rows_emitted = 0;
    for (const auto &[key, observations] : series) {
        struct WindowEntry {
            std::int64_t event_micros;
            std::optional<double> value;
            std::string quality;
        };
        std::deque<WindowEntry> window;
        std::multiset<double> values;
        double sum = 0.0;
        double sum_squares = 0.0;
        std::uint64_t uncertain = 0;
        for (const auto &observation : observations) {
            const auto value = numeric_value(observation);
            window.push_back({observation.event_micros, value, observation.quality});
            if (value.has_value()) {
                values.insert(*value);
                sum += *value;
                sum_squares += *value * *value;
            }
            if (observation.quality != "good") ++uncertain;
            const auto cutoff = observation.event_micros - window_micros;
            while (!window.empty() && window.front().event_micros < cutoff) {
                const auto &expired = window.front();
                if (expired.value.has_value()) {
                    const auto found = values.find(*expired.value);
                    if (found != values.end()) values.erase(found);
                    sum -= *expired.value;
                    sum_squares -= *expired.value * *expired.value;
                }
                if (expired.quality != "good") --uncertain;
                window.pop_front();
            }

            std::optional<double> derived;
            if (!values.empty()) {
                if (statistic == "mean") {
                    derived = sum / static_cast<double>(values.size());
                } else if (statistic == "min") {
                    derived = *values.begin();
                } else if (statistic == "max") {
                    derived = *values.rbegin();
                } else {
                    const auto mean = sum / static_cast<double>(values.size());
                    const auto variance = std::max(
                        0.0, sum_squares / static_cast<double>(values.size()) -
                                  mean * mean);
                    derived = std::sqrt(variance);
                }
            }
            const auto quality = derived.has_value()
                                     ? (uncertain == 0 ? "good" : "uncertain")
                                     : "missing";
            const auto status = derived.has_value() ? "derived" : "no_numeric_value";
            output.append_row(output_row(
                observation, observation.signal_id + "::rolling_" + statistic,
                derived.has_value() ? "real" : "missing", derived, std::nullopt,
                quality, status, observation.event_id + ":rolling:" + statistic));
            ++rows_emitted;
            if (output.row_count() == 1024) {
                output.emit(access, "sensorTimeseriesTable");
                output = make_output_batch();
            }
        }
    }
    if (output.row_count() > 0) output.emit(access, "sensorTimeseriesTable");
    access.report_progress(1.0, "Sensor rolling statistics completed");
    return Json{{"contract", "SENSOR_TIMESERIES"},
                {"input_rows", input_rows},
                {"rows_emitted", rows_emitted},
                {"window_seconds", window_seconds},
                {"statistic", statistic}};
}

}  // namespace streamfind::sensors::operations::detail
