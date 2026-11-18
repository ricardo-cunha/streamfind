#include "operations/operations.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"
#include <arrow/api.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::raman::tests {

class MemoryProjectAccess final : public sdk::PluginProjectAccess {
  public:
    std::map<std::string, Json> tables;
    std::map<std::string, Json> emitted;
    std::string_view operation_instance() const noexcept override { return "normalize-test"; }
    bool is_cancelled() const noexcept override { return false; }
    void require_table(const std::string &) override {}
    void read_typed_batches(const std::string &table, const std::vector<std::string> &names, const BatchCallback &callback) override {
        const auto &rows = tables.at(table);
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (const auto &name : names) {
            const Json *sample = nullptr;
            for (const auto &row : rows)
                if (row.contains(name) && !row.at(name).is_null()) { sample = &row.at(name); break; }
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
                    else builder.Append(value->get<std::string>());
                }
                builder.Finish(&array);
            }
            fields.back()->type()->id();
            arrays.push_back(std::move(array));
        }
        callback(BatchView(arrow::RecordBatch::Make(arrow::schema(std::move(fields)),
            static_cast<std::int64_t>(rows.size()), std::move(arrays))));
    }
    std::uint64_t count_rows(const std::string &table) override {
        return tables.at(table).size();
    }
    void emit_table_batch(const std::string &contract,
                          const std::shared_ptr<arrow::RecordBatch> &batch) override {
        for (std::int64_t row_index = 0; row_index < batch->num_rows(); ++row_index) {
            Json row = Json::object();
            for (int column = 0; column < batch->num_columns(); ++column) {
                const auto &array = batch->column(column);
                const auto name = batch->schema()->field(column)->name();
                if (array->IsNull(row_index)) row[name] = nullptr;
                else if (array->type_id() == arrow::Type::STRING)
                    row[name] = std::static_pointer_cast<arrow::StringArray>(array)->GetString(row_index);
                else if (array->type_id() == arrow::Type::INT64)
                    row[name] = std::static_pointer_cast<arrow::Int64Array>(array)->Value(row_index);
                else if (array->type_id() == arrow::Type::DOUBLE)
                    row[name] = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row_index);
                else if (array->type_id() == arrow::Type::BOOL)
                    row[name] = std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row_index);
            }
            emitted[contract].push_back(std::move(row));
        }
    }    void emit_result(const std::string &, const std::string &) override {}
    void report_progress(double, std::string_view) override {}
};

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 1e-12, message);
}

}  // namespace streamfind::raman::tests

