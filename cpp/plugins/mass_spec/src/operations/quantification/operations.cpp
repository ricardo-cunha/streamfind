#include "operations/quantification/operations.hpp"

#include "operations/base.hpp"
#include "streamfind/sdk/debug_session.hpp"
#include "streamfind/sdk/table_batch_builder.hpp"
#include "utils/chromatograms.hpp"
#include "utils/quantification.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <limits>

namespace streamfind::mass_spec::quantification
{
namespace detail
{
using Json = nlohmann::json;

void reject(const std::vector<utils::ValidationError> &errors)
{
    if (!errors.empty())
        throw std::invalid_argument(utils::format_validation_errors(errors));
}

std::vector<sdk::TableBatchBuilder::Column> table_schema(
    const std::vector<std::string> &columns, const std::vector<std::string> &types)
{
    std::vector<sdk::TableBatchBuilder::Column> schema;
    schema.reserve(columns.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto &type = types.at(index);
        if (type == "integer") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_INT64});
        else if (type == "real") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_FLOAT64});
        else if (type == "boolean") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_BOOL});
        else if (type == "timestamp") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_TIMESTAMP});
        else if (type == "decimal") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_DECIMAL});
        else if (type == "array") schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8, true,
                                                     STREAMFIND_PLUGIN_COLUMN_FLOAT64});
        else schema.push_back({columns[index], STREAMFIND_PLUGIN_COLUMN_UTF8});
    }
    return schema;
}

using TypedRow = std::vector<sdk::TableBatchBuilder::Cell>;

void emit_typed_rows(sdk::PluginProjectAccess &access, const std::string &contract,
                     const std::vector<std::string> &columns,
                     const std::vector<std::string> &types,
                     const std::vector<TypedRow> &rows)
{
    sdk::TableBatchBuilder output(table_schema(columns, types));
    for (const auto &row : rows) output.append_row(row);
    output.emit(access, contract);
}

std::size_t column_index(const std::vector<std::string> &columns, const std::string &name)
{
    const auto found = std::find(columns.begin(), columns.end(), name);
    if (found == columns.end()) throw std::logic_error("unknown quantification result column: " + name);
    return static_cast<std::size_t>(std::distance(columns.begin(), found));
}

void set_cell(TypedRow &row, const std::vector<std::string> &columns,
              const std::string &name, sdk::TableBatchBuilder::Cell value)
{
    row.at(column_index(columns, name)) = std::move(value);
}

const sdk::TableBatchBuilder::Cell &get_cell(const TypedRow &row,
                                              const std::vector<std::string> &columns,
                                              const std::string &name)
{
    return row.at(column_index(columns, name));
}

std::string row_text(const TypedRow &row, const std::vector<std::string> &columns,
                     const std::string &name)
{
    const auto &cell = get_cell(row, columns, name);
    if (const auto value = std::get_if<std::string>(&cell)) return *value;
    return {};
}

double row_number(const TypedRow &row, const std::vector<std::string> &columns,
                  const std::string &name, const double fallback = std::numeric_limits<double>::quiet_NaN())
{
    const auto &cell = get_cell(row, columns, name);
    if (const auto value = std::get_if<double>(&cell)) return *value;
    if (const auto value = std::get_if<std::int64_t>(&cell)) return static_cast<double>(*value);
    return fallback;
}

bool row_boolean(const TypedRow &row, const std::vector<std::string> &columns,
                 const std::string &name, const bool fallback = false)
{
    const auto &cell = get_cell(row, columns, name);
    if (const auto value = std::get_if<bool>(&cell)) return *value;
    return fallback;
}

sdk::TableBatchBuilder::Cell config_cell(const Json &source, const std::string &field,
                                        const std::string &type, const Json *default_value)
{
    const auto it = source.find(field);
    const Json *value = it == source.end() || it->is_null() ? default_value : &*it;
    if (value == nullptr || value->is_null()) return std::monostate{};
    if (type == "integer") return value->get<std::int64_t>();
    if (type == "real") return value->get<double>();
    if (type == "boolean") return value->get<bool>();
    if (value->is_string()) return value->get<std::string>();
    return value->dump();
}

void append_config_row(sdk::TableBatchBuilder &output, const Json &source,
                       const std::vector<std::string> &columns,
                       const std::vector<std::string> &types,
                       const std::map<std::string, Json> &defaults = {})
{
    std::vector<sdk::TableBatchBuilder::Cell> row;
    row.reserve(columns.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto fallback = defaults.find(columns[index]);
        row.push_back(config_cell(source, columns[index], types[index],
                                  fallback == defaults.end() ? nullptr : &fallback->second));
    }
    output.append_row(row);
}

bool has_value(const Json &row, const std::string &field)
{
    const auto it = row.find(field);
    if (it == row.end() || it->is_null()) return false;
    if (it->is_string()) return !it->get<std::string>().empty();
    return true;
}

