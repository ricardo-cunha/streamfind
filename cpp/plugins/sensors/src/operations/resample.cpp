#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace resample_internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

const Json &required_timeseries_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorTimeseriesTable"))
        throw std::invalid_argument(
            "resample_timeseries requires a sensorTimeseriesTable connection");
    const auto &input = inputs->at("sensorTimeseriesTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorTimeseriesTable must identify a persisted table");
    return input;
}

std::string string_parameter(const Json &parameters, const char *name,
                             const std::string &default_value = {}) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->is_null()) return default_value;
    if (!found->is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

std::int64_t number(const std::string &text, std::size_t begin,
                    std::size_t count) {
    if (begin + count > text.size()) throw std::invalid_argument("invalid timestamp");
    std::int64_t value = 0;
    for (std::size_t index = begin; index < begin + count; ++index) {
        if (text[index] < '0' || text[index] > '9')
            throw std::invalid_argument("invalid timestamp");
        value = value * 10 + (text[index] - '0');
    }
    return value;
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
    const auto day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const auto days = era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
    return ((days * 24 + hour) * 60 + minute) * 60 * 1000000 +
           second * 1000000 + fraction;
}

std::string format_timestamp_microseconds(std::int64_t micros) {
    constexpr std::int64_t per_second = 1000000;
    const auto seconds = micros / per_second;
    const auto fraction = micros % per_second;
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) throw std::runtime_error("failed to format timestamp");
#else
    if (gmtime_r(&time, &utc) == nullptr) throw std::runtime_error("failed to format timestamp");
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << fraction << 'Z';
    return output.str();
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

struct GroupKey {
    std::string source_id;
    std::string signal_id;
    std::int64_t bucket_micros = 0;
    bool operator<(const GroupKey &other) const {
        return std::tie(source_id, signal_id, bucket_micros) <
               std::tie(other.source_id, other.signal_id, other.bucket_micros);
    }
};

struct Group {
    Observation latest;
    double numeric_sum = 0.0;
    std::uint64_t numeric_count = 0;
    int quality_rank = 0;
    std::string quality;
};

int quality_rank(const std::string &quality) {
    if (quality == "bad") return 4;
    if (quality == "uncertain") return 3;
    if (quality == "missing") return 2;
    if (quality == "good") return 1;
    throw std::invalid_argument("SENSOR_TIMESERIES quality is invalid");
}

std::optional<double> numeric_value(const sdk::PluginProjectAccess::BatchView &batch,
                                    std::uint32_t value_type, std::uint32_t value_double,
                                    std::uint32_t value_int, std::uint64_t row) {
    const auto type = text(batch, value_type, row, "value_type");
    if (type == "real" && !batch.is_null(value_double, row))
        return batch.real(value_double, row);
    if (type == "integer" && !batch.is_null(value_int, row))
        return static_cast<double>(batch.integer(value_int, row));
    if (type != "missing" && type != "real" && type != "integer" &&
        type != "boolean" && type != "string" && type != "json")
        throw std::invalid_argument("SENSOR_TIMESERIES value_type is invalid");
    return std::nullopt;
}

Observation read_observation(const sdk::PluginProjectAccess::BatchView &batch,
                             const std::vector<std::uint32_t> &index,
                             std::uint64_t row) {
    Observation value;
    value.source_id = text(batch, index[0], row, "source_id");
    value.signal_id = text(batch, index[1], row, "signal_id");
    const auto event_text = text(batch, index[2], row, "event_time");
    value.event_micros = parse_timestamp_microseconds(event_text);
    value.received_at = text(batch, index[3], row, "received_at");
    value.time_origin = text(batch, index[4], row, "time_origin");
    value.value_type = text(batch, index[5], row, "value_type");
    if (!batch.is_null(index[6], row)) value.value_double = batch.real(index[6], row);
    if (!batch.is_null(index[7], row)) value.value_int = batch.integer(index[7], row);
    if (!batch.is_null(index[8], row)) value.value_bool = batch.boolean(index[8], row);
    if (!batch.is_null(index[9], row)) value.value_text = std::string(batch.text(index[9], row));
    if (!batch.is_null(index[10], row)) value.value_json = std::string(batch.text(index[10], row));
    if (!batch.is_null(index[11], row)) value.unit = std::string(batch.text(index[11], row));
    value.quality = text(batch, index[12], row, "quality");
    if (!batch.is_null(index[13], row)) value.status_code = std::string(batch.text(index[13], row));
    value.capture_id = text(batch, index[14], row, "capture_id");
    value.event_id = text(batch, index[15], row, "event_id");
    return value;
}

std::vector<Cell> output_row(const GroupKey &key, Group group,
                             const std::string &aggregation) {
    const auto bucket_text = format_timestamp_microseconds(key.bucket_micros);
    if (aggregation == "mean") {
        if (group.numeric_count > 0) {
            group.latest.value_type = "real";
            group.latest.value_double = group.numeric_sum /
                                        static_cast<double>(group.numeric_count);
            group.latest.value_int.reset();
            group.latest.value_bool.reset();
            group.latest.value_text.reset();
            group.latest.value_json.reset();
            if (group.quality == "missing") group.quality = "uncertain";
        } else {
            group.latest.value_type = "missing";
            group.latest.value_double.reset();
            group.latest.value_int.reset();
            group.latest.value_bool.reset();
            group.latest.value_text.reset();
            group.latest.value_json.reset();
            group.quality = "missing";
        }
    }
    group.latest.event_id = group.latest.capture_id + ":resampled:" +
                            group.latest.signal_id + ":" + bucket_text;
    group.latest.quality = group.quality;
    return {group.latest.source_id, group.latest.signal_id, bucket_text,
            group.latest.received_at, group.latest.time_origin, group.latest.value_type,
            group.latest.value_double.has_value() ? Cell(*group.latest.value_double) : Cell(std::monostate{}),
            group.latest.value_int.has_value() ? Cell(*group.latest.value_int) : Cell(std::monostate{}),
            group.latest.value_bool.has_value() ? Cell(*group.latest.value_bool) : Cell(std::monostate{}),
            group.latest.value_text.has_value() ? Cell(*group.latest.value_text) : Cell(std::monostate{}),
            group.latest.value_json.has_value() ? Cell(*group.latest.value_json) : Cell(std::monostate{}),
            group.latest.unit.has_value() ? Cell(*group.latest.unit) : Cell(std::monostate{}),
            group.latest.quality,
            group.latest.status_code.has_value() ? Cell(*group.latest.status_code) : Cell(std::monostate{}),
            group.latest.capture_id, group.latest.event_id};
}

}  // namespace resample_internal

