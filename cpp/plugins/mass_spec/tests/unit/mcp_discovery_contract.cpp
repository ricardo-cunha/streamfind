#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "streamfind/mcp.hpp"
#include "streamfind/service_runtime_client.hpp"
#include "../../../../core/tests/support/tmp_projects.hpp"

namespace streamfind::mcp_test_detail {

streamfind::Json call(streamfind::mcp::Session &session, int id, const std::string &name,
                      streamfind::Json arguments = streamfind::Json::object()) {
    return session.handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                           {"params", {{"name", name}, {"arguments", arguments}}}});
}

streamfind::Json text_json(const streamfind::Json &response) {
    const auto text = response.at("result").at("content").at(0).at("text").get<std::string>();
    return streamfind::Json::parse(text);
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class ServiceGuard {
public:
    explicit ServiceGuard(const std::filesystem::path &executable)
        : runtime_root_(streamfind::test::tmp_projects_dir() / "mcp-discovery-service-runtime") {
        std::error_code error;
        std::filesystem::remove_all(runtime_root_, error);
        std::filesystem::create_directories(runtime_root_, error);
        if (error) throw std::runtime_error("cannot create isolated service runtime: " + error.message());
        save_and_set("STREAMFIND_SERVICE_ENDPOINT", "http://127.0.0.1:18787");
        save_and_set("STREAMFIND_RUNTIME_DIR", (runtime_root_ / "runtime").string());
#ifdef _WIN32
        save_and_set("LOCALAPPDATA", runtime_root_.string());
#else
        save_and_set("XDG_RUNTIME_DIR", runtime_root_.string());
#endif
        start(executable);
        wait_until_ready();
    }

    ServiceGuard(const ServiceGuard &) = delete;
    ServiceGuard &operator=(const ServiceGuard &) = delete;

    ~ServiceGuard() {
        stop();
        restore("STREAMFIND_SERVICE_ENDPOINT");
        restore("STREAMFIND_RUNTIME_DIR");
#ifdef _WIN32
        restore("LOCALAPPDATA");
#else
        restore("XDG_RUNTIME_DIR");
#endif
        std::error_code error;
        std::filesystem::remove_all(runtime_root_, error);
    }

private:
    struct EnvironmentValue {
        std::string name;
        std::string value;
        bool present;
    };

    void save_and_set(const char *name, const std::string &value) {
        const auto *existing = std::getenv(name);
        environment_.push_back({name, existing == nullptr ? std::string{} : std::string(existing), existing != nullptr});
#ifdef _WIN32
        if (_putenv_s(name, value.c_str()) != 0)
#else
        if (setenv(name, value.c_str(), 1) != 0)
#endif
            throw std::runtime_error(std::string("cannot set test environment variable: ") + name);
    }

    void restore(const char *name) {
        for (auto iterator = environment_.rbegin(); iterator != environment_.rend(); ++iterator) {
            if (iterator->name != name) continue;
#ifdef _WIN32
            _putenv_s(name, iterator->present ? iterator->value.c_str() : "");
#else
            if (iterator->present) setenv(name, iterator->value.c_str(), 1);
            else unsetenv(name);
#endif
            return;
        }
    }

    void start(const std::filesystem::path &executable) {
#ifdef _WIN32
        std::wstring command = L"\"" + executable.wstring() + L"\" 18787";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process))
            throw std::runtime_error("cannot start service test process");
        CloseHandle(process.hThread);
        process_handle_ = process.hProcess;
#else
        process_id_ = fork();
        if (process_id_ < 0) throw std::runtime_error("cannot start service test process");
        if (process_id_ == 0) {
            const auto port = std::string("18787");
            execl(executable.c_str(), executable.c_str(), port.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
#endif
    }

    void wait_until_ready() {
        constexpr auto attempts = 100;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            try {
                for (const auto &service : streamfind::service::list_services()) {
                    if (service.value("endpoint", std::string{}) == "http://127.0.0.1:18787" &&
                        service.value("health", std::string{}) == "healthy")
                        return;
                }
            } catch (...) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw std::runtime_error("service test process did not become healthy at http://127.0.0.1:18787");
    }

    void stop() noexcept {
#ifdef _WIN32
        if (process_handle_ == nullptr) return;
        TerminateProcess(process_handle_, 0);
        WaitForSingleObject(process_handle_, 2000);
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
#else
        if (process_id_ <= 0) return;
        kill(process_id_, SIGTERM);
        int status = 0;
        waitpid(process_id_, &status, 0);
        process_id_ = -1;
#endif
    }

    std::filesystem::path runtime_root_;
    std::vector<EnvironmentValue> environment_;
#ifdef _WIN32
    HANDLE process_handle_{nullptr};
#else
    pid_t process_id_{-1};
#endif
};

} // namespace streamfind::mcp_test_detail

