#include "operations/quantification/operations.hpp"

#include "operations/base.hpp"
#include "utils/chromatograms.hpp"
#include "utils/quantification.hpp"

#include <chrono>
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <map>
#include <set>

namespace streamfind::mass_spec::quantification
{
namespace detail
{
using Json = nlohmann::json;

std::string utc_now()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

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
    row["created_at"] = utc_now();
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
                          const std::map<std::string, std::vector<Json>> &headers_by_analysis,
                          const std::map<std::pair<std::string, int>, std::vector<Json>> &peaks_by_chromatogram)
{
    std::vector<std::pair<int, Json>> matched_headers;
    const auto header_it = headers_by_analysis.find(analysis);
    if (header_it == headers_by_analysis.end()) return {};
    for (const auto &header : header_it->second)
    {
        if (!transition_matches(method, header, prefix)) continue;
        matched_headers.emplace_back(integer_value(header, "index"), header);
    }
    if (matched_headers.empty()) return {};

    std::vector<Json> candidates;
    const auto has_rt = has_value(method, "rt");
    const auto expected_rt = has_rt ? number_value(method, "rt") : 0.0;
    const auto rt_tolerance = has_value(method, "rt_tolerance")
        ? number_value(method, "rt_tolerance") : 30.0;
    for (const auto &header : matched_headers)
    {
        const auto peak_it = peaks_by_chromatogram.find({analysis, header.first});
        if (peak_it == peaks_by_chromatogram.end()) continue;
        for (const auto &peak : peak_it->second)
        {
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
    std::sort(candidates.begin(), candidates.end(), [&](const Json &left, const Json &right) {
        const auto left_distance = has_rt ? std::abs(number_value(left, "rt") - expected_rt) : number_value(left, "rt");
        const auto right_distance = has_rt ? std::abs(number_value(right, "rt") - expected_rt) : number_value(right, "rt");
        if (left_distance != right_distance) return left_distance < right_distance;
        if (integer_value(left, "chromatogram_index") != integer_value(right, "chromatogram_index"))
            return integer_value(left, "chromatogram_index") < integer_value(right, "chromatogram_index");
        return integer_value(left, "peak_id") < integer_value(right, "peak_id");
    });
    PeakSelection result;
    result.status = candidates.size() == 1 ? "matched" : "ambiguous_peak";
    result.peak = candidates.front();
    result.candidates = static_cast<int>(candidates.size());
    if (candidates.size() != 1) result.peak = Json::object();
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

const std::vector<std::string> response_columns{
    "analysis", "target_id", "role", "sample_type", "calibration_level", "nominal_concentration",
    "dilution_factor", "exclude", "internal_standard_id", "normalization", "chromatogram_index", "peak_id", "peak_rt",
    "peak_area", "peak_height", "response_metric", "response", "internal_standard_response", "response_ratio",
    "qualifier_response", "qualifier_ratio", "status", "candidate_count", "qualifier_status", "created_at"};

const std::vector<std::string> response_types{
    "string", "string", "string", "string", "integer", "real", "real", "boolean", "string", "string", "integer",
    "integer", "real", "real", "real", "string", "real", "real", "real", "real", "real", "string",
    "integer", "string", "timestamp"};
}

Json set_quantification_method(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto targets = parameters.value("quantification_method", Json::array());
    detail::reject(utils::validate_method(targets));
    const std::vector<std::string> columns{
        "target_id", "name", "role", "internal_standard_id", "rt", "rt_tolerance",
        "response_metric", "normalization", "concentration_unit", "regression_model", "weighting",
        "intercept_mode", "quantifier_signal_type", "quantifier_chromatogram_type", "quantifier_polarity",
        "quantifier_precursor_mz", "quantifier_product_mz", "quantifier_activation_ce", "quantifier_detector",
        "quantifier_wavelength_nm", "quantifier_mz_tolerance_ppm", "qualifier_signal_type",
        "qualifier_chromatogram_type", "qualifier_polarity", "qualifier_precursor_mz", "qualifier_product_mz",
        "qualifier_activation_ce", "qualifier_detector", "qualifier_wavelength_nm", "qualifier_mz_tolerance_ppm",
        "expected_ratio", "ratio_tolerance", "enabled", "created_at"};
    const std::vector<std::string> types{
        "string", "string", "string", "string", "real", "real", "string", "string", "string",
        "string", "string", "string", "string", "string", "integer", "real", "real", "real", "string",
        "real", "real", "string", "string", "integer", "real", "real", "real", "string", "real", "real",
        "real", "real", "boolean", "timestamp"};
    Json rows = Json::array();
    for (const auto &target : targets)
    {
        auto row = detail::copy_fields(target, columns);
        if (!row.contains("enabled") || row.at("enabled").is_null()) row["enabled"] = true;
        if (!row.contains("regression_model") || row.at("regression_model").is_null()) row["regression_model"] = "linear";
        if (!row.contains("weighting") || row.at("weighting").is_null()) row["weighting"] = "none";
        if (!row.contains("intercept_mode") || row.at("intercept_mode").is_null()) row["intercept_mode"] = "free";
        rows.push_back(std::move(row));
    }
    detail::emit(access, "quantificationMethodTable", columns, types, rows);
    return Json{{"status", "finished"}, {"targets", rows.size()}};
}

Json set_quantification_analyses(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto assignments = parameters.value("quant_sample_assignments", Json::array());
    detail::reject(utils::validate_assignments(assignments));
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
    const std::vector<std::string> columns{
        "analysis", "target_id", "sample_type", "calibration_level", "nominal_concentration",
        "is_concentration", "dilution_factor", "exclude", "created_at"};
    const std::vector<std::string> types{
        "string", "string", "string", "integer", "real", "real", "real", "boolean", "timestamp"};
    Json rows = Json::array();
    for (const auto &assignment : assignments)
    {
        auto row = detail::copy_fields(assignment, columns);
        if (!row.contains("dilution_factor") || row.at("dilution_factor").is_null()) row["dilution_factor"] = 1.0;
        if (!row.contains("exclude") || row.at("exclude").is_null()) row["exclude"] = false;
        rows.push_back(std::move(row));
    }
    detail::emit(access, "quantificationAnalysesTable", columns, types, rows);
    return Json{{"status", "finished"}, {"assignments", rows.size()}};
}

Json extract_quantification_responses(sdk::PluginProjectAccess &access, const Json &parameters)
{
    const auto method = base::utils::input_rows(
        access, parameters, "quantificationMethodTable",
        {"target_id", "role", "internal_standard_id", "rt", "rt_tolerance", "response_metric",
         "normalization", "quantifier_signal_type", "quantifier_chromatogram_type", "quantifier_polarity",
         "quantifier_precursor_mz", "quantifier_product_mz", "quantifier_detector", "quantifier_wavelength_nm",
         "quantifier_mz_tolerance_ppm", "qualifier_signal_type", "qualifier_chromatogram_type", "qualifier_polarity",
         "qualifier_precursor_mz", "qualifier_product_mz", "qualifier_detector", "qualifier_wavelength_nm",
         "qualifier_mz_tolerance_ppm", "expected_ratio", "ratio_tolerance", "enabled"}, "target_id");
    const auto assignments = base::utils::input_rows(
        access, parameters, "quantificationAnalysesTable",
        {"analysis", "target_id", "sample_type", "calibration_level", "nominal_concentration", "dilution_factor", "exclude"}, "analysis");
    const auto headers = base::utils::input_rows(
        access, parameters, "chromatogramsHeadersTable", chromatograms::utils::header_columns(), "analysis");
    std::map<std::string, std::vector<Json>> headers_by_analysis;
    for (const auto &header : headers)
        headers_by_analysis[detail::text_value(header, "analysis")].push_back(header);

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

    // Peak tables can contain millions of rows. Read them in bounded batches
    // and retain only chromatograms whose headers match a configured MRM
    // transition. The previous full-table read duplicated the complete peak
    // artifact as JSON before response extraction even began.
    std::set<std::pair<std::string, int>> matched_chromatograms;
    for (const auto &[analysis, analysis_headers] : headers_by_analysis)
    {
        for (const auto &header : analysis_headers)
        {
            for (const auto &[target_id, target] : methods)
            {
                if (detail::transition_matches(target, header, "quantifier") ||
                    detail::transition_matches(target, header, "qualifier"))
                {
                    matched_chromatograms.emplace(analysis, detail::integer_value(header, "index"));
                    break;
                }
            }
        }
    }

    std::map<std::pair<std::string, int>, std::vector<Json>> peaks_by_chromatogram;
    const auto peak_table = base::utils::input_table(parameters, "chromatogramPeaksTable");
    const auto peak_count = access.count_rows(peak_table);
    std::uint64_t scanned_peaks = 0;
    std::uint64_t peak_batches = 0;
    const auto peak_scan_started = std::chrono::steady_clock::now();
    const std::vector<std::string> quantification_peak_columns{
        "analysis", "chromatogram_index", "peak_id", "rt", "rt_start", "rt_end", "height", "raw_height",
        "area", "raw_area", "baseline_area", "width", "fwhm", "snr", "asymmetry", "sharpness", "plates",
        "number_points", "integration_algorithm", "integration_status", "manual_override"};
    access.read_batches(peak_table, quantification_peak_columns, "analysis",
        [&](const Json &batch)
        {
            for (const auto &peak : batch)
            {
                const auto key = std::make_pair(detail::text_value(peak, "analysis"),
                                                detail::integer_value(peak, "chromatogram_index"));
                if (matched_chromatograms.find(key) != matched_chromatograms.end())
                    peaks_by_chromatogram[key].push_back(peak);
            }
            scanned_peaks += batch.size();
            ++peak_batches;
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - peak_scan_started).count();
            std::ostringstream progress;
            progress << "Filtering chromatogram peaks for quantification"
                     << " (batch " << peak_batches
                     << ", rows " << scanned_peaks << "/" << peak_count
                     << ", retained chromatograms " << peaks_by_chromatogram.size()
                     << ", elapsed " << std::fixed << std::setprecision(1) << elapsed << " s)";
            access.report_progress(0.35 + (peak_count == 0 ? 0.0 :
                0.25 * static_cast<double>(scanned_peaks) / static_cast<double>(peak_count)),
                progress.str());
        });
    std::map<std::string, Json> results;
    Json output = Json::array();
    for (const auto &assignment : assignments)
    {
        const auto analysis = assignment.value("analysis", std::string{});
        const auto target_id = assignment.value("target_id", std::string{});
        try
        {
            const auto method_it = methods.find(target_id);
            if (method_it == methods.end()) continue;
            const auto &target = method_it->second;
            const auto quantifier = detail::select_peak(target, "quantifier", analysis,
                                                         headers_by_analysis, peaks_by_chromatogram);
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
                 {"candidate_count", quantifier.candidates}, {"qualifier_status", "not_configured"}};
        if (quantifier.status == "matched")
        {
            row["chromatogram_index"] = detail::integer_value(quantifier.peak, "chromatogram_index");
            row["peak_id"] = detail::integer_value(quantifier.peak, "peak_id");
            row["peak_rt"] = detail::number_value(quantifier.peak, "rt");
            row["peak_area"] = detail::number_value(quantifier.peak, "area");
            row["peak_height"] = detail::number_value(quantifier.peak, "height");
            row["response"] = detail::response_value(quantifier.peak, target);
        }
        if (detail::has_value(target, "qualifier_signal_type"))
        {
            const auto qualifier = detail::select_peak(target, "qualifier", analysis,
                                                       headers_by_analysis, peaks_by_chromatogram);
            row["qualifier_status"] = qualifier.status;
            if (qualifier.status == "matched")
            {
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
        }
        catch (const std::exception &error)
        {
            throw std::invalid_argument("response extraction for analysis '" + analysis +
                                        "' target '" + target_id + "': " + error.what());
        }
    }
    try
    {
        for (auto &row : output)
        {
            if (detail::text_value(row, "normalization") == "internal_standard") continue;
            const auto is_id = detail::text_value(row, "internal_standard_id");
            if (is_id.empty() || detail::text_value(row, "role") != "analyte") continue;
            const auto is_it = results.find(detail::text_value(row, "analysis") + "\n" + is_id);
            if (is_it == results.end()) { row["status"] = "internal_standard_not_assigned"; continue; }
            const auto &is_row = is_it->second;
            if (is_row.value("status", std::string{}) != "matched") { row["status"] = "internal_standard_" + is_row.value("status", std::string{}); continue; }
            const auto is_response = detail::number_value(is_row, "response");
            row["internal_standard_response"] = is_response;
            if (!row["response"].is_number() || row["response"].get<double>() == 0.0) continue;
            if (is_response == 0.0) { row["status"] = "internal_standard_zero_response"; continue; }
            row["response_ratio"] = row["response"].get<double>() / is_response;
        }
        for (auto &row : output) row["created_at"] = detail::utc_now();
        access.emit_table_rows("quantificationResponsesTable", detail::response_columns, detail::response_types, output);
    }
    catch (const std::exception &error)
    {
        throw std::invalid_argument(std::string("response table assembly: ") + error.what());
    }
    return Json{{"status", "finished"}, {"responses", output.size()}};
}
}
