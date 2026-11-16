#include "operations/quantification/operations.hpp"

#include "operations/base.hpp"
#include "streamfind/sdk/debug_session.hpp"
#include "utils/chromatograms.hpp"
#include "utils/quantification.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <cctype>
#include <map>
#include <set>

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

Json copy_fields(const Json &source, const std::vector<std::string> &fields)
{
    Json row;
    for (const auto &field : fields)
        row[field] = source.contains(field) ? source.at(field) : Json(nullptr);
    return row;
}

void emit(sdk::PluginProjectAccess &access, const char *artifact,
          const std::vector<std::string> &columns, const std::vector<std::string> &types,
          const Json &rows)
{
    access.emit_table_rows(artifact, columns, types, rows);
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

bool text_matches_if_present(const Json &method, const Json &header,
                            const std::string &method_field, const std::string &header_field)
{
    return !has_value(method, method_field) ||
           text_value(method, method_field) == text_value(header, header_field);
}

bool real_matches_if_present(const Json &method, const Json &header,
                            const std::string &method_field, const std::string &header_field,
                            double tolerance)
{
    if (!has_value(method, method_field)) return true;
    if (!has_value(header, header_field)) return false;
    return std::abs(number_value(method, method_field) - number_value(header, header_field)) <= tolerance;
}

bool transition_matches(const Json &method, const Json &header, const std::string &prefix)
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
    Json peak = Json::object();
    int candidates = 0;
};

