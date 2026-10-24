#include "operations.hpp"
#include "streamfind/sdk/plugin_project_access.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamfind::statistics::tests {

using Json = nlohmann::json;

class MemoryProjectAccess final : public sdk::PluginProjectAccess {
  public:
    std::map<std::string, Json> tables;
    std::map<std::string, Json> emitted;
    std::map<std::string, Json> results;
    std::string_view operation_instance() const noexcept override { return "statistics-test"; }
    bool is_cancelled() const noexcept override { return false; }
    Json query(const std::string &) override { throw std::runtime_error("query not expected"); }
    void require_table(const std::string &) override {}
    Json read(const std::string &, const std::vector<std::string> &, const std::string &) override {
        throw std::runtime_error("read not expected");
    }
    void read_batches(const std::string &table, const std::vector<std::string> &,
                      const std::string &, const JsonBatchCallback &callback) override {
        callback(tables.at(table));
    }
    std::uint64_t count_rows(const std::string &table) override {
        return tables.at(table).size();
    }
    void clear_table(const std::string &) override {}
    void append(const std::string &, const std::vector<std::string> &,
                const std::vector<std::vector<std::optional<std::string>>> &) override {}
    void emit_table_rows(const std::string &contract, const std::vector<std::string> &,
                         const std::vector<std::string> &, const Json &rows) override {
        for (const auto &row : rows) emitted[contract].push_back(row);
    }
    void emit_table_batch(const std::string &, const std::vector<streamfind_plugin_batch_column> &,
                          std::uint64_t) override {
        throw std::runtime_error("typed batch emission not expected");
    }
    void emit_result(const std::string &contract, const std::string &payload) override {
        results[contract] = Json::parse(payload);
    }
    void update_composite(const std::string &, const std::vector<std::string> &,
                          const std::vector<std::string> &,
                          const std::vector<std::vector<std::optional<std::string>>> &) override {}
    void delete_rows(const std::string &, const std::string &,
                     const std::vector<std::vector<std::optional<std::string>>> &) override {}
    void report_progress(double, std::string_view) override {}
};

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, double tolerance,
                  const std::string &message) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

Json base_parameters(const char *layout) {
    return {{"layout", layout}, {"sample_id_column", "sample"},
            {"preprocessing", "center"}, {"n_components", 1},
            {"_inputs", {{"tableInput", {{"physical_table", "source"}}}}}};
}

void test_registry_and_long_pca() {
    const auto &registry = operations::capabilities();
    require(registry.find("statistics.pca") != nullptr, "PCA capability was not registered");
    require(registry.find("statistics.pls") != nullptr, "PLS capability was not registered");
    require(registry.find("statistics.fold_change") != nullptr, "fold-change capability was not registered");

    MemoryProjectAccess access;
    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v2"}, {"intensity", 1.0}, {"condition", "A"}},
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 2.0}, {"condition", "A"}},
        {{"sample", "s2"}, {"feature", "v2"}, {"intensity", 2.0}, {"condition", "B"}},
        {{"sample", "s2"}, {"feature", "v1"}, {"intensity", 4.0}, {"condition", "B"}},
        {{"sample", "s3"}, {"feature", "v2"}, {"intensity", 3.0}, {"condition", "A"}},
        {{"sample", "s3"}, {"feature", "v1"}, {"intensity", 6.0}, {"condition", "A"}},
    });
    auto parameters = base_parameters("long");
    parameters["variable_id_column"] = "feature";
    parameters["value_column"] = "intensity";
    parameters["duplicate_aggregation"] = "error";
    parameters["group_column"] = "condition";
    const auto *binding = registry.find("statistics.pca");
    const auto response = binding->handler(access, parameters);
    require(response.at("samples") == 3, "PCA did not report the input sample count");
    require(access.emitted.at("pcaScoresTable").size() == 3,
            "PCA score output row count does not match sample/component keys");
    require(access.emitted.at("pcaScoresTable")[0].at("sample_id") == "s1" &&
            access.emitted.at("pcaScoresTable")[2].at("sample_id") == "s3",
            "PCA did not preserve first-seen sample ID order");
    require(access.emitted.at("pcaLoadingsTable")[0].at("variable_id") == "v2" &&
            access.emitted.at("pcaLoadingsTable")[1].at("variable_id") == "v1",
            "PCA did not preserve first-seen variable ID order");
    require(access.results.at("pcaDiagnostics").at("components") == 1,
            "PCA diagnostics result contract was not emitted");
}

