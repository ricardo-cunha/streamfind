#include "utils/quantification.hpp"

#include <set>
#include <sstream>

namespace streamfind::mass_spec::quantification::utils
{
namespace detail
{
using Json = nlohmann::json;

void require_array(const Json &value, const char *name, std::vector<ValidationError> &errors)
{
    if (!value.is_array())
        errors.push_back({name, "must be an array"});
}

bool required_string(const Json &row, const char *field, const std::string &path,
                     std::vector<ValidationError> &errors)
{
    if (!row.contains(field) || !row.at(field).is_string() || row.at(field).get<std::string>().empty())
    {
        errors.push_back({path + "." + field, "must be a non-empty string"});
        return false;
    }
    return true;
}

bool positive_number(const Json &row, const char *field, const std::string &path,
                     std::vector<ValidationError> &errors, bool required)
{
    if (!row.contains(field))
    {
        if (required)
            errors.push_back({path + "." + field, "is required"});
        return !required;
    }
    if (!row.at(field).is_number() || row.at(field).get<double>() <= 0.0)
    {
        errors.push_back({path + "." + field, "must be greater than zero"});
        return false;
    }
    return true;
}

std::string value_or_empty(const Json &row, const char *field)
{
    const auto it = row.find(field);
    if (it == row.end() || it->is_null()) return {};
    return it->is_string() ? it->get<std::string>() : std::string{};
}

bool contains(const std::set<std::string> &values, const std::string &value)
{
    return values.find(value) != values.end();
}
}

void validate_transition(const nlohmann::json &, const std::string &, const std::string &, bool,
                         std::vector<ValidationError> &);

std::vector<ValidationError> validate_method(const nlohmann::json &targets)
{
    using Json = nlohmann::json;
    std::vector<ValidationError> errors;
    detail::require_array(targets, "targets", errors);
    if (!targets.is_array())
        return errors;

    const std::set<std::string> roles{"analyte", "internal_standard"};
    const std::set<std::string> metrics{"area", "height"};
    const std::set<std::string> normalizations{"none", "internal_standard"};
    const std::set<std::string> models{"linear", "quadratic"};
    const std::set<std::string> weightings{"none", "1/x", "1/x2"};
    const std::set<std::string> intercepts{"free", "forced_zero"};
    std::set<std::string> ids;

    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const auto &row = targets.at(i);
        const auto path = "targets[" + std::to_string(i) + "]";
        if (!row.is_object())
        {
            errors.push_back({path, "must be an object"});
            continue;
        }
        if (!detail::required_string(row, "target_id", path, errors))
            continue;
        const auto id = row.at("target_id").get<std::string>();
        if (!ids.insert(id).second)
            errors.push_back({path + ".target_id", "must be unique"});
        const auto role = detail::value_or_empty(row, "role");
        if (!detail::contains(roles, role))
            errors.push_back({path + ".role", "must be analyte or internal_standard"});
        if (role == "internal_standard" && row.contains("internal_standard_id") &&
            !row.at("internal_standard_id").is_null())
            errors.push_back({path + ".internal_standard_id", "must be absent for an internal standard"});
        if (role == "analyte" && detail::value_or_empty(row, "normalization") == "internal_standard")
            detail::required_string(row, "internal_standard_id", path, errors);
        const auto metric = detail::value_or_empty(row, "response_metric");
        if (!detail::contains(metrics, metric))
            errors.push_back({path + ".response_metric", "must be area or height"});
        const auto normalization = detail::value_or_empty(row, "normalization");
        if (!detail::contains(normalizations, normalization))
            errors.push_back({path + ".normalization", "must be none or internal_standard"});
        if (normalization == "internal_standard" && role != "analyte")
            errors.push_back({path + ".normalization", "internal-standard normalization is only valid for analytes"});
        const auto model = row.contains("regression_model") && !row.at("regression_model").is_null()
            ? detail::value_or_empty(row, "regression_model") : std::string{"linear"};
        if (!detail::contains(models, model))
            errors.push_back({path + ".regression_model", "must be linear or quadratic"});
        const auto weighting = row.contains("weighting") && !row.at("weighting").is_null()
            ? detail::value_or_empty(row, "weighting") : std::string{"none"};
        if (!detail::contains(weightings, weighting))
            errors.push_back({path + ".weighting", "must be none, 1/x, or 1/x2"});
        const auto intercept = row.contains("intercept_mode") && !row.at("intercept_mode").is_null()
            ? detail::value_or_empty(row, "intercept_mode") : std::string{"free"};
        if (!detail::contains(intercepts, intercept))
            errors.push_back({path + ".intercept_mode", "must be free or forced_zero"});
        if (row.contains("rt") && !row.at("rt").is_null())
            detail::positive_number(row, "rt", path, errors, false);
        if (row.contains("rt_tolerance") && !row.at("rt_tolerance").is_null())
            detail::positive_number(row, "rt_tolerance", path, errors, false);
        if (row.contains("enabled") && !row.at("enabled").is_null() && !row.at("enabled").is_boolean())
            errors.push_back({path + ".enabled", "must be boolean"});
        validate_transition(row, path, "quantifier", true, errors);
        validate_transition(row, path, "qualifier", false, errors);
    }

