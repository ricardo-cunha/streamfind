#include "streamfind/api.hpp"
#include "streamfind/project.hpp"
#include "../tmp_projects.hpp"

#include <filesystem>
#include <iostream>

namespace {

int run() {
    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-core-project-lifecycle.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);

    streamfind::OperationRegistry operations;
    streamfind::OperationDefinition definition;
    definition.id = "test.tail";
    definition.name = "Tail operation";
    definition.domain = "test";
    operations.register_operation(streamfind::Operation(
        definition,
        [](streamfind::Project &, const streamfind::Json &, const std::string &, const streamfind::Json &) {
            return streamfind::Json{{"status", "tail"}};
        }));

    auto project = streamfind::Project::create({path, {{"owner", "test"}}});
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
                              {{"database_path", path.string()}}, operations).at("valid") ||
        streamfind::api::run(streamfind::api::ProjectCommand::get_cache_size,
                             {{"database_path", path.string()}}, operations) != 0) {
        std::cerr << "project validation API failed\n";
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
