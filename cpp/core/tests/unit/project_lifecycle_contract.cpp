#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "../support/tmp_projects.hpp"

#include <filesystem>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <thread>

namespace streamfind::test_detail {

streamfind::OperationRunner test_runner(streamfind::Project &project,
                                        const streamfind::OperationRegistry &operations,
                                        std::atomic<int> &executions) {
    return [&project, &operations, &executions](const std::string &operation_id, const streamfind::Json &,
                                                 const std::string &instance, const streamfind::Json &,
                                                 const std::string &, bool) {
        const auto *operation = operations.find(operation_id);
        if (!operation) throw std::runtime_error("test runner received an unknown operation");
        ++executions;
        streamfind::Json emitted = streamfind::Json::object();
        for (const auto &port : operation->definition().output_ports) {
            if (operation_id == "test.incomplete" && port.id == "test.missing") continue;
            const auto result_id = "lifecycle-result-" + std::to_string(executions.load());
            project.register_result_manifest({
                {"result_id", result_id}, {"contract_id", port.semantic_contract},
                {"kind", port.data_kind}, {"format", port.data_kind == "table" ? "arrow-ipc-file" : "json"},
                {"state", "published"}, {"producer_operation", operation_id},
                {"producer_instance", instance}, {"workflow_revision", project.get_workflow().version},
                {"schema", streamfind::Json::array()}});
            emitted[port.id] = {{"result_id", result_id}, {"contract_id", port.semantic_contract},
                                {"representation", port.data_kind}};
        }
        return streamfind::Json{{"status", operation_id == "test.tail" ? "tail" : "finished"},
                                {"emitted_results", std::move(emitted)}};
    };
}

int run() {
    bool string_ids_rejected = false;
    try {
        static_cast<void>(streamfind::Workflow::from_json({
        {"schema_version", 2}, {"definition_revision", 1},
        {"operations", streamfind::Json::array({
            {{"id", "operation-1"}, {"operation", "test.producer"}},
            {{"id", "operation-2"}, {"operation", "test.consumer"}}})},
        {"connections", streamfind::Json::array({
            {{"source_operation", "operation-1"}, {"source_port", "result"},
             {"target_operation", "operation-2"}, {"target_port", "input"}}})}}));
    } catch (const streamfind::Error &) {
        string_ids_rejected = true;
    }
    if (!string_ids_rejected) {
        std::cerr << "workflow string node IDs were accepted\n";
        return 1;
    }
    const auto numeric_id_workflow = streamfind::Workflow::from_json({
        {"schema_version", 2}, {"definition_revision", 1},
        {"operations", streamfind::Json::array({
            {{"id", 1}, {"operation", "test.producer"}},
            {{"id", 2}, {"operation", "test.consumer"}}})},
        {"connections", streamfind::Json::array({
            {{"source_operation", 1}, {"source_port", "result"},
             {"target_operation", 2}, {"target_port", "input"}}})}});
    const auto numeric_id_round_trip = numeric_id_workflow.to_json();
    if (numeric_id_round_trip.at("operations").at(0).at("id") != 1 ||
        numeric_id_round_trip.at("connections").at(0).at("source_operation") != 1) {
        std::cerr << "integer workflow node IDs changed JSON representation\n";
        return 1;
    }

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
    operations.register_operation(streamfind::Operation(definition));

    streamfind::OperationDefinition consumer_definition;
    consumer_definition.id = "test.consumer";
    consumer_definition.name = "Consumer operation";
    consumer_definition.domain = "test";
    consumer_definition.input_ports.push_back({"input", "test.table", "one", "table", {"table"}, false});
    operations.register_operation(streamfind::Operation(consumer_definition));

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
    workflow.definition_revision = 7;
    workflow.operations.push_back({"2", "test.tail", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()});
    project.set_workflow(workflow, operations);
    if (project.get_workflow().definition_revision != 7) {
        std::cerr << "workflow definition revision was not persisted\n";
        return 1;
    }
    const auto result = project.run_operation_graph(operations, test_runner(project, operations, executions));

    if (result.at("status") != "completed" ||
        result.at("operations").size() != 1 ||
        result.at("operations").at(0).at("result").at("status") != "tail") {
        std::cerr << "operation graph execution failed\n";
        return 1;
    }
    const auto second_result = project.run_operation_graph(operations, test_runner(project, operations, executions));

    if (executions != 1 || !second_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "unchanged operation did not reuse its published artifact\n";
        return 1;
    }
    workflow.operations.at(0).id = "3";
    project.set_workflow(workflow, operations);
    streamfind::Json concurrent_first;
    streamfind::Json concurrent_second;
    std::thread first_worker([&] { concurrent_first = project.run_operation_graph(operations, test_runner(project, operations, executions)); });
    std::thread second_worker([&] { concurrent_second = project.run_operation_graph(operations, test_runner(project, operations, executions)); });
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

    const auto parameter_result = project.run_operation_graph(operations, test_runner(project, operations, executions));

    if (executions != 3 || parameter_result.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "changed parameters did not invalidate the artifact cache\n";
        return 1;
    }
    workflow.operations.at(0).parameters.values = {{"b", 2}, {"a", 1}};
    project.set_workflow(workflow, operations);

    const auto reordered_parameter_result = project.run_operation_graph(operations, test_runner(project, operations, executions));

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
    const auto registered_table = std::find_if(inventory.begin(), inventory.end(), [&](const auto &entry) {
        return entry.value("result_id", "") == result_id;
    });
    if (registered_table == inventory.end()) {
        std::cerr << "published table registration was not present in inventory\n";
        return 1;
    }
    if (registered_table->value("representation", "") != "json" ||
        (registered_table->contains("payload") && !registered_table->at("payload").is_null())) {
        std::cerr << "result registry stored payload data instead of result metadata\n";
        return 1;
    }
    project.clear_workflow_history();

    project.validate();
    project.execute_sql("DELETE FROM RESULT_CACHE_OUTPUT WHERE result_id IN (SELECT result_id FROM RESULT_REGISTRY WHERE producer_instance = '3')");
    const auto stale_cache_result = project.run_operation_graph(operations, test_runner(project, operations, executions));
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
    operations.register_operation(streamfind::Operation(incomplete_definition));
    workflow.operations = {{"4", "test.incomplete", {streamfind::Json::object()}, streamfind::Json::object(), streamfind::Json::object()}};
    project.set_workflow(workflow, operations);
    const auto incomplete_first = project.run_operation_graph(operations, test_runner(project, operations, executions));
    const auto incomplete_second = project.run_operation_graph(operations, test_runner(project, operations, executions));
    if (executions != 6 || incomplete_first.at("operations").at(0).value("cache_hit", false) ||
        incomplete_second.at("operations").at(0).value("cache_hit", false)) {
        std::cerr << "incomplete cached output set was incorrectly reused\n";
        return 1;
    }
    project.register_result_manifest({
        {"result_id", "invalidation-atomic-result"}, {"contract_id", "test.result"},
        {"kind", "json"}, {"format", "json"}, {"state", "published"},
        {"producer_operation", "test.producer"}, {"producer_instance", "atomic"},
        {"workflow_revision", project.get_workflow().version}});
    project.execute_sql("INSERT INTO RESULT_CACHE (fingerprint, operation_id, status) "
                        "VALUES ('invalidation-atomic-fingerprint', 'test.producer', 'complete')");
    project.execute_sql("INSERT INTO RESULT_CACHE_OUTPUT (fingerprint, output_port_id, ordinal, result_id) "
                        "VALUES ('invalidation-atomic-fingerprint', 'test.result', 0, 'invalidation-atomic-result')");
    project.execute_sql("DROP TABLE AUDIT_TRAIL");
    bool invalidation_failed = false;
    try {
        static_cast<void>(project.invalidate_published_results("injected audit write failure"));
    } catch (const std::exception &) {
        invalidation_failed = true;
    }
    if (!invalidation_failed) {
        std::cerr << "invalidation unexpectedly succeeded without its audit table\n";
        return 1;
    }
    const auto after_failed_invalidation = project.get_result_inventory();
    const auto atomic_registration = std::find_if(after_failed_invalidation.begin(), after_failed_invalidation.end(),
        [](const auto &entry) { return entry.value("result_id", "") == "invalidation-atomic-result"; });
    if (atomic_registration == after_failed_invalidation.end() ||
        atomic_registration->value("status", "") != "published" ||
        project.query_json("SELECT fingerprint FROM RESULT_CACHE WHERE fingerprint = 'invalidation-atomic-fingerprint'").empty() ||
        project.query_json("SELECT result_id FROM RESULT_CACHE_OUTPUT WHERE fingerprint = 'invalidation-atomic-fingerprint'").empty()) {
        std::cerr << "failed result invalidation partially modified registration or cache metadata\n";
        return 1;
    }
    project.execute_sql("CREATE TABLE AUDIT_TRAIL (operation_type VARCHAR NOT NULL, object_type VARCHAR NOT NULL, "
                        "operation_details JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
    const auto invalidated_count = project.invalidate_published_results("atomic invalidation contract");
    const auto after_invalidation = project.get_result_inventory();
    const auto invalidated_registration = std::find_if(after_invalidation.begin(), after_invalidation.end(),
        [](const auto &entry) { return entry.value("result_id", "") == "invalidation-atomic-result"; });
    if (invalidated_count == 0 || invalidated_registration == after_invalidation.end() ||
        invalidated_registration->value("status", "") != "stale" ||
        !project.query_json("SELECT fingerprint FROM RESULT_CACHE WHERE fingerprint = 'invalidation-atomic-fingerprint'").empty() ||
        !project.query_json("SELECT result_id FROM RESULT_CACHE_OUTPUT WHERE fingerprint = 'invalidation-atomic-fingerprint'").empty()) {
        std::cerr << "successful result invalidation did not stale registrations and clear cache metadata\n";
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