int main() {
    using namespace streamfind::raman::tests;
    using streamfind::Json;
    try {
        MemoryProjectAccess access;
        access.tables["headers"] = Json::array({
            {{"analysis_id", "a"}, {"spectrum_id", "a-0"}, {"spectrum_index", 0},
             {"source_frame_index", 0}, {"source_path", "sample.asc"}, {"point_count", 3},
             {"frame_time_value", 2.5}, {"frame_time_unit", "seconds"},
             {"time_seconds", 2.5}, {"time_kind", "elapsed_time"}, {"axis_kind", "raman_shift"},
             {"axis_unit", "cm^-1"}, {"source_axis_kind", "raman_shift"},
             {"source_axis_unit", "cm^-1"}, {"axis_order", "ascending"},
             {"signal_kind", "intensity"}, {"signal_unit", "CCD counts"}},
            {{"analysis_id", "b"}, {"spectrum_id", "b-0"}, {"spectrum_index", 0},
             {"source_frame_index", 0}, {"source_path", "sample2.asc"}, {"point_count", 3},
             {"frame_time_value", nullptr}, {"frame_time_unit", "unknown"},
             {"time_seconds", nullptr}, {"time_kind", "none"}, {"axis_kind", "raman_shift"},
             {"axis_unit", "cm^-1"}, {"source_axis_kind", "raman_shift"},
             {"source_axis_unit", "cm^-1"}, {"axis_order", "ascending"},
             {"signal_kind", "intensity"}, {"signal_unit", "CCD counts"}}
        });
        access.tables["points"] = Json::array({
            {{"analysis_id", "a"}, {"spectrum_id", "a-0"}, {"point_index", 2},
             {"source_point_index", 2}, {"source_spectral_coordinate", 20.0},
             {"spectral_coordinate", 20.0}, {"intensity", 3.0}},
            {{"analysis_id", "a"}, {"spectrum_id", "a-0"}, {"point_index", 0},
             {"source_point_index", 0}, {"source_spectral_coordinate", 10.0},
             {"spectral_coordinate", 10.0}, {"intensity", 1.0}},
            {{"analysis_id", "a"}, {"spectrum_id", "a-0"}, {"point_index", 1},
             {"source_point_index", 1}, {"source_spectral_coordinate", 15.0},
             {"spectral_coordinate", 15.0}, {"intensity", 2.0}},
            {{"analysis_id", "b"}, {"spectrum_id", "b-0"}, {"point_index", 0},
             {"source_point_index", 0}, {"source_spectral_coordinate", 10.0},
             {"spectral_coordinate", 10.0}, {"intensity", -4.0}},
            {{"analysis_id", "b"}, {"spectrum_id", "b-0"}, {"point_index", 1},
             {"source_point_index", 1}, {"source_spectral_coordinate", 15.0},
             {"spectral_coordinate", 15.0}, {"intensity", 0.0}},
            {{"analysis_id", "b"}, {"spectrum_id", "b-0"}, {"point_index", 2},
             {"source_point_index", 2}, {"source_spectral_coordinate", 20.0},
             {"spectral_coordinate", 20.0}, {"intensity", 4.0}}
        });
        const std::vector<std::string> frame_columns = {
            "spectrum_index", "source_frame_index", "source_path", "frame_time_value",
            "frame_time_unit", "time_seconds", "time_kind", "axis_kind", "axis_unit",
            "source_axis_kind", "source_axis_unit", "axis_order", "signal_kind", "signal_unit"};
        for (auto &point : access.tables["points"]) {
            const auto &header = access.tables["headers"][point.at("spectrum_id") == "a-0" ? 0 : 1];
            for (const auto &column : frame_columns) point[column] = header.at(column);
        }

        const auto *read_capability = streamfind::raman::operations::capabilities().find(
            "raman.read_raman_files");
        require(read_capability != nullptr, "read_raman_files capability was not registered");
        const auto *summary_capability = streamfind::raman::operations::capabilities().find(
            "raman.summarize_spectra");
        require(summary_capability != nullptr, "summarize_spectra capability was not registered");
        const auto timed_fixture = std::filesystem::path(RAMAN_TEST_SOURCE_DIR) /
            "unit/fixtures/timed_frames.asc";
        const Json read_parameters = {
            {"source_paths", Json::array({timed_fixture.string()})}
        };
        const auto read_result = read_capability->handler(access, read_parameters);
        require(read_result.at("spectra").get<std::uint64_t>() == 2,
                "reader operation did not report both timed frames");
        require(access.emitted.at("spectraTable").size() == 6,
                "reader operation did not emit every spectral point");
        const auto &first_point = access.emitted.at("spectraTable")[0];
        const auto &last_frame_point = access.emitted.at("spectraTable")[3];
        require(first_point.at("spectrum_index") == 0 &&
                first_point.at("frame_time_unit") == "seconds" &&
                first_point.at("time_kind") == "elapsed_time" &&
                first_point.at("time_seconds") == 0.0,
                "first frame metadata was not copied onto its spectral points");
        require(last_frame_point.at("spectrum_index") == 1 &&
                last_frame_point.at("time_seconds") == 0.5 &&
                last_frame_point.at("axis_kind") == "pixel" &&
                last_frame_point.at("axis_unit") == "pixel",
                "second frame or axis metadata was not copied onto its spectral points: " +
                    last_frame_point.dump());
        access.tables["ingested_points"] = access.emitted.at("spectraTable");
        access.emitted.clear();
        const Json ingested_summary_parameters = {
            {"_inputs", {{"spectraTable", {{"result_ref", "ingested_points"}}}}}
        };
        const auto ingested_summary = summary_capability->handler(access, ingested_summary_parameters);
        require(ingested_summary.at("spectra").get<std::uint64_t>() == 2 &&
                access.emitted.at("spectraSummaryTable").size() == 2,
                "summary operation did not summarize ingested time frames");
        require(access.emitted.at("spectraSummaryTable")[1].at("time_seconds") == 0.5,
                "summary operation lost ingested frame-time values");
        access.emitted.clear();

        const Json summary_parameters = {
            {"_inputs", {{"spectraTable", {{"result_ref", "points"}}}}}
        };
        const auto summary_result = summary_capability->handler(access, summary_parameters);
        require(summary_result.at("spectra").get<std::uint64_t>() == 2,
                "summary operation reported the wrong spectrum count");
        require(access.emitted.at("spectraSummaryTable").size() == 2,
                "summary operation did not emit one row per spectrum");
        require(access.emitted.at("spectraSummaryTable")[0].at("point_count") == 3,
                "summary operation reported the wrong point count");
        require(access.emitted.at("spectraSummaryTable")[0].at("axis_unit") == "cm^-1",
                "summary operation lost spectral-axis metadata");
        require(access.emitted.at("spectraSummaryTable")[0].at("time_seconds") == 2.5,
                "summary operation lost frame time metadata");
        access.emitted.clear();

        const auto *capability = streamfind::raman::operations::capabilities().find(
            "raman.normalize_spectra");
        require(capability != nullptr, "normalize_spectra capability was not registered");
        const Json parameters = {
            {"normalization_algorithm", "minmax"},
            {"_inputs", {
                {"spectraTable", {{"result_ref", "points"}}}
            }}
        };
        const auto result = capability->handler(access, parameters);
        require(result.at("spectra").get<std::uint64_t>() == 2, "unexpected spectrum count");
        require(result.at("points").get<std::uint64_t>() == 6, "unexpected point count");
        require(access.emitted.at("spectraTable").size() == 6, "normalized point rows were not emitted");
        require(!access.emitted.contains("spectraSummaryTable"),
                "normalization unexpectedly emitted spectrum headers");
        require_near(access.emitted.at("spectraTable")[0].at("intensity").get<double>(),
                     0.0, "point rows were not sorted by point index");
        require_near(access.emitted.at("spectraTable")[1].at("intensity").get<double>(),
                     0.5, "first spectrum min-max midpoint");
        require_near(access.emitted.at("spectraTable")[4].at("intensity").get<double>(),
                     0.5, "second spectrum min-max midpoint");
        access.emitted.clear();
        const auto *baseline_capability = streamfind::raman::operations::capabilities().find(
            "raman.correct_spectra_baseline");
        require(baseline_capability != nullptr, "correct_spectra_baseline capability was not registered");
        const Json baseline_parameters = {
            {"baseline_algorithm", "airpls"}, {"lambda", 10.0}, {"differences", 2}, {"itermax", 20},
            {"_inputs", {
                {"spectraTable", {{"result_ref", "points"}}}
            }}
        };
        const auto baseline_result = baseline_capability->handler(access, baseline_parameters);
        require(baseline_result.at("spectra").get<std::uint64_t>() == 2,
                "baseline operation reported the wrong spectrum count");
        require(access.emitted.at("baselineTable").size() == 6,
                "point-aligned baseline values were not emitted");
        require(access.emitted.at("spectraTable").size() == 6,
                "baseline-corrected point rows were not emitted");
        for (std::size_t index = 0; index < 6; ++index) {
            const auto &baseline_row = access.emitted.at("baselineTable")[index];
            const auto &corrected_row = access.emitted.at("spectraTable")[index];
            require(baseline_row.at("spectrum_id") == corrected_row.at("spectrum_id") &&
                    baseline_row.at("point_index") == corrected_row.at("point_index"),
                    "baseline is not aligned with corrected point keys");
            const auto source = std::find_if(access.tables["points"].begin(),
                access.tables["points"].end(), [&](const Json &row) {
                    return row.at("spectrum_id") == corrected_row.at("spectrum_id") &&
                        row.at("point_index") == corrected_row.at("point_index");
                });
            require(source != access.tables["points"].end(), "corrected output point has no source match");
            const auto raw = source->at("intensity").get<double>();
            const auto baseline = baseline_row.at("baseline_intensity").get<double>();
            const auto corrected = corrected_row.at("intensity").get<double>();
            require_near(corrected + std::min(raw, baseline), raw,
                         "corrected signal does not follow clipped-baseline subtraction");
        }
        require(!access.emitted.contains("spectraSummaryTable"),
                "baseline correction unexpectedly emitted spectrum headers");
        for (std::size_t index = 0; index < 3; ++index) {
            const auto row = std::find_if(access.emitted.at("baselineTable").begin(),
                access.emitted.at("baselineTable").end(), [&](const Json &candidate) {
                    return candidate.at("spectrum_id") == "a-0" &&
                        candidate.at("point_index").get<std::size_t>() == index;
                });
            require(row != access.emitted.at("baselineTable").end(),
                    "linear spectrum baseline point was not emitted");
            require_near(row->at("baseline_intensity").get<double>(),
                         static_cast<double>(index + 1),
                         "airPLS linear baseline mismatch: " +
                            std::to_string(row->at("baseline_intensity").get<double>()));
        }
        access.emitted.clear();
        const auto *range_capability = streamfind::raman::operations::capabilities().find(
            "raman.filter_spectral_range");
        require(range_capability != nullptr, "filter_spectral_range capability was not registered");
        const Json range_parameters = {
            {"minimum_coordinate", 15.0}, {"maximum_coordinate", 20.0}, {"mode", "include"},
            {"_inputs", {
                {"spectraTable", {{"result_ref", "points"}}}
            }}
        };
        const auto range_result = range_capability->handler(access, range_parameters);
        require(range_result.at("points").get<std::uint64_t>() == 4,
                "range filter retained the wrong number of points");
        require(!access.emitted.contains("spectraSummaryTable"),
                "range filter unexpectedly emitted spectrum headers");
        for (const auto &row : access.emitted.at("spectraTable")) {
            const auto coordinate = row.at("spectral_coordinate").get<double>();
            require(coordinate >= 15.0 && coordinate <= 20.0,
                    "inclusive range filter emitted a point outside its bounds");
        }
        access.emitted.clear();
        const auto *smoothing_capability = streamfind::raman::operations::capabilities().find(
            "raman.smooth_spectra");
        require(smoothing_capability != nullptr, "smooth_spectra capability was not registered");
        const Json smoothing_parameters = {
            {"smoothing_algorithm", "moving_average"}, {"window_size", 3},
            {"_inputs", {
                {"spectraTable", {{"result_ref", "points"}}}
            }}
        };
        const auto smoothing_result = smoothing_capability->handler(access, smoothing_parameters);
        require(smoothing_result.at("spectra").get<std::uint64_t>() == 2,
                "smoothing operation reported the wrong spectrum count");
        require(access.emitted.at("spectraTable").size() == 6,
                "smoothing changed the number of spectral points");
        const auto smoothed_value = [&](const std::string &spectrum_id, std::size_t point_index) {
            const auto row = std::find_if(access.emitted.at("spectraTable").begin(),
                access.emitted.at("spectraTable").end(), [&](const Json &candidate) {
                    return candidate.at("spectrum_id") == spectrum_id &&
                        candidate.at("point_index").get<std::size_t>() == point_index;
                });
            require(row != access.emitted.at("spectraTable").end(),
                    "smoothed point was not emitted");
            return row->at("intensity").get<double>();
        };
        require_near(smoothed_value("a-0", 0), 1.5, "moving average left-edge result");
        require_near(smoothed_value("a-0", 1), 2.0, "moving average center result");
        require_near(smoothed_value("a-0", 2), 2.5, "moving average right-edge result");
        require_near(smoothed_value("b-0", 1), 0.0,
                     "moving average must not mix neighboring spectra");
        require(!access.emitted.contains("spectraSummaryTable"),
                "moving average unexpectedly emitted spectrum headers");
        access.emitted.clear();
        const auto *savgol_capability = streamfind::raman::operations::capabilities().find(
            "raman.smooth_spectra_savgol");
        require(savgol_capability != nullptr,
                "smooth_spectra_savgol capability was not registered");
        const Json savgol_parameters = {
            {"window_length", 3}, {"polynomial_order", 2},
            {"_inputs", {
                {"spectraTable", {{"result_ref", "points"}}}
            }}
        };
        const auto savgol_result = savgol_capability->handler(access, savgol_parameters);
        require(savgol_result.at("derivative_order").get<std::uint64_t>() == 0,
                "Savitzky-Golay operation unexpectedly produced a derivative");
        require(access.emitted.at("spectraTable").size() == 6,
                "Savitzky-Golay changed the number of spectral points");
        require_near(smoothed_value("a-0", 0), 1.0,
                     "Savitzky-Golay did not preserve a quadratic fit at the left edge");
        require_near(smoothed_value("a-0", 1), 2.0,
                     "Savitzky-Golay did not preserve a quadratic fit at the center");
        require_near(smoothed_value("a-0", 2), 3.0,
                     "Savitzky-Golay did not preserve a quadratic fit at the right edge");
        require(!access.emitted.contains("spectraSummaryTable"),
                "Savitzky-Golay unexpectedly emitted spectrum headers");

        access.tables["interleaved_points"] = Json::array({
            access.tables["points"][0], access.tables["points"][1], access.tables["points"][2],
            access.tables["points"][3], access.tables["points"][4], access.tables["points"][5],
            access.tables["points"][0], access.tables["points"][1], access.tables["points"][2]});
        const auto require_interleaving_rejected = [&](const auto *operation, Json operation_parameters,
                                                       const std::string &label) {
            operation_parameters["_inputs"]["spectraTable"] = {{"result_ref", "interleaved_points"}};
            try {
                (void)operation->handler(access, operation_parameters);
            } catch (const std::exception &operation_error) {
                if (std::string(operation_error.what()).find("not grouped by spectrum_id") != std::string::npos)
                    return;
                throw std::runtime_error(label + " returned an unclear interleaved-row error: " +
                                         operation_error.what());
            }
            throw std::runtime_error(label + " accepted rows interleaved across spectrum IDs");
        };
        require_interleaving_rejected(capability, parameters, "normalization");
        require_interleaving_rejected(baseline_capability, baseline_parameters, "baseline correction");
        require_interleaving_rejected(smoothing_capability, smoothing_parameters, "moving-average smoothing");
        require_interleaving_rejected(savgol_capability, savgol_parameters, "Savitzky-Golay smoothing");
        std::cout << "Raman normalization operation contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
