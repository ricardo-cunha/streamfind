#include "streamfind/sdk/plugin_project_access.hpp"

#include <arrow/api.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace streamfind::sensors::operations::detail {
nlohmann::json normalize_sensor_table(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json validate_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json filter_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json resample_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json align_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json project_aligned_timeseries(sdk::PluginProjectAccess &, const nlohmann::json &);
nlohmann::json rolling_statistics(sdk::PluginProjectAccess &, const nlohmann::json &);
}

namespace streamfind::sensors::tests {

using Json = nlohmann::json;

class MemoryProjectAccess final : public sdk::PluginProjectAccess {
  public:
    std::map<std::string, Json> tables;
    std::map<std::string, std::shared_ptr<arrow::RecordBatch>> emitted;
    std::map<std::string, Json> emitted_results;

    std::string_view operation_instance() const noexcept override { return "sensors-test"; }
    bool is_cancelled() const noexcept override { return false; }
    void require_table(const std::string &name) override {
        if (!tables.contains(name)) throw std::runtime_error("missing test table");
    }
    void read_typed_batches(const std::string &table, const std::vector<std::string> &names,
                            const BatchCallback &callback) override {
        const auto &rows = tables.at(table);
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (const auto &name : names) {
            const Json *sample = nullptr;
            for (const auto &row : rows) {
                if (row.contains(name) && !row.at(name).is_null()) {
                    sample = &row.at(name);
                    break;
                }
            }
            std::shared_ptr<arrow::DataType> type = arrow::utf8();
            if (sample && sample->is_boolean()) type = arrow::boolean();
            else if (sample && sample->is_number_integer()) type = arrow::int64();
            else if (sample && sample->is_number()) type = arrow::float64();
            fields.push_back(arrow::field(name, type, true));

            std::shared_ptr<arrow::Array> array;
            if (type->id() == arrow::Type::BOOL) {
                arrow::BooleanBuilder builder;
                for (const auto &row : rows) {
                    const auto value = row.find(name);
                    if (value == row.end() || value->is_null()) builder.AppendNull();
                    else builder.Append(value->get<bool>());
                }
                builder.Finish(&array);
            } else if (type->id() == arrow::Type::INT64) {
                arrow::Int64Builder builder;
                for (const auto &row : rows) {
                    const auto value = row.find(name);
                    if (value == row.end() || value->is_null()) builder.AppendNull();
                    else builder.Append(value->get<std::int64_t>());
                }
                builder.Finish(&array);
            } else if (type->id() == arrow::Type::DOUBLE) {
                arrow::DoubleBuilder builder;
                for (const auto &row : rows) {
                    const auto value = row.find(name);
                    if (value == row.end() || value->is_null()) builder.AppendNull();
                    else builder.Append(value->get<double>());
                }
                builder.Finish(&array);
            } else {
                arrow::StringBuilder builder;
                for (const auto &row : rows) {
                    const auto value = row.find(name);
                    if (value == row.end() || value->is_null()) builder.AppendNull();
                    else builder.Append(value->is_string() ? value->get<std::string>() : value->dump());
                }
                builder.Finish(&array);
            }
            arrays.push_back(std::move(array));
        }
        callback(BatchView(arrow::RecordBatch::Make(
            arrow::schema(std::move(fields)), static_cast<std::int64_t>(rows.size()),
            std::move(arrays))));
    }
    std::uint64_t count_rows(const std::string &table) override { return tables.at(table).size(); }
    void emit_table_batch(const std::string &contract,
                          const std::shared_ptr<arrow::RecordBatch> &batch) override {
        emitted[contract] = batch;
    }
    void emit_result(const std::string &contract, const std::string &payload) override {
        emitted_results[contract] = Json::parse(payload);
    }
    void report_progress(double, std::string_view) override {}
};

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void test_sensor_timeseries_normalization() {
    MemoryProjectAccess access;
    access.tables["source"] = Json::array({
        {{"device", "ph-meter"}, {"sensor", "pH"},
         {"when", "2026-11-19T13:00:00.123456+01:00"}, {"reading", 7.25},
         {"unit", "pH"}, {"quality", "good"}, {"status", "0x0"}},
        {{"device", "ph-meter"}, {"sensor", "pH"},
         {"when", "2026-11-19T12:00:01.654321Z"}, {"reading", nullptr},
         {"unit", "pH"}, {"quality", "missing"}, {"status", "BadNoData"}},
    });
    const auto parameters = Json{
        {"_inputs", {{"tableInput", {{"result_ref", "source"}}}}},
        {"source_id_column", "device"},
        {"signal_id_column", "sensor"},
        {"event_time_column", "when"},
        {"value_column", "reading"},
        {"unit_column", "unit"},
        {"quality_column", "quality"},
        {"status_code_column", "status"},
        {"capture_id", "capture-1"},
    };
    const auto result = operations::detail::normalize_sensor_table(access, parameters);
    require(result.at("contract") == "SENSOR_TIMESERIES", "normalizer returned the wrong contract");
    require(result.at("rows_emitted") == 2, "normalizer emitted the wrong row count");
    require(access.emitted.contains("sensorTimeseriesTable"), "normalizer did not emit its table");

    const auto &batch = access.emitted.at("sensorTimeseriesTable");
    require(batch->num_rows() == 2, "sensor time-series row count is incorrect");
    const auto event_index = batch->schema()->GetFieldIndex("event_time");
    const auto value_index = batch->schema()->GetFieldIndex("value_double");
    const auto type_index = batch->schema()->GetFieldIndex("value_type");
    const auto quality_index = batch->schema()->GetFieldIndex("quality");
    require(batch->schema()->field(event_index)->type()->id() == arrow::Type::TIMESTAMP,
            "event_time is not an Arrow timestamp");
    const auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(batch->column(event_index));
    require(timestamps->Value(0) == 1795089600123456LL &&
            timestamps->Value(1) == 1795089601654321LL,
            "event_time did not preserve microseconds or UTC offsets");
    const auto values = std::static_pointer_cast<arrow::DoubleArray>(batch->column(value_index));
    require(!values->IsNull(0) && std::abs(values->Value(0) - 7.25) < 1e-12 && values->IsNull(1),
            "typed value columns did not preserve real and missing observations");
    const auto types = std::static_pointer_cast<arrow::StringArray>(batch->column(type_index));
    const auto qualities = std::static_pointer_cast<arrow::StringArray>(batch->column(quality_index));
    require(types->GetString(0) == "real" && types->GetString(1) == "missing" &&
            qualities->GetString(0) == "good" && qualities->GetString(1) == "missing",
            "value type or quality normalization is incorrect");
}

void test_sensor_timeseries_validation() {
    MemoryProjectAccess access;
    access.tables["timeseries"] = Json::array({
        {{"source_id", "ph-meter"},
         {"signal_id", "pH"},
         {"event_time", "2026-11-19T12:00:00.000000Z"},
         {"received_at", "2026-11-19T12:00:00.100000Z"},
         {"time_origin", "source"},
         {"value_type", "real"},
         {"value_double", 7.1},
         {"value_int", nullptr},
         {"value_bool", nullptr},
         {"value_text", nullptr},
         {"value_json", nullptr},
         {"unit", "pH"},
         {"quality", "good"},
         {"status_code", "0x0"},
         {"capture_id", "capture-1"},
         {"event_id", "event-1"}},
        {{"source_id", "ph-meter"},
         {"signal_id", "pH"},
         {"event_time", "2026-11-19T12:00:01.000000Z"},
         {"received_at", "2026-11-19T12:00:01.100000Z"},
         {"time_origin", "source"},
         {"value_type", "missing"},
         {"value_double", nullptr},
         {"value_int", nullptr},
         {"value_bool", nullptr},
         {"value_text", nullptr},
         {"value_json", nullptr},
         {"unit", "pH"},
         {"quality", "missing"},
         {"status_code", "BadNoData"},
         {"capture_id", "capture-1"},
         {"event_id", "event-2"}},
        {{"source_id", "ph-meter"},
         {"signal_id", "pH"},
         {"event_time", "2026-11-19T11:59:59.000000Z"},
         {"received_at", "2026-11-19T12:00:02.100000Z"},
         {"time_origin", "source"},
         {"value_type", "real"},
         {"value_double", 7.0},
         {"value_int", nullptr},
         {"value_bool", nullptr},
         {"value_text", nullptr},
         {"value_json", nullptr},
         {"unit", "pH"},
         {"quality", "good"},
         {"status_code", "0x0"},
         {"capture_id", "capture-1"},
         {"event_id", "event-3"}}});

    const auto parameters = Json{
        {"_inputs", {{"sensorTimeseriesTable", {{"result_ref", "timeseries"}}}}}};
    const auto report = operations::detail::validate_timeseries(access, parameters);
    require(report.at("contract") == "SENSOR_TIMESERIES_V1",
            "validation returned the wrong contract");
    require(!report.at("valid").get<bool>(), "invalid fixture was reported as valid");
    require(report.at("rows") == 3, "validation counted the wrong rows");
    require(report.at("signals") == 1, "validation counted the wrong signals");
    require(report.at("missing_value_rows") == 1,
            "validation counted the wrong missing values");
    require(report.at("out_of_order_rows") == 1,
            "validation missed the out-of-order observation");
    require(report.at("quality_rows").at("good") == 2,
            "validation counted good quality incorrectly");
    require(access.emitted_results.contains("sensorValidationReport"),
            "validation did not emit its report");
}

void test_sensor_timeseries_wide_normalization() {
    MemoryProjectAccess access;
    access.tables["wide_source"] = Json::array({
        {{"device", "ph-meter"}, {"when", "2026-11-19T12:00:00.123456Z"},
         {"pH", 7.25}, {"temperature", 25.5}},
        {{"device", "ph-meter"}, {"when", "2026-11-19T12:00:01.654321Z"},
         {"pH", nullptr}, {"temperature", 25.6}},
    });
    const auto parameters = Json{
        {"_inputs", {{"tableInput", {{"result_ref", "wide_source"}}}}},
        {"layout", "wide"},
        {"source_id_column", "device"},
        {"event_time_column", "when"},
        {"variable_columns", Json::array({"pH", "temperature"})},
        {"capture_id", "wide-capture"},
    };
    const auto result = operations::detail::normalize_sensor_table(access, parameters);
    require(result.at("rows_emitted") == 4, "wide normalizer emitted the wrong row count");
    const auto &batch = access.emitted.at("sensorTimeseriesTable");
    require(batch->num_rows() == 4, "wide sensor time-series row count is incorrect");
    const auto signal_index = batch->schema()->GetFieldIndex("signal_id");
    const auto value_index = batch->schema()->GetFieldIndex("value_double");
    const auto type_index = batch->schema()->GetFieldIndex("value_type");
    const auto signals = std::static_pointer_cast<arrow::StringArray>(batch->column(signal_index));
    const auto values = std::static_pointer_cast<arrow::DoubleArray>(batch->column(value_index));
    const auto types = std::static_pointer_cast<arrow::StringArray>(batch->column(type_index));
    require(signals->GetString(0) == "pH" && signals->GetString(1) == "temperature" &&
            signals->GetString(2) == "pH" && signals->GetString(3) == "temperature",
            "wide layout did not preserve variable column order");
    require(!values->IsNull(0) && std::abs(values->Value(0) - 7.25) < 1e-12 &&
            !values->IsNull(1) && std::abs(values->Value(1) - 25.5) < 1e-12 &&
            values->IsNull(2) && !values->IsNull(3),
            "wide layout did not preserve typed and missing values");
    require(types->GetString(0) == "real" && types->GetString(2) == "missing",
            "wide layout value types are incorrect");
}

void test_sensor_timeseries_filter() {
    MemoryProjectAccess access;
    const auto make_row = [](const char *event_time, double value, const char *event_id) {
        return Json{{"source_id", "ph-meter"},
                    {"signal_id", "pH"},
                    {"event_time", event_time},
                    {"received_at", event_time},
                    {"time_origin", "source"},
                    {"value_type", "real"},
                    {"value_double", value},
                    {"value_int", nullptr},
                    {"value_bool", nullptr},
                    {"value_text", nullptr},
                    {"value_json", nullptr},
                    {"unit", "pH"},
                    {"quality", "good"},
                    {"status_code", "0x0"},
                    {"capture_id", "capture-1"},
                    {"event_id", event_id}};
    };
    access.tables["timeseries"] = Json::array({
        make_row("2026-11-19T12:00:00.000000Z", 7.1, "event-1"),
        make_row("2026-11-19T12:00:01.000000Z", 7.2, "event-2"),
        make_row("2026-11-19T12:00:02.000000Z", 7.3, "event-3")});

    const auto parameters = Json{
        {"_inputs", {{"sensorTimeseriesTable", {{"result_ref", "timeseries"}}}}},
        {"start_time", "2026-11-19T12:00:01.000000Z"},
        {"end_time", "2026-11-19T12:00:02.000000Z"}};
    const auto result = operations::detail::filter_timeseries(access, parameters);
    require(result.at("input_rows") == 3, "filter counted the wrong input rows");
    require(result.at("rows_selected") == 1, "filter selected the wrong time window");
    require(access.emitted.contains("sensorTimeseriesTable"), "filter did not emit a table");
    require(access.emitted.at("sensorTimeseriesTable")->num_rows() == 1,
            "filter emitted the wrong row count");
}

void test_sensor_timeseries_resampling() {
    MemoryProjectAccess access;
    const auto make_row = [](const char *event_time, const Json &value,
                             const char *quality, const char *event_id) {
        return Json{{"source_id", "ph-meter"},
                    {"signal_id", "pH"},
                    {"event_time", event_time},
                    {"received_at", event_time},
                    {"time_origin", "source"},
                    {"value_type", value.is_null() ? "missing" : "real"},
                    {"value_double", value},
                    {"value_int", nullptr},
                    {"value_bool", nullptr},
                    {"value_text", nullptr},
                    {"value_json", nullptr},
                    {"unit", "pH"},
                    {"quality", quality},
                    {"status_code", "0x0"},
                    {"capture_id", "capture-1"},
                    {"event_id", event_id}};
    };
    access.tables["timeseries"] = Json::array({
        make_row("2026-11-19T12:00:10.000000Z", 7.0, "good", "event-1"),
        make_row("2026-11-19T12:00:20.000000Z", 9.0, "good", "event-2"),
        make_row("2026-11-19T12:01:10.000000Z", nullptr, "missing", "event-3")});

    const auto parameters = Json{
        {"_inputs", {{"sensorTimeseriesTable", {{"result_ref", "timeseries"}}}}},
        {"window_seconds", 60.0},
        {"aggregation", "mean"}};
    const auto result = operations::detail::resample_timeseries(access, parameters);
    require(result.at("rows_emitted") == 2, "resampler emitted the wrong row count");
    const auto &batch = access.emitted.at("sensorTimeseriesTable");
    require(batch->num_rows() == 2, "resampler output row count is incorrect");
    const auto event_index = batch->schema()->GetFieldIndex("event_time");
    const auto value_index = batch->schema()->GetFieldIndex("value_double");
    const auto type_index = batch->schema()->GetFieldIndex("value_type");
    const auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(batch->column(event_index));
    const auto values = std::static_pointer_cast<arrow::DoubleArray>(batch->column(value_index));
    const auto types = std::static_pointer_cast<arrow::StringArray>(batch->column(type_index));
    require(timestamps->Value(0) == 1795089600000000LL &&
            timestamps->Value(1) == 1795089660000000LL,
            "resampler did not anchor windows at UTC minute boundaries");
    require(!values->IsNull(0) && std::abs(values->Value(0) - 8.0) < 1e-12 &&
            values->IsNull(1) && types->GetString(1) == "missing",
            "resampler did not aggregate numeric and missing observations correctly");
}

void test_sensor_timeseries_rolling_statistics() {
    MemoryProjectAccess access;
    const auto make_row = [](const char *event_time, double value,
                             const char *event_id) {
        return Json{{"source_id", "ph-meter"},
                    {"signal_id", "pH"},
                    {"event_time", event_time},
                    {"received_at", event_time},
                    {"time_origin", "source"},
                    {"value_type", "real"},
                    {"value_double", value},
                    {"value_int", nullptr},
                    {"value_bool", nullptr},
                    {"value_text", nullptr},
                    {"value_json", nullptr},
                    {"unit", "pH"},
                    {"quality", "good"},
                    {"status_code", "0x0"},
                    {"capture_id", "capture-1"},
                    {"event_id", event_id}};
    };
    access.tables["timeseries"] = Json::array({
        make_row("2026-11-19T12:00:00.000000Z", 2.0, "event-1"),
        make_row("2026-11-19T12:00:01.000000Z", 4.0, "event-2"),
        make_row("2026-11-19T12:00:03.000000Z", 8.0, "event-3")});

    const auto parameters = Json{
        {"_inputs", {{"sensorTimeseriesTable", {{"result_ref", "timeseries"}}}}},
        {"rolling_window_seconds", 2.0},
        {"rolling_statistic", "mean"}};
    const auto result = operations::detail::rolling_statistics(access, parameters);
    require(result.at("contract") == "SENSOR_TIMESERIES",
            "rolling statistics returned the wrong contract");
    require(result.at("rows_emitted") == 3,
            "rolling statistics emitted the wrong row count");
    const auto &batch = access.emitted.at("sensorTimeseriesTable");
    const auto signal_index = batch->schema()->GetFieldIndex("signal_id");
    const auto value_index = batch->schema()->GetFieldIndex("value_double");
    const auto signals = std::static_pointer_cast<arrow::StringArray>(batch->column(signal_index));
    const auto values = std::static_pointer_cast<arrow::DoubleArray>(batch->column(value_index));
    require(signals->GetString(0) == "pH::rolling_mean" &&
                signals->GetString(2) == "pH::rolling_mean",
            "rolling statistics did not emit a derived signal");
    require(std::abs(values->Value(0) - 2.0) < 1e-12 &&
                std::abs(values->Value(1) - 3.0) < 1e-12 &&
                std::abs(values->Value(2) - 6.0) < 1e-12,
            "rolling statistics used the wrong trailing window");
}

void test_sensor_timeseries_alignment() {
    MemoryProjectAccess access;
    const auto make_row = [](const char *signal, const char *event_time,
                             const Json &value, const char *event_id) {
        return Json{{"source_id", "ph-meter"},
                    {"signal_id", signal},
                    {"event_time", event_time},
                    {"received_at", event_time},
                    {"time_origin", "source"},
                    {"value_type", value.is_null() ? "missing" : "real"},
                    {"value_double", value},
                    {"value_int", nullptr},
                    {"value_bool", nullptr},
                    {"value_text", nullptr},
                    {"value_json", nullptr},
                    {"unit", signal},
                    {"quality", value.is_null() ? "missing" : "good"},
                    {"status_code", "0x0"},
                    {"capture_id", "capture-1"},
                    {"event_id", event_id}};
    };
    access.tables["timeseries"] = Json::array({
        make_row("pH", "2026-11-19T12:00:00.000000Z", 7.1, "ph-1"),
        make_row("pH", "2026-11-19T12:00:01.000000Z", 7.2, "ph-2"),
        make_row("temperature", "2026-11-19T12:00:00.200000Z", 25.5, "temp-1"),
        make_row("temperature", "2026-11-19T12:00:03.000000Z", 25.8, "temp-2")});

    const auto parameters = Json{
        {"_inputs", {{"sensorTimeseriesTable", {{"result_ref", "timeseries"}}}}},
        {"reference_signal", "pH"},
        {"tolerance_seconds", 0.5}};
    const auto result = operations::detail::align_timeseries(access, parameters);
    require(result.at("contract") == "SENSOR_ALIGNED_V1",
            "alignment returned the wrong contract");
    require(result.at("reference_rows") == 2 && result.at("rows_emitted") == 4,
            "alignment emitted the wrong row count");
    require(result.at("matched_rows") == 3 && result.at("missing_rows") == 1,
            "alignment counted matches incorrectly");
    const auto &batch = access.emitted.at("sensorAlignedTable");
    const auto status_index = batch->schema()->GetFieldIndex("alignment_status");
    const auto delta_index = batch->schema()->GetFieldIndex("time_delta_microseconds");
    const auto value_index = batch->schema()->GetFieldIndex("value_double");
    const auto statuses = std::static_pointer_cast<arrow::StringArray>(batch->column(status_index));
    const auto deltas = std::static_pointer_cast<arrow::Int64Array>(batch->column(delta_index));
    const auto values = std::static_pointer_cast<arrow::DoubleArray>(batch->column(value_index));
    require(statuses->GetString(0) == "reference" && statuses->GetString(1) == "matched" &&
            statuses->GetString(2) == "reference" && statuses->GetString(3) == "missing",
            "alignment statuses are incorrect");
    require(deltas->Value(1) == 200000 && deltas->IsNull(3),
            "alignment time deltas are incorrect");
    require(!values->IsNull(1) && values->Value(1) == 25.5 && values->IsNull(3),
            "alignment typed values are incorrect");
}

void test_project_aligned_timeseries() {
    MemoryProjectAccess access;
    access.tables["aligned"] = Json::array({
        { {"source_id", "alignment-rig"}, {"reference_signal", "pH"},
          {"reference_event_time", "2026-01-01T00:00:00.000000Z"},
          {"signal_id", "temperature"}, {"event_time", "2026-01-01T00:00:00.200000Z"},
          {"received_at", "2026-01-01T00:00:00.300000Z"}, {"time_origin", "source"},
          {"value_type", "real"}, {"value_double", 25.5}, {"value_int", nullptr},
          {"value_bool", nullptr}, {"value_text", nullptr}, {"value_json", nullptr},
          {"unit", "degC"}, {"quality", "good"}, {"status_code", "0x0"},
          {"capture_id", "capture-1"}, {"event_id", "temp-1"},
          {"alignment_status", "matched"}, {"time_delta_microseconds", 200000}},
        { {"source_id", "alignment-rig"}, {"reference_signal", "pH"},
          {"reference_event_time", "2026-01-01T00:00:01.000000Z"},
          {"signal_id", "conductivity"}, {"event_time", nullptr},
          {"received_at", nullptr}, {"time_origin", nullptr},
          {"value_type", "missing"}, {"value_double", nullptr}, {"value_int", nullptr},
          {"value_bool", nullptr}, {"value_text", nullptr}, {"value_json", nullptr},
          {"unit", nullptr}, {"quality", "missing"}, {"status_code", nullptr},
          {"capture_id", "capture-1"}, {"event_id", "conductivity-1"},
          {"alignment_status", "missing"}, {"time_delta_microseconds", nullptr}}});

    const auto base_parameters = Json{
        {"_inputs", {{"sensorAlignedTable", {{"result_ref", "aligned"}}}}}};
    auto reference_parameters = base_parameters;
    reference_parameters["time_axis"] = "reference";
    const auto reference_result =
        operations::detail::project_aligned_timeseries(access, reference_parameters);
    require(reference_result.at("rows_emitted") == 2 &&
                reference_result.at("missing_rows") == 1,
            "aligned projection emitted incorrect reference-axis counts");
    const auto &reference_batch = access.emitted.at("sensorTimeseriesTable");
    const auto reference_time_index = reference_batch->schema()->GetFieldIndex("event_time");
    const auto reference_times = std::static_pointer_cast<arrow::TimestampArray>(
        reference_batch->column(reference_time_index));
    require(reference_times->Value(0) == 1767225600000000LL &&
                reference_times->Value(1) == 1767225601000000LL,
            "reference-axis projection did not use reference timestamps");

    access.emitted.clear();
    auto observed_parameters = base_parameters;
    observed_parameters["time_axis"] = "observed";
    const auto observed_result =
        operations::detail::project_aligned_timeseries(access, observed_parameters);
    require(observed_result.at("observed_axis_fallback_rows") == 1,
            "observed-axis projection did not count missing fallbacks");
    const auto &observed_batch = access.emitted.at("sensorTimeseriesTable");
    const auto observed_times = std::static_pointer_cast<arrow::TimestampArray>(
        observed_batch->column(observed_batch->schema()->GetFieldIndex("event_time")));
    require(observed_times->Value(0) == 1767225600200000LL &&
                observed_times->Value(1) == 1767225601000000LL,
            "observed-axis projection did not preserve matched time or fallback");
}

}  // namespace streamfind::sensors::tests

int main() {
    try {
        streamfind::sensors::tests::test_sensor_timeseries_normalization();
        streamfind::sensors::tests::test_sensor_timeseries_wide_normalization();
        streamfind::sensors::tests::test_sensor_timeseries_filter();
        streamfind::sensors::tests::test_sensor_timeseries_resampling();
        streamfind::sensors::tests::test_sensor_timeseries_rolling_statistics();
        streamfind::sensors::tests::test_sensor_timeseries_alignment();
        streamfind::sensors::tests::test_project_aligned_timeseries();
        streamfind::sensors::tests::test_sensor_timeseries_validation();
        return 0;
    } catch (const std::exception &error) {
        return (std::cerr << error.what() << '\n') ? 1 : 1;
    }
}
