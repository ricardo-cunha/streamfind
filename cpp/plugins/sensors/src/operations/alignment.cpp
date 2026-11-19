#include "streamfind/sdk/plugin_project_access.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::sensors::operations::detail {
namespace alignment_internal {

using Json = nlohmann::json;
using Cell = sdk::TableBatchBuilder::Cell;

const Json &required_timeseries_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() ||
        !inputs->contains("sensorTimeseriesTable"))
        throw std::invalid_argument(
            "align_timeseries requires a sensorTimeseriesTable connection");
    const auto &input = inputs->at("sensorTimeseriesTable");
    if (!input.is_object() || !input.contains("result_ref") ||
        !input.at("result_ref").is_string() ||
        input.at("result_ref").get<std::string>().empty())
        throw std::invalid_argument(
            "sensorTimeseriesTable must identify a persisted table");
    return input;
}

std::int64_t number(const std::string &text, std::size_t begin,
                    std::size_t count) {
    if (begin + count > text.size())
        throw std::invalid_argument("invalid timestamp");
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
        if (digits == 0)
            throw std::invalid_argument("event_time fraction is empty");
        while (digits++ < 6)
            fraction *= 10;
    }
    if (position < value.size() && value[position] == 'Z')
        ++position;
    if (position != value.size())
        throw std::invalid_argument("SENSOR_TIMESERIES event_time must be UTC");
    const auto adjusted_year = year - (month <= 2);
    const auto era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const auto year_of_era = static_cast<std::uint32_t>(adjusted_year - era * 400);
    const auto day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto day_of_era = year_of_era * 365 + year_of_era / 4 -
                            year_of_era / 100 + day_of_year;
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
    if (gmtime_s(&utc, &time) != 0)
        throw std::runtime_error("failed to format timestamp");
#else
    if (gmtime_r(&time, &utc) == nullptr)
        throw std::runtime_error("failed to format timestamp");
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << fraction << 'Z';
    return output.str();
}

sdk::TableBatchBuilder make_output_batch() {
    return sdk::TableBatchBuilder({
        {"source_id", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"reference_signal", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"reference_event_time", STREAMFIND_PLUGIN_COLUMN_TIMESTAMP},
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
        {"alignment_status", STREAMFIND_PLUGIN_COLUMN_UTF8},
        {"time_delta_microseconds", STREAMFIND_PLUGIN_COLUMN_INT64},
    });
}

std::string text(const sdk::PluginProjectAccess::BatchView &batch,
                 std::uint32_t column, std::uint64_t row, const char *name) {
    if (batch.is_null(column, row))
        throw std::invalid_argument(std::string("SENSOR_TIMESERIES column is null: ") + name);
    return std::string(batch.text(column, row));
}