std::string text_value(const Json &row, const std::string &field)
{
    const auto it = row.find(field);
    if (it == row.end() || it->is_null()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

bool is_water_blank(const Json &row)
{
    if (text_value(row, "sample_type") != "blank") return false;
    auto analysis = text_value(row, "analysis");
    std::transform(analysis.begin(), analysis.end(), analysis.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return analysis.find("wasser") != std::string::npos;
}

struct PeakRecord
{
    std::string analysis;
    std::string signal_type;
    std::string chromatogram_type;
    std::string detector;
    std::optional<std::int64_t> chromatogram_index;
    std::optional<std::int64_t> peak_id;
    std::optional<std::int64_t> polarity;
    std::optional<std::int64_t> eic_size;
    std::optional<double> rt;
    std::optional<double> height;
    std::optional<double> area;
    std::optional<double> precursor_mz;
    std::optional<double> product_mz;
    std::optional<double> wavelength_nm;
    bool has_eic_rt = false;
    bool has_eic_intensity = false;
    std::vector<double> eic_rt;
    std::vector<double> eic_intensity;
};

bool has_value(const PeakRecord &row, const std::string &field)
{
    if (field == "signal_type") return !row.signal_type.empty();
    if (field == "chromatogram_type") return !row.chromatogram_type.empty();
    if (field == "detector") return !row.detector.empty();
    if (field == "polarity") return row.polarity.has_value();
    if (field == "precursor_mz") return row.precursor_mz.has_value();
    if (field == "product_mz") return row.product_mz.has_value();
    if (field == "wavelength_nm") return row.wavelength_nm.has_value();
    return false;
}

std::string text_value(const PeakRecord &row, const std::string &field)
{
    if (field == "signal_type") return row.signal_type;
    if (field == "chromatogram_type") return row.chromatogram_type;
    if (field == "detector") return row.detector;
    return {};
}

double number_value(const PeakRecord &row, const std::string &field, double fallback = 0.0)
{
    if (field == "rt") return row.rt.value_or(fallback);
    if (field == "height") return row.height.value_or(fallback);
    if (field == "area") return row.area.value_or(fallback);
    if (field == "precursor_mz") return row.precursor_mz.value_or(fallback);
    if (field == "product_mz") return row.product_mz.value_or(fallback);
    if (field == "wavelength_nm") return row.wavelength_nm.value_or(fallback);
    return fallback;
}

int integer_value(const PeakRecord &row, const std::string &field, int fallback = 0)
{
    if (field == "chromatogram_index") return row.chromatogram_index.value_or(fallback);
    if (field == "peak_id") return row.peak_id.value_or(fallback);
    if (field == "polarity") return row.polarity.value_or(fallback);
    return fallback;
}

double number_value(const Json &row, const std::string &field, double fallback = 0.0)
{
    const auto it = row.find(field);
    return it == row.end() || it->is_null() ? fallback : it->get<double>();
}

int integer_value(const Json &row, const std::string &field, int fallback = 0)
{
    const auto it = row.find(field);
    return it == row.end() || it->is_null() ? fallback : it->get<int>();
}

bool text_matches_if_present(const Json &method, const PeakRecord &header,
                            const std::string &method_field, const std::string &header_field)
{
    return !has_value(method, method_field) ||
           text_value(method, method_field) == text_value(header, header_field);
}

bool real_matches_if_present(const Json &method, const PeakRecord &header,
                            const std::string &method_field, const std::string &header_field,
                            double tolerance)
{
    if (!has_value(method, method_field)) return true;
    if (!has_value(header, header_field)) return false;
    return std::abs(number_value(method, method_field) - number_value(header, header_field)) <= tolerance;
}

bool transition_matches(const Json &method, const PeakRecord &header, const std::string &prefix)
{
    const auto signal_field = prefix + "_signal_type";
    if (!text_matches_if_present(method, header, signal_field, "signal_type")) return false;
    const auto chromatogram_field = prefix + "_chromatogram_type";
    if (!text_matches_if_present(method, header, chromatogram_field, "chromatogram_type")) return false;
    const auto polarity_field = prefix + "_polarity";
    // Some vendor chromatogram headers expose polarity as 0 (unknown) even
    // though the transition itself is fully identified by its precursor and
    // product m/z. Unknown polarity must not prevent a valid MRM match.
    if (has_value(method, polarity_field) && integer_value(header, "polarity") != 0 &&
        integer_value(method, polarity_field) != integer_value(header, "polarity")) return false;

    const auto mz_tolerance = has_value(method, prefix + "_mz_tolerance_ppm")
        ? number_value(method, prefix + "_mz_tolerance_ppm") : 20.0;
    const auto precursor = has_value(method, prefix + "_precursor_mz")
        ? number_value(method, prefix + "_precursor_mz") : 0.0;
    const auto product = has_value(method, prefix + "_product_mz")
        ? number_value(method, prefix + "_product_mz") : 0.0;
    if (precursor > 0.0 && !real_matches_if_present(method, header, prefix + "_precursor_mz", "precursor_mz",
                                                       precursor * mz_tolerance * 1e-6)) return false;
    if (product > 0.0 && !real_matches_if_present(method, header, prefix + "_product_mz", "product_mz",
                                                    product * mz_tolerance * 1e-6)) return false;
    if (!text_matches_if_present(method, header, prefix + "_detector", "detector")) return false;
    const auto wavelength = has_value(method, prefix + "_wavelength_nm")
        ? number_value(method, prefix + "_wavelength_nm") : 0.0;
    if (wavelength > 0.0 && !real_matches_if_present(method, header, prefix + "_wavelength_nm", "wavelength_nm", 0.01))
        return false;
    return true;
}

struct PeakSelection
{
    std::string status = "channel_mismatch";
    PeakRecord peak;
    int candidates = 0;
};

PeakSelection select_peak(const Json &method, const std::string &prefix, const std::string &analysis,
                          const std::map<std::pair<std::string, int>, std::vector<PeakRecord>> &peaks_by_chromatogram)
{
    std::vector<PeakRecord> candidates;
    const auto has_rt = has_value(method, "rt");
    const auto expected_rt = has_rt ? number_value(method, "rt") : 0.0;
    const auto rt_tolerance = has_value(method, "rt_tolerance")
        ? number_value(method, "rt_tolerance") : 30.0;
    for (const auto &[key, peaks] : peaks_by_chromatogram)
    {
        if (key.first != analysis) continue;
        for (const auto &peak : peaks)
        {
            if (!transition_matches(method, peak, prefix)) continue;
            if (has_rt && std::abs(number_value(peak, "rt") - expected_rt) > rt_tolerance) continue;
            candidates.push_back(peak);
        }
    }
    if (candidates.empty())
    {
        PeakSelection result;
        result.status = "peak_not_found";
        return result;
    }
    // The retention-time window limits eligibility; it must not make a
    // multi-peak result ambiguous. Once candidates are inside the window,
    // choose the most intense apex. Area is retained as the quantification
    // response, but peak selection must prefer the highest intensity when a
    // trace contains multiple eligible peaks in the window.
    std::sort(candidates.begin(), candidates.end(), [&](const PeakRecord &left, const PeakRecord &right) {
        if (number_value(left, "height") != number_value(right, "height"))
            return number_value(left, "height") > number_value(right, "height");
        if (number_value(left, "area") != number_value(right, "area"))
            return number_value(left, "area") > number_value(right, "area");
        if (number_value(left, "height") != number_value(right, "height"))
            return number_value(left, "height") > number_value(right, "height");
        if (has_rt)
        {
            const auto left_distance = std::abs(number_value(left, "rt") - expected_rt);
            const auto right_distance = std::abs(number_value(right, "rt") - expected_rt);
            if (left_distance != right_distance) return left_distance < right_distance;
        }
        if (integer_value(left, "chromatogram_index") != integer_value(right, "chromatogram_index"))
            return integer_value(left, "chromatogram_index") < integer_value(right, "chromatogram_index");
        return integer_value(left, "peak_id") < integer_value(right, "peak_id");
    });
    PeakSelection result;
    result.status = "matched";
    result.peak = candidates.front();
    result.candidates = static_cast<int>(candidates.size());
    return result;
}

double response_value(const PeakRecord &peak, const Json &method)
{
    const auto metric = text_value(method, "response_metric");
    return metric == "height" ? peak.height.value_or(0.0) : peak.area.value_or(0.0);
}

const std::vector<std::string> quantification_columns{
    "analysis", "target_id", "role", "sample_type", "calibration_level", "nominal_concentration",
    "dilution_factor", "exclude", "internal_standard_id", "normalization", "chromatogram_index", "peak_id", "peak_rt",
    "peak_area", "peak_height", "eic_size", "eic_rt", "eic_intensity", "qualifier_eic_size", "qualifier_eic_rt", "qualifier_eic_intensity", "response_metric", "response", "internal_standard_response", "response_ratio",
    "qualifier_response", "qualifier_ratio", "status", "quantifier_status", "internal_standard_status", "candidate_count",
    "qualifier_status", "included", "back_calculated_concentration", "residual", "relative_residual", "exclusion_reason",
    "rt_error", "calculated_concentration", "final_concentration", "concentration_unit", "qualifier_ratio_pass", "accuracy_percent", "flags"};

const std::vector<std::string> quantification_types{
    "string", "string", "string", "string", "integer", "real", "real", "boolean", "string", "string", "integer", "integer",
    "real", "real", "real", "integer", "array", "array", "integer", "array", "array", "string", "real", "real", "real", "real",
    "real", "string", "string", "string", "integer", "string", "boolean", "real", "real", "real", "string", "real", "real",
    "real", "string", "boolean", "real", "string"};
}

Json set_quantification_plan(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto targets = parameters.value("quantification_method", Json::array());
    detail::reject(utils::validate_method(targets));
    const std::vector<std::string> method_columns{
        "target_id", "role", "internal_standard_id", "rt", "rt_tolerance",
        "response_metric", "normalization", "concentration_unit", "regression_model", "weighting",
        "intercept_mode", "quantifier_signal_type", "quantifier_chromatogram_type", "quantifier_polarity",
        "quantifier_precursor_mz", "quantifier_product_mz", "quantifier_detector",
        "quantifier_wavelength_nm", "quantifier_mz_tolerance_ppm", "qualifier_signal_type",
        "qualifier_chromatogram_type", "qualifier_polarity", "qualifier_precursor_mz", "qualifier_product_mz",
        "qualifier_detector", "qualifier_wavelength_nm", "qualifier_mz_tolerance_ppm",
        "expected_ratio", "ratio_tolerance", "enabled"};
    const std::vector<std::string> method_types{
        "string", "string", "string", "real", "real", "string", "string", "string", "string",
        "string", "string", "string", "string", "integer", "real", "real", "string", "real", "real", "string",
        "string", "integer", "real", "real", "string", "real", "real", "real", "real", "boolean"};
    sdk::TableBatchBuilder method_rows(detail::table_schema(method_columns, method_types));
    const std::map<std::string, Json> method_defaults{
        {"enabled", true}, {"regression_model", "linear"}, {"weighting", "none"},
        {"intercept_mode", "free"}};
    for (const auto &target : targets)
        detail::append_config_row(method_rows, target, method_columns, method_types, method_defaults);
    method_rows.emit(access, "quantificationMethodTable");

    const auto assignments = parameters.value("quant_sample_assignments", Json::array());
    detail::reject(utils::validate_assignments(assignments));
    std::map<std::string, std::string> roles_by_target;
    for (const auto &target : targets)
        roles_by_target[target.value("target_id", std::string{})] = target.value("role", std::string{});
    for (std::size_t i = 0; i < assignments.size(); ++i)
    {
        const auto &assignment = assignments.at(i);
        const auto target_id = assignment.value("target_id", std::string{});
        const auto role = roles_by_target[target_id];
        const auto has_nominal = assignment.contains("nominal_concentration") &&
                                 !assignment.at("nominal_concentration").is_null();
        if (role == "internal_standard")
        {
            if (!has_nominal || !assignment.at("nominal_concentration").is_number() ||
                assignment.at("nominal_concentration").get<double>() < 0.0)
                throw std::invalid_argument("quant_sample_assignments[" + std::to_string(i) +
                                            "].nominal_concentration: internal-standard concentration is required");
        }
        else if (has_nominal && assignment.value("sample_type", std::string{}) != "calibrator" &&
                 assignment.value("sample_type", std::string{}) != "qc")
        {
            throw std::invalid_argument("quant_sample_assignments[" + std::to_string(i) +
                                        "].nominal_concentration: analyte concentration is only valid for calibrators and QCs");
        }
    }
    std::set<std::string> analysis_names;
    access.read_typed_batches(base::utils::input_table(parameters, "analysesTable"), {"analysis"}, "analysis",
        [&](const sdk::PluginProjectAccess::BatchView &batch) {
            const auto analysis_column = batch.column_index("analysis");
            for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                if (!batch.is_null(analysis_column, row))
                    analysis_names.emplace(batch.text(analysis_column, row));
        });
    for (std::size_t i = 0; i < assignments.size(); ++i)
    {
        if (assignments.at(i).contains("analysis") && assignments.at(i).at("analysis").is_string() &&
            analysis_names.find(assignments.at(i).at("analysis").get<std::string>()) == analysis_names.end())
            throw std::invalid_argument("assignments[" + std::to_string(i) + "].analysis: must reference an analysis in analysesTable");
    }
    const std::vector<std::string> assignment_columns{
        "analysis", "target_id", "sample_type", "calibration_level", "nominal_concentration",
        "dilution_factor", "exclude"};
    const std::vector<std::string> assignment_types{
        "string", "string", "string", "integer", "real", "real", "boolean"};
    sdk::TableBatchBuilder assignment_rows(detail::table_schema(assignment_columns, assignment_types));
    const std::map<std::string, Json> assignment_defaults{{"dilution_factor", 1.0}, {"exclude", false}};
    for (const auto &assignment : assignments)
        detail::append_config_row(assignment_rows, assignment, assignment_columns, assignment_types,
                                  assignment_defaults);
    assignment_rows.emit(access, "quantificationAnalysesTable");
    return Json{{"status", "finished"}, {"targets", targets.size()}, {"assignments", assignments.size()}};
}

std::vector<detail::TypedRow> extract_response_rows(sdk::PluginProjectAccess &access, const Json &parameters,
                           std::vector<Json> *method_output = nullptr)
{
    std::string stage = "opening debug session";
    try
    {
    const auto debug_analysis = parameters.contains("debug_analysis") && parameters.at("debug_analysis").is_string()
        ? parameters.at("debug_analysis").get<std::string>() : std::string{};
    auto debug = sdk::DebugSession::open(
        "", "mass_spec.quantify", access.operation_instance(),
        sdk::DebugOptions{debug_analysis, 0.0, -1});
    const auto debug_line = [&](const std::string &message)
    {
        if (debug.enabled()) debug.write_line(message);
    };
    access.report_progress(0.0, "Quantification: loading input tables");
    stage = "reading quantificationMethodTable";
    debug_line("=== Quantification Debug Log ===");
    debug_line("Debug analysis: " + (debug_analysis.empty() ? std::string{"<all analyses>"} : debug_analysis));
    debug_line("Loading quantificationMethodTable");
    access.report_progress(0.01, "Quantification: reading method table");
    std::vector<Json> method;
    try
    {
        const auto rows = base::utils::read_control_rows(
            access, parameters, "quantificationMethodTable",
            {"target_id", "role", "internal_standard_id", "rt", "rt_tolerance", "response_metric",
             "normalization", "quantifier_signal_type", "quantifier_chromatogram_type", "quantifier_polarity",
             "quantifier_precursor_mz", "quantifier_product_mz", "quantifier_detector", "quantifier_wavelength_nm",
             "quantifier_mz_tolerance_ppm", "qualifier_signal_type", "qualifier_chromatogram_type", "qualifier_polarity",
             "qualifier_precursor_mz", "qualifier_product_mz", "qualifier_detector", "qualifier_wavelength_nm",
             "qualifier_mz_tolerance_ppm", "expected_ratio", "ratio_tolerance", "enabled"}, "target_id");
        access.report_progress(0.02, "Quantification: method table read returned");
        if (!rows.is_array()) throw std::runtime_error("reader returned a non-array row set");
        for (const auto &row : rows)
        {
            if (!row.is_object()) throw std::runtime_error("reader returned a non-object method row");
            method.push_back(row);
        }
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("reading quantificationMethodTable: ") + error.what());
    }
    debug_line("quantificationMethodTable rows: " + std::to_string(method.size()));
    access.report_progress(0.06, "Read quantification method table (" + std::to_string(method.size()) + " rows)");
    access.report_progress(0.08, "Loaded quantification method (" + std::to_string(method.size()) + " rows)");
    debug_line("Loading quantificationAnalysesTable");
    stage = "reading quantificationAnalysesTable";
    std::vector<Json> assignments;
    try
    {
        const auto rows = base::utils::read_control_rows(
            access, parameters, "quantificationAnalysesTable",
            {"analysis", "target_id", "sample_type", "calibration_level", "nominal_concentration", "dilution_factor", "exclude"}, "analysis");
        if (!rows.is_array()) throw std::runtime_error("reader returned a non-array row set");
        for (const auto &row : rows)
        {
            if (!row.is_object()) throw std::runtime_error("reader returned a non-object assignment row");
            assignments.push_back(row);
        }
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("reading quantificationAnalysesTable: ") + error.what());
    }
    debug_line("quantificationAnalysesTable rows: " + std::to_string(assignments.size()));
    access.report_progress(0.12, "Read quantification assignment table (" + std::to_string(assignments.size()) + " rows)");
    access.report_progress(0.14, "Loaded quantification assignments (" + std::to_string(assignments.size()) + " rows)");
    std::map<std::string, Json> methods;
    for (const auto &row : method)
        if (row.value("enabled", true)) methods[row.value("target_id", std::string{})] = row;
    for (std::size_t i = 0; i < assignments.size(); ++i)
    {
        const auto target_id = assignments.at(i).value("target_id", std::string{});
        if (!target_id.empty() && methods.find(target_id) == methods.end())
            throw std::invalid_argument("quantificationAnalysesTable assignment " + std::to_string(i) +
                                        " references target_id not present in quantificationMethodTable: " + target_id);
    }

    debug_line("Validated enabled methods: " + std::to_string(methods.size()));
    stage = "counting chromatogramPeaksTable rows";
    access.report_progress(0.20, "Validated quantification method and assignments");
    std::map<std::pair<std::string, int>, std::vector<detail::PeakRecord>> peaks_by_chromatogram;
    const auto peak_table = base::utils::input_table(parameters, "chromatogramPeaksTable");
    const auto peak_count = access.count_rows(peak_table);
    debug_line("chromatogramPeaksTable input: " + peak_table + ", rows: " + std::to_string(peak_count));
    access.report_progress(0.25, "Reading chromatogram peak table (" + std::to_string(peak_count) + " rows)");
    stage = "reading chromatogramPeaksTable";
    const std::vector<std::string> quantification_peak_columns{
        "analysis", "chromatogram_index", "peak_id", "rt", "rt_start", "rt_end", "height", "raw_height",
        "area", "raw_area", "baseline_area", "width", "fwhm", "snr", "asymmetry", "sharpness", "plates",
        "number_points", "integration_algorithm", "integration_status", "manual_override", "chromatogram_id",
        "polarity", "precursor_mz", "product_mz", "activation_ce", "signal_type", "chromatogram_type",
        "detector", "channel", "wavelength_nm", "units", "eic_size", "eic_rt", "eic_intensity"};
    std::size_t peak_row_count = 0;
    try
    {
        access.read_typed_batches(peak_table, quantification_peak_columns, "analysis",
            [&](const sdk::PluginProjectAccess::BatchView &batch) {
                const auto column = [&](const char *name) { return batch.column_index(name); };
                const auto analysis_col = column("analysis");
                const auto chromatogram_col = column("chromatogram_index");
                const auto peak_id_col = column("peak_id");
                const auto rt_col = column("rt");
                const auto height_col = column("height");
                const auto area_col = column("area");
                const auto polarity_col = column("polarity");
                const auto precursor_col = column("precursor_mz");
                const auto product_col = column("product_mz");
                const auto wavelength_col = column("wavelength_nm");
                const auto signal_col = column("signal_type");
                const auto chromatogram_type_col = column("chromatogram_type");
                const auto detector_col = column("detector");
                const auto eic_size_col = column("eic_size");
                const auto eic_rt_col = column("eic_rt");
                const auto eic_intensity_col = column("eic_intensity");
                for (std::uint64_t row = 0; row < batch.row_count(); ++row)
                {
                    detail::PeakRecord peak;
                    peak.analysis = batch.is_null(analysis_col, row) ? std::string{} : std::string(batch.text(analysis_col, row));
                    if (!batch.is_null(chromatogram_col, row)) peak.chromatogram_index = batch.integer(chromatogram_col, row);
                    if (!batch.is_null(peak_id_col, row)) peak.peak_id = batch.integer(peak_id_col, row);
                    if (!batch.is_null(rt_col, row)) peak.rt = batch.real(rt_col, row);
                    if (!batch.is_null(height_col, row)) peak.height = batch.real(height_col, row);
                    if (!batch.is_null(area_col, row)) peak.area = batch.real(area_col, row);
                    if (!batch.is_null(polarity_col, row)) peak.polarity = batch.integer(polarity_col, row);
                    if (!batch.is_null(precursor_col, row)) peak.precursor_mz = batch.real(precursor_col, row);
                    if (!batch.is_null(product_col, row)) peak.product_mz = batch.real(product_col, row);
                    if (!batch.is_null(wavelength_col, row)) peak.wavelength_nm = batch.real(wavelength_col, row);
                    if (!batch.is_null(signal_col, row)) peak.signal_type = batch.text(signal_col, row);
                    if (!batch.is_null(chromatogram_type_col, row)) peak.chromatogram_type = batch.text(chromatogram_type_col, row);
                    if (!batch.is_null(detector_col, row)) peak.detector = batch.text(detector_col, row);
                    if (!batch.is_null(eic_size_col, row)) peak.eic_size = batch.integer(eic_size_col, row);
                    peak.has_eic_rt = !batch.is_null(eic_rt_col, row);
                    peak.has_eic_intensity = !batch.is_null(eic_intensity_col, row);
                    if (peak.has_eic_rt && batch.is_list(eic_rt_col))
                        for (std::uint64_t item = 0; item < batch.list_size(eic_rt_col, row); ++item)
                            if (!batch.list_is_null(eic_rt_col, row, item)) peak.eic_rt.push_back(batch.list_real(eic_rt_col, row, item));
                    if (peak.has_eic_intensity && batch.is_list(eic_intensity_col))
                        for (std::uint64_t item = 0; item < batch.list_size(eic_intensity_col, row); ++item)
                            if (!batch.list_is_null(eic_intensity_col, row, item)) peak.eic_intensity.push_back(batch.list_real(eic_intensity_col, row, item));
                    const auto key = std::make_pair(peak.analysis, static_cast<int>(peak.chromatogram_index.value_or(0)));
                    peaks_by_chromatogram[key].push_back(std::move(peak));
                    ++peak_row_count;
                }
            });
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("reading chromatogramPeaksTable: ") + error.what());
    }
    debug_line("Peak table snapshot returned: rows=" + std::to_string(peak_row_count) +
               ", retained chromatograms=" + std::to_string(peaks_by_chromatogram.size()));
    access.report_progress(0.60, "Peak table read completed (" + std::to_string(peak_row_count) +
                           " rows; " + std::to_string(peaks_by_chromatogram.size()) +
                           " chromatograms retained)");
    access.report_progress(0.62, "Selecting quantification peaks from indexed chromatogram metadata");
    stage = "selecting quantification peaks";
    std::map<std::string, std::size_t> result_indices;
    std::vector<detail::TypedRow> output;
    std::size_t processed_assignments = 0;
    std::size_t skipped_water_blanks = 0;
    for (const auto &assignment : assignments)
    {
        const auto analysis = assignment.value("analysis", std::string{});
        const auto target_id = assignment.value("target_id", std::string{});
        if (detail::is_water_blank(assignment))
        {
            ++skipped_water_blanks;
            continue;
        }
        try
        {
            const auto method_it = methods.find(target_id);
            if (method_it == methods.end()) continue;
            const auto &target = method_it->second;
            const auto quantifier = detail::select_peak(target, "quantifier", analysis, peaks_by_chromatogram);
            if (debug.enabled() && (debug_analysis.empty() || debug_analysis == analysis))
                debug_line("Analysis " + analysis + ", target " + target_id + ": quantifier=" + quantifier.status +
                           ", candidates=" + std::to_string(quantifier.candidates));
        detail::TypedRow row(detail::quantification_columns.size(), std::monostate{});
        const auto role = target.value("role", std::string{});
        const auto normalization = target.value("normalization", std::string{"none"});
        detail::set_cell(row, detail::quantification_columns, "analysis", analysis);
        detail::set_cell(row, detail::quantification_columns, "target_id", target_id);
        detail::set_cell(row, detail::quantification_columns, "role", role);
        detail::set_cell(row, detail::quantification_columns, "sample_type", assignment.value("sample_type", std::string{}));
        if (assignment.contains("calibration_level") && !assignment.at("calibration_level").is_null())
            detail::set_cell(row, detail::quantification_columns, "calibration_level", assignment.at("calibration_level").get<std::int64_t>());
        if (assignment.contains("nominal_concentration") && !assignment.at("nominal_concentration").is_null())
            detail::set_cell(row, detail::quantification_columns, "nominal_concentration", assignment.at("nominal_concentration").get<double>());
        detail::set_cell(row, detail::quantification_columns, "dilution_factor", assignment.value("dilution_factor", 1.0));
        detail::set_cell(row, detail::quantification_columns, "exclude", assignment.value("exclude", false));
        if (target.contains("internal_standard_id") && !target.at("internal_standard_id").is_null())
            detail::set_cell(row, detail::quantification_columns, "internal_standard_id", target.at("internal_standard_id").get<std::string>());
        detail::set_cell(row, detail::quantification_columns, "normalization", normalization);
        detail::set_cell(row, detail::quantification_columns, "response_metric", target.value("response_metric", std::string{"area"}));
        detail::set_cell(row, detail::quantification_columns, "status", quantifier.status);
        detail::set_cell(row, detail::quantification_columns, "quantifier_status", quantifier.status);
        detail::set_cell(row, detail::quantification_columns, "internal_standard_status",
            role == "analyte" && normalization == "internal_standard" ? std::string("not_evaluated") :
            role == "internal_standard" ? std::string("not_applicable") : std::string("not_required"));
        detail::set_cell(row, detail::quantification_columns, "candidate_count", static_cast<std::int64_t>(quantifier.candidates));
        detail::set_cell(row, detail::quantification_columns, "qualifier_status", std::string("not_configured"));
        if (quantifier.status == "matched")
        {
            detail::set_cell(row, detail::quantification_columns, "chromatogram_index", static_cast<std::int64_t>(quantifier.peak.chromatogram_index.value_or(0)));
            detail::set_cell(row, detail::quantification_columns, "peak_id", static_cast<std::int64_t>(quantifier.peak.peak_id.value_or(0)));
            detail::set_cell(row, detail::quantification_columns, "peak_rt", quantifier.peak.rt.value_or(0.0));
            detail::set_cell(row, detail::quantification_columns, "peak_area", quantifier.peak.area.value_or(0.0));
            detail::set_cell(row, detail::quantification_columns, "peak_height", quantifier.peak.height.value_or(0.0));
            if (quantifier.peak.eic_size) detail::set_cell(row, detail::quantification_columns, "eic_size", static_cast<std::int64_t>(*quantifier.peak.eic_size));
            if (quantifier.peak.has_eic_rt) {
                std::vector<sdk::TableBatchBuilder::ListCell> values; for (const auto value : quantifier.peak.eic_rt) values.emplace_back(value);
                detail::set_cell(row, detail::quantification_columns, "eic_rt", std::move(values));
            }
            if (quantifier.peak.has_eic_intensity) {
                std::vector<sdk::TableBatchBuilder::ListCell> values; for (const auto value : quantifier.peak.eic_intensity) values.emplace_back(value);
                detail::set_cell(row, detail::quantification_columns, "eic_intensity", std::move(values));
            }
            detail::set_cell(row, detail::quantification_columns, "response", detail::response_value(quantifier.peak, target));
        }
        if (detail::has_value(target, "qualifier_signal_type"))
        {
            const auto qualifier = detail::select_peak(target, "qualifier", analysis, peaks_by_chromatogram);
            if (debug.enabled() && (debug_analysis.empty() || debug_analysis == analysis))
                debug_line("Analysis " + analysis + ", target " + target_id + ": qualifier=" + qualifier.status +
                           ", candidates=" + std::to_string(qualifier.candidates));
            detail::set_cell(row, detail::quantification_columns, "qualifier_status", qualifier.status);
            if (qualifier.status == "matched")
            {
                if (qualifier.peak.eic_size) detail::set_cell(row, detail::quantification_columns, "qualifier_eic_size", static_cast<std::int64_t>(*qualifier.peak.eic_size));
                if (qualifier.peak.has_eic_rt) {
                    std::vector<sdk::TableBatchBuilder::ListCell> values; for (const auto value : qualifier.peak.eic_rt) values.emplace_back(value);
                    detail::set_cell(row, detail::quantification_columns, "qualifier_eic_rt", std::move(values));
                }
                if (qualifier.peak.has_eic_intensity) {
                    std::vector<sdk::TableBatchBuilder::ListCell> values; for (const auto value : qualifier.peak.eic_intensity) values.emplace_back(value);
                    detail::set_cell(row, detail::quantification_columns, "qualifier_eic_intensity", std::move(values));
                }
                const auto qualifier_response = detail::response_value(qualifier.peak, target);
                detail::set_cell(row, detail::quantification_columns, "qualifier_response", qualifier_response);
                const auto response = detail::row_number(row, detail::quantification_columns, "response");
                if (std::isfinite(response) && response != 0.0)
                {
                    const auto ratio = qualifier_response / response;
                    detail::set_cell(row, detail::quantification_columns, "qualifier_ratio", ratio);
                    if (detail::has_value(target, "expected_ratio") && detail::has_value(target, "ratio_tolerance"))
                        detail::set_cell(row, detail::quantification_columns, "qualifier_status",
                            std::string(std::abs(ratio - detail::number_value(target, "expected_ratio")) <=
                            detail::number_value(target, "ratio_tolerance") ? "pass" : "ratio_out_of_range"));
                }
            }
        }
            result_indices[analysis + "\n" + target_id] = output.size();
            output.push_back(std::move(row));
            ++processed_assignments;
            if (processed_assignments % 16 == 0 || processed_assignments == assignments.size())
            {
                access.report_progress(
                    0.62 + 0.18 * static_cast<double>(processed_assignments) /
                        static_cast<double>(std::max<std::size_t>(assignments.size(), 1)),
                    "Selecting quantification peaks (assignment " + std::to_string(processed_assignments) +
                        "/" + std::to_string(assignments.size()) + ")");
            }
        }
        catch (const std::exception &error)
        {
            throw std::invalid_argument("response extraction for analysis '" + analysis +
                                        "' target '" + target_id + "': " + error.what());
        }
    }
    try
    {
        debug_line("Peak selection completed; output rows: " + std::to_string(output.size()) +
                   ", Wasser blanks skipped: " + std::to_string(skipped_water_blanks));
        access.report_progress(0.82, "Calculating internal-standard responses");
        for (auto &row : output)
        {
            if (detail::row_text(row, detail::quantification_columns, "normalization") != "internal_standard") continue;
            const auto is_id = detail::row_text(row, detail::quantification_columns, "internal_standard_id");
            const auto analysis = detail::row_text(row, detail::quantification_columns, "analysis");
            if (is_id.empty() || detail::row_text(row, detail::quantification_columns, "role") != "analyte") continue;
            const auto is_it = result_indices.find(analysis + "\n" + is_id);
            if (is_it == result_indices.end())
            {
                detail::set_cell(row, detail::quantification_columns, "internal_standard_status", std::string("not_assigned"));
                if (detail::row_text(row, detail::quantification_columns, "quantifier_status") == "matched")
                    detail::set_cell(row, detail::quantification_columns, "status", std::string("internal_standard_not_assigned"));
                continue;
            }
            const auto &is_row = output.at(is_it->second);
            const auto is_status = detail::row_text(is_row, detail::quantification_columns, "quantifier_status");
            if (is_status != "matched")
            {
                detail::set_cell(row, detail::quantification_columns, "internal_standard_status", is_status);
                if (detail::row_text(row, detail::quantification_columns, "quantifier_status") == "matched")
                    detail::set_cell(row, detail::quantification_columns, "status", "internal_standard_" + is_status);
                continue;
            }
            detail::set_cell(row, detail::quantification_columns, "internal_standard_status", std::string("matched"));
            const auto is_response = detail::row_number(is_row, detail::quantification_columns, "response");
            detail::set_cell(row, detail::quantification_columns, "internal_standard_response", is_response);
            const auto response = detail::row_number(row, detail::quantification_columns, "response");
            if (!std::isfinite(response) || response == 0.0) continue;
            if (is_response == 0.0) { detail::set_cell(row, detail::quantification_columns, "status", std::string("internal_standard_zero_response")); continue; }
            detail::set_cell(row, detail::quantification_columns, "response_ratio", response / is_response);
        }
        debug_line("Internal-standard response calculation completed");
        stage = "assembling quantificationTable";
        access.report_progress(0.90, "Quantification response rows assembled; continuing calibration");
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("response table assembly: ") + error.what());
    }
    debug_line("=== End Quantification Response Extraction Debug Log ===");
    if (debug.enabled())
        access.emit_result("quantificationDebugResult", Json{
            {"schema", "streamfind.mass_spec.quantify_debug/v1"},
            {"enabled", true},
            {"operation", "mass_spec.quantify"},
            {"analysis", debug_analysis},
            {"log_text", debug.text()}}.dump());
    if (method_output != nullptr) *method_output = method;
    return output;
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument("mass_spec.quantify response extraction at " + stage + ": " + error.what());
    }
}

