#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "../tmp_projects.hpp"

int run() {
    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-core-project-smoke.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);

    auto project = streamfind::Project::create({path, "test", {{"owner", "test"}}});
    int table_runs = 0;
    int tail_runs = 0;
    streamfind::MethodRegistry registry;
    streamfind::MethodDefinition table_method;
    table_method.id = "test.table";
    table_method.name = "Table";
    table_method.domain = "test";
    table_method.cacheable = true;
    table_method.writes = {"TEST_OUTPUT"};
    registry.register_method(streamfind::Method(
        table_method,
        [&table_runs](streamfind::Project &project, const streamfind::Json &) {
            ++table_runs;
            project.execute_sql("CREATE TABLE IF NOT EXISTS TEST_OUTPUT (value VARCHAR)");
            project.execute_sql("DELETE FROM TEST_OUTPUT");
            project.execute_sql("INSERT INTO TEST_OUTPUT VALUES ('materialized')");
            return streamfind::Json{{"status", "finished"}};
        }));
    streamfind::MethodDefinition tail_method;
    tail_method.id = "test.tail";
    tail_method.name = "Tail";
    tail_method.domain = "test";
    tail_method.cacheable = false;
    registry.register_method(streamfind::Method(
        tail_method,
        [&tail_runs](streamfind::Project &, const streamfind::Json &) {
            ++tail_runs;
            return streamfind::Json{{"status", "tail"}};
        }));
    project.set_metadata({{"owner", "test"}});
    if (project.get_metadata().at("owner") != "test") {
        std::cerr << "metadata getter failed\n";
        return 1;
    }
    project.set_cache("test", "test cache", "hash", {{"value", 42}});
    if (project.get_cache().size() != 1) {
        std::cerr << "cache creation failed\n";
        return 1;
    }
    project.delete_cache();
    if (!project.get_cache().empty()) {
        std::cerr << "cache clear failed\n";
        return 1;
    }
    const auto copy_path = streamfind::test::tmp_projects_dir() / "streamfind-core-project-copy.duckdb";
    std::filesystem::remove(copy_path, error);
    auto copied = project.copy({copy_path, "test", {}});
    if (copied.get_metadata().at("owner") != "test") {
        std::cerr << "project copy failed\n";
        return 1;
    }
    copied.close();
    std::filesystem::remove(copy_path, error);
    streamfind::Workflow table_workflow;
    table_workflow.domain = "test";
    table_workflow.steps.push_back({"test.table", streamfind::ParameterValues{streamfind::Json::object()}});
    project.set_workflow(table_workflow, registry);
    project.run_workflow(registry);
    const auto launch_snapshot = streamfind::Json::parse(project.query_json("SELECT launch_snapshot FROM WORKFLOW_EXECUTION").at(0).at("launch_snapshot").get<std::string>());
    if (launch_snapshot.at("domain") != "test" || launch_snapshot.at("steps").size() != 1 || launch_snapshot.at("steps").at(0).at("id") != "test.table") {
        std::cerr << "workflow launch snapshot was not persisted\n";
        return 1;
    }
    project.execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'running'");
    project.close();
    project = streamfind::Project::open({path, "test", {}});
    if (streamfind::WorkflowExecutionManager(project).current().at("status") != "interrupted") {
        std::cerr << "workflow restart did not reconcile the active parent\n";
        return 1;
    }
    project.run_workflow(registry);
    if (table_runs != 1) {
        std::cerr << "workflow restart reran a valid completed cached step\n";
        return 1;
    }
    project.execute_sql("INSERT INTO WORKFLOW_EXECUTION_STEP (workflow_revision, step_index, method, parameters, parameter_hash, cache_key, status) VALUES (1, 99, 'stale', '{}', 'stale', 'stale', 'completed')");
    project.execute_sql("DELETE FROM TEST_OUTPUT");
    project.run_workflow(registry);
    if (table_runs != 1 || project.query_json("SELECT COUNT(*) AS count FROM WORKFLOW_EXECUTION_STEP").at(0).at("count") != "1" || project.query_json("SELECT value FROM TEST_OUTPUT") != streamfind::Json::array({{{"value", "materialized"}}})) {
        std::cerr << "cache table materialization failed\n";
        return 1;
    }
    if (project.run_method("test.tail", streamfind::Json::object(), registry).at("status") != "tail" ||
        table_runs != 1 || tail_runs != 1) {
        std::cerr << "workflow tail execution did not reuse the completed prefix\n";
        return 1;
    }
    project.execute_sql("DELETE FROM WORKFLOW_EXECUTION");
    project.delete_cache();
    if (project.run_method("test.tail", streamfind::Json::object(), registry).at("status") != "tail" ||
        table_runs != 2 || tail_runs != 3) {
        std::cerr << "workflow tail execution did not fall back to the full workflow\n";
        return 1;
    }
    const auto execution = project.get_workflow_execution();
    if (execution.at(0).at("status") != "completed") {
        std::cerr << "workflow execution tracking failed\n";
        return 1;
    }
    project.delete_cache();
    const auto execution_table = streamfind::api::run(
        streamfind::api::ProjectCommand::get_workflow_execution,
        {{"database_path", path.string()}});
    if (!execution_table.contains("columns") || execution_table.at("columns").at("step_index").empty()) {
        std::cerr << "workflow execution table API failed\n";
        return 1;
    }
    const auto metadata = streamfind::api::run(
        streamfind::api::ProjectCommand::get_metadata,
        {{"database_path", path.string()}});
    if (streamfind::Json::parse(metadata.at("columns").at("metadata").at(0).get<std::string>()).at("owner") != "test") {
        std::cerr << "metadata API failed\n";
        return 1;
    }
    if (streamfind::api::run(streamfind::api::ProjectCommand::validate,
                             {{"database_path", path.string()}}).at("valid") != true ||
        streamfind::api::run(streamfind::api::ProjectCommand::get_cache_size,
                             {{"database_path", path.string()}}) != 0) {
        std::cerr << "project validation API failed\n";
        return 1;
    }
    project.close();
    auto reopened = streamfind::Project::open({path, "test", {}});
    if (reopened.get_domain() != "test" ||
        reopened.get_metadata().at("owner") != "test") {
        std::cerr << "project reopen failed\n";
        return 1;
    }
    reopened.validate();
    reopened.close();
    std::filesystem::remove(path, error);
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
