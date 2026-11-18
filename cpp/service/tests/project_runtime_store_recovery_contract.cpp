#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "streamfind/project.hpp"
#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/result_store.hpp"

namespace streamfind::service::runtime_store_recovery_test {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

}

using streamfind::service::runtime_store_recovery_test::require;

int main() {
    const auto root = std::filesystem::path(STREAMFIND_TEST_PROJECTS_DIR) /
                      "project-runtime-store-recovery";
    const auto database_path = root / "legacy-store.duckdb";
    const auto data_path = streamfind::service::ResultStore::data_path_for_database(database_path);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(data_path, error);
    try {
        std::filesystem::create_directories(root);
        {
            auto project = streamfind::Project::create({database_path, {}});
            project.register_result_manifest({
                {"result_id", "registered-old-result"},
                {"contract_id", "test.table"},
                {"kind", "table"},
                {"format", "arrow-ipc-file"},
                {"state", "published"},
                {"producer_operation", "test.produce"},
                {"producer_instance", "1"},
                {"workflow_revision", 1}});
            const auto created_inventory = project.get_result_inventory();
            require(created_inventory.size() == 1 &&
                    created_inventory.at(0).value("status", std::string{}) == "published",
                    "test project did not retain its published registration");
        }

        streamfind::service::ResultStore store(database_path);
        store.initialize(false);
        require(store.inspect(false, false).status == streamfind::service::ResultStoreStatus::ready,
                "test result store did not initialize");
        std::filesystem::remove(data_path / "store.json");
        std::filesystem::create_directories(data_path / "legacy-indexes");
        std::ofstream(data_path / "legacy-indexes" / "index.bin", std::ios::binary) << "legacy-index";
        require(store.inspect(false, false).status == streamfind::service::ResultStoreStatus::missing,
                "test old store was not classified as missing current metadata");

        streamfind::OperationRegistry operations;
        streamfind::service::ProjectRuntimeManager manager(operations);
        const auto opened = manager.open("legacy-session", {database_path, {}});
        const auto warning = std::find_if(opened.diagnostics.begin(), opened.diagnostics.end(),
            [](const streamfind::Json &diagnostic) {
                return diagnostic.value("code", std::string{}) == "legacy_result_store_invalidated";
            });
        require(warning != opened.diagnostics.end(),
                "legacy result-store open did not report its recovery diagnostic: " + opened.diagnostics.dump());
        require(warning->value("message", std::string{}).find("No migration was attempted") != std::string::npos,
                "recovery diagnostic did not explain the no-migration action");
        const auto preserved_message = warning->value("message", std::string{});
        require(preserved_message.find(".legacy-") != std::string::npos,
                "recovery diagnostic did not identify the preserved store folder");
        std::filesystem::path preserved_path;
        const auto preserved_prefix = data_path.filename().string() + ".legacy-";
        for (const auto &entry : std::filesystem::directory_iterator(data_path.parent_path())) {
            const auto name = entry.path().filename().string();
            if (entry.is_directory() && name.starts_with(preserved_prefix)) {
                preserved_path = entry.path();
                break;
            }
        }
        require(!preserved_path.empty(), "the legacy result-store folder was not renamed for preservation");
        require(std::filesystem::exists(preserved_path / "legacy-indexes" / "index.bin"),
                "the old result-store data was not preserved intact");
        require(store.inspect(false, false).status == streamfind::service::ResultStoreStatus::ready,
                "current result-store sidecar was not initialized after invalidation");
        require(opened.result_store.value("status", std::string{}) == "ready" &&
                    opened.result_store.value("published_count", std::size_t{1}) == 0 &&
                    opened.result_store.value("invalid_count", std::size_t{1}) == 0,
                "project-open metadata did not report the refreshed empty result store");
        const auto inventory = manager.result_inventory("legacy-session");
        require(inventory.empty(), "unavailable legacy result payloads were exposed by service inventory");

        manager.close("legacy-session");
        {
            auto project = streamfind::Project::open({database_path, {}});
            const auto registrations = project.get_result_inventory();
            require(registrations.size() == 1 &&
                    registrations.at(0).value("status", std::string{}) == "stale",
                    "published result registration was not marked stale in project metadata");
        }
        const auto reopened = manager.open("reopened-session", {database_path, {}});
        require(std::none_of(reopened.diagnostics.begin(), reopened.diagnostics.end(),
            [](const streamfind::Json &diagnostic) {
                return diagnostic.value("code", std::string{}) == "legacy_result_store_invalidated";
            }), "a second open repeated the old-store recovery warning");
        require(manager.result_inventory("reopened-session").empty(),
                "reopening exposed a result whose payload was preserved as legacy data");
        manager.close("reopened-session");

        const auto failed_root = root / "invalidation-failure";
        const auto failed_database = failed_root / "retry-after-failure.duckdb";
        const auto failed_data_path = streamfind::service::ResultStore::data_path_for_database(failed_database);
        std::filesystem::create_directories(failed_root);
        {
            auto project = streamfind::Project::create({failed_database, {}});
            project.register_result_manifest({
                {"result_id", "failure-preserved-result"},
                {"contract_id", "test.table"},
                {"kind", "table"},
                {"format", "arrow-ipc-file"},
                {"state", "published"},
                {"producer_operation", "test.produce"},
                {"producer_instance", "1"},
                {"workflow_revision", 1}});
        }
        streamfind::service::ResultStore failed_store(failed_database);
        failed_store.initialize(false);
        std::filesystem::remove(failed_data_path / "store.json");
        std::filesystem::create_directories(failed_data_path / "legacy-indexes");
        std::ofstream(failed_data_path / "legacy-indexes" / "index.bin", std::ios::binary) << "keep-on-failure";
        {
            auto project = streamfind::Project::open({failed_database, {}});
            project.execute_sql("DROP TABLE AUDIT_TRAIL");
            project.execute_sql("CREATE TABLE AUDIT_TRAIL (operation_type VARCHAR NOT NULL CHECK (operation_type != 'invalidate'), "
                                "object_type VARCHAR NOT NULL, operation_details JSON, "
                                "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        }

        streamfind::service::ProjectRuntimeManager retry_manager(operations);
        bool recovery_failed = false;
        try {
            static_cast<void>(retry_manager.open("failed-recovery-session", {failed_database, {}}));
        } catch (const std::exception &) {
            recovery_failed = true;
        }
        require(recovery_failed, "legacy recovery unexpectedly passed its injected audit failure");
        require(!std::filesystem::exists(failed_data_path),
                "failed recovery left the old store at its active path");
        std::filesystem::path failed_preserved_path;
        const auto failed_preserved_prefix = failed_data_path.filename().string() + ".legacy-";
        for (const auto &entry : std::filesystem::directory_iterator(failed_data_path.parent_path())) {
            const auto name = entry.path().filename().string();
            if (entry.is_directory() && name.starts_with(failed_preserved_prefix)) {
                failed_preserved_path = entry.path();
                break;
            }
        }
        require(!failed_preserved_path.empty() &&
                    std::filesystem::exists(failed_preserved_path / "legacy-indexes" / "index.bin"),
                "failed recovery did not preserve the legacy result-store payload");
        {
            auto project = streamfind::Project::open({failed_database, {}});
            const auto registrations = project.get_result_inventory();
            require(registrations.size() == 1 &&
                        registrations.front().value("status", std::string{}) == "published",
                    "failed recovery partially invalidated the published registration");
            project.execute_sql("DROP TABLE AUDIT_TRAIL");
            project.execute_sql("CREATE TABLE AUDIT_TRAIL (operation_type VARCHAR NOT NULL, "
                                "object_type VARCHAR NOT NULL, operation_details JSON, "
                                "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        }
        const auto recovered = retry_manager.open("recovered-session", {failed_database, {}});
        require(std::any_of(recovered.diagnostics.begin(), recovered.diagnostics.end(),
                    [](const streamfind::Json &diagnostic) {
                        return diagnostic.value("code", std::string{}) == "legacy_result_store_invalidated";
                    }), "recovery retry did not report successful invalidation");
        require(failed_store.inspect(false, false).status == streamfind::service::ResultStoreStatus::ready,
                "recovery retry did not initialize the current result store");
        require(recovered.result_store.value("status", std::string{}) == "ready" &&
                    recovered.result_store.value("published_count", std::size_t{1}) == 0 &&
                    recovered.result_store.value("invalid_count", std::size_t{1}) == 0,
                "recovery retry did not return refreshed result-store card metadata");
        require(retry_manager.result_inventory("recovered-session").empty(),
                "recovery retry exposed an unavailable legacy result");
        require(std::filesystem::exists(failed_preserved_path / "legacy-indexes" / "index.bin"),
                "recovery retry modified the preserved legacy result-store payload");
        retry_manager.close("recovered-session");

        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(data_path, error);
        std::cout << "project runtime store recovery contract passed\n";
        return 0;
    } catch (const std::exception &exception) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(data_path, error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
