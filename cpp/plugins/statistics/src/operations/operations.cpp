#include "operations.hpp"

#include "statistics.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace streamfind::statistics::operations {
namespace detail {

using Json = nlohmann::json;
using Row = std::unordered_map<std::size_t, double>;
using ::streamfind::statistics::detail::Matrix;
using ::streamfind::statistics::detail::compute_fold_change;
using ::streamfind::statistics::detail::compute_pca;
using ::streamfind::statistics::detail::compute_pls;

struct Dataset {
    std::vector<std::string> sample_ids;
    std::vector<std::string> variable_ids;
    std::vector<std::string> groups;
    std::vector<bool> has_group;
    std::vector<std::string> response_ids;
    std::vector<std::vector<double>> response_values;
    Matrix values;
};

std::string string_parameter(const Json &parameters, const char *name,
                             const std::string &default_value = {}) {
    const auto found = parameters.find(name);
    if (found == parameters.end()) return default_value;
    if (!found->is_string()) throw std::invalid_argument(std::string(name) + " must be a string");
    return found->get<std::string>();
}

std::size_t size_parameter(const Json &parameters, const char *name) {
    const auto found = parameters.find(name);
    if (found == parameters.end() || !found->is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    const auto value = found->get<std::int64_t>();
    if (value < 0) throw std::invalid_argument(std::string(name) + " must not be negative");
    return static_cast<std::size_t>(value);
}

std::vector<std::string> string_array(const Json &parameters, const char *name,
                                      bool required) {
    const auto found = parameters.find(name);
    if (found == parameters.end()) {
        if (required) throw std::invalid_argument(std::string(name) + " must be a non-empty array of strings");
        return {};
    }
    if (!found->is_array() || (required && found->empty()))
        throw std::invalid_argument(std::string(name) + " must be a non-empty array of strings");
    std::vector<std::string> values;
    std::unordered_set<std::string> seen;
    for (const auto &item : *found) {
        if (!item.is_string() || item.get<std::string>().empty())
            throw std::invalid_argument(std::string(name) + " must contain non-empty strings");
        auto value = item.get<std::string>();
        if (!seen.insert(value).second)
            throw std::invalid_argument(std::string(name) + " must not contain duplicate values");
        values.push_back(std::move(value));
    }
    return values;
}

std::string scalar_text(const Json &value, const std::string &column,
                        std::size_t source_row) {
    if (value.is_null())
        throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " is missing");
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.empty()) throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " is empty");
        return text;
    }
    if (value.is_number()) return value.dump();
    throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " must be a string or number");
}

double numeric_value(const Json &value, const std::string &column,
                     std::size_t source_row) {
    double number = 0.0;
    if (value.is_number()) {
        number = value.get<double>();
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        char *end = nullptr;
        errno = 0;
        number = std::strtod(text.c_str(), &end);
        if (errno == ERANGE || end == text.c_str() || end == nullptr || *end != '\0')
            throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " is not numeric");
    } else {
        throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " is not numeric");
    }
    if (!std::isfinite(number))
        throw std::invalid_argument("row " + std::to_string(source_row) + " column " + column + " must be finite");
    return number;
}

const Json &required_input(const Json &parameters) {
    const auto inputs = parameters.find("_inputs");
    if (inputs == parameters.end() || !inputs->is_object() || !inputs->contains("tableInput"))
        throw std::invalid_argument("statistics operation requires a tableInput AnyTable connection");
    const auto &input = inputs->at("tableInput");
    if (!input.is_object() || !input.contains("physical_table") || !input.at("physical_table").is_string() ||
        input.at("physical_table").get<std::string>().empty())
        throw std::invalid_argument("statistics tableInput must identify a persisted table");
    return input;
}