namespace detail
{
struct CalibrationFit
{
    TypedRow model;
    std::vector<TypedRow> points;
};

const std::vector<std::string> calibration_model_columns{
    "target_id", "model", "weighting", "intercept_mode", "coefficient_0", "coefficient_1",
    "coefficient_2", "r_squared", "adjusted_r_squared", "rmse", "n_points", "range_min", "range_max", "fit_status"};
const std::vector<std::string> calibration_model_types{
    "string", "string", "string", "string", "real", "real", "real", "real", "real", "real", "integer", "real", "real", "string"};
const std::vector<std::string> calibration_point_columns{
    "target_id", "analysis", "peak_id", "calibration_level", "nominal_concentration", "response",
    "internal_standard_response", "response_ratio", "included", "back_calculated_concentration", "residual",
    "relative_residual", "exclusion_reason"};
const std::vector<std::string> calibration_point_types{
    "string", "string", "integer", "integer", "real", "real", "real", "real", "boolean", "real", "real", "real", "string"};

double response_for_calibration(const TypedRow &row, const Json &method)
{
    const auto normalization = text_value(method, "normalization");
    return normalization == "internal_standard"
        ? row_number(row, quantification_columns, "response_ratio")
        : row_number(row, quantification_columns, "response");
}

std::string calibration_exclusion_reason(const TypedRow &row, const Json &method)
{
    if (row_text(row, quantification_columns, "role") != "analyte") return "not_analyte";
    if (row_text(row, quantification_columns, "sample_type") != "calibrator") return "not_calibrator";
    if (row_boolean(row, quantification_columns, "exclude")) return "excluded_by_assignment";
    if (!std::isfinite(row_number(row, quantification_columns, "nominal_concentration"))) return "missing_nominal_concentration";
    if (row_text(row, quantification_columns, "status") != "matched") return "response_status_" + row_text(row, quantification_columns, "status");
    if (text_value(method, "normalization") == "internal_standard" &&
        row_text(row, quantification_columns, "internal_standard_status") != "matched")
        return "internal_standard_" + row_text(row, quantification_columns, "internal_standard_status");
    const auto response = response_for_calibration(row, method);
    if (!std::isfinite(response) || response <= 0.0) return "nonpositive_response";
    return {};
}

std::vector<Json> read_quantification_method(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::read_control_rows(
        access, parameters, "quantificationMethodTable",
        {"target_id", "role", "normalization", "concentration_unit", "regression_model", "weighting",
         "intercept_mode", "rt", "enabled", "expected_ratio", "ratio_tolerance"}, "target_id");
    if (!rows.is_array())
        throw std::invalid_argument("quantificationMethodTable reader returned a non-array row set");
    std::vector<Json> result;
    result.reserve(rows.size());
    for (const auto &row : rows)
    {
        if (!row.is_object())
            throw std::invalid_argument("quantificationMethodTable reader returned a non-object row");
        result.push_back(row);
    }
    return result;
}

std::map<std::string, Json> enabled_methods(const std::vector<Json> &rows)
{
    std::map<std::string, Json> result;
    for (const auto &row : rows)
        if (row.value("enabled", true)) result[text_value(row, "target_id")] = row;
    return result;
}

CalibrationFit fit_target(const std::string &target_id, const Json &method,
                          std::vector<TypedRow> &responses, const int minimum_points)
{
    CalibrationFit result;
    result.model.resize(calibration_model_columns.size(), std::monostate{});
    set_cell(result.model, calibration_model_columns, "target_id", target_id);
    set_cell(result.model, calibration_model_columns, "model", method.value("regression_model", std::string{"linear"}));
    set_cell(result.model, calibration_model_columns, "weighting", method.value("weighting", std::string{"none"}));
    set_cell(result.model, calibration_model_columns, "intercept_mode", method.value("intercept_mode", std::string{"free"}));
    set_cell(result.model, calibration_model_columns, "n_points", std::int64_t{0});
    set_cell(result.model, calibration_model_columns, "fit_status", std::string("insufficient_data"));

    std::vector<std::pair<double, double>> valid;
    std::vector<std::size_t> included_rows;
    for (std::size_t row_index = 0; row_index < responses.size(); ++row_index)
    {
        auto &row = responses[row_index];
        if (row_text(row, quantification_columns, "target_id") != target_id) continue;
        TypedRow point(calibration_point_columns.size(), std::monostate{});
        set_cell(point, calibration_point_columns, "target_id", target_id);
        set_cell(point, calibration_point_columns, "analysis", row_text(row, quantification_columns, "analysis"));
        const auto peak_id = row_number(row, quantification_columns, "peak_id");
        if (std::isfinite(peak_id)) set_cell(point, calibration_point_columns, "peak_id", static_cast<std::int64_t>(peak_id));
        const auto level = row_number(row, quantification_columns, "calibration_level");
        if (std::isfinite(level)) set_cell(point, calibration_point_columns, "calibration_level", static_cast<std::int64_t>(level));
        for (const auto *field : {"nominal_concentration", "response", "internal_standard_response", "response_ratio"}) {
            const auto value = row_number(row, quantification_columns, field);
            if (std::isfinite(value)) set_cell(point, calibration_point_columns, field, value);
        }
        set_cell(point, calibration_point_columns, "included", false);
        const auto reason = calibration_exclusion_reason(row, method);
        if (!reason.empty())
        {
            set_cell(point, calibration_point_columns, "exclusion_reason", reason);
            result.points.push_back(std::move(point));
            continue;
        }
        const auto x = row_number(row, quantification_columns, "nominal_concentration");
        const auto y = response_for_calibration(row, method);
        set_cell(point, calibration_point_columns, "included", true);
        included_rows.push_back(row_index);
        valid.emplace_back(x, y);
        result.points.push_back(std::move(point));
    }

    if (valid.size() < static_cast<std::size_t>(std::max(minimum_points, 2)))
        return result;

    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    for (const auto &[x, y] : valid)
    {
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const auto n = static_cast<double>(valid.size());
    const auto denominator = n * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) <= 1e-15) return result;
    const auto slope = (n * sum_xy - sum_x * sum_y) / denominator;
    const auto intercept = (sum_y - slope * sum_x) / n;
    if (!std::isfinite(slope) || !std::isfinite(intercept) || std::abs(slope) <= 1e-15)
        return result;

