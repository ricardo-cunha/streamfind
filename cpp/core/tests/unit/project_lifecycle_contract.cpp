#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "../support/tmp_projects.hpp"

#include <filesystem>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <thread>

namespace streamfind::test_detail {

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
        [&executions](streamfind::OperationExecutionContext &context, const streamfind::Json &, const std::string &, const streamfind::Json &) {
            ++executions;
            const auto published = context.publish_result(
                "test.result", {{"payload", {{"status", "tail"}}},
                                  {"producer_operation", "test.tail"}}, {});
            return streamfind::Json{{"status", "tail"},
                                    {"emitted_results", {{"test.result", {{"result_id", published.value("result_id", "")},
                                                                            {"payload", {{"status", "tail"}}}}}}}};
        }));

    streamfind::OperationDefinition consumer_definition;
    consumer_definition.id = "test.consumer";
    consumer_definition.name = "Consumer operation";
    consumer_definition.domain = "test";
    consumer_definition.input_ports.push_back({"input", "test.table", "one", "duckdb_table", {"table"}, false});
    operations.register_operation(streamfind::Operation(
        consumer_definition,
        [](streamfind::OperationExecutionContext &, const streamfind::Json &, const std::string &, const streamfind::Json &) {
            return streamfind::Json{{"status", "consumer"}};
        }));

    auto project = streamfind::Project::create({path, {{"owner", "test"}}});

    auto incremental_workflow = project.get_workflow();
    incremental_workflow.operations.push_back({"1", "test.consumer", {streamfind::Json::object()},
                                               streamfind::Json::object(), streamfind::Json::object()});
    project.set_workflow(incremental_workflow);
    const auto added = streamfind::Json{{"updated", true}, {"workflow", project.get_workflow().to_json()}};
    if (!added.at("updated") || added.at("workflow").at("operations").size() != 1) {
        std::cerr << "incremental operation insertion failed\n";
        return 1;
    }
    streamfind::Workflow workflow;
    workflow.name = "operation graph lifecycle";
    workflow.operations.push_back({"2", "test.tail", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()});
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
    workflow.operations.at(0).id = "3";
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
    const auto result_id = result.at("operations").at(0).at("result").at("emitted_results").at("test.result").at("result_id").get<std::string>();
    const auto inventory = project.get_result_inventory();
    auto inventory_metadata = inventory;
    for (auto &entry : inventory_metadata) entry.erase("payload");

    if (inventory_metadata.size() < 2 || inventory_metadata.at(0).contains("payload")) {
        std::cerr << "artifact inventory was not metadata-only\n";
        return 1;
    }
    const auto current_inventory = project.get_current_result_inventory();

    if (current_inventory.size() != 1 || current_inventory.at(0).value("status", "") != "published") {
        std::cerr << "current artifact inventory was not filtered to published outputs\n";
        return 1;
    }
    const auto requested = project.query_json(
        "SELECT result_id, payload FROM RESULT_REGISTRY WHERE result_id = '" + result_id + "'");
    const auto requested_it = std::find_if(requested.begin(), requested.end(), [&](const auto &entry) {
        return entry.value("result_id", "") == result_id;
    });
    Json requested_payload;
    if (requested_it != requested.end() && requested_it->contains("payload")) {
        requested_payload = requested_it->at("payload");
        if (requested_payload.is_string()) requested_payload = Json::parse(requested_payload.get<std::string>());
        while (requested_payload.is_array() && requested_payload.size() == 1)
            requested_payload = requested_payload.at(0);
        if (requested_payload.is_object() && requested_payload.contains("payload"))
            requested_payload = requested_payload.at("payload");
        if (requested_payload.is_string()) requested_payload = Json::parse(requested_payload.get<std::string>());
    }
    if (requested_it == requested.end() || !requested_payload.is_object() ||
        requested_payload.value("status", "") != "tail") {
        std::cerr << "targeted artifact request failed\n";
        return 1;
    }
    project.clear_workflow_history();

    project.validate();
    project.execute_sql("DELETE FROM RESULT_CACHE_OUTPUT WHERE result_id IN (SELECT result_id FROM RESULT_REGISTRY WHERE producer_instance = '3')");
    const auto stale_cache_result = project.run_operation_graph(operations);
    if (executions != 4 || stale_cache_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "stale cache output was incorrectly reused\n";
        return 1;
    }

    const auto execution_table = project.get_workflow_execution();
    if (!execution_table.is_array() || execution_table.size() != 1) {
        std::cerr << "workflow execution table API failed\n";
        return 1;
    }
    const auto metadata = project.get_metadata();
    if (metadata.at("owner") != "test") {
        std::cerr << "metadata API failed\n";
        return 1;
    }
    try {
        project.validate();
    } catch (...) {
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
        [&executions](streamfind::OperationExecutionContext &context, const streamfind::Json &, const std::string &, const streamfind::Json &) {
            ++executions;
            const auto published = context.publish_result(
                "test.result", {{"payload", {{"status", "incomplete"}}},
                                  {"producer_operation", "test.incomplete"}}, {});
            return streamfind::Json{{"emitted_results", {{"test.result", {{"result_id", published.value("result_id", "")}}}}}};
        }));
    workflow.operations = {{"4", "test.incomplete", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()}};
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

} // namespace streamfind::test_detail

int main() {
    try {
        return streamfind::test_detail::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