void test_wide_fold_change_and_degenerate_values() {
    MemoryProjectAccess access;
    access.tables["source"] = Json::array({
        {{"sample", "a1"}, {"condition", "A"}, {"stable", 1.0}, {"same", 2.0}},
        {{"sample", "a2"}, {"condition", "A"}, {"stable", 1.0}, {"same", 2.0}},
        {{"sample", "b1"}, {"condition", "B"}, {"stable", 3.0}, {"same", 2.0}},
        {{"sample", "b2"}, {"condition", "B"}, {"stable", 3.0}, {"same", 2.0}},
    });
    auto parameters = base_parameters("wide");
    parameters["variable_columns"] = Json::array({"stable", "same"});
    parameters["group_column"] = "condition";
    parameters["group_a"] = "A";
    parameters["group_b"] = "B";
    const auto *binding = operations::capabilities().find("statistics.fold_change");
    const auto summary = binding->handler(access, parameters);
    const auto &rows = access.emitted.at("foldChangeTable");
    require(rows.size() == 2 && rows[0].at("variable_id") == "stable",
            "wide fold-change output did not preserve variable ID order");
    require(rows[0].at("p_value") == 0.0 && rows[0].at("adjusted_p_value") == 0.0,
            "different constant groups did not produce p=q=0");
    require_near(rows[0].at("log2_fold_change").get<double>(), 1.0, 1e-12,
                 "fold-change operation emitted an incorrect pseudocount-adjusted log ratio");
    require(rows[0].at("effect_size").is_null(),
            "zero pooled variance did not produce null Hedges' g");
    require(rows[1].at("p_value") == 1.0 && rows[1].at("adjusted_p_value") == 1.0,
            "matching constant groups did not produce p=q=1");
    require(summary.at("samples_group_a") == 2 && summary.at("samples_group_b") == 2,
            "fold-change structured summary has incorrect group counts");
    require(access.results.contains("foldChangeDiagnostics"),
            "fold-change diagnostics result contract was not emitted");
}

void test_pls_regression_and_pls_da_contracts() {
    MemoryProjectAccess regression_access;
    regression_access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", 0.0}, {"response", 3.0}},
        {{"sample", "s2"}, {"feature", 1.0}, {"response", 5.0}},
        {{"sample", "s3"}, {"feature", 2.0}, {"response", 7.0}},
        {{"sample", "s4"}, {"feature", 3.0}, {"response", 9.0}},
        {{"sample", "s5"}, {"feature", 4.0}, {"response", 11.0}},
    });
    auto regression = base_parameters("wide");
    regression["variable_columns"] = Json::array({"feature"});
    regression["response_mode"] = "regression";
    regression["response_columns"] = Json::array({"response"});
    const auto result = operations::capabilities().find("statistics.pls")->handler(regression_access, regression);
    const auto &predictions = regression_access.emitted.at("plsPredictionsTable");
    const auto &coefficients = regression_access.emitted.at("plsCoefficientsTable");
    require(predictions.size() == 5 && predictions.front().at("sample_id") == "s1",
            "PLS regression predictions lost stable sample IDs");
    require_near(predictions.front().at("predicted_value").get<double>(), 3.0, 1e-8,
                 "PLS regression operation did not preserve the response scale");
    require_near(coefficients.front().at("coefficient").get<double>(), 2.0, 1e-8,
                 "PLS regression operation emitted an incorrect source-unit coefficient");
    require_near(coefficients.front().at("intercept").get<double>(), 3.0, 1e-8,
                 "PLS regression operation emitted an incorrect intercept");
    require(result.at("diagnostics").at("prediction_scope") == "in_sample",
            "PLS diagnostics did not label fitted predictions as in-sample");

    MemoryProjectAccess da_access;
    da_access.tables["source"] = Json::array({
        {{"sample", "c1"}, {"group", "control"}, {"feature", 0.0}},
        {{"sample", "c2"}, {"group", "control"}, {"feature", 0.1}},
        {{"sample", "c3"}, {"group", "control"}, {"feature", 0.2}},
        {{"sample", "t1"}, {"group", "treated"}, {"feature", 4.8}},
        {{"sample", "t2"}, {"group", "treated"}, {"feature", 4.9}},
        {{"sample", "t3"}, {"group", "treated"}, {"feature", 5.0}},
    });
    auto da = base_parameters("wide");
    da["variable_columns"] = Json::array({"feature"});
    da["response_mode"] = "pls_da";
    da["group_column"] = "group";
    operations::capabilities().find("statistics.pls")->handler(da_access, da);
    const auto &class_predictions = da_access.emitted.at("plsPredictionsTable");
    require(class_predictions.size() == 6 &&
            class_predictions[0].at("predicted_label") == "control" &&
            class_predictions[5].at("predicted_label") == "treated",
            "PLS-DA operation did not select fitted classes by their scores");
}