    const auto mean_y = sum_y / n;
    double ss_res = 0.0, ss_tot = 0.0;
    double range_min = valid.front().first, range_max = valid.front().first;
    for (const auto &[x, y] : valid)
    {
        const auto predicted = intercept + slope * x;
        ss_res += (y - predicted) * (y - predicted);
        ss_tot += (y - mean_y) * (y - mean_y);
        range_min = std::min(range_min, x);
        range_max = std::max(range_max, x);
    }
    const auto r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    const auto rmse = std::sqrt(ss_res / n);
    const auto adjusted = valid.size() > 2
        ? 1.0 - (1.0 - r_squared) * (n - 1.0) / (n - 2.0) : 0.0;
    set_cell(result.model, calibration_model_columns, "coefficient_0", intercept);
    set_cell(result.model, calibration_model_columns, "coefficient_1", slope);
    set_cell(result.model, calibration_model_columns, "r_squared", r_squared);
    set_cell(result.model, calibration_model_columns, "adjusted_r_squared", adjusted);
    set_cell(result.model, calibration_model_columns, "rmse", rmse);
    set_cell(result.model, calibration_model_columns, "n_points", static_cast<std::int64_t>(valid.size()));
    set_cell(result.model, calibration_model_columns, "range_min", range_min);
    set_cell(result.model, calibration_model_columns, "range_max", range_max);
    set_cell(result.model, calibration_model_columns, "fit_status", std::string(slope > 0.0 ? "ok" : "failed"));
    if (slope <= 0.0)
        return result;
    for (auto &point : result.points)
    {
        if (!row_boolean(point, calibration_point_columns, "included")) continue;
        const auto x = row_number(point, calibration_point_columns, "nominal_concentration");
        const auto y = text_value(method, "normalization") == "internal_standard"
            ? row_number(point, calibration_point_columns, "response_ratio")
            : row_number(point, calibration_point_columns, "response");
        const auto back_calculated = (y - intercept) / slope;
        set_cell(point, calibration_point_columns, "back_calculated_concentration", back_calculated);
        set_cell(point, calibration_point_columns, "residual", y - (intercept + slope * x));
        if (x != 0.0) set_cell(point, calibration_point_columns, "relative_residual", 100.0 * (back_calculated - x) / x);
    }
    for (const auto index : included_rows) {
        const auto point = std::find_if(result.points.begin(), result.points.end(), [&](const TypedRow &candidate) {
            return row_text(candidate, calibration_point_columns, "analysis") == row_text(responses[index], quantification_columns, "analysis");
        });
        if (point == result.points.end()) continue;
        for (const auto *field : {"included", "back_calculated_concentration", "residual", "relative_residual", "exclusion_reason"}) {
            set_cell(responses[index], quantification_columns, field,
                     get_cell(*point, calibration_point_columns, field));
        }
    }
    return result;
}

}

