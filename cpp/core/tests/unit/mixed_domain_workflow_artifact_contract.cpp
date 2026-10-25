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

namespace streamfind::mixed_domain_test {

Operation make_table_operation(const std::string &operation_id,
                               const std::string &domain,
                               const std::string &contract) {
    OperationDefinition definition;
    definition.id = operation_id;
    definition.name = operation_id;
    definition.domain = domain;
    definition.output_ports.push_back({"table", contract, "one", "duckdb_table", {"duckdb-table"}, false});
    return Operation(std::move(definition),
                     [contract](OperationExecutionContext &context, const Json &, const std::string &instance,
                                const Json &inputs) {
                         std::string source_result;
                         if (!inputs.empty())
                             source_result = inputs.begin().value().value("result_id",
                                 inputs.begin().value().value("result_id", ""));
                         const auto producer = instance == "1" ? "domain_a.make_table" : "domain_b.make_table";
                         const Json descriptor = {{"contract_id", contract}, {"kind", "table"},
                             {"format", "json-lines"}, {"producer_operation", producer},
                             {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                             {"schema", Json::array({{{"name", "value"}, {"type", "string"}}})}};
                         const auto allocation = context.outputs().begin(descriptor);
                         const auto handle = allocation.value("output_handle", "");
                         if (handle.empty()) throw std::runtime_error("table output lease was not created");
                         context.outputs().write_rows(handle, 0,
                             Json::array({{{"value", instance == "1" ? "domain-a" : "domain-b"}}}));
                         Json lineage = Json::array();
                         if (!source_result.empty())
                             lineage.push_back({{"result_id", source_result}, {"source_port", "table"},
                                                {"target_port", "input"}});
                         const auto published = context.outputs().finish(handle,
                             {{"provenance", {{"lineage_inputs", lineage}}}});
                         return Json{{"emitted_results", {{"table", published}}}};
                     });
}

void run() {
    const auto path = streamfind::test::tmp_projects_dir() /
                      "streamfind-mixed-domain-workflow-artifact.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);
    try {
        OperationRegistry registry;
        {
            auto project = Project::create({path, {}});
        registry.register_operation(make_table_operation("domain_a.make_table", "domain_a", "anyTable"));

        OperationDefinition second_definition;
        second_definition.id = "domain_b.make_table";
        second_definition.name = "domain_b.make_table";
        second_definition.domain = "domain_b";
        second_definition.input_ports.push_back({"input", "anyTable", "one", "duckdb_table", {"duckdb-table"}, false});
        second_definition.output_ports.push_back({"table", "anyTable", "one", "duckdb_table", {"duckdb-table"}, false});
        registry.register_operation(Operation(
            std::move(second_definition),
            [](OperationExecutionContext &context, const Json &, const std::string &instance, const Json &inputs) {
                const auto source_result = inputs.at("input").value("result_id",
                    inputs.at("input").value("result_id", ""));
                const Json descriptor = {{"contract_id", "anyTable"}, {"kind", "table"},
                    {"format", "json-lines"}, {"producer_operation", "domain_b.make_table"},
                    {"producer_instance", instance}, {"workflow_revision", context.workflow_revision()},
                    {"schema", Json::array({{{"name", "value"}, {"type", "string"}}})}};
                const auto allocation = context.outputs().begin(descriptor);
                const auto handle = allocation.value("output_handle", "");
                context.outputs().write_rows(handle, 0,
                    Json::array({{{"value", "domain-b"}}}));
                Json lineage = Json::array({{{"result_id", source_result},
                    {"source_port", "table"}, {"target_port", "input"}}});
                const auto published = context.outputs().finish(
                    handle, Json{{"provenance", Json{{"lineage_inputs", lineage}}}});
                return Json{{"emitted_results", {{"table", published}}}};
            }));

        Workflow workflow;
        workflow.workflow_id = "mixed-domain";
        workflow.name = "Mixed domain artifact workflow";
        workflow.version = 7;
        workflow.operations = {
            {"1", "domain_a.make_table", ParameterValues{Json::object()}, Json::object(), Json::object()},
            {"2", "domain_b.make_table", ParameterValues{Json::object()}, Json::object(), Json::object()}};
        workflow.connections.push_back({"1", "table", "2", "input"});

        project.set_workflow(workflow, registry);
        const auto stored = project.get_workflow();
        if (stored.operations.size() != 2 || stored.connections.size() != 1)
            throw std::runtime_error("mixed-domain workflow persistence mismatch: operations=" +
                                     std::to_string(stored.operations.size()) + ", connections=" +
                                     std::to_string(stored.connections.size()));

        const auto result = project.run_operation_graph(registry);
        if (result.value("status", "") != "completed" || result.at("operations").size() != 2)
            throw std::runtime_error("mixed-domain operation graph did not execute");

        const auto inventory = project.get_result_inventory();
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
            source->value("physical_table", "") == consumer->value("physical_table", ""))
            throw std::runtime_error("table artifacts do not have distinct explicit identity");

        const auto resolved = project.resolve_workflow_inputs("2");
        if (resolved.at("input").value("result_id", "") != source->value("result_id", ""))
            throw std::runtime_error("explicit artifact binding did not select the source instance");
        const auto lineage = project.query_json(
            "SELECT source_result_id, source_port_id, target_port_id FROM RESULT_LINEAGE");
        if (lineage.size() != 1 || lineage.at(0).value("source_result_id", "") != source->value("result_id", "") ||
            lineage.at(0).value("source_port_id", "") != "table" ||
            lineage.at(0).value("target_port_id", "") != "input")
            throw std::runtime_error("artifact lineage binding was not recorded");

        project.close();
        std::filesystem::remove(path, error);
        std::cout << "Mixed-domain workflow artifact contract passed\n";
        }
    } catch (...) {
        std::filesystem::remove(path, error);
        throw;
    }
}

} // namespace streamfind::mixed_domain_test

int main() {
    try {
        streamfind::mixed_domain_test::run();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
