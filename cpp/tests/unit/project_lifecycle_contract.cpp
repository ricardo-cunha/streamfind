#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "../tmp_projects.hpp"

#include <filesystem>
#include <iostream>
#include <atomic>
#include <thread>

namespace {

int run() {
    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-core-project-lifecycle.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);

    streamfind::OperationRegistry operations;
    std::atomic<int> executions{0};
    streamfind::OperationDefinition definition;
    definition.id = "test.tail";
    definition.name = "Tail operation";
    definition.domain = "test";
    definition.parameters.definitions.push_back({"a", "First fingerprint parameter", {streamfind::ParameterType::integer}, nullptr, false});
    definition.parameters.definitions.push_back({"b", "Second fingerprint parameter", {streamfind::ParameterType::integer}, nullptr, false});
    definition.output_ports.push_back({"test.result", "test.result", "one", "json", {"json"}, false});
    operations.register_operation(streamfind::Operation(
        definition,
        [&executions](streamfind::Project &project, const streamfind::Json &, const std::string &instance, const streamfind::Json &) {
            ++executions;
            const auto artifact_id = project.publish_result_artifact(
                "test.result", {{"status", "tail"}}, "test.tail", instance, project.get_workflow().version);
            return streamfind::Json{{"status", "tail"},
                                    {"emitted_results", {{"test.result", {{"artifact_id", artifact_id}, {"payload", {{"status", "tail"}}}}}}}};
        }));

    streamfind::OperationDefinition consumer_definition;
    consumer_definition.id = "test.consumer";
    consumer_definition.name = "Consumer operation";
    consumer_definition.domain = "test";
    consumer_definition.input_ports.push_back({"input", "test.table", "one", "duckdb_table", {"table"}, false});
    operations.register_operation(streamfind::Operation(
        consumer_definition,
        [](streamfind::Project &, const streamfind::Json &, const std::string &, const streamfind::Json &) {
            return streamfind::Json{{"status", "consumer"}};
        }));

    auto project = streamfind::Project::create({path, {{"owner", "test"}}});

    const auto added = streamfind::api::run(
        streamfind::api::ProjectCommand::add_operation,
        {{"database_path", path.string()}, {"operation_id", "consumer-1"},
         {"operation", "test.consumer"}}, operations);
    if (!added.at("updated") || added.at("workflow").at("operations").size() != 1) {
        std::cerr << "incremental operation insertion failed\n";
        return 1;
    }
    streamfind::Workflow workflow;
    workflow.name = "operation graph lifecycle";
    workflow.operations.push_back({"tail-1", "test.tail", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()});
    project.set_workflow(workflow, operations);
    const auto result = project.run_operation_graph(operations);

    if (result.at("status") != "completed" ||
        result.at("operations").size() != 1 ||
        result.at("operations").at(0).at("result").at("status") != "tail") {
        std::cerr << "operation graph execution failed\n";
        return 1;
    }
    const auto second_result = project.run_operation_graph(operations);

    if (executions != 1 || !second_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "unchanged operation did not reuse its published artifact\n";
        return 1;
    }
    workflow.operations.at(0).id = "tail-concurrent";
    project.set_workflow(workflow, operations);
    streamfind::Json concurrent_first;
    streamfind::Json concurrent_second;
    std::thread first_worker([&] { concurrent_first = project.run_operation_graph(operations); });
    std::thread second_worker([&] { concurrent_second = project.run_operation_graph(operations); });
    first_worker.join();
    second_worker.join();

    if (executions != 2 ||
        (concurrent_first.at("operations").at(0).value("cache_hit", false) ==
         concurrent_second.at("operations").at(0).value("cache_hit", false))) {
        std::cerr << "concurrent identical executions did not produce one cache miss and one cache hit\n";
        return 1;
    }
    workflow.operations.at(0).parameters.values = {{"a", 1}, {"b", 2}};
    project.set_workflow(workflow, operations);

    const auto parameter_result = project.run_operation_graph(operations);

    if (executions != 3 || parameter_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "changed parameters did not invalidate the artifact cache\n";
        return 1;
    }
    workflow.operations.at(0).parameters.values = {{"b", 2}, {"a", 1}};
    project.set_workflow(workflow, operations);

    const auto reordered_parameter_result = project.run_operation_graph(operations);