namespace detail
{
std::pair<std::vector<TypedRow>, std::vector<TypedRow>> fit_quantification_calibration(
    const std::map<std::string, Json> &methods, std::vector<TypedRow> &responses,
    const int minimum_points)
{
    std::vector<TypedRow> models;
    std::vector<TypedRow> points;
    for (const auto &[target_id, method] : methods)
    {
        if (detail::text_value(method, "role") != "analyte") continue;
        auto fit = detail::fit_target(target_id, method, responses, minimum_points);
        models.push_back(std::move(fit.model));
        for (auto &point : fit.points)
            points.push_back(std::move(point));
    }
    return {models, points};
}

void quantify_samples(const std::map<std::string, Json> &methods,
                      std::vector<TypedRow> &responses, const std::vector<TypedRow> &models,
                      const bool require_qualifier, const double max_qc_error)
{
    std::map<std::string, const TypedRow *> model_by_target;
    for (const auto &model : models) model_by_target[row_text(model, calibration_model_columns, "target_id")] = &model;
    for (auto &row : responses)
    {
        if (row_text(row, quantification_columns, "role") != "analyte") continue;
        const auto target_id = row_text(row, quantification_columns, "target_id");
        const auto method_it = methods.find(target_id);
        if (method_it == methods.end()) continue;
        const auto &method = method_it->second;
        if (method.contains("concentration_unit") && !method.at("concentration_unit").is_null())
            set_cell(row, quantification_columns, "concentration_unit", method.at("concentration_unit").get<std::string>());
        const auto peak_rt = row_number(row, quantification_columns, "peak_rt");
        const auto method_rt = number_value(method, "rt");
        if (std::isfinite(peak_rt) && std::isfinite(method_rt))
            set_cell(row, quantification_columns, "rt_error", peak_rt - method_rt);
        std::vector<std::string> flags;
        const auto qualifier_status = row_text(row, quantification_columns, "qualifier_status");
        if (qualifier_status == "pass") set_cell(row, quantification_columns, "qualifier_ratio_pass", true);
        else if (qualifier_status != "not_configured" && qualifier_status != "matched" && !qualifier_status.empty()) {
            set_cell(row, quantification_columns, "qualifier_ratio_pass", false);
            flags.push_back("qualifier_" + qualifier_status);
        }
        const auto model_it = model_by_target.find(target_id);
        if (row_text(row, quantification_columns, "status") == "matched" && model_it != model_by_target.end() &&
            row_text(*model_it->second, calibration_model_columns, "fit_status") == "ok")
        {
            const auto &model = *model_it->second;
            const auto y = text_value(method, "normalization") == "internal_standard"
                ? row_number(row, quantification_columns, "response_ratio") : row_number(row, quantification_columns, "response");
            const auto slope = row_number(model, calibration_model_columns, "coefficient_1");
            const auto intercept = row_number(model, calibration_model_columns, "coefficient_0");
            if (std::isfinite(y) && y > 0.0 && std::isfinite(slope) && std::abs(slope) > 1e-15)
            {
                const auto calculated = (y - intercept) / slope;
                const auto dilution = row_number(row, quantification_columns, "dilution_factor", 1.0);
                const auto final_value = calculated * dilution;
                set_cell(row, quantification_columns, "calculated_concentration", calculated);
                set_cell(row, quantification_columns, "final_concentration", final_value);
                const auto lower = row_number(model, calibration_model_columns, "range_min");
                const auto upper = row_number(model, calibration_model_columns, "range_max");
                std::string status = calculated < lower ? "below_calibration_range" : calculated > upper ? "above_calibration_range" : "ok";
                if (status != "ok") flags.push_back(status);
                else if (require_qualifier && std::holds_alternative<bool>(get_cell(row, quantification_columns, "qualifier_ratio_pass")) &&
                         !row_boolean(row, quantification_columns, "qualifier_ratio_pass")) { status = "qualifier_failed"; flags.push_back(status); }
                const auto nominal = row_number(row, quantification_columns, "nominal_concentration");
                if (std::isfinite(nominal) && nominal != 0.0) {
                    const auto accuracy = 100.0 * final_value / nominal;
                    set_cell(row, quantification_columns, "accuracy_percent", accuracy);
                    if (row_text(row, quantification_columns, "sample_type") == "qc" && std::abs(accuracy - 100.0) > max_qc_error) {
                        flags.push_back("qc_accuracy_out_of_range"); status = "qc_failed";
                    }
                }
                set_cell(row, quantification_columns, "status", std::move(status));
            }
            else { set_cell(row, quantification_columns, "status", std::string("invalid_response")); flags.push_back("invalid_response"); }
        }
        else if (model_it == model_by_target.end() || row_text(*model_it->second, calibration_model_columns, "fit_status") != "ok")
        {
            set_cell(row, quantification_columns, "status", std::string("calibration_not_available"));
            flags.push_back("calibration_not_available");
        }
        if (!flags.empty())
        {
            std::ostringstream joined;
            for (std::size_t i = 0; i < flags.size(); ++i) { if (i != 0) joined << ','; joined << flags[i]; }
            set_cell(row, quantification_columns, "flags", joined.str());
        }
    }
}
}