Dataset read_dataset(sdk::PluginProjectAccess &access, const Json &parameters,
                     const std::vector<std::string> &responses = {}) {
    const auto layout = string_parameter(parameters, "layout");
    if (layout != "long" && layout != "wide")
        throw std::invalid_argument("layout must be long or wide");
    const auto sample_column = string_parameter(parameters, "sample_id_column");
    if (sample_column.empty()) throw std::invalid_argument("sample_id_column must not be empty");
    const auto group_column = string_parameter(parameters, "group_column");
    const auto duplicate_aggregation = string_parameter(parameters, "duplicate_aggregation", "error");
    if (duplicate_aggregation != "error" && duplicate_aggregation != "mean")
        throw std::invalid_argument("duplicate_aggregation must be error or mean");
    const auto missing_imputation = string_parameter(parameters, "missing_value_imputation", "error");
    if (missing_imputation != "error" && missing_imputation != "zero" &&
        missing_imputation != "mean" && missing_imputation != "median")
        throw std::invalid_argument("missing_value_imputation must be error, zero, mean, or median");
    const auto variable_column = layout == "long" ? string_parameter(parameters, "variable_id_column") : std::string{};
    const auto value_column = layout == "long" ? string_parameter(parameters, "value_column") : std::string{};
    auto wide_variables = layout == "wide" ? string_array(parameters, "variable_columns", true) : std::vector<std::string>{};
    if (layout == "long" && (variable_column.empty() || value_column.empty()))
        throw std::invalid_argument("long layout requires variable_id_column and value_column");
    std::unordered_set<std::string> field_names{sample_column};
    auto add_field = [&](const std::string &name, const char *description) {
        if (name.empty()) return;
        if (!field_names.insert(name).second)
            throw std::invalid_argument(std::string(description) + " must not overlap another selected column: " + name);
    };
    if (layout == "long") { add_field(variable_column, "variable_id_column"); add_field(value_column, "value_column"); }
    for (const auto &name : wide_variables) add_field(name, "variable_columns");
    if (!group_column.empty()) add_field(group_column, "group_column");
    for (const auto &response : responses) add_field(response, "response_columns");

    std::vector<std::string> columns{sample_column};
    if (layout == "long") { columns.push_back(variable_column); columns.push_back(value_column); }
    else columns.insert(columns.end(), wide_variables.begin(), wide_variables.end());
    if (!group_column.empty()) columns.push_back(group_column);
    columns.insert(columns.end(), responses.begin(), responses.end());
    const auto physical_table = required_input(parameters).at("physical_table").get<std::string>();

    Dataset dataset;
    dataset.response_ids = responses;
    std::vector<Row> row_values;
    std::vector<std::unordered_map<std::size_t, std::size_t>> row_counts;
    std::vector<std::vector<bool>> response_seen;
    std::vector<std::size_t> sample_first_rows;
    std::unordered_map<std::string, std::size_t> sample_indices;
    std::unordered_map<std::string, std::size_t> variable_indices;
    std::size_t source_row = 0;
    access.read_batches(physical_table, columns, sample_column, [&](const Json &batch) {
        if (!batch.is_array()) throw std::runtime_error("table reader returned a malformed batch");
        for (const auto &row : batch) {
            ++source_row;
            if (!row.is_object())
                throw std::invalid_argument("row " + std::to_string(source_row) + " is not an object");
            if (access.is_cancelled()) throw std::runtime_error("statistics operation cancelled");
            const auto sample_id = scalar_text(row.value(sample_column, Json(nullptr)), sample_column, source_row);
            std::size_t sample_index = 0;
            const auto sample_found = sample_indices.find(sample_id);
            if (sample_found == sample_indices.end()) {
                sample_index = dataset.sample_ids.size();
                sample_indices.emplace(sample_id, sample_index);
                dataset.sample_ids.push_back(sample_id);
                sample_first_rows.push_back(source_row);
                row_values.emplace_back();
                row_counts.emplace_back();
                response_seen.emplace_back(responses.size(), false);
                dataset.groups.emplace_back();
                dataset.has_group.push_back(false);
                dataset.response_values.emplace_back(responses.size(), 0.0);
            } else {
                sample_index = sample_found->second;
                if (layout == "wide")
                    throw std::invalid_argument("row " + std::to_string(source_row) + " duplicates sample ID " + sample_id + " in wide layout");
            }
            if (!group_column.empty()) {
                const auto &group_value = row.value(group_column, Json(nullptr));
                // Long tables may repeat sample-level metadata only on some
                // rows. Missing group cells are therefore ignored; a group is
                // still required once all rows for the sample have been read.
                if (!group_value.is_null() && !(group_value.is_string() && group_value.get<std::string>().empty())) {
                    const auto group = scalar_text(group_value, group_column, source_row);
                    if (dataset.has_group[sample_index] && dataset.groups[sample_index] != group)
                        throw std::invalid_argument("row " + std::to_string(source_row) + " column " + group_column +
                                                    " changes within sample " + sample_id);
                    dataset.groups[sample_index] = group;
                    dataset.has_group[sample_index] = true;
                }
            }
            for (std::size_t response_index = 0; response_index < responses.size(); ++response_index) {
                const auto response = numeric_value(row.value(responses[response_index], Json(nullptr)), responses[response_index], source_row);
                if (response_seen[sample_index][response_index] &&
                    dataset.response_values[sample_index][response_index] != response)
                    throw std::invalid_argument("row " + std::to_string(source_row) + " response column " + responses[response_index] +
                                                " changes within sample " + sample_id);
                dataset.response_values[sample_index][response_index] = response;
                response_seen[sample_index][response_index] = true;
            }
            if (layout == "wide") {
                for (const auto &name : wide_variables) {
                    const auto variable = variable_indices.emplace(name, dataset.variable_ids.size());
                    if (variable.second) dataset.variable_ids.push_back(name);
                    const auto raw = row.value(name, Json(nullptr));
                    if (!raw.is_null())
                        row_values[sample_index][variable.first->second] = numeric_value(raw, name, source_row);
                }
            } else {
                const auto variable_id = scalar_text(row.value(variable_column, Json(nullptr)), variable_column, source_row);
                const auto variable = variable_indices.emplace(variable_id, dataset.variable_ids.size());
                if (variable.second) dataset.variable_ids.push_back(variable_id);
                const auto raw = row.value(value_column, Json(nullptr));
                if (raw.is_null()) continue;
                const auto value = numeric_value(raw, value_column, source_row);
                const auto variable_index = variable.first->second;
                const auto existing = row_values[sample_index].find(variable_index);
                if (existing != row_values[sample_index].end()) {
                    if (duplicate_aggregation == "error")
                        throw std::invalid_argument("row " + std::to_string(source_row) + " duplicates sample-variable pair " +
                                                    sample_id + "/" + variable_id);
                    const auto count = row_counts[sample_index][variable_index];
                    existing->second = (existing->second * static_cast<double>(count) + value) /
                                       static_cast<double>(count + 1);
                    row_counts[sample_index][variable_index] = count + 1;
                } else {
                    row_values[sample_index][variable_index] = value;
                    row_counts[sample_index][variable_index] = 1;
                }
            }
        }
    });
    if (source_row == 0) throw std::invalid_argument("statistics input table contains no rows");
    for (std::size_t sample = 0; sample < response_seen.size(); ++sample)
        for (std::size_t response = 0; response < responses.size(); ++response)
            if (!response_seen[sample][response])
                throw std::invalid_argument("sample " + dataset.sample_ids[sample] + " has no value in response column " + responses[response]);
    if (!group_column.empty())
        for (std::size_t sample = 0; sample < dataset.has_group.size(); ++sample)
            if (!dataset.has_group[sample])
                throw std::invalid_argument("sample " + dataset.sample_ids[sample] + " has no group value in " + group_column);
    if (dataset.variable_ids.empty()) throw std::invalid_argument("statistics input has no variables");
    dataset.values.assign(dataset.sample_ids.size(), std::vector<double>(dataset.variable_ids.size(), 0.0));
    for (std::size_t sample = 0; sample < row_values.size(); ++sample) {
        for (std::size_t variable = 0; variable < dataset.variable_ids.size(); ++variable) {
            const auto found = row_values[sample].find(variable);
            if (found != row_values[sample].end()) {
                dataset.values[sample][variable] = found->second;
                continue;
            }
            if (missing_imputation == "error")
                throw std::invalid_argument("row " + std::to_string(sample_first_rows[sample]) + " column " +
                    (layout == "long" ? value_column : dataset.variable_ids[variable]) + " is missing for sample " +
                    dataset.sample_ids[sample] + " and variable " + dataset.variable_ids[variable] +
                    (layout == "long" ? " (variable column " + variable_column + ")" : ""));
            if (missing_imputation == "zero") continue;
            std::vector<double> observed;
            observed.reserve(row_values.size());
            for (const auto &row : row_values) {
                const auto value = row.find(variable);
                if (value != row.end()) observed.push_back(value->second);
            }
            if (observed.empty())
                throw std::invalid_argument("cannot impute variable " + dataset.variable_ids[variable] +
                    ": no observed values");
            if (missing_imputation == "mean") {
                const auto total = std::accumulate(observed.begin(), observed.end(), 0.0);
                dataset.values[sample][variable] = total / static_cast<double>(observed.size());
            } else {
                std::sort(observed.begin(), observed.end());
                const auto middle = observed.size() / 2;
                dataset.values[sample][variable] = observed.size() % 2 == 0
                    ? (observed[middle - 1] + observed[middle]) / 2.0
                    : observed[middle];
            }
        }
    }
    return dataset;
}