struct Observation {
    std::string source_id;
    std::string signal_id;
    std::int64_t event_micros = 0;
    std::string event_text;
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

using SignalSeries = std::vector<Observation>;
using SourceSignals = std::map<std::string, SignalSeries>;

Observation read_observation(const sdk::PluginProjectAccess::BatchView &batch,
                             const std::vector<std::uint32_t> &index,
                             std::uint64_t row) {
    Observation value;
    value.source_id = text(batch, index[0], row, "source_id");
    value.signal_id = text(batch, index[1], row, "signal_id");
    value.event_text = text(batch, index[2], row, "event_time");
    value.event_micros = parse_timestamp_microseconds(value.event_text);
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

const Observation *nearest(const SignalSeries &series, std::int64_t target,
                           std::int64_t tolerance) {
    const auto found = std::lower_bound(
        series.begin(), series.end(), target,
        [](const Observation &value, std::int64_t time) {
            return value.event_micros < time;
        });
    const Observation *best = nullptr;
    auto consider = [&](const Observation *candidate) {
        if (candidate == nullptr)
            return;
        const auto distance = std::llabs(candidate->event_micros - target);
        if (distance > tolerance)
            return;
        if (best == nullptr || distance < std::llabs(best->event_micros - target) ||
            (distance == std::llabs(best->event_micros - target) &&
             candidate->event_micros < best->event_micros))
            best = candidate;
    };
    if (found != series.end())
        consider(&*found);
    if (found != series.begin())
        consider(&*std::prev(found));
    return best;
}

std::vector<Cell> output_row(const Observation &reference,
                             const std::string &reference_signal,
                             const std::string &signal,
                             const Observation *matched,
                             const std::string &alignment_status) {
    const auto reference_time = format_timestamp_microseconds(reference.event_micros);
    const auto missing = matched == nullptr;
    const auto &value = missing ? reference : *matched;
    const auto event_id = reference.event_id + ":aligned:" + signal;
    return {reference.source_id,
            reference_signal,
            reference_time,
            signal,
            missing ? Cell(std::monostate{}) : Cell(value.event_text),
            missing ? Cell(std::monostate{}) : Cell(value.received_at),
            missing ? Cell(std::monostate{}) : Cell(value.time_origin),
            missing ? Cell(std::string("missing")) : Cell(value.value_type),
            missing || !value.value_double.has_value() ? Cell(std::monostate{}) : Cell(*value.value_double),
            missing || !value.value_int.has_value() ? Cell(std::monostate{}) : Cell(*value.value_int),
            missing || !value.value_bool.has_value() ? Cell(std::monostate{}) : Cell(*value.value_bool),
            missing || !value.value_text.has_value() ? Cell(std::monostate{}) : Cell(*value.value_text),
            missing || !value.value_json.has_value() ? Cell(std::monostate{}) : Cell(*value.value_json),
            missing || !value.unit.has_value() ? Cell(std::monostate{}) : Cell(*value.unit),
            missing ? Cell(std::string("missing")) : Cell(value.quality),
            missing || !value.status_code.has_value() ? Cell(std::monostate{}) : Cell(*value.status_code),
            reference.capture_id,
            event_id,
            alignment_status,
            missing ? Cell(std::monostate{})
                    : Cell(value.event_micros - reference.event_micros)};
}

}  // namespace alignment_internal

nlohmann::json align_timeseries(sdk::PluginProjectAccess &access,
                                const nlohmann::json &parameters) {
    using namespace alignment_internal;
    const auto &input = required_timeseries_input(parameters);
    const auto result_ref = input.at("result_ref").get<std::string>();
    const auto reference_signal = parameters.value("reference_signal", std::string{});
    if (reference_signal.empty())
        throw std::invalid_argument("reference_signal must not be empty");
    const auto tolerance_value = parameters.value("tolerance_seconds", 1.0);
    if (!parameters.contains("tolerance_seconds") ||
        !parameters.at("tolerance_seconds").is_number() ||
        !std::isfinite(tolerance_value) || tolerance_value < 0.0)
        throw std::invalid_argument("tolerance_seconds must be a non-negative number");
    const auto tolerance = static_cast<std::int64_t>(
        std::llround(tolerance_value * 1000000.0));

    access.require_table(result_ref);
    const auto input_rows = access.count_rows(result_ref);
    const std::vector<std::string> columns{
        "source_id", "signal_id", "event_time", "received_at", "time_origin",
        "value_type", "value_double", "value_int", "value_bool", "value_text",
        "value_json", "unit", "quality", "status_code", "capture_id", "event_id"};
    std::map<std::string, SourceSignals> sources;
    std::uint64_t rows_seen = 0;
    access.read_typed_batches(result_ref, columns,
                              [&](const sdk::PluginProjectAccess::BatchView &batch) {
        std::vector<std::uint32_t> indices;
        indices.reserve(columns.size());
        for (const auto &column : columns)
            indices.push_back(batch.column_index(column));
        for (std::uint64_t row = 0; row < batch.row_count(); ++row, ++rows_seen) {
            if (access.is_cancelled())
                throw std::runtime_error("sensor alignment cancelled");
            auto observation = read_observation(batch, indices, row);
            sources[observation.source_id][observation.signal_id].push_back(
                std::move(observation));
            access.report_progress(
                input_rows == 0 ? 1.0
                                : static_cast<double>(rows_seen + 1) /
                                      static_cast<double>(input_rows),
                "Reading sensor observations for alignment");
        }
    });

    auto output = make_output_batch();
    std::uint64_t reference_rows = 0;
    std::uint64_t rows_emitted = 0;
    std::uint64_t matched_rows = 0;
    std::uint64_t missing_rows = 0;
    for (auto &[source_id, signals] : sources) {
        auto reference_found = signals.find(reference_signal);
        if (reference_found == signals.end())
            continue;
        for (auto &entry : signals)
            std::sort(entry.second.begin(), entry.second.end(),
                      [](const Observation &left, const Observation &right) {
                          return std::tie(left.event_micros, left.event_id) <
                                 std::tie(right.event_micros, right.event_id);
                      });
        const auto &references = reference_found->second;
        reference_rows += references.size();
        for (const auto &reference : references) {
            for (const auto &[signal, series] : signals) {
                const auto *matched = signal == reference_signal
                                          ? &reference
                                          : nearest(series, reference.event_micros, tolerance);
                const auto status = signal == reference_signal ? "reference"
                                  : matched == nullptr ? "missing" : "matched";
                output.append_row(output_row(reference, reference_signal, signal,
                                             matched, status));
                ++rows_emitted;
                if (status == std::string("missing"))
                    ++missing_rows;
                else
                    ++matched_rows;
                if (output.row_count() == 1024) {
                    output.emit(access, "sensorAlignedTable");
                    output = make_output_batch();
                }
            }
        }
    }
    if (output.row_count() > 0)
        output.emit(access, "sensorAlignedTable");
    access.report_progress(1.0, "Sensor time-series alignment completed");
    return Json{{"contract", "SENSOR_ALIGNED_V1"},
                {"input_rows", input_rows},
                {"reference_signal", reference_signal},
                {"tolerance_seconds", tolerance_value},
                {"reference_rows", reference_rows},
                {"rows_emitted", rows_emitted},
                {"matched_rows", matched_rows},
                {"missing_rows", missing_rows},
                {"result_ref", result_ref}};
}

}  // namespace streamfind::sensors::operations::detail