nlohmann::json resample_timeseries(sdk::PluginProjectAccess &access,
                                   const nlohmann::json &parameters) {
    using namespace resample_internal;
    const auto &input = required_timeseries_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto window_value = parameters.value("window_seconds", 60.0);
    if (!parameters.contains("window_seconds") || !parameters.at("window_seconds").is_number() ||
        !std::isfinite(window_value) || window_value <= 0.0)
        throw std::invalid_argument("window_seconds must be a positive number");
    const auto window_micros = static_cast<std::int64_t>(std::llround(window_value * 1000000.0));
    if (window_micros <= 0) throw std::invalid_argument("window_seconds is below microsecond precision");
    const auto aggregation = string_parameter(parameters, "aggregation", "mean");
    if (aggregation != "mean" && aggregation != "last")
        throw std::invalid_argument("aggregation must be mean or last");

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    const std::vector<std::string> columns{
        "source_id", "signal_id", "event_time", "received_at", "time_origin",
        "value_type", "value_double", "value_int", "value_bool", "value_text",
        "value_json", "unit", "quality", "status_code", "capture_id", "event_id"};
    std::map<GroupKey, Group> groups;
    std::uint64_t rows_seen = 0;
    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        std::vector<std::uint32_t> indices;
        indices.reserve(columns.size());
        for (const auto &column : columns) indices.push_back(batch.column_index(column));
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++rows_seen) {
            if (access.is_cancelled()) throw std::runtime_error("sensor resampling cancelled");
            auto observation = read_observation(batch, indices, row);
            const auto bucket = (observation.event_micros / window_micros) * window_micros;
            GroupKey key{observation.source_id, observation.signal_id, bucket};
            auto found = groups.find(key);
            if (found == groups.end()) {
                found = groups.emplace(key, Group{observation, 0.0, 0, 0, {}}).first;
                found->second.quality_rank = quality_rank(observation.quality);
                found->second.quality = observation.quality;
            }
            auto &group = found->second;
            if (observation.event_micros >= group.latest.event_micros)
                group.latest = observation;
            const auto current_rank = quality_rank(observation.quality);
            if (current_rank > group.quality_rank) {
                group.quality_rank = current_rank;
                group.quality = observation.quality;
            }
            const auto numeric = numeric_value(batch, indices[5], indices[6], indices[7], row);
            if (numeric.has_value()) {
                group.numeric_sum += *numeric;
                ++group.numeric_count;
            }
            access.report_progress(
                input_rows == 0 ? 1.0 : static_cast<double>(rows_seen + 1) /
                    static_cast<double>(input_rows), "Grouping sensor observations");
        }
    });

    auto output = make_output_batch();
    std::uint64_t rows_emitted = 0;
    for (const auto &[key, group] : groups) {
        if (access.is_cancelled()) throw std::runtime_error("sensor resampling cancelled");
        output.append_row(output_row(key, group, aggregation));
        if (output.row_count() == 1024) {
            rows_emitted += output.row_count();
            output.emit(access, "sensorTimeseriesTable");
            output = make_output_batch();
        }
    }
    if (output.row_count() > 0) {
        rows_emitted += output.row_count();
        output.emit(access, "sensorTimeseriesTable");
    }
    access.report_progress(1.0, "Sensor time-series resampling completed");
    return Json{{"contract", "SENSOR_TIMESERIES"}, {"input_rows", input_rows},
                {"rows_emitted", rows_emitted}, {"window_seconds", window_value},
                {"aggregation", aggregation}, {"result_ref", result_ref}};
}

}  // namespace streamfind::sensors::operations::detail