    if (executions != 3 || !reordered_parameter_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "parameter key order changed the artifact fingerprint\n";
        return 1;
    }
    const auto artifact_id = result.at("operations").at(0).at("result").at("emitted_results").at("test.result").at("artifact_id").get<std::string>();
    const auto inventory = streamfind::api::run(
        streamfind::api::ProjectCommand::get_artifact_inventory,
        {{"database_path", path.string()}}, operations);

    if (inventory.size() < 2 || inventory.at(0).contains("payload")) {
        std::cerr << "artifact inventory was not metadata-only\n";
        return 1;
    }
    const auto current_inventory = streamfind::api::run(
        streamfind::api::ProjectCommand::get_current_artifact_inventory,
        {{"database_path", path.string()}}, operations);

    if (current_inventory.size() != 1 || current_inventory.at(0).contains("payload") ||
        current_inventory.at(0).value("status", "") != "published") {
        std::cerr << "current artifact inventory was not filtered to published outputs\n";
        return 1;
    }
    const auto requested = streamfind::api::run(
        streamfind::api::ProjectCommand::request_artifact,
        {{"database_path", path.string()}, {"artifact_id", artifact_id}, {"include_data", true}}, operations);

    if (requested.size() != 1 || requested.at(0).at("data").at("status") != "tail") {
        std::cerr << "targeted artifact request failed\n";
        return 1;
    }
    project.clear_workflow_history();

    project.validate();
    project.execute_sql("DELETE FROM ARTIFACT_CACHE_OUTPUT WHERE artifact_id IN (SELECT artifact_id FROM ARTIFACT_INVENTORY WHERE producer_instance = 'tail-concurrent')");
    const auto stale_cache_result = project.run_operation_graph(operations);
    if (executions != 4 || stale_cache_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "stale cache output was incorrectly reused\n";
        return 1;
    }

    const auto execution_table = streamfind::api::run(
        streamfind::api::ProjectCommand::get_workflow_execution,
        {{"database_path", path.string()}}, operations);
    if (execution_table.at("row_count") != 1) {
        std::cerr << "workflow execution table API failed\n";
        return 1;
    }
    const auto metadata = streamfind::api::run(
        streamfind::api::ProjectCommand::get_metadata,
        {{"database_path", path.string()}}, operations);
    if (streamfind::Json::parse(metadata.at("columns").at("metadata").at(0).get<std::string>()).at("owner") != "test") {
        std::cerr << "metadata API failed\n";
        return 1;
    }
    if (!streamfind::api::run(streamfind::api::ProjectCommand::validate,
                              {{"database_path", path.string()}}, operations).at("valid")) {
        std::cerr << "project validation API failed\n";
        return 1;
    }
    streamfind::OperationDefinition incomplete_definition;
    incomplete_definition.id = "test.incomplete";
    incomplete_definition.name = "Incomplete output operation";
    incomplete_definition.domain = "test";
    incomplete_definition.output_ports.push_back({"test.result", "test.result", "one", "json", {"json"}, false});
    incomplete_definition.output_ports.push_back({"test.missing", "test.missing", "one", "json", {"json"}, false});
    operations.register_operation(streamfind::Operation(
        incomplete_definition,
        [&executions](streamfind::Project &project, const streamfind::Json &, const std::string &instance, const streamfind::Json &) {
            ++executions;
            const auto artifact_id = project.publish_result_artifact(
                "test.result", {{"status", "incomplete"}}, "test.incomplete", instance, project.get_workflow().version);
            return streamfind::Json{{"emitted_results", {{"test.result", {{"artifact_id", artifact_id}}}}}};
        }));
    workflow.operations = {{"incomplete-1", "test.incomplete", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()}};
    project.set_workflow(workflow, operations);
    const auto incomplete_first = project.run_operation_graph(operations);
    const auto incomplete_second = project.run_operation_graph(operations);
    if (executions != 6 || incomplete_first.at("operations").at(0).value("cache_hit", false) ||
        incomplete_second.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "incomplete cached output set was incorrectly reused\n";
        return 1;
    }
    project.close();
    auto reopened = streamfind::Project::open({path, {}});
    if (reopened.get_domains().size() != 1 ||
        reopened.get_metadata().at("owner") != "test") {
        std::cerr << "project reopen failed\n";
        return 1;
    }
    reopened.validate();
    reopened.close();
    std::filesystem::remove(path, error);
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