void emit_rows(sdk::PluginProjectAccess &access, const std::string &contract,
               const std::vector<std::string> &columns,
               const std::vector<std::string> &types, const Json &rows) {
    constexpr std::size_t batch_size = 2048;
    for (std::size_t begin = 0; begin < rows.size(); begin += batch_size) {
        Json batch = Json::array();
        const auto end = std::min(rows.size(), begin + batch_size);
        for (std::size_t row = begin; row < end; ++row) batch.push_back(rows[row]);
        access.emit_table_rows(contract, columns, types, batch);
    }
}

std::size_t validate_components(const Json &parameters, std::size_t samples,
                                std::size_t variables) {
    if (samples < 2)
        throw std::invalid_argument("statistics analysis requires at least two samples");
    const auto count = size_parameter(parameters, "n_components");
    if (count < 1 || count > std::min(samples, variables))
        throw std::invalid_argument("n_components must be between 1 and the smaller matrix dimension");
    return count;
}

Json run_pca(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto dataset = read_dataset(access, parameters);
    const auto preprocessing = string_parameter(parameters, "preprocessing", "center");
    const auto components = validate_components(parameters, dataset.sample_ids.size(), dataset.variable_ids.size());
    const auto result = compute_pca(dataset.values, preprocessing, components);
    Json score_rows = Json::array();
    for (std::size_t sample = 0; sample < dataset.sample_ids.size(); ++sample)
        for (std::size_t component = 0; component < components; ++component)
            score_rows.push_back({{"sample_id", dataset.sample_ids[sample]}, {"component", component + 1},
                                  {"score", result.scores[sample][component]},
                                  {"group_label", dataset.has_group[sample] ? Json(dataset.groups[sample]) : Json(nullptr)}});
    Json loading_rows = Json::array();
    for (std::size_t variable = 0; variable < dataset.variable_ids.size(); ++variable)
        for (std::size_t component = 0; component < components; ++component)
            loading_rows.push_back({{"variable_id", dataset.variable_ids[variable]}, {"component", component + 1},
                                   {"loading", result.loadings[variable][component]}});
    Json variance_rows = Json::array();
    for (std::size_t component = 0; component < components; ++component)
        variance_rows.push_back({{"component", component + 1}, {"eigenvalue", result.eigenvalues[component]},
                                 {"explained_variance_ratio", result.explained_variance[component]},
                                 {"cumulative_explained_variance", result.cumulative_variance[component]}});
    emit_rows(access, "pcaScoresTable", {"sample_id", "component", "score", "group_label"},
              {"string", "integer", "real", "string"}, score_rows);
    emit_rows(access, "pcaLoadingsTable", {"variable_id", "component", "loading"},
              {"string", "integer", "real"}, loading_rows);
    emit_rows(access, "pcaVarianceTable", {"component", "eigenvalue", "explained_variance_ratio", "cumulative_explained_variance"},
              {"integer", "real", "real", "real"}, variance_rows);
    const Json diagnostics{{"method", "symmetric_covariance_eigendecomposition"},
                          {"samples", dataset.sample_ids.size()}, {"variables", dataset.variable_ids.size()},
                          {"components", components}, {"preprocessing", preprocessing},
                          {"total_variance", result.total_variance}};
    access.emit_result("pcaDiagnostics", diagnostics.dump());
    access.report_progress(1.0, "PCA completed");
    return {{"samples", dataset.sample_ids.size()}, {"variables", dataset.variable_ids.size()},
            {"components", components}, {"diagnostics", diagnostics}};
}

