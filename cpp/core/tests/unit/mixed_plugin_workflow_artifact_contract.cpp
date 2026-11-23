#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../support/tmp_projects.hpp"

namespace streamfind::mixed_plugin_test {

OperationRunner test_runner(Project &project, const OperationRegistry &registry) {
    return [&project, &registry](const std::string &operation_id, const Json &parameters,
                                 const std::string &instance, const Json &inputs,
                                 const std::string &, bool) {
        const auto *operation = registry.find(operation_id);
        if (!operation) throw std::runtime_error("test runner received an unknown operation");
        if (operation_id == "plugin_b.make_table") {
            const auto source_id = inputs.at("input").value("result_id", std::string{});
            const auto &reference = parameters.at("sourceTable");
            if (reference.value("$result_ref", std::string{}) != source_id ||
                reference.value("result_id", std::string{}) != source_id ||
                reference.value("output_port_id", std::string{}) != "table")
                throw std::runtime_error("connected table parameter lost its enriched result reference");
        }
        Json emitted = Json::object();
        const auto workflow = project.get_workflow();
        for (const auto &port : operation->definition().output_ports) {
            const auto result_id = "test-result-" + instance + "-" + port.id;
            Json lineage = Json::array();
            for (const auto &edge : workflow.connections) {
                if (edge.target_operation != instance || edge.target_port.rfind("parameter:", 0) == 0) continue;
                const auto input = inputs.find(edge.target_port);
                if (input == inputs.end()) continue;
                const auto source_id = input->value("result_id", std::string{});
                if (!source_id.empty())
                    lineage.push_back({{"result_id", source_id}, {"source_port", edge.source_port},
                                       {"target_port", edge.target_port}});
            }
            project.register_result_manifest({
                {"result_id", result_id}, {"contract_id", port.semantic_contract},
                {"kind", port.data_kind}, {"format", port.data_kind == "table" ? "arrow-ipc-file" : "json"},
                {"state", "published"}, {"producer_operation", operation_id},
                {"producer_instance", instance}, {"workflow_revision", workflow.version},
                {"row_count", port.data_kind == "table" ? 1 : 0},
                {"schema", port.data_kind == "table" ? Json::array({{{"name", "value"}, {"type", "string"}}}) : Json::array()},
                {"provenance", {{"lineage_inputs", lineage}}}});
            emitted[port.id] = {{"result_id", result_id}, {"output_port_id", port.id},
                                {"contract_id", port.semantic_contract}, {"representation", port.data_kind}};
        }
        return Json{{"status", "finished"}, {"emitted_results", std::move(emitted)}};
    };
}

Operation make_table_operation(const std::string &operation_id,
                               const std::string &plugin,
                               const std::string &contract) {
    OperationDefinition definition;
    definition.id = operation_id;
    definition.name = operation_id;
    definition.plugin = plugin;
    definition.output_ports.push_back({"table", contract, "one", "table", {"table"}, false});
    return Operation(std::move(definition));
}

void run() {
    const auto path = streamfind::test::tmp_projects_dir() /
                      "streamfind-mixed-plugin-workflow-artifact.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);
    try {
        OperationRegistry registry;
        {
            auto project = Project::create({path, {}});
        registry.register_operation(make_table_operation("plugin_a.make_table", "plugin_a", "anyTable"));

        OperationDefinition second_definition;
        second_definition.id = "plugin_b.make_table";
        second_definition.name = "plugin_b.make_table";
        second_definition.plugin = "plugin_b";
        second_definition.input_ports.push_back({"input", "anyTable", "one", "table", {"table"}, false});
        second_definition.output_ports.push_back({"table", "anyTable", "one", "table", {"table"}, false});
        ParameterDefinition table_parameter;
        table_parameter.name = "sourceTable";
        table_parameter.description = "Table result supplied by a workflow connection.";
        table_parameter.type.kind = ParameterType::table;
        table_parameter.required = true;
        table_parameter.semantic_contract = "anyTable";
        second_definition.parameters.definitions.push_back(std::move(table_parameter));
        registry.register_operation(Operation(std::move(second_definition)));

        Workflow workflow;
        workflow.workflow_id = "mixed-plugin";
        workflow.name = "Mixed plugin artifact workflow";
        workflow.version = 7;
        workflow.operations = {
            {"1", "plugin_a.make_table", ParameterValues{Json::object()}, Json::object(), Json::object()},
            {"2", "plugin_b.make_table", ParameterValues{Json::object()}, Json::object(), Json::object()}};
        workflow.connections.push_back({"1", "table", "2", "input"});
        workflow.connections.push_back({"1", "table", "2", "parameter:sourceTable"});

        project.set_workflow(workflow, registry);
        const auto stored = project.get_workflow();
        if (stored.operations.size() != 2 || stored.connections.size() != 2)
            throw std::runtime_error("mixed-plugin workflow persistence mismatch: operations=" +
                                     std::to_string(stored.operations.size()) + ", connections=" +
                                     std::to_string(stored.connections.size()));

        const auto result = project.run_operation_graph(registry, test_runner(project, registry));
        if (result.value("status", "") != "completed" || result.at("operations").size() != 2)
            throw std::runtime_error("mixed-plugin operation graph did not execute");

        const auto initial_inventory = project.get_result_inventory();
        const auto cached_run = project.run_operation_graph(registry, test_runner(project, registry));
        if (cached_run.value("status", "") != "completed")
            throw std::runtime_error("mixed-plugin cached workflow rerun did not complete");
        if (cached_run.at("operations").size() != 2 ||
            !cached_run.at("operations").at(0).value("cache_hit", false) ||
            !cached_run.at("operations").at(1).value("cache_hit", false))
            throw std::runtime_error("fresh-to-cache workflow rerun did not reuse both connected table results");
        const auto inventory = project.get_result_inventory();
        if (inventory.size() != initial_inventory.size())
            throw std::runtime_error("repeated workflow run changed the published result count from " +
                std::to_string(initial_inventory.size()) + " to " + std::to_string(inventory.size()) +
                "; initial consumer inputs: " + result.at("operations").at(1).value("inputs", Json::object()).dump() +
                "; repeated consumer inputs: " + cached_run.at("operations").at(1).value("inputs", Json::object()).dump());
        for (std::size_t index = 0; index < inventory.size(); ++index)
            if (inventory.at(index).value("result_id", "") != initial_inventory.at(index).value("result_id", ""))
                throw std::runtime_error("cached workflow rerun replaced a published result");
        if (inventory.size() != 2)
            throw std::runtime_error("expected two table artifacts");
        const auto source = std::find_if(inventory.begin(), inventory.end(), [](const auto &artifact) {
            return artifact.value("producer_instance", "") == "1";
        });
        const auto consumer = std::find_if(inventory.begin(), inventory.end(), [](const auto &artifact) {
            return artifact.value("producer_instance", "") == "2";
        });
        if (source == inventory.end() || consumer == inventory.end())
            throw std::runtime_error("artifact inventory lost producer instances");
        if (source->value("contract_id", "") != "anyTable" ||
            consumer->value("contract_id", "") != "anyTable" ||
            source->value("workflow_revision", "") != "7" ||
            consumer->value("workflow_revision", "") != "7" ||
            source->value("result_id", "") == consumer->value("result_id", "") ||
            source->value("result_id", "") == consumer->value("result_id", ""))
            throw std::runtime_error("table artifacts do not have distinct explicit identity");

        const auto resolved = project.resolve_workflow_inputs("2");
        if (resolved.at("input").value("result_id", "") != source->value("result_id", ""))
            throw std::runtime_error("explicit artifact binding did not select the source instance");
        const auto &resolved_table_parameter = resolved.at("parameter:sourceTable");
        if (resolved_table_parameter.value("$result_ref", "") != source->value("result_id", "") ||
            resolved_table_parameter.value("result_id", "") != source->value("result_id", "") ||
            resolved_table_parameter.value("output_port_id", "") != "table")
            throw std::runtime_error("table-valued workflow parameter did not resolve to its enriched Arrow result reference");
        const auto lineage = project.query_json(
            "SELECT source_result_id, source_port_id, target_port_id FROM RESULT_LINEAGE");
        if (lineage.size() != 1 || lineage.at(0).value("source_result_id", "") != source->value("result_id", "") ||
            lineage.at(0).value("source_port_id", "") != "table" ||
            lineage.at(0).value("target_port_id", "") != "input")
            throw std::runtime_error("artifact lineage binding was not recorded");

        Json replacement_manifest = *consumer;
        replacement_manifest["state"] = "published";
        replacement_manifest["workflow_revision"] = 7;
        replacement_manifest["provenance"]["lineage_inputs"] = Json::array({
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}},
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}}
        });
        bool duplicate_edge_rejected = false;
        try {
            project.register_result_manifest(replacement_manifest);
        } catch (const std::exception &) {
            duplicate_edge_rejected = true;
        }
        if (!duplicate_edge_rejected)
            throw std::runtime_error("duplicate lineage edges were accepted during result replacement");
        const auto lineage_after_failed_replacement = project.query_json(
            "SELECT source_result_id, source_port_id, target_port_id FROM RESULT_LINEAGE "
            "WHERE result_id = '" + consumer->value("result_id", std::string{}) + "'");
        const auto registry_after_failed_replacement = project.query_json(
            "SELECT producer_operation, producer_instance, workflow_revision FROM RESULT_REGISTRY "
            "WHERE result_id = '" + consumer->value("result_id", std::string{}) + "'");
        if (lineage_after_failed_replacement.size() != 1 ||
            lineage_after_failed_replacement.at(0).value("source_result_id", "") != source->value("result_id", "") ||
            lineage_after_failed_replacement.at(0).value("source_port_id", "") != "table" ||
            lineage_after_failed_replacement.at(0).value("target_port_id", "") != "input" ||
            registry_after_failed_replacement.size() != 1 ||
            registry_after_failed_replacement.at(0).value("producer_operation", "") != "plugin_b.make_table" ||
            registry_after_failed_replacement.at(0).value("producer_instance", "") != "2" ||
            registry_after_failed_replacement.at(0).value("workflow_revision", std::string{}) != "7")
            throw std::runtime_error("failed lineage replacement changed the published result registry");

        replacement_manifest["provenance"]["lineage_inputs"] = Json::array({
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}},
            {{"result_id", source->value("result_id", "")}, {"source_port", "alternateTable"}, {"target_port", "input"}}
        });
        project.register_result_manifest(replacement_manifest);
        const auto replaced_lineage = project.query_json(
            "SELECT source_result_id, source_port_id, target_port_id FROM RESULT_LINEAGE "
            "WHERE result_id = '" + consumer->value("result_id", std::string{}) + "' ORDER BY source_port_id");
        if (replaced_lineage.size() != 2 ||
            replaced_lineage.at(0).value("source_port_id", "") != "alternateTable" ||
            replaced_lineage.at(1).value("source_port_id", "") != "table" ||
            replaced_lineage.at(0).value("source_result_id", "") != source->value("result_id", "") ||
            replaced_lineage.at(1).value("source_result_id", "") != source->value("result_id", ""))
            throw std::runtime_error("valid lineage replacement did not preserve distinct source ports");

        Json batch_valid_manifest = replacement_manifest;
        batch_valid_manifest["result_id"] = "batch-valid-before-failure";
        batch_valid_manifest["producer_operation"] = "plugin_b.make_table";
        batch_valid_manifest["producer_instance"] = "batch-valid";
        batch_valid_manifest["provenance"]["lineage_inputs"] = Json::array({
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}}
        });
        Json batch_invalid_manifest = batch_valid_manifest;
        batch_invalid_manifest["result_id"] = "batch-invalid-lineage";
        batch_invalid_manifest["producer_instance"] = "batch-invalid";
        batch_invalid_manifest["provenance"]["lineage_inputs"] = Json::array({
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}},
            {{"result_id", source->value("result_id", "")}, {"source_port", "table"}, {"target_port", "input"}}
        });
        bool batch_registration_rejected = false;
        try {
            project.register_result_manifests({batch_valid_manifest, batch_invalid_manifest});
        } catch (const std::exception &) {
            batch_registration_rejected = true;
        }
        if (!batch_registration_rejected)
            throw std::runtime_error("result manifest batch with duplicate lineage was accepted");
        const auto partially_registered_batch = project.query_json(
            "SELECT result_id FROM RESULT_REGISTRY WHERE result_id IN "
            "('batch-valid-before-failure', 'batch-invalid-lineage')");
        if (!partially_registered_batch.empty())
            throw std::runtime_error("failed result batch registration left partial registry rows");

        project.close();
        auto reopened = Project::open({path, {}});
        const auto reopened_cached_run = reopened.run_operation_graph(registry, test_runner(reopened, registry));
        if (reopened_cached_run.value("status", "") != "completed")
            throw std::runtime_error("mixed-plugin workflow did not complete after project reopen");
        if (reopened_cached_run.at("operations").size() != 2 ||
            !reopened_cached_run.at("operations").at(0).value("cache_hit", false) ||
            !reopened_cached_run.at("operations").at(1).value("cache_hit", false))
            throw std::runtime_error("reopened workflow did not reuse both connected table results");
        const auto reopened_inventory = reopened.get_result_inventory();
        if (reopened_inventory.size() != inventory.size())
            throw std::runtime_error("reopened cached run changed the published result count");
        for (std::size_t index = 0; index < reopened_inventory.size(); ++index)
            if (reopened_inventory.at(index).value("result_id", "") != inventory.at(index).value("result_id", ""))
                throw std::runtime_error("reopened cached run replaced a published result");
        const auto reopened_lineage = reopened.query_json(
            "SELECT source_result_id, source_port_id, target_port_id FROM RESULT_LINEAGE "
            "WHERE result_id = '" + consumer->value("result_id", std::string{}) + "' ORDER BY source_port_id");
        if (reopened_lineage != replaced_lineage)
            throw std::runtime_error("replacement lineage did not persist after reopening the project");
        if (reopened.invalidate_published_results("mixed-plugin cache invalidation contract") != 2)
            throw std::runtime_error("result invalidation did not mark both connected outputs stale");
        const auto invalidated_rerun = reopened.run_operation_graph(registry, test_runner(reopened, registry));
        if (invalidated_rerun.value("status", "") != "completed" ||
            invalidated_rerun.at("operations").size() != 2 ||
            invalidated_rerun.at("operations").at(0).value("cache_hit", false) ||
            invalidated_rerun.at("operations").at(1).value("cache_hit", false))
            throw std::runtime_error("stale connected table results were reused after invalidation");
        reopened.close();
        std::filesystem::remove(path, error);
        std::cout << "Mixed-plugin workflow artifact contract passed\n";
        }
    } catch (...) {
        std::filesystem::remove(path, error);
        throw;
    }
}

} // namespace streamfind::mixed_plugin_test

int main() {
    try {
        streamfind::mixed_plugin_test::run();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
