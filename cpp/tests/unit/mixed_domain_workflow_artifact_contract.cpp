#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../tmp_projects.hpp"

namespace streamfind::mixed_domain_test {

Operation make_table_operation(const std::string &operation_id,
                               const std::string &domain,
                               const std::string &contract) {
    OperationDefinition definition;
    definition.id = operation_id;
    definition.name = operation_id;
    definition.domain = domain;
    definition.output_ports.push_back({"table", contract, "one", "table", {"duckdb-table"}, false});
    return Operation(std::move(definition),
                     [contract](Project &project, const Json &, const std::string &instance,
                                const Json &inputs) {
                         std::string source_artifact;
                         if (!inputs.empty())
                             source_artifact = inputs.begin().value().value("artifact_id", "");
                         std::string artifact_id;
                         std::string physical_table;
                         const auto workflow_revision = project.get_workflow().version;
                         ProjectTableStore::transaction(project, {}, [&](ProjectTableStore &tables) {
                             const auto allocation = tables.allocate_table_artifact(
                                 contract, instance == "source" ? "domain_a.make_table" : "domain_b.make_table",
                                 instance, workflow_revision,
                                 Json::array({{{"name", "value"}, {"type", "string"}}}));
                             artifact_id = allocation.first;
                             physical_table = allocation.second;
                             tables.append(physical_table, {"value"},
                                           {{{instance == "source" ? "domain-a" : "domain-b"}}});
                             if (!source_artifact.empty())
                                 tables.append_artifact_lineage(
                                     artifact_id, {{source_artifact, "table", "input"}});
                         });
                         return Json{{"artifact_id", artifact_id}, {"physical_table", physical_table}};
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
        registry.register_operation(make_table_operation("domain_a.make_table", "domain_a", "table"));

        OperationDefinition second_definition;
        second_definition.id = "domain_b.make_table";
        second_definition.name = "domain_b.make_table";
        second_definition.domain = "domain_b";
        second_definition.input_ports.push_back({"input", "table", "one", "table", {"duckdb-table"}, false});
        second_definition.output_ports.push_back({"table", "table", "one", "table", {"duckdb-table"}, false});
        second_definition.parameters.definitions.push_back(
            {"targets", "Connected table rows", TypeDescriptor{ParameterType::array,
             std::make_shared<TypeDescriptor>(TypeDescriptor{ParameterType::object})}, nullptr, false});
        registry.register_operation(Operation(
            std::move(second_definition),
            [](Project &project, const Json &parameters, const std::string &instance, const Json &inputs) {
                if (!parameters.contains("targets") || !parameters.at("targets").is_array() ||
                    parameters.at("targets").size() != 1 ||
                    parameters.at("targets").at(0).value("value", "") != "domain-a")
                    throw std::runtime_error("table parameter binding did not resolve table rows");
                const auto source_artifact = inputs.at("input").at("artifact_id").get<std::string>();
                std::string artifact_id;
                std::string physical_table;
                const auto workflow_revision = project.get_workflow().version;
                ProjectTableStore::transaction(project, {}, [&](ProjectTableStore &tables) {
                    const auto allocation = tables.allocate_table_artifact(
                        "table", "domain_b.make_table", instance, workflow_revision,
                        Json::array({{{"name", "value"}, {"type", "string"}}}));
                    artifact_id = allocation.first;
                    physical_table = allocation.second;
                    tables.append(physical_table, {"value"}, {{{std::string("domain-b")}}});
                    tables.append_artifact_lineage(
                        artifact_id, {{source_artifact, "table", "input"}});
                });
                return Json{{"artifact_id", artifact_id}, {"physical_table", physical_table}};
            }));

        Workflow workflow;
        workflow.workflow_id = "mixed-domain";
        workflow.name = "Mixed domain artifact workflow";
        workflow.version = 7;
        workflow.operations = {
            {"source", "domain_a.make_table", ParameterValues{Json::object()}, Json::object(), Json::object()},
            {"consumer", "domain_b.make_table", ParameterValues{Json{{"targets", Json::array()}}}, Json::object(), Json::object()}};
        workflow.connections.push_back({"source", "table", "consumer", "input"});
        workflow.connections.push_back({"source", "table", "consumer", "parameter:targets"});

        project.set_workflow(workflow, registry);
        const auto stored = project.get_workflow();
        if (stored.operations.size() != 2 || stored.connections.size() != 2 ||
            registry.find(stored.operations.at(0).operation)->definition().domain ==
                registry.find(stored.operations.at(1).operation)->definition().domain)
            throw std::runtime_error("mixed-domain workflow was not persisted without domain rejection");

        const auto result = project.run_operation_graph(registry);
        if (result.value("status", "") != "completed" || result.at("operations").size() != 2)
            throw std::runtime_error("mixed-domain operation graph did not execute");

        const auto inventory = project.get_artifact_inventory();
        if (inventory.size() != 2)
            throw std::runtime_error("expected two table artifacts");
        const auto source = std::find_if(inventory.begin(), inventory.end(), [](const auto &artifact) {
            return artifact.value("producer_instance", "") == "source";
        });
        const auto consumer = std::find_if(inventory.begin(), inventory.end(), [](const auto &artifact) {
            return artifact.value("producer_instance", "") == "consumer";
        });
        if (source == inventory.end() || consumer == inventory.end())
            throw std::runtime_error("artifact inventory lost producer instances");
        if (source->value("contract_id", "") != "table" ||
            consumer->value("contract_id", "") != "table" ||
            source->value("workflow_revision", "") != "7" ||
            consumer->value("workflow_revision", "") != "7" ||
            source->value("artifact_id", "") == consumer->value("artifact_id", "") ||
            source->value("physical_table", "") == consumer->value("physical_table", ""))
            throw std::runtime_error("table artifacts do not have distinct explicit identity");

        const auto resolved = project.resolve_workflow_inputs("consumer");
        if (resolved.at("input").value("artifact_id", "") != source->value("artifact_id", ""))
            throw std::runtime_error("explicit artifact binding did not select the source instance");
        const auto lineage = project.query_json(
            "SELECT source_artifact_id, source_port_id, target_port_id FROM ARTIFACT_LINEAGE");
        if (lineage.size() != 1 || lineage.at(0).value("source_artifact_id", "") != source->value("artifact_id", "") ||
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
