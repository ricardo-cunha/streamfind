#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "streamfind/project.hpp"
#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/result_store.hpp"

namespace streamfind::service::runtime_execution_retry_test {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

}

int main() {
    using namespace streamfind;
    using namespace streamfind::service;
    using streamfind::service::runtime_execution_retry_test::require;

    const auto root = std::filesystem::path(STREAMFIND_TEST_PROJECTS_DIR) /
                      "project-runtime-execution-retry";
    const auto database_path = root / "retry.sf" / "project.duckdb";
    const auto data_path = ResultStore::data_path_for_database(database_path);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(data_path, error);

    try {
        std::filesystem::create_directories(root);
        OperationDefinition definition;
        definition.id = "test.retry_worker";
        definition.name = "Retry worker contract";
        definition.domain = "test";
        definition.parameters = ParameterSchema::from_json(Json::array({Json{
            {"name", "mode"}, {"type", Json{{"type", "string"}}}, {"default", "success"}
        }}));

        OperationRegistry operations;
        operations.register_operation(Operation(std::move(definition)));

        ProjectRuntimeManager manager(operations,
            std::filesystem::path(STREAMFIND_RETRY_WORKER_DIR) / "streamfind.json", 18791);
        manager.create("retry-session", {database_path, {}});

        bool first_run_failed = false;
        try {
            static_cast<void>(manager.run_operation("retry-session", "test.retry_worker",
                Json{{"mode", "crash"}}, "retry-node", Json::object()));
        } catch (const std::exception &) {
            first_run_failed = true;
        }
        require(first_run_failed, "the fixture worker crash was not reported as an operation failure");

        const auto retry = manager.run_operation("retry-session", "test.retry_worker",
            Json{{"mode", "success"}}, "retry-node", Json::object());
        require(retry.value("ok", false),
                "a successful retry in the same service session did not complete");
        require(manager.result_inventory("retry-session").empty(),
                "a failed worker left published output visible after retry");

        manager.close("retry-session");
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(data_path, error);
        std::cout << "project runtime execution retry contract passed\n";
        return 0;
    } catch (const std::exception &exception) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(data_path, error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
