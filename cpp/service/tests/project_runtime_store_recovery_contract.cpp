#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "streamfind/project_layout.hpp"
#include "streamfind/plugin_lockfile.hpp"
#include "streamfind/plugin_registry.hpp"
#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/result_store.hpp"

namespace streamfind::service::runtime_store_recovery_test {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

}

using streamfind::service::runtime_store_recovery_test::require;

int main() {
    const auto root = std::filesystem::path(STREAMFIND_TEST_PROJECTS_DIR) / "project-container-contract";
    const auto container = root / "run with ünicode.v2.sf";
    const auto layout = streamfind::ProjectLayout::from_container(container);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try {
        std::filesystem::create_directories(root);
        streamfind::OperationRegistry operations;
        streamfind::service::ProjectRuntimeManager first(operations);
        streamfind::service::ProjectRuntimeManager second(operations);
        const auto created = first.create("container-session", {layout.database(), {}});
        require(created.project_path == streamfind::path_to_utf8(layout.container()),
                "service did not report the canonical project folder");
        require(std::filesystem::is_regular_file(layout.database()),
                "project database was not created inside the .sf folder");
        require(std::filesystem::is_regular_file(layout.data() / "store.json"),
                "result store was not created under .sf/data");
        const auto store = streamfind::service::ResultStore(layout.database()).inspect(false, false);
        require(store.status == streamfind::service::ResultStoreStatus::ready && !store.store_id.empty(),
                "new project result-store manifest is invalid");
        first.set_metadata("container-session", streamfind::Json{{"label", "source project"}});

        bool duplicate_rejected = false;
        try {
            static_cast<void>(first.create("duplicate-session", {layout.database(), {}}));
        } catch (const std::exception &exception) {
            const std::string message = exception.what();
            duplicate_rejected = message.find("already exists") != std::string::npos ||
                                 message.find("already open") != std::string::npos;
        }
        require(duplicate_rejected && std::filesystem::is_regular_file(layout.database()) &&
                    std::filesystem::is_regular_file(layout.data() / "store.json"),
                "duplicate creation was not rejected without changing the existing container");

        bool missing_parent_rejected = false;
        const auto missing_parent_container = root / "missing-parent" / "invalid.sf";
        try {
            static_cast<void>(first.create("missing-parent-session",
                {streamfind::ProjectLayout::from_container(missing_parent_container).database(), {}}));
        } catch (const std::exception &exception) {
            missing_parent_rejected = std::string(exception.what()).find("parent folder does not exist") != std::string::npos;
        }
        require(missing_parent_rejected && !std::filesystem::exists(missing_parent_container.parent_path()),
                "creation with a missing parent was not safely rejected");

        bool legacy_path_rejected = false;
        try {
            static_cast<void>(streamfind::ProjectLayout::from_container(root / "legacy.duckdb"));
        } catch (const std::exception &exception) {
            legacy_path_rejected = std::string(exception.what()).find("matching streamfind release") != std::string::npos;
        }
        require(legacy_path_rejected,
                "legacy DuckDB paths do not explain the no-migration rebuild procedure");

        const auto copy_layout = streamfind::ProjectLayout::from_container(root / "copy.sf");
        const auto copied = first.copy_project("container-session", copy_layout.container());
        require(copied.value("project_path", "") == streamfind::path_to_utf8(copy_layout.container()) &&
                    copied.at("session_id").is_null(),
                "project copy did not return the new container path without attaching a session");
        const auto copied_store = streamfind::service::ResultStore(copy_layout.database()).inspect(false, false);
        require(copied_store.status == streamfind::service::ResultStoreStatus::ready &&
                    copied_store.store_id != store.store_id && copied_store.published_count == 0,
                "project copy did not receive a distinct, empty result store");
        const auto copied_project = second.open("copied-session", {copy_layout.database(), {}});
        require(copied_project.metadata == streamfind::Json{{"label", "source project"}},
                "project copy did not preserve project metadata");
        second.close("copied-session");

        const auto same = first.open("another-session-id", {layout.database(), {}});
        require(same.session_id == "container-session",
                "opening the same folder did not reuse its existing service session");
        bool cross_service_rejected = false;
        try {
            static_cast<void>(second.open("competing-session", {layout.database(), {}}));
        } catch (const std::exception &exception) {
            cross_service_rejected = std::string(exception.what()).find("already open") != std::string::npos;
        }
        require(cross_service_rejected, "a second service acquired an already-open project container");

        const auto original_manifest = layout.data() / "store.json";
        std::ifstream input(original_manifest, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(input)), {});
        first.close("container-session");

        auto altered = streamfind::Json::parse(original);
        altered["store_id"] = "store-from-another-container";
        {
            std::ofstream output(original_manifest, std::ios::binary | std::ios::trunc);
            output << altered.dump(2) << '\n';
        }
        bool mismatch_rejected = false;
        try {
            static_cast<void>(second.open("mismatch-session", {layout.database(), {}}));
        } catch (const std::exception &exception) {
            mismatch_rejected = std::string(exception.what()).find("do not belong to the same .sf container") != std::string::npos;
        }
        require(mismatch_rejected, "database and result-store identity mismatch was not reported");
        {
            std::ifstream check(original_manifest, std::ios::binary);
            const std::string unchanged((std::istreambuf_iterator<char>(check)), {});
            require(unchanged == altered.dump(2) + "\n",
                    "failed project open modified the mismatched result-store manifest");
        }

        {
            std::ofstream output(original_manifest, std::ios::binary | std::ios::trunc);
            output << original;
        }
        const auto reopened = second.open("reopened-session", {layout.database(), {}});
        require(reopened.session_id == "reopened-session",
                "the second service could not open the project after the first closed it");
        second.close("reopened-session");

        const auto registry_path = root / "registry.json";
        streamfind::write_plugin_registry_atomic(registry_path, streamfind::PluginRegistry{
            1, 7, {streamfind::InstalledPlugin{"native.plugin.sensors", "0.8.0",
                streamfind::native_plugin_platform(), root / "installed-sensors", "sha256:sensors",
                "local", "verified", true}}});
        const auto configuration_path = root / "streamfind.json";
        {
            std::ofstream output(configuration_path, std::ios::binary | std::ios::trunc);
            output << streamfind::Json{{"registry_path", registry_path.generic_string()}}.dump(2) << '\n';
        }
        streamfind::write_plugin_lockfile_atomic(layout.container() / "project.extensions.lock.json",
            streamfind::PluginLockfile{1, {streamfind::PluginLockEntry{
                "native.plugin.sensors", "0.8.0", "sha256:sensors", 3, 0, {}, {}}}});
        streamfind::OperationRegistry locked_operations;
        streamfind::service::ProjectRuntimeManager locked(locked_operations, configuration_path);
        const auto locked_open = locked.open("locked-session", {layout.database(), {}});
        require(locked_open.session_id == "locked-session", "matching project plugin lockfile was rejected");
        locked.close("locked-session");
        streamfind::write_plugin_lockfile_atomic(layout.container() / "project.extensions.lock.json",
            streamfind::PluginLockfile{1, {streamfind::PluginLockEntry{
                "native.plugin.sensors", "0.8.1", "sha256:sensors", 3, 0, {}, {}}}});
        bool lock_mismatch_rejected = false;
        try {
            static_cast<void>(locked.open("mismatched-lock-session", {layout.database(), {}}));
        } catch (const std::exception &exception) {
            lock_mismatch_rejected = std::string(exception.what()).find("does not match") != std::string::npos;
        }
        require(lock_mismatch_rejected, "project plugin lockfile mismatch was not rejected");

        std::filesystem::remove_all(root, error);
        std::cout << "project container ownership and store identity contract passed\n";
        return 0;
    } catch (const std::exception &exception) {
        std::filesystem::remove_all(root, error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