PeakSelection select_peak(const Json &method, const std::string &prefix, const std::string &analysis,
                          const std::map<std::pair<std::string, int>, std::vector<Json>> &peaks_by_chromatogram)
{
    std::vector<Json> candidates;
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
    std::sort(candidates.begin(), candidates.end(), [&](const Json &left, const Json &right) {
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

double response_value(const Json &peak, const Json &method)
{
    const auto metric = text_value(method, "response_metric");
    const auto field = metric == "height" ? "height" : "area";
    const auto it = peak.find(field);
    if (it == peak.end() || it->is_null()) return 0.0;
    return it->get<double>();
}

const std::vector<std::string> quantification_columns{
    "analysis", "target_id", "role", "sample_type", "calibration_level", "nominal_concentration",
    "dilution_factor", "exclude", "internal_standard_id", "normalization", "chromatogram_index", "peak_id", "peak_rt",
    "rt_error", "peak_area", "peak_height", "eic_size", "eic_rt", "eic_intensity", "qualifier_eic_size", "qualifier_eic_rt", "qualifier_eic_intensity", "response_metric", "response", "internal_standard_response", "response_ratio",
    "qualifier_response", "qualifier_ratio", "status", "quantifier_status", "internal_standard_status", "candidate_count",
    "qualifier_status", "included", "back_calculated_concentration", "residual", "relative_residual", "exclusion_reason",
    "calculated_concentration", "final_concentration", "concentration_unit", "qualifier_ratio_pass", "accuracy_percent", "flags"};

const std::vector<std::string> quantification_types{
    "string", "string", "string", "string", "integer", "real", "real", "boolean", "string", "string", "integer", "integer",
    "real", "real", "real", "real", "integer", "array", "array", "integer", "array", "array", "string", "real", "real", "real", "real", "real", "string", "string", "string",
    "integer", "string", "boolean", "real", "real", "real", "string", "real", "real", "string", "boolean", "real", "string"
    };
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
    Json method_rows = Json::array();
    for (const auto &target : targets)
    {
        auto row = detail::copy_fields(target, method_columns);
        if (!row.contains("enabled") || row.at("enabled").is_null()) row["enabled"] = true;
        if (!row.contains("regression_model") || row.at("regression_model").is_null()) row["regression_model"] = "linear";
        if (!row.contains("weighting") || row.at("weighting").is_null()) row["weighting"] = "none";
        if (!row.contains("intercept_mode") || row.at("intercept_mode").is_null()) row["intercept_mode"] = "free";
        method_rows.push_back(std::move(row));
    }
    detail::emit(access, "quantificationMethodTable", method_columns, method_types, method_rows);

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
    const auto analyses = base::utils::input_rows(
        access, parameters, "analysesTable", {"analysis"}, "analysis");
    std::set<std::string> analysis_names;
    for (const auto &analysis : analyses)
        if (analysis.contains("analysis") && analysis.at("analysis").is_string())
            analysis_names.insert(analysis.at("analysis").get<std::string>());
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
    Json assignment_rows = Json::array();
    for (const auto &assignment : assignments)
    {
        auto row = detail::copy_fields(assignment, assignment_columns);
        if (!row.contains("dilution_factor") || row.at("dilution_factor").is_null()) row["dilution_factor"] = 1.0;
        if (!row.contains("exclude") || row.at("exclude").is_null()) row["exclude"] = false;
        assignment_rows.push_back(std::move(row));
    }
    detail::emit(access, "quantificationAnalysesTable", assignment_columns, assignment_types, assignment_rows);
    return Json{{"status", "finished"}, {"targets", method_rows.size()}, {"assignments", assignment_rows.size()}};
}

Json extract_response_rows(sdk::PluginProjectAccess &access, const Json &parameters,
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
        const auto rows = base::utils::input_rows(
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
        const auto rows = base::utils::input_rows(
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
    std::map<std::pair<std::string, int>, std::vector<Json>> peaks_by_chromatogram;
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
    Json peak_rows;
    try
    {
        peak_rows = access.read(peak_table, quantification_peak_columns, "analysis");
        if (!peak_rows.is_array()) throw std::runtime_error("reader returned a non-array row set");
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("reading chromatogramPeaksTable: ") + error.what());
    }
    for (const auto &peak : peak_rows)
    {
        const auto key = std::make_pair(detail::text_value(peak, "analysis"),
                                        detail::integer_value(peak, "chromatogram_index"));
        peaks_by_chromatogram[key].push_back(peak);
    }
    debug_line("Peak table snapshot returned: rows=" + std::to_string(peak_rows.size()) +
               ", retained chromatograms=" + std::to_string(peaks_by_chromatogram.size()));
    access.report_progress(0.60, "Peak table read completed (" + std::to_string(peak_rows.size()) +
                           " rows; " + std::to_string(peaks_by_chromatogram.size()) +
                           " chromatograms retained)");
    access.report_progress(0.62, "Selecting quantification peaks from indexed chromatogram metadata");
    stage = "selecting quantification peaks";
    std::map<std::string, Json> results;
    Json output = Json::array();
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
        Json row{{"analysis", analysis}, {"target_id", target_id}, {"role", target.value("role", std::string{})},
                 {"sample_type", assignment.value("sample_type", std::string{})},
                 {"calibration_level", assignment.value("calibration_level", Json(nullptr))},
                 {"nominal_concentration", assignment.value("nominal_concentration", Json(nullptr))},
                 {"dilution_factor", assignment.value("dilution_factor", 1.0)},
                 {"exclude", assignment.value("exclude", false)},
                 {"internal_standard_id", target.value("internal_standard_id", Json(nullptr))},
                 {"normalization", target.value("normalization", std::string{"none"})},
                 {"chromatogram_index", nullptr}, {"peak_id", nullptr}, {"peak_rt", nullptr},
                 {"peak_area", nullptr}, {"peak_height", nullptr},
                 {"response_metric", target.value("response_metric", std::string{"area"})},
                 {"response", nullptr}, {"internal_standard_response", nullptr}, {"response_ratio", nullptr},
                 {"qualifier_response", nullptr}, {"qualifier_ratio", nullptr}, {"status", quantifier.status},
                 {"quantifier_status", quantifier.status},
                 {"internal_standard_status",
                  target.value("role", std::string{}) == "analyte" &&
                          target.value("normalization", std::string{"none"}) == "internal_standard"
                      ? Json("not_evaluated")
                      : Json(target.value("role", std::string{}) == "internal_standard" ? "not_applicable" : "not_required")},
                 {"candidate_count", quantifier.candidates}, {"qualifier_status", "not_configured"},
                 {"eic_size", nullptr}, {"eic_rt", nullptr}, {"eic_intensity", nullptr},
                 {"qualifier_eic_size", nullptr}, {"qualifier_eic_rt", nullptr}, {"qualifier_eic_intensity", nullptr}};
        if (quantifier.status == "matched")
        {
            row["chromatogram_index"] = detail::integer_value(quantifier.peak, "chromatogram_index");
            row["peak_id"] = detail::integer_value(quantifier.peak, "peak_id");
            row["peak_rt"] = detail::number_value(quantifier.peak, "rt");
            row["peak_area"] = detail::number_value(quantifier.peak, "area");
            row["peak_height"] = detail::number_value(quantifier.peak, "height");
            row["eic_size"] = quantifier.peak.value("eic_size", Json(nullptr));
            row["eic_rt"] = quantifier.peak.value("eic_rt", Json(nullptr));
            row["eic_intensity"] = quantifier.peak.value("eic_intensity", Json(nullptr));
            row["response"] = detail::response_value(quantifier.peak, target);
        }
        if (detail::has_value(target, "qualifier_signal_type"))
        {
            const auto qualifier = detail::select_peak(target, "qualifier", analysis, peaks_by_chromatogram);
            if (debug.enabled() && (debug_analysis.empty() || debug_analysis == analysis))
                debug_line("Analysis " + analysis + ", target " + target_id + ": qualifier=" + qualifier.status +
                           ", candidates=" + std::to_string(qualifier.candidates));
            row["qualifier_status"] = qualifier.status;
            if (qualifier.status == "matched")
            {
                row["qualifier_eic_size"] = qualifier.peak.value("eic_size", Json(nullptr));
                row["qualifier_eic_rt"] = qualifier.peak.value("eic_rt", Json(nullptr));
                row["qualifier_eic_intensity"] = qualifier.peak.value("eic_intensity", Json(nullptr));
                const auto qualifier_response = detail::response_value(qualifier.peak, target);
                row["qualifier_response"] = qualifier_response;
                if (row["response"].is_number() && row["response"].get<double>() != 0.0)
                {
                    const auto ratio = qualifier_response / row["response"].get<double>();
                    row["qualifier_ratio"] = ratio;
                    if (detail::has_value(target, "expected_ratio") && detail::has_value(target, "ratio_tolerance"))
                        row["qualifier_status"] = std::abs(ratio - detail::number_value(target, "expected_ratio")) <=
                            detail::number_value(target, "ratio_tolerance") ? "pass" : "ratio_out_of_range";
                }
            }
        }
            results[analysis + "\n" + target_id] = row;
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
            if (detail::text_value(row, "normalization") != "internal_standard") continue;
            const auto is_id = detail::text_value(row, "internal_standard_id");
            if (is_id.empty() || detail::text_value(row, "role") != "analyte") continue;
            const auto is_it = results.find(detail::text_value(row, "analysis") + "\n" + is_id);
            if (is_it == results.end())
            {
                row["internal_standard_status"] = "not_assigned";
                if (detail::text_value(row, "quantifier_status") == "matched")
                    row["status"] = "internal_standard_not_assigned";
                continue;
            }
            const auto &is_row = is_it->second;
            const auto is_status = is_row.value("quantifier_status", is_row.value("status", std::string{}));
            if (is_status != "matched")
            {
                row["internal_standard_status"] = is_status;
                if (detail::text_value(row, "quantifier_status") == "matched")
                    row["status"] = "internal_standard_" + is_status;
                continue;
            }
            row["internal_standard_status"] = "matched";
            const auto is_response = detail::number_value(is_row, "response");
            row["internal_standard_response"] = is_response;
            if (!row["response"].is_number() || row["response"].get<double>() == 0.0) continue;
            if (is_response == 0.0) { row["status"] = "internal_standard_zero_response"; continue; }
            row["response_ratio"] = row["response"].get<double>() / is_response;
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
    Json model = Json::object();
    Json points = Json::array();
};

bool finite_number(const Json &row, const std::string &field)
{
    return has_value(row, field) && row.at(field).is_number() && std::isfinite(number_value(row, field));
}

double response_for_calibration(const Json &row, const Json &method)
{
    const auto normalization = text_value(method, "normalization");
    return normalization == "internal_standard"
        ? number_value(row, "response_ratio")
        : number_value(row, "response");
}

std::string calibration_exclusion_reason(const Json &row, const Json &method)
{
    if (text_value(row, "role") != "analyte") return "not_analyte";
    if (text_value(row, "sample_type") != "calibrator") return "not_calibrator";
    if (row.value("exclude", false)) return "excluded_by_assignment";
    if (!finite_number(row, "nominal_concentration")) return "missing_nominal_concentration";
    if (text_value(row, "status") != "matched") return "response_status_" + text_value(row, "status");
    if (text_value(method, "normalization") == "internal_standard" &&
        text_value(row, "internal_standard_status") != "matched")
        return "internal_standard_" + text_value(row, "internal_standard_status");
    const auto response = response_for_calibration(row, method);
    if (!std::isfinite(response) || response <= 0.0) return "nonpositive_response";
    return {};
}

std::vector<Json> read_quantification_method(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::input_rows(
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

std::vector<Json> read_quantification_responses(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto rows = base::utils::input_rows(
        access, parameters, "quantificationTable",
        {"analysis", "target_id", "role", "sample_type", "calibration_level", "nominal_concentration",
         "dilution_factor", "exclude", "internal_standard_id", "normalization",
         "chromatogram_index", "peak_id", "peak_rt", "peak_area", "peak_height", "response_metric",
         "response", "internal_standard_response", "response_ratio", "qualifier_response", "qualifier_ratio",
         "status", "quantifier_status", "internal_standard_status", "candidate_count", "qualifier_status"}, "analysis");
    if (!rows.is_array())
        throw std::invalid_argument("quantificationTable reader returned a non-array row set");
    std::vector<Json> result;
    result.reserve(rows.size());
    for (const auto &row : rows)
    {
        if (!row.is_object())
            throw std::invalid_argument("quantificationTable reader returned a non-object row");
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

CalibrationFit fit_target(const std::string &target_id, const Json &method, const std::vector<Json> &responses,
                          const int minimum_points)
{
    CalibrationFit result;
    result.model = Json{
        {"target_id", target_id}, {"model", method.value("regression_model", std::string{"linear"})},
        {"weighting", method.value("weighting", std::string{"none"})},
        {"intercept_mode", method.value("intercept_mode", std::string{"free"})},
        {"coefficient_0", nullptr}, {"coefficient_1", nullptr}, {"coefficient_2", nullptr},
        {"r_squared", nullptr}, {"adjusted_r_squared", nullptr}, {"rmse", nullptr}, {"n_points", 0},
        {"range_min", nullptr}, {"range_max", nullptr}, {"fit_status", "insufficient_data"}};

    std::vector<std::pair<double, double>> valid;
    for (const auto &row : responses)
    {
        if (text_value(row, "target_id") != target_id) continue;
        Json point{
            {"target_id", target_id}, {"analysis", row.value("analysis", std::string{})},
            {"peak_id", row.value("peak_id", Json(nullptr))},
            {"calibration_level", row.value("calibration_level", Json(nullptr))},
            {"nominal_concentration", row.value("nominal_concentration", Json(nullptr))},
            {"response", row.value("response", Json(nullptr))},
        {"internal_standard_response", row.value("internal_standard_response", Json(nullptr))},
            {"response_ratio", row.value("response_ratio", Json(nullptr))},
            {"included", false}, {"back_calculated_concentration", nullptr}, {"residual", nullptr},
            {"relative_residual", nullptr}, {"exclusion_reason", nullptr}};
        const auto reason = calibration_exclusion_reason(row, method);
        if (!reason.empty())
        {
            point["exclusion_reason"] = reason;
            result.points.push_back(std::move(point));
            continue;
        }
        const auto x = number_value(row, "nominal_concentration");
        const auto y = response_for_calibration(row, method);
        point["included"] = true;
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
    result.model["coefficient_0"] = intercept;
    result.model["coefficient_1"] = slope;
    result.model["r_squared"] = r_squared;
    result.model["adjusted_r_squared"] = adjusted;
    result.model["rmse"] = rmse;
    result.model["n_points"] = valid.size();
    result.model["range_min"] = range_min;
    result.model["range_max"] = range_max;
    result.model["fit_status"] = slope > 0.0 ? "ok" : "failed";
    if (slope <= 0.0)
        return result;
    for (auto &point : result.points)
    {
        if (!point.value("included", false)) continue;
        const auto x = number_value(point, "nominal_concentration");
        const auto y = text_value(method, "normalization") == "internal_standard"
            ? number_value(point, "response_ratio") : number_value(point, "response");
        const auto back_calculated = (y - intercept) / slope;
        point["back_calculated_concentration"] = back_calculated;
        point["residual"] = y - (intercept + slope * x);
        point["relative_residual"] = x != 0.0 ? Json(100.0 * (back_calculated - x) / x) : Json(nullptr);
    }
    return result;
}

const std::vector<std::string> calibration_model_columns{
    "target_id", "model", "weighting", "intercept_mode", "coefficient_0", "coefficient_1",
    "coefficient_2", "r_squared", "adjusted_r_squared", "rmse", "n_points", "range_min", "range_max",
    "fit_status"};

const std::vector<std::string> calibration_model_types{
    "string", "string", "string", "string", "real", "real", "real", "real", "real", "real", "integer",
    "real", "real", "string"};

const std::vector<std::string> calibration_point_columns{
    "target_id", "analysis", "peak_id", "calibration_level", "nominal_concentration", "response", "internal_standard_response",
    "response_ratio", "included", "back_calculated_concentration", "residual", "relative_residual",
    "exclusion_reason"};

const std::vector<std::string> calibration_point_types{
    "string", "string", "integer", "integer", "real", "real", "real", "real", "boolean", "real", "real",
    "real", "string"};
}

namespace detail
{
std::pair<Json, Json> fit_quantification_calibration(const std::map<std::string, Json> &methods,
                                                     const Json &responses, const int minimum_points)
{
    Json models = Json::array();
    Json points = Json::array();
    for (const auto &[target_id, method] : methods)
    {
        if (detail::text_value(method, "role") != "analyte") continue;
        auto fit = detail::fit_target(target_id, method, responses, minimum_points);
        models.push_back(fit.model);
        for (auto &point : fit.points)
        {
            points.push_back(std::move(point));
        }
    }
    return {models, points};
}

Json quantify_samples(const std::map<std::string, Json> &methods, const Json &responses, const Json &models,
                      const bool require_qualifier, const double max_qc_error)
{
    std::map<std::string, Json> model_by_target;
    for (const auto &model : models) model_by_target[detail::text_value(model, "target_id")] = model;
    Json output = Json::array();
    for (const auto &row : responses)
    {
        if (detail::text_value(row, "role") != "analyte") continue;
        const auto target_id = detail::text_value(row, "target_id");
        const auto method_it = methods.find(target_id);
        if (method_it == methods.end()) continue;
        const auto &method = method_it->second;
        Json result{
            {"analysis", row.value("analysis", std::string{})}, {"target_id", target_id},
            {"sample_type", row.value("sample_type", std::string{})}, {"peak_id", row.value("peak_id", Json(nullptr))},
            {"peak_rt", row.value("peak_rt", Json(nullptr))}, {"rt_error", nullptr},
            {"peak_area", row.value("peak_area", Json(nullptr))}, {"peak_height", row.value("peak_height", Json(nullptr))},
            {"response", row.value("response", Json(nullptr))},
            {"internal_standard_response", row.value("internal_standard_response", Json(nullptr))},
            {"response_ratio", row.value("response_ratio", Json(nullptr))},
            {"calculated_concentration", nullptr}, {"dilution_factor", row.value("dilution_factor", 1.0)},
            {"final_concentration", nullptr}, {"concentration_unit", method.value("concentration_unit", Json(nullptr))},
            {"qualifier_ratio", row.value("qualifier_ratio", Json(nullptr))}, {"qualifier_ratio_pass", nullptr},
            {"accuracy_percent", nullptr}, {"status", row.value("status", std::string{"invalid_response"})},
            {"flags", Json(nullptr)}};
        std::vector<std::string> flags;
        const auto qualifier_status = detail::text_value(row, "qualifier_status");
        if (qualifier_status == "pass") result["qualifier_ratio_pass"] = true;
        else if (qualifier_status == "not_configured" || qualifier_status == "matched")
            result["qualifier_ratio_pass"] = Json(nullptr);
        else if (!qualifier_status.empty()) { result["qualifier_ratio_pass"] = false; flags.push_back("qualifier_" + qualifier_status); }

        if (detail::finite_number(row, "peak_rt") && detail::finite_number(method, "rt"))
            result["rt_error"] = detail::number_value(row, "peak_rt") - detail::number_value(method, "rt");

        const auto model_it = model_by_target.find(target_id);
        if (detail::text_value(row, "status") == "matched" && model_it != model_by_target.end() &&
            detail::text_value(model_it->second, "fit_status") == "ok")
        {
            const auto &model = model_it->second;
            const auto y = detail::text_value(method, "normalization") == "internal_standard"
                ? detail::number_value(row, "response_ratio") : detail::number_value(row, "response");
            const auto slope = detail::number_value(model, "coefficient_1");
            const auto intercept = detail::number_value(model, "coefficient_0");
            if (std::isfinite(y) && y > 0.0 && std::isfinite(slope) && std::abs(slope) > 1e-15)
            {
                const auto calculated = (y - intercept) / slope;
                result["calculated_concentration"] = calculated;
                result["final_concentration"] = calculated * detail::number_value(row, "dilution_factor", 1.0);
                const auto lower = detail::number_value(model, "range_min");
                const auto upper = detail::number_value(model, "range_max");
                if (calculated < lower) { result["status"] = "below_calibration_range"; flags.push_back("below_calibration_range"); }
                else if (calculated > upper) { result["status"] = "above_calibration_range"; flags.push_back("above_calibration_range"); }
                else if (require_qualifier && result["qualifier_ratio_pass"].is_boolean() && !result["qualifier_ratio_pass"].get<bool>())
                { result["status"] = "qualifier_failed"; flags.push_back("qualifier_failed"); }
                else result["status"] = "ok";
                if (detail::finite_number(row, "nominal_concentration") && detail::number_value(row, "nominal_concentration") != 0.0)
                    result["accuracy_percent"] = 100.0 * result["final_concentration"].get<double>() /
                        detail::number_value(row, "nominal_concentration");
                if (detail::text_value(row, "sample_type") == "qc" && result["accuracy_percent"].is_number() &&
                    std::abs(result["accuracy_percent"].get<double>() - 100.0) > max_qc_error)
                { flags.push_back("qc_accuracy_out_of_range"); result["status"] = "qc_failed"; }
            }
            else { result["status"] = "invalid_response"; flags.push_back("invalid_response"); }
        }
        else if (model_it == model_by_target.end() || detail::text_value(model_it->second, "fit_status") != "ok")
        {
            result["status"] = "calibration_not_available";
            flags.push_back("calibration_not_available");
        }
        if (!flags.empty())
        {
            std::ostringstream joined;
            for (std::size_t i = 0; i < flags.size(); ++i) { if (i != 0) joined << ','; joined << flags[i]; }
            result["flags"] = joined.str();
        }
        output.push_back(std::move(result));
    }
    return output;
}
}

Json quantify(sdk::PluginProjectAccess &access, const Json &parameters)
{
    std::string stage = "response extraction";
    try
    {
        std::vector<Json> method_rows;
        const auto responses = extract_response_rows(access, parameters, &method_rows);
        stage = "building enabled method index";
        const auto methods = detail::enabled_methods(method_rows);
        stage = "reading quantification parameters";
        const auto minimum_points = parameters.value("minimum_calibration_points", 3);
        stage = "fitting calibration models";
        access.report_progress(0.92, "Fitting quantification calibration models");
        const auto calibration = detail::fit_quantification_calibration(methods, responses, minimum_points);
        const auto &models = calibration.first;
        const auto &points = calibration.second;
        stage = "publishing calibration models";
        access.emit_table_rows("quantificationCalibrationModelsTable", detail::calibration_model_columns,
                               detail::calibration_model_types, models);
        stage = "quantifying samples";
        access.report_progress(0.96, "Applying calibration models to samples");
        const auto results = detail::quantify_samples(
            methods, responses, models, parameters.value("require_qualifier", false),
            parameters.value("qc_max_relative_error", 20.0));
        stage = "assembling quantification table";
        Json quantification_rows = responses;
        std::map<std::string, std::size_t> row_indices;
        for (std::size_t index = 0; index < quantification_rows.size(); ++index)
            row_indices[detail::text_value(quantification_rows.at(index), "analysis") + "\n" +
                        detail::text_value(quantification_rows.at(index), "target_id")] = index;
        const auto merge_fields = [&quantification_rows, &row_indices](const Json &rows, const std::vector<std::string> &fields)
        {
            for (const auto &source : rows)
            {
                const auto key = detail::text_value(source, "analysis") + "\n" + detail::text_value(source, "target_id");
                const auto index = row_indices.find(key);
                if (index == row_indices.end()) continue;
                auto &destination = quantification_rows.at(index->second);
                for (const auto &field : fields)
                    if (source.contains(field)) destination[field] = source.at(field);
            }
        };
        merge_fields(points, {"included", "back_calculated_concentration", "residual", "relative_residual", "exclusion_reason"});
        merge_fields(results, {"rt_error", "calculated_concentration", "final_concentration", "concentration_unit",
                               "qualifier_ratio_pass", "accuracy_percent", "flags", "status"});
        for (auto &row : quantification_rows)
        {
            for (const auto &column : detail::quantification_columns)
                if (!row.contains(column)) row[column] = Json(nullptr);
        }
        stage = "publishing quantification table";
        access.emit_table_rows("quantificationTable", detail::quantification_columns,
                               detail::quantification_types, quantification_rows);
        access.report_progress(1.0, "Quantification workflow completed");
        return Json{{"status", "finished"}, {"responses", responses.size()}, {"models", models.size()},
                    {"points", points.size()}, {"results", results.size()}, {"rows", quantification_rows.size()}};
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument("mass_spec.quantify at " + stage + ": " + error.what());
    }
}
}