    std::set<std::string> target_ids;
    std::set<std::string> internal_standard_ids;
    for (const auto &row : targets)
        if (row.is_object() && row.contains("target_id") && row.at("target_id").is_string())
        {
            const auto target_id = row.at("target_id").get<std::string>();
            target_ids.insert(target_id);
            if (row.value("role", std::string{}) == "internal_standard")
                internal_standard_ids.insert(target_id);
        }
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const auto &row = targets.at(i);
        if (!row.is_object() || row.value("role", std::string{}) != "analyte" ||
            row.value("normalization", std::string{}) != "internal_standard")
            continue;
        const auto is_id = row.value("internal_standard_id", std::string{});
        if (!is_id.empty() && !detail::contains(target_ids, is_id))
            errors.push_back({"targets[" + std::to_string(i) + "].internal_standard_id", "must reference an existing target"});
        else if (!is_id.empty() && !detail::contains(internal_standard_ids, is_id))
            errors.push_back({"targets[" + std::to_string(i) + "].internal_standard_id", "must reference a target with role internal_standard"});
    }
    return errors;
}

void validate_transition(const nlohmann::json &row, const std::string &path,
                         const std::string &prefix, bool required,
                         std::vector<ValidationError> &errors)
{
    const auto signal_field = prefix + "_signal_type";
    const auto signal = detail::value_or_empty(row, signal_field.c_str());
    const std::set<std::string> signals{"MS", "UV"};
    const auto configured = [&row](const std::string &field) {
        return row.contains(field) && !row.at(field).is_null();
    };
    const bool any_value = configured(signal_field) || configured(prefix + "_precursor_mz") ||
                           configured(prefix + "_product_mz") || configured(prefix + "_wavelength_nm");
    if (!required && !any_value)
        return;
    if (!detail::contains(signals, signal))
    {
        errors.push_back({path + "." + signal_field, "must be MS or UV"});
        return;
    }
    if (signal == "MS")
    {
        detail::positive_number(row, (prefix + "_precursor_mz").c_str(), path, errors, true);
        detail::positive_number(row, (prefix + "_product_mz").c_str(), path, errors, true);
    }
    else
        detail::positive_number(row, (prefix + "_wavelength_nm").c_str(), path, errors, true);
    const auto polarity_field = prefix + "_polarity";
    if (configured(polarity_field) && (!row.at(polarity_field).is_number_integer() ||
                                         (row.at(polarity_field) != 1 && row.at(polarity_field) != -1)))
        errors.push_back({path + "." + polarity_field, "must be 1 or -1"});
}

std::vector<ValidationError> validate_assignments(const nlohmann::json &assignments)
{
    using Json = nlohmann::json;
    std::vector<ValidationError> errors;
    detail::require_array(assignments, "assignments", errors);
    if (!assignments.is_array())
        return errors;
    const std::set<std::string> sample_types{"blank", "zero", "calibrator", "qc", "unknown"};
    std::set<std::string> keys;
    for (std::size_t i = 0; i < assignments.size(); ++i)
    {
        const auto &row = assignments.at(i);
        const auto path = "assignments[" + std::to_string(i) + "]";
        if (!row.is_object())
        {
            errors.push_back({path, "must be an object"});
            continue;
        }
        const bool has_analysis = detail::required_string(row, "analysis", path, errors);
        const bool has_target = detail::required_string(row, "target_id", path, errors);
        const auto target_id = detail::value_or_empty(row, "target_id");
        const auto sample_type = detail::value_or_empty(row, "sample_type");
        if (!detail::contains(sample_types, sample_type))
            errors.push_back({path + ".sample_type", "has an unsupported value"});
        if ((sample_type == "calibrator" || sample_type == "qc") &&
            (!row.contains("nominal_concentration") || !row.at("nominal_concentration").is_number() ||
             row.at("nominal_concentration").get<double>() < 0.0))
            errors.push_back({path + ".nominal_concentration", "is required and must be non-negative for calibrators and QCs"});
        if ((sample_type == "blank" || sample_type == "zero" || sample_type == "unknown") &&
            row.contains("nominal_concentration") && !row.at("nominal_concentration").is_null())
            errors.push_back({path + ".nominal_concentration", "must be absent for blank, zero, and unknown samples"});
        if (row.contains("dilution_factor") && (!row.at("dilution_factor").is_number() || row.at("dilution_factor").get<double>() <= 0.0))
            errors.push_back({path + ".dilution_factor", "must be greater than zero"});
        if (row.contains("exclude") && !row.at("exclude").is_boolean())
            errors.push_back({path + ".exclude", "must be boolean"});
        if (has_analysis && has_target)
        {
            const auto key = row.at("analysis").get<std::string>() + "\n" + target_id;
            if (!keys.insert(key).second)
                errors.push_back({path, "analysis and target_id must form a unique key"});
        }
    }
    return errors;
}

std::string format_validation_errors(const std::vector<ValidationError> &errors)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < errors.size(); ++i)
    {
        if (i != 0)
            out << "; ";
        out << errors[i].path << ": " << errors[i].message;
    }
    return out.str();
}
}