int main() {
    try {
        using namespace streamfind::mcp_test_detail;
        const ServiceGuard service_guard(STREAMFIND_TEST_SERVICE_EXECUTABLE);
        streamfind::mcp::Session session;
        const auto initialized = session.handle({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}});
        require(!initialized.at("result").at("capabilities").contains("resources"),
                "MCP still advertises HTML visualization resources");
        const auto instructions = initialized.at("result").value("instructions", std::string{});
        require(instructions.find("streamfind web app") != std::string::npos &&
                    instructions.find("result") != std::string::npos,
                "MCP initialize guidance does not direct rich visualization to the web app results");
        require(session.handle({{"jsonrpc", "2.0"}, {"method", "initialized"}}).is_null(),
                "MCP initialized notification produced a response");
        const auto resources = session.handle({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "resources/list"}});
        require(resources.contains("error") && resources.at("error").value("code", 0) == -32601,
                "MCP still serves the removed visualization resource surface");
        const auto listed = session.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
        const auto tools = listed.at("result").at("tools");
        bool has_plugins = false;
        bool has_modules = false;
        bool has_operations = false;
        bool has_operation = false;
        bool has_run_operation = false;
        bool has_create = false;
        bool has_add_operation = false;
        bool has_connect_operations = false;
        bool has_set_workflow = false;
        bool has_request_result = false;
        bool has_current_artifact_inventory = false;
        bool has_workflow_demos = false;
        for (const auto &tool : tools) {
            const auto name = tool.value("name", tool.value("canonical_id", ""));
            require(name.rfind("mass_spec.", 0) != 0, "operation-specific MCP tool leaked into tools/list");
            require(name != "add_method" && name != "remove_method" && name != "get_available_methods" &&
                        name != "run_method",
                    "legacy workflow-method MCP tool leaked into tools/list");
            has_plugins = has_plugins || name == "get_plugins";
            has_modules = has_modules || name == "get_modules";
            has_operations = has_operations || name == "get_operations";
            has_operation = has_operation || name == "get_operation";
            has_run_operation = has_run_operation || name == "run_operation";
            has_create = has_create || name == "create";
            has_add_operation = has_add_operation || name == "add_operation";
            has_connect_operations = has_connect_operations || name == "connect_operations";
            has_set_workflow = has_set_workflow || name == "set_workflow";
            has_request_result = has_request_result || name == "request_result";
            has_current_artifact_inventory = has_current_artifact_inventory || name == "get_current_result_inventory";
            has_workflow_demos = has_workflow_demos || name == "list_workflow_demos";
        }
        require(has_plugins && has_modules && has_operations && has_operation && has_run_operation,
                "stable MCP discovery tools are incomplete");
        require(has_create && has_add_operation && has_connect_operations && has_set_workflow,
                "operation-graph MCP tools are incomplete");
        require(has_request_result, "result request MCP tool is missing");
        require(has_current_artifact_inventory, "current artifact inventory MCP tool is missing");
        require(has_workflow_demos, "workflow demo discovery MCP tool is missing");
        for (const auto &tool : tools) {
            const auto name = tool.value("name", tool.value("canonical_id", ""));
            if (name == "create")
                require(tool.at("inputSchema").at("required") == streamfind::Json::array({"project_path"}),
                        "create schema does not require project_path");
            if (name == "set_workflow")
                require(tool.at("inputSchema").at("required") ==
                            streamfind::Json::array({"project_path", "workflow"}) &&
                            tool.at("inputSchema").at("properties").contains("workflow"),
                        "set_workflow schema does not advertise workflow");
            if (name == "validate_workflow")
                require(tool.at("inputSchema").at("required") == streamfind::Json::array({"project_path"}),
                        "validate_workflow should allow validation of the saved workflow");
            if (name == "request_result") {
                const auto &properties = tool.at("inputSchema").at("properties");
                require(properties.contains("result_id") && properties.contains("operation_instance") &&
                            properties.contains("include_data") && properties.contains("limit") &&
                            properties.contains("offset"),
                        "request_result schema does not advertise targeted data selection");
                require(properties.at("limit").is_object() && properties.at("limit").value("type", "") == "integer" &&
                            properties.at("limit").value("minimum", 0) == 1 &&
                            properties.at("limit").value("maximum", 0) == 10000,
                        "request_result limit schema is not a valid bounded integer schema");
                require(properties.at("offset").is_object() && properties.at("offset").value("type", "") == "integer" &&
                            properties.at("offset").value("minimum", -1) == 0,
                        "request_result offset schema is not a valid non-negative integer schema");
            }
        }

        const auto metadata_path = streamfind::test::tmp_projects_dir() / "mcp-workflow-metadata-contract.sf";
        std::filesystem::remove_all(metadata_path);
        const auto created = call(session, 6, "create", {{"project_path", metadata_path.string()}});
        require(!created.at("result").value("isError", false),
                (std::string("MCP create rejected metadata-optional workflow creation: ") +
                 created.dump()).c_str());
        const auto created_workflow = text_json(call(session, 61, "get_workflow", {{"project_path", metadata_path.string()}}));
        require(created_workflow.at("metadata") ==
                             streamfind::Json({{"name", "Untitled workflow"},
                                                {"description", "Describe the purpose of this workflow."}}),
                "new workflow did not receive required metadata defaults");
        auto replacement_workflow = created_workflow;
        replacement_workflow["metadata"] = {{"name", "MCP workflow"},
                                             {"description", "MCP contract test workflow"},
                                             {"owner", "mcp"}, {"purpose", "contract test"}};
        const auto updated = call(session, 7, "set_workflow",
                                  {{"project_path", metadata_path.string()},
                                   {"workflow", replacement_workflow}});
        require(!updated.at("result").value("isError", false), "MCP workflow metadata update failed");
        const auto saved_metadata = text_json(call(session, 8, "get_workflow", {{"project_path", metadata_path.string()}}));
        require(saved_metadata.at("metadata") ==
                             streamfind::Json({{"name", "MCP workflow"}, {"description", "MCP contract test workflow"},
                                                {"owner", "mcp"}, {"purpose", "contract test"}}),
                "MCP workflow metadata update was not persisted");
        const auto closed = call(session, 9, "close", {{"project_path", metadata_path.string()}});
        require(!closed.at("result").value("isError", false), "MCP could not close the container test project");
        std::filesystem::remove_all(metadata_path);

        const auto plugins = text_json(call(session, 2, "get_plugins"));
        require(plugins.is_array() && std::find(plugins.begin(), plugins.end(), "mass_spec") != plugins.end(),
                "mass_spec was not discoverable");

        const auto operations = text_json(call(session, 3, "get_operations",
                                                {{"plugin", "mass_spec"}, {"module", "mass_spec.nta"}}));
        require(operations.is_array() && !operations.empty(), "mass_spec.nta operations were not discoverable");
        const auto first = operations.at(0);
        const std::set<std::string> summary_fields = {"canonical_id", "label", "plugin", "module_id", "definition"};
        require(first.size() == summary_fields.size(), "operation summary has unexpected fields");
        for (const auto &field : summary_fields)
            require(first.contains(field) && first.at(field).is_string(), "operation summary field is missing or not a string");
        require(first.value("canonical_id", "").rfind("mass_spec.", 0) == 0 &&
                    first.value("module_id", "") == "mass_spec.nta",
                "filtered operation summary is not canonical");
        require(!first.contains("parameters") && !first.contains("input_ports") && !first.contains("executable"),
                "operation summary leaked full catalogue detail");
        for (const auto &operation : operations)
            require(operation.value("plugin", "") == "mass_spec" &&
                        operation.value("module_id", "") == "mass_spec.nta",
                    "operation filtering returned the wrong module");
        for (const auto &operation : operations)
            require(!operation.contains("cacheable"),
                    "operation catalogue still exposes the removed cacheable flag");

        const auto searched = text_json(call(session, 4, "get_operations",
                                             {{"plugin", "mass_spec"}, {"search", "nta"}}));
        require(searched.is_array() && !searched.empty() && searched.size() <= operations.size(),
                "operation search returned an invalid result");
        for (const auto &operation : searched)
            require(operation.value("canonical_id", "").find("nta") != std::string::npos ||
                        operation.value("label", "").find("nta") != std::string::npos ||
                        operation.value("definition", "").find("nta") != std::string::npos ||
                        operation.value("module_id", "").find("nta") != std::string::npos,
                    "operation search returned a non-matching summary");

        const auto detail = text_json(call(session, 5, "get_operation",
                                           {{"operation", first.at("canonical_id")}}));
        require(detail.is_array() && detail.size() == 1, "operation detail lookup returned the wrong shape");
        require(detail.at(0).value("canonical_id", "") == first.at("canonical_id").get<std::string>(),
                "operation detail does not match the requested operation");
        require(detail.at(0).value("exposed", false) && detail.at(0).value("executable", false),
                "operation detail is not an exposed executable operation");
        require(detail.at(0).contains("input_ports") || detail.at(0).contains("parameters"),
                "operation detail omitted its schema");

        const auto missing = call(session, 5, "get_operation", {{"operation", "mass_spec.no_such_operation"}});
        require(missing.contains("result") && missing.at("result").value("isError", false),
                "unknown operation did not return an MCP error result");
        std::cout << "MCP discovery contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