void test_validation_diagnostics() {
    MemoryProjectAccess access;
    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 1.0}},
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 2.0}},
    });
    auto parameters = base_parameters("long");
    parameters["variable_id_column"] = "feature";
    parameters["value_column"] = "intensity";
    bool duplicate_rejected = false;
    try {
        operations::capabilities().find("statistics.pca")->handler(access, parameters);
    } catch (const std::invalid_argument &error) {
        duplicate_rejected = std::string(error.what()).find("row 2") != std::string::npos &&
                             std::string(error.what()).find("sample-variable") != std::string::npos;
    }
    require(duplicate_rejected, "long-form duplicate pair was not reported with source row context");

    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 1.0}},
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 3.0}},
        {{"sample", "s2"}, {"feature", "v1"}, {"intensity", 2.0}},
    });
    parameters["duplicate_aggregation"] = "mean";
    parameters["missing_value_imputation"] = "zero";
    const auto duplicate_response = operations::capabilities().find("statistics.pca")->handler(access, parameters);
    require(duplicate_response.at("samples") == 2 && duplicate_response.at("variables") == 1,
            "PCA did not aggregate repeated long-layout sample-variable values");

    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", "bad"}},
        {{"sample", "s2"}, {"feature", "v1"}, {"intensity", 2.0}},
    });
    bool numeric_rejected = false;
    try {
        operations::capabilities().find("statistics.pca")->handler(access, parameters);
    } catch (const std::invalid_argument &error) {
        numeric_rejected = std::string(error.what()).find("row 1") != std::string::npos &&
                           std::string(error.what()).find("intensity") != std::string::npos;
    }
    require(numeric_rejected, "nonnumeric value error did not identify row and column");

    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 1.0}, {"response", 4.0}},
        {{"sample", "s1"}, {"feature", "v2"}, {"intensity", 2.0}, {"response", 5.0}},
        {{"sample", "s2"}, {"feature", "v1"}, {"intensity", 3.0}, {"response", 6.0}},
        {{"sample", "s2"}, {"feature", "v2"}, {"intensity", 4.0}, {"response", 6.0}},
    });
    auto long_regression = base_parameters("long");
    long_regression["variable_id_column"] = "feature";
    long_regression["value_column"] = "intensity";
    long_regression["response_mode"] = "regression";
    long_regression["response_columns"] = Json::array({"response"});
    bool inconsistent_response_rejected = false;
    try {
        operations::capabilities().find("statistics.pls")->handler(access, long_regression);
    } catch (const std::invalid_argument &error) {
        inconsistent_response_rejected = std::string(error.what()).find("row 2") != std::string::npos &&
                                        std::string(error.what()).find("response") != std::string::npos;
    }
    require(inconsistent_response_rejected,
            "long-layout repeated response values must be validated within each sample");

    access.tables["source"] = Json::array({
        {{"sample", "same"}, {"feature", 1.0}},
        {{"sample", "same"}, {"feature", 2.0}},
    });
    auto wide = base_parameters("wide");
    wide["variable_columns"] = Json::array({"feature"});
    bool duplicate_wide_sample_rejected = false;
    try {
        operations::capabilities().find("statistics.pca")->handler(access, wide);
    } catch (const std::invalid_argument &error) {
        duplicate_wide_sample_rejected = std::string(error.what()).find("row 2") != std::string::npos &&
                                         std::string(error.what()).find("duplicates sample ID") != std::string::npos;
    }
    require(duplicate_wide_sample_rejected, "wide layout accepted duplicate sample IDs");
}

void test_missing_value_imputation() {
    MemoryProjectAccess access;
    access.tables["source"] = Json::array({
        {{"sample", "s1"}, {"feature", "v1"}, {"intensity", 2.0}},
        {{"sample", "s1"}, {"feature", "v2"}, {"intensity", 10.0}},
        {{"sample", "s2"}, {"feature", "v1"}, {"intensity", 4.0}},
        {{"sample", "s3"}, {"feature", "v1"}, {"intensity", 6.0}},
        {{"sample", "s3"}, {"feature", "v2"}, {"intensity", 14.0}},
    });
    auto parameters = base_parameters("long");
    parameters["variable_id_column"] = "feature";
    parameters["value_column"] = "intensity";
    parameters["missing_value_imputation"] = "mean";
    const auto response = operations::capabilities().find("statistics.pca")->handler(access, parameters);
    require(response.at("samples") == 3, "mean imputation changed the sample count");
    require(access.emitted.at("pcaScoresTable").size() == 3,
            "mean imputation did not complete the matrix for PCA");

    parameters["missing_value_imputation"] = "error";
    bool missing_rejected = false;
    try {
        operations::capabilities().find("statistics.pca")->handler(access, parameters);
    } catch (const std::invalid_argument &error) {
        missing_rejected = std::string(error.what()).find("missing for sample s2") != std::string::npos;
    }
    require(missing_rejected, "default missing-value validation was not preserved");
}

}  // namespace streamfind::statistics::tests

int main() {
    using namespace streamfind::statistics::tests;
    try {
        test_registry_and_long_pca();
        test_wide_fold_change_and_degenerate_values();
        test_pls_regression_and_pls_da_contracts();
        test_validation_diagnostics();
        test_missing_value_imputation();
        std::cout << "Statistics operation contract tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