Json run_pls(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto response_mode = string_parameter(parameters, "response_mode");
    if (response_mode != "regression" && response_mode != "pls_da")
        throw std::invalid_argument("response_mode must be regression or pls_da");
    const auto group_column = string_parameter(parameters, "group_column");
    if (response_mode == "pls_da" && parameters.contains("response_columns") &&
        !parameters.at("response_columns").empty())
        throw std::invalid_argument("response_columns are only valid in regression mode");
    std::vector<std::string> response_ids;
    Dataset dataset;
    if (response_mode == "regression") {
        response_ids = string_array(parameters, "response_columns", true);
        dataset = read_dataset(access, parameters, response_ids);
    } else {
        if (group_column.empty()) throw std::invalid_argument("PLS-DA requires group_column");
        dataset = read_dataset(access, parameters);
        response_ids = {group_column};
    }
    const auto preprocessing = string_parameter(parameters, "preprocessing", "center");
    const auto components = validate_components(parameters, dataset.sample_ids.size(), dataset.variable_ids.size());
    Matrix responses;
    std::vector<std::string> classes;
    if (response_mode == "regression") {
        responses = dataset.response_values;
    } else {
        for (const auto &group : dataset.groups)
            if (std::find(classes.begin(), classes.end(), group) == classes.end()) classes.push_back(group);
        if (classes.size() < 2) throw std::invalid_argument("PLS-DA requires at least two distinct group labels");
        responses.assign(dataset.sample_ids.size(), std::vector<double>(classes.size(), 0.0));
        for (std::size_t sample = 0; sample < dataset.groups.size(); ++sample) {
            const auto class_index = std::find(classes.begin(), classes.end(), dataset.groups[sample]) - classes.begin();
            responses[sample][static_cast<std::size_t>(class_index)] = 1.0;
        }
        response_ids = classes;
    }
    const auto result = compute_pls(dataset.values, responses, preprocessing, components);
    const auto computed = result.x_scores.front().size();
    Json score_rows = Json::array();
    for (std::size_t sample = 0; sample < dataset.sample_ids.size(); ++sample)
        for (std::size_t component = 0; component < computed; ++component)
            score_rows.push_back({{"sample_id", dataset.sample_ids[sample]}, {"component", component + 1},
                                  {"x_score", result.x_scores[sample][component]},
                                  {"y_score", result.y_scores[sample][component]},
                                  {"group_label", dataset.has_group[sample] ? Json(dataset.groups[sample]) : Json(nullptr)}});
    Json loading_rows = Json::array();
    for (std::size_t variable = 0; variable < dataset.variable_ids.size(); ++variable)
        for (std::size_t component = 0; component < computed; ++component)
            loading_rows.push_back({{"block", "x"}, {"item_id", dataset.variable_ids[variable]},
                                   {"component", component + 1}, {"loading", result.x_loadings[variable][component]}});
    for (std::size_t response = 0; response < response_ids.size(); ++response)
        for (std::size_t component = 0; component < computed; ++component)
            loading_rows.push_back({{"block", "y"}, {"item_id", response_ids[response]},
                                   {"component", component + 1}, {"loading", result.y_loadings[response][component]}});
    Json coefficient_rows = Json::array();
    for (std::size_t variable = 0; variable < dataset.variable_ids.size(); ++variable)
        for (std::size_t response = 0; response < response_ids.size(); ++response)
            coefficient_rows.push_back({{"predictor_id", dataset.variable_ids[variable]},
                                        {"response_id", response_ids[response]},
                                        {"coefficient", result.coefficients[variable][response]},
                                        {"intercept", result.intercept[response]}});
    Json prediction_rows = Json::array();
    if (response_mode == "regression") {
        for (std::size_t sample = 0; sample < dataset.sample_ids.size(); ++sample)
            for (std::size_t response = 0; response < response_ids.size(); ++response)
                prediction_rows.push_back({{"sample_id", dataset.sample_ids[sample]}, {"response_id", response_ids[response]},
                                           {"observed_value", dataset.response_values[sample][response]},
                                           {"predicted_value", result.predictions[sample][response]},
                                           {"observed_label", nullptr}, {"predicted_label", nullptr}, {"confidence", nullptr}});
    } else {
        for (std::size_t sample = 0; sample < dataset.sample_ids.size(); ++sample) {
            auto predicted = std::max_element(result.predictions[sample].begin(), result.predictions[sample].end());
            const auto class_index = static_cast<std::size_t>(predicted - result.predictions[sample].begin());
            prediction_rows.push_back({{"sample_id", dataset.sample_ids[sample]}, {"response_id", group_column},
                                       {"observed_value", nullptr}, {"predicted_value", nullptr},
                                       {"observed_label", dataset.groups[sample]}, {"predicted_label", classes[class_index]},
                                       {"confidence", *predicted}});
        }
    }
    emit_rows(access, "plsScoresTable", {"sample_id", "component", "x_score", "y_score", "group_label"},
              {"string", "integer", "real", "real", "string"}, score_rows);
    emit_rows(access, "plsLoadingsTable", {"block", "item_id", "component", "loading"},
              {"string", "string", "integer", "real"}, loading_rows);
    emit_rows(access, "plsCoefficientsTable", {"predictor_id", "response_id", "coefficient", "intercept"},
              {"string", "string", "real", "real"}, coefficient_rows);
    emit_rows(access, "plsPredictionsTable", {"sample_id", "response_id", "observed_value", "predicted_value", "observed_label", "predicted_label", "confidence"},
              {"string", "string", "real", "real", "string", "string", "real"}, prediction_rows);
    const Json diagnostics{{"method", "nipals_pls2"}, {"response_mode", response_mode},
                          {"samples", dataset.sample_ids.size()}, {"predictors", dataset.variable_ids.size()},
                          {"responses", response_ids.size()}, {"components", computed},
                          {"preprocessing", preprocessing}, {"iterations", result.iterations},
                          {"converged", result.converged}, {"prediction_scope", "in_sample"},
                          {"classes", response_mode == "pls_da" ? Json(classes) : Json::array()}};
    access.emit_result("plsDiagnostics", diagnostics.dump());
    access.report_progress(1.0, "PLS completed");
    return {{"samples", dataset.sample_ids.size()}, {"predictors", dataset.variable_ids.size()},
            {"responses", response_ids.size()}, {"components", computed}, {"diagnostics", diagnostics}};
}