Json quantify(sdk::PluginProjectAccess &access, const Json &parameters)
{
    std::string stage = "response extraction";
    try
    {
        std::vector<Json> method_rows;
        auto responses = extract_response_rows(access, parameters, &method_rows);
        stage = "building enabled method index";
        const auto methods = detail::enabled_methods(method_rows);
        stage = "reading quantification parameters";
        const auto minimum_points = parameters.value("minimum_calibration_points", 3);
        stage = "fitting calibration models";
        access.report_progress(0.92, "Fitting quantification calibration models");
        const auto calibration = detail::fit_quantification_calibration(methods, responses, minimum_points);
        const auto &models = calibration.first;
        const auto &points = calibration.second;
        std::map<std::string, std::size_t> response_by_key;
        for (std::size_t index = 0; index < responses.size(); ++index)
            response_by_key[detail::row_text(responses[index], detail::quantification_columns, "analysis") + "\n" +
                            detail::row_text(responses[index], detail::quantification_columns, "target_id")] = index;
        for (const auto &point : points) {
            const auto key = detail::row_text(point, detail::calibration_point_columns, "analysis") + "\n" +
                             detail::row_text(point, detail::calibration_point_columns, "target_id");
            const auto found = response_by_key.find(key);
            if (found == response_by_key.end()) continue;
            auto &response = responses[found->second];
            for (const auto *field : {"included", "back_calculated_concentration", "residual", "relative_residual", "exclusion_reason"})
                detail::set_cell(response, detail::quantification_columns, field,
                    detail::get_cell(point, detail::calibration_point_columns, field));
        }
        stage = "publishing calibration models";
        detail::emit_typed_rows(access, "quantificationCalibrationModelsTable",
                                detail::calibration_model_columns, detail::calibration_model_types, models);
        stage = "quantifying samples";
        access.report_progress(0.96, "Applying calibration models to samples");
        detail::quantify_samples(
            methods, responses, models, parameters.value("require_qualifier", false),
            parameters.value("qc_max_relative_error", 20.0));
        stage = "publishing quantification table";
        detail::emit_typed_rows(access, "quantificationTable", detail::quantification_columns,
                                detail::quantification_types, responses);
        access.report_progress(1.0, "Quantification workflow completed");
        return Json{{"status", "finished"}, {"responses", responses.size()}, {"models", models.size()},
                    {"points", points.size()}, {"results", responses.size()}, {"rows", responses.size()}};
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument("mass_spec.quantify at " + stage + ": " + error.what());
    }
}
}