Json run_fold_change(sdk::PluginProjectAccess &access, const Json &parameters) {
    const auto group_column = string_parameter(parameters, "group_column");
    if (group_column.empty()) throw std::invalid_argument("fold change requires group_column");
    const auto dataset = read_dataset(access, parameters);
    const auto group_a = string_parameter(parameters, "group_a");
    const auto group_b = string_parameter(parameters, "group_b");
    const auto pseudocount_it = parameters.find("pseudocount");
    const auto pseudocount = pseudocount_it == parameters.end() ? 1.0 : numeric_value(*pseudocount_it, "pseudocount", 0);
    for (const auto &row : dataset.values)
        for (const auto value : row)
            if (value < 0.0) throw std::invalid_argument("fold change accepts only non-negative values");
    const auto stats = compute_fold_change(dataset.values, dataset.groups, group_a, group_b, pseudocount);
    Json rows = Json::array();
    std::size_t significant = 0;
    for (std::size_t variable = 0; variable < dataset.variable_ids.size(); ++variable) {
        const auto &feature = stats[variable];
        const auto log2fc = std::log2((feature.mean_b + pseudocount) / (feature.mean_a + pseudocount));
        const bool is_significant = feature.adjusted_p_value < 0.05;
        if (is_significant) ++significant;
        rows.push_back({{"variable_id", dataset.variable_ids[variable]}, {"mean_group_a", feature.mean_a},
                        {"mean_group_b", feature.mean_b}, {"log2_fold_change", log2fc},
                        {"effect_size", feature.effect_size_defined ? Json(feature.effect_size) : Json(nullptr)},
                        {"p_value", feature.p_value}, {"adjusted_p_value", feature.adjusted_p_value},
                        {"significant", is_significant}});
    }
    emit_rows(access, "foldChangeTable", {"variable_id", "mean_group_a", "mean_group_b", "log2_fold_change", "effect_size", "p_value", "adjusted_p_value", "significant"},
              {"string", "real", "real", "real", "real", "real", "real", "boolean"}, rows);
    const auto count_a = std::count(dataset.groups.begin(), dataset.groups.end(), group_a);
    const auto count_b = std::count(dataset.groups.begin(), dataset.groups.end(), group_b);
    const Json summary{{"method", "welch_t_test_benjamini_hochberg"}, {"group_a", group_a}, {"group_b", group_b},
                       {"samples_group_a", count_a}, {"samples_group_b", count_b},
                       {"variables_tested", dataset.variable_ids.size()}, {"significant_variables", significant},
                       {"adjusted_p_value_cutoff", 0.05}, {"pseudocount", pseudocount}};
    access.emit_result("foldChangeDiagnostics", summary.dump());
    access.report_progress(1.0, "Fold-change analysis completed");
    return summary;
}

}  // namespace detail

const sdk::CapabilityRegistry &capabilities() {
    static const sdk::CapabilityRegistry registry{
        {"statistics.pca", sdk::CapabilityKind::Operation, &detail::run_pca},
        {"statistics.pls", sdk::CapabilityKind::Operation, &detail::run_pls},
        {"statistics.fold_change", sdk::CapabilityKind::Operation, &detail::run_fold_change},
    };
    return registry;
}

}  // namespace streamfind::statistics::operations
