#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "streamfind/catalogue.hpp"
#include "streamfind/core_operations.hpp"
#include "streamfind/dev/command_protocol.hpp"
#include "streamfind/mcp.hpp"
#include "streamfind/plugin_configuration.hpp"
#include "streamfind/plugin_registry.hpp"
#include "streamfind/service/service_discovery.hpp"
#include "streamfind/sdk/dynamic_plugin_manager.hpp"
#include "streamfind/sdk/plugin_data_service.hpp"
#include "streamfind/vendor_runtime.hpp"
#include "streamfind/version.hpp"

namespace streamfind::mcp::detail {

std::string compatibility_diagnostic(const std::filesystem::path &package_root) {
    const auto manifest_path = package_root / "plugin.json";
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input) return "plugin manifest is missing: " + manifest_path.string();
    std::ostringstream contents;
    contents << input.rdbuf();
    try {
        const auto manifest = streamfind::Json::parse(contents.str());
        const auto abi = manifest.at("abi_version");
        if (abi.at("major").get<int>() != STREAMFIND_PLUGIN_ABI_MAJOR)
            return "plugin ABI major is incompatible with this runtime";
        const auto sdk = manifest.at("sdk_compatibility");
        const auto current = std::string(streamfind::version());
        const auto minimum = sdk.at("minimum").get<std::string>();
        const auto maximum = sdk.at("maximum").get<std::string>();
        const auto parse = [](const std::string &value) {
            std::array<int, 3> result{0, 0, 0};
            std::size_t begin = 0;
            for (std::size_t index = 0; index < result.size(); ++index) {
                const auto end = value.find('.', begin);
                const auto part = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (part == "x" || part == "X") break;
                result[index] = std::stoi(part);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            return result;
        };
        const auto current_version = parse(current);
        const auto minimum_version = parse(minimum);
        const auto maximum_version = parse(maximum);
        if (current_version < minimum_version)
            return "runtime version is below plugin sdk_compatibility.minimum";
        if (maximum.find('x') == std::string::npos && maximum.find('X') == std::string::npos &&
            maximum_version < current_version)
            return "runtime version is above plugin sdk_compatibility.maximum";
    } catch (const std::exception &error) {
        return std::string("plugin compatibility metadata is invalid: ") + error.what();
    }
    return {};
}

void quarantine_plugin(PluginRegistry &registry, const std::string &plugin_id,
                       const std::filesystem::path &package_root, const std::string &diagnostic) {
    for (auto &entry : registry.installed) {
        if (entry.plugin_id == plugin_id && entry.package_path == package_root) {
            entry.active = false;
            entry.compatibility = "incompatible";
            entry.diagnostics = diagnostic;
        }
    }
}

std::filesystem::path executable_path(int argc, char **argv) {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) break;
        if (length < buffer.size() - 1)
            return std::filesystem::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length <= 0) break;
        if (static_cast<std::size_t>(length) < buffer.size())
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        buffer.resize(buffer.size() * 2);
    }
#endif
    if (argc > 0 && argv != nullptr && argv[0] != nullptr)
        return std::filesystem::absolute(std::filesystem::path(argv[0]));
    return std::filesystem::current_path() / "streamfind-mcp";
}

void ensure_local_service(const std::filesystem::path &mcp_executable) {
    const auto *endpoint = std::getenv("STREAMFIND_SERVICE_ENDPOINT");
    if (endpoint != nullptr && *endpoint != '\0') return;
#ifdef _WIN32
    const auto cli = mcp_executable.parent_path() / "streamfind.exe";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const auto null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("unable to prepare streamfind service startup");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_handle;
    startup.hStdOutput = null_handle;
    startup.hStdError = null_handle;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + cli.wstring() + L"\" service start";
    const auto launched = CreateProcessW(cli.c_str(), command.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW, nullptr,
                                         mcp_executable.parent_path().c_str(), &startup, &process);
    CloseHandle(null_handle);
    if (!launched) throw std::runtime_error("unable to start streamfind service manager");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        throw std::runtime_error("timed out waiting for streamfind service manager");
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0) throw std::runtime_error("streamfind service manager startup failed");
#elif defined(__linux__)
    const auto cli = mcp_executable.parent_path() / "streamfind";
    const auto child = fork();
    if (child < 0) throw std::runtime_error("unable to start streamfind service manager");
    if (child == 0) {
        const auto null_descriptor = open("/dev/null", O_RDWR);
        if (null_descriptor >= 0) {
            dup2(null_descriptor, STDIN_FILENO);
            dup2(null_descriptor, STDOUT_FILENO);
            dup2(null_descriptor, STDERR_FILENO);
            if (null_descriptor > STDERR_FILENO) close(null_descriptor);
        }
        execl(cli.c_str(), cli.c_str(), "service", "start", static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("streamfind service manager startup failed");
#else
    (void)mcp_executable;
#endif
}

namespace host_callbacks {

void report_error(const char *message, uint32_t size, void *) {
    if (message != nullptr && size != 0)
        std::cerr.write(message, static_cast<std::streamsize>(size)) << '\n';
}

void *allocate(uint64_t size, uint64_t, void *) {
    return std::malloc(static_cast<std::size_t>(size));
}

void deallocate(void *pointer, uint64_t, uint64_t, void *) {
    std::free(pointer);
}

}  // namespace host_callbacks

class DynamicPluginRuntime {
public:
    DynamicPluginRuntime() = default;
    DynamicPluginRuntime(const DynamicPluginRuntime &) = delete;
    DynamicPluginRuntime &operator=(const DynamicPluginRuntime &) = delete;

    void load_and_register(const std::filesystem::path &configuration_path,
                           MethodRegistry &methods,
                           OperationRegistry &operations) {
        const auto configuration = streamfind::load_plugin_configuration_file(configuration_path);
        if (!configuration.valid) throw std::runtime_error(configuration.diagnostics);
        const auto core_path = catalogue::find_core_path();
        if (!core_path) throw std::runtime_error("installed core catalogue not found");
        auto merged = catalogue::load_document(*core_path);
        if (!merged) throw std::runtime_error("installed core catalogue could not be loaded");
        core_operations::register_operations(merged->at("entries"), operations);
        std::optional<PluginRegistry> registry;
        if (!configuration.configuration.registry_path.empty()) {
            const auto loaded_registry = load_plugin_registry(configuration.configuration.registry_path);
            if (!loaded_registry.valid) throw std::runtime_error(loaded_registry.diagnostics);
            registry = loaded_registry.registry;
        }
        std::set<std::string> loaded_ids;
        for (const auto &plugin_id : configuration.configuration.enabled_plugins) {
            std::filesystem::path package_root;
            if (registry) {
                const auto active = find_active_plugin(*registry, plugin_id);
                if (!active) throw std::runtime_error("plugin is not active in the registry: " + plugin_id);
                package_root = active->package_path;
            } else {
                for (const auto &root : configuration.configuration.plugin_deployment_roots) {
                    const auto candidate = root / plugin_id;
                    if (std::filesystem::exists(candidate / "plugin.json")) {
                        package_root = candidate;
                        break;
                    }
                }
            }
            if (package_root.empty())
                throw std::runtime_error("enabled dynamic plugin package not found: " + plugin_id);
            const auto compatibility = compatibility_diagnostic(package_root);
            if (!compatibility.empty()) {
                if (registry) {
                    quarantine_plugin(*registry, plugin_id, package_root, compatibility);
                    registry->generation++;
                    write_plugin_registry_atomic(configuration.configuration.registry_path, *registry);
                }
                continue;
            }
            if (!loaded_ids.insert(plugin_id).second)
                throw std::runtime_error("duplicate enabled dynamic plugin: " + plugin_id);

            auto loaded = std::make_unique<LoadedPlugin>();
            loaded->package_root = package_root;
            loaded->plugin = sdk::load_dynamic_plugin_package(package_root);
            if (!loaded->plugin.loaded || loaded->plugin.manifest.plugin_id != plugin_id) {
                throw std::runtime_error("dynamic plugin load failed for " + plugin_id + ": " +
                                         loaded->plugin.diagnostics);
            }
            loaded->host.struct_size = sizeof(loaded->host);
            loaded->host.abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
            loaded->host.abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
            loaded->host.report_error = &host_callbacks::report_error;
            loaded->host.allocate = &host_callbacks::allocate;
            loaded->host.deallocate = &host_callbacks::deallocate;
            loaded->host.has_table = &sdk::plugin_has_table;
            loaded->host.read_batch = &sdk::plugin_read_batch;
            loaded->host.count_rows = &sdk::plugin_count_rows;
            loaded->host.emit_table_batch = &sdk::plugin_emit_table_batch;
            loaded->host.emit_result = &sdk::plugin_emit_result;
            loaded->host.report_progress = &sdk::plugin_report_progress;
            loaded->host.is_cancelled = &sdk::plugin_is_cancelled;
            if (sdk::register_dynamic_plugin(loaded->plugin, loaded->host) != STREAMFIND_PLUGIN_OK) {
                throw std::runtime_error("dynamic plugin registration failed for " + plugin_id + ": " +
                                         loaded->plugin.diagnostics);
            }

            const auto catalogue_path = package_root / loaded->plugin.manifest.semantic_catalogue;
            const auto plugin_document = catalogue::load_document(catalogue_path.string());
            if (!plugin_document)
                throw std::runtime_error("dynamic plugin catalogue could not be loaded for " + plugin_id);
            loaded->catalogue = std::move(*plugin_document);
            const auto &plugin = loaded->plugin.manifest.plugin;
            std::set<std::string> modules;
            for (const auto &entry : loaded->catalogue.at("entries")) {
                if (entry.value("plugin", "") == plugin && entry.contains("module_id"))
                    modules.insert(entry.at("module_id").get<std::string>());
            }
            if (modules.empty()) {
                const auto module_id = loaded->plugin.plugin.module_id == nullptr
                                           ? std::string{}
                                           : loaded->plugin.plugin.module_id;
                const auto expected_module = plugin + ".base";
                if (module_id != expected_module)
                    throw std::runtime_error("dynamic plugin catalogue has no owned modules for " + plugin_id);
                modules.insert(expected_module);
            }
            try {
                merged = catalogue::import_plugin_catalogue(
                    *merged, loaded->catalogue, plugin,
                    std::vector<std::string>(modules.begin(), modules.end()),
                    loaded->plugin.manifest.ontology_namespace);
            } catch (const std::exception &error) {
                throw std::runtime_error("dynamic catalogue merge failed for " + plugin_id + ": " + error.what());
            }
            plugins_.push_back(std::move(loaded));
        }
        catalogue::set_runtime_document(std::move(*merged));
        for (auto &loaded : plugins_) {
            sdk::register_dynamic_plugin_capabilities(
                loaded->plugin, loaded->catalogue.at("entries"), methods, operations);
        }
    }

    Json dependencies() const {
        Json result = Json::array();
        for (const auto &loaded : plugins_) {
            if (loaded->plugin.plugin.describe_dependencies == nullptr) continue;
            streamfind_plugin_buffer buffer{};
            if (loaded->plugin.plugin.describe_dependencies(&buffer, loaded->plugin.plugin.user_data) != STREAMFIND_PLUGIN_OK)
                throw std::runtime_error("plugin dependency discovery failed");
            const auto values = Json::parse(std::string(buffer.data, buffer.size));
            loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
            if (values.is_array()) for (const auto &value : values) result.push_back(value);
        }
        return result;
    }

    Json install_dependencies(const Json &request) const {
        Json result = Json::array();
        const auto text = request.dump();
        for (const auto &loaded : plugins_) {
            if (loaded->plugin.plugin.install_dependencies == nullptr) continue;
            streamfind_plugin_buffer buffer{};
            const auto status = loaded->plugin.plugin.install_dependencies(
                text.data(), static_cast<uint32_t>(text.size()), &buffer, loaded->plugin.plugin.user_data);
            if (status == STREAMFIND_PLUGIN_INVALID_ARGUMENT) continue;
            if (status != STREAMFIND_PLUGIN_OK) throw std::runtime_error("plugin dependency installation failed");
            const auto values = Json::parse(std::string(buffer.data, buffer.size));
            loaded->plugin.plugin.release_buffer(&buffer, loaded->plugin.plugin.user_data);
            if (values.contains("results") && values.at("results").is_array())
                for (const auto &value : values.at("results")) result.push_back(value);
        }
        return Json{{"results", result}};
    }

    std::vector<std::filesystem::path> workflow_demo_directories() const {
        std::vector<std::filesystem::path> directories;
        directories.reserve(plugins_.size());
        for (const auto &loaded : plugins_)
            directories.push_back(loaded->package_root / "resources" / "workflows");
        return directories;
    }

private:
    struct LoadedPlugin {
        std::filesystem::path package_root;
        sdk::DynamicPluginLoadResult plugin;
        streamfind_plugin_host_api host{};
        Json catalogue;
    };

    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
};

}  // namespace streamfind::mcp::detail

int main(int argc, char **argv) {
    streamfind::configure_vendor_runtime_paths();
    std::string line;
    streamfind::MethodRegistry registry;
    streamfind::OperationRegistry operations;
    std::unique_ptr<streamfind::mcp::detail::DynamicPluginRuntime> dynamic_plugins;
    std::filesystem::path executable;
    std::filesystem::path configuration_path;
    try {
        executable = streamfind::mcp::detail::executable_path(argc, argv);
        streamfind::service::ServiceDiscovery::configure_runtime_scope();
        configuration_path = executable.parent_path() / "streamfind.json";
        if (!std::filesystem::exists(configuration_path))
            throw std::runtime_error("streamfind.json is required for dynamic plugin loading");
        streamfind::mcp::detail::ensure_local_service(executable);
        dynamic_plugins = std::make_unique<streamfind::mcp::detail::DynamicPluginRuntime>();
        dynamic_plugins->load_and_register(configuration_path, registry, operations);
    } catch (const std::exception &error) {
        std::cerr << "streamfind-mcp: registration failed: " << error.what() << '\n';
        return 3;
    }
    const auto dev_executable = executable.parent_path() /
#ifdef _WIN32
        "streamfind-dev.exe";
#else
        "streamfind-dev";
#endif
    const auto development_invoker = [dev_executable](const std::string &tool,
                                                        const streamfind::Json &arguments,
                                                        const std::function<bool()> &cancel_requested = {}) {
        using streamfind::Json;
        std::vector<std::string> command;
        const auto required = [&arguments](const char *key) {
            if (!arguments.contains(key) || !arguments.at(key).is_string() ||
                arguments.at(key).get<std::string>().empty())
                throw std::invalid_argument(std::string("missing required development argument: ") + key);
            return arguments.at(key).get<std::string>();
        };
        const auto optional = [&arguments](const char *key) -> std::optional<std::string> {
            if (!arguments.contains(key)) return std::nullopt;
            if (!arguments.at(key).is_string() || arguments.at(key).get<std::string>().empty())
                throw std::invalid_argument(std::string("development argument must be a non-empty string: ") + key);
            return arguments.at(key).get<std::string>();
        };
        const auto add_optional = [&command, &optional](const char *key, const char *option) {
            if (const auto value = optional(key)) {
                command.push_back(option);
                command.push_back(*value);
            }
        };
        if (tool == "dev_doctor") {
            command = {"doctor", "--json"};
            add_optional("managed_home", "--managed-home");
            add_optional("workspace", "--workspace");
            add_optional("sdk", "--sdk");
            add_optional("package", "--package");
        } else if (tool == "dev_list") {
            command = {"list", "--json"};
            add_optional("target", "--target");
            add_optional("managed_home", "--managed-home");
        } else if (tool == "dev_inspect") {
            command = {"inspect", "--package", required("package"), "--json"};
            add_optional("target", "--target");
            add_optional("managed_home", "--managed-home");
        } else if (tool == "dev_plugin_create") {
            command = {"plugin", "create", required("name"), "--json"};
            add_optional("workspace", "--workspace");
        } else if (tool == "dev_plugin_add_operation") {
            command = {"plugin", "add-operation", required("name"), "--plugin", required("plugin"), "--json"};
        } else if (tool == "dev_plugin_build" || tool == "dev_plugin_test" ||
                   tool == "dev_plugin_validate" || tool == "dev_plugin_package") {
            const auto action = tool.substr(std::string("dev_plugin_").size());
            command = {"plugin", action, "--plugin", required("plugin"), "--sdk", required("sdk"), "--json"};
            add_optional("build", "--build");
        } else if (tool == "dev_plugin_rebuild") {
            command = {"rebuild", "--plugin", required("plugin"), "--sdk", required("sdk"), "--json"};
            add_optional("build", "--build");
        } else if (tool == "dev_plugin_install" || tool == "dev_viewer_install") {
            command = {tool.starts_with("dev_plugin_") ? "plugin" : "viewer", "install",
                       "--package", required("package"), "--json"};
            add_optional("managed_home", "--managed-home");
        } else if (tool == "dev_plugin_activate" || tool == "dev_viewer_activate") {
            command = {tool.starts_with("dev_plugin_") ? "plugin" : "viewer", "activate",
                       required("id"), "--json"};
            add_optional("version", "--version");
        } else if (tool == "dev_viewer_create") {
            command = {"viewer", "create", required("name"), "--contract", required("contract"), "--json"};
            add_optional("workspace", "--workspace");
        } else if (tool == "dev_viewer_build" || tool == "dev_viewer_test" ||
                   tool == "dev_viewer_validate" || tool == "dev_viewer_package") {
            const auto action = tool.substr(std::string("dev_viewer_").size());
            command = {"viewer", action, "--viewer", required("viewer"), "--json"};
        } else {
            throw std::invalid_argument("unknown development MCP tool: " + tool);
        }
        const auto result = streamfind::dev::run_process(
            dev_executable, command, std::filesystem::current_path(), 3600000,
            1024 * 1024, std::nullopt, false, {}, cancel_requested);
        const auto audit_directory = std::filesystem::current_path() / "tmp" / "logs";
        std::error_code audit_error;
        std::filesystem::create_directories(audit_directory, audit_error);
        if (!audit_error) {
            std::ofstream audit(audit_directory / "mcp-development-audit.jsonl", std::ios::app);
            if (audit) {
                audit << Json{{"tool", tool}, {"arguments", arguments}, {"command", command},
                              {"exit_code", result.exit_code}, {"timed_out", result.timed_out},
                              {"cancelled", result.cancelled}, {"output_truncated", result.output_truncated}}
                                  .dump()
                       << '\n';
            }
        }
        return Json{{"tool", tool}, {"command", command}, {"exit_code", result.exit_code},
                    {"timed_out", result.timed_out}, {"cancelled", result.cancelled},
                    {"output_truncated", result.output_truncated}, {"output", result.output}};
    };
    struct DevelopmentTask {
        std::mutex mutex;
        std::string status{"queued"};
        streamfind::Json result = streamfind::Json::object();
        bool cancel_requested{false};
    };
    const auto development_tasks = std::make_shared<std::map<std::string, std::shared_ptr<DevelopmentTask>>>();
    const auto development_tasks_mutex = std::make_shared<std::mutex>();
    const auto development_task_sequence = std::make_shared<std::atomic_uint64_t>(0);
    const auto task_aware_invoker = [development_invoker, development_tasks, development_tasks_mutex,
                                     development_task_sequence](const std::string &tool,
                                                                const streamfind::Json &arguments) {
        using streamfind::Json;
        if (tool == "dev_task_status" || tool == "dev_task_cancel") {
            const auto task_id = arguments.value("task_id", "");
            std::shared_ptr<DevelopmentTask> task;
            {
                std::lock_guard lock(*development_tasks_mutex);
                const auto found = development_tasks->find(task_id);
                if (found == development_tasks->end()) throw std::invalid_argument("development task was not found: " + task_id);
                task = found->second;
            }
            std::lock_guard lock(task->mutex);
            if (tool == "dev_task_cancel" && (task->status == "queued" || task->status == "running")) {
                task->cancel_requested = true;
                task->status = "cancelling";
            }
            return Json{{"task_id", task_id}, {"status", task->status},
                        {"cancel_requested", task->cancel_requested}, {"result", task->result}};
        }
        if (!arguments.value("async", false)) return development_invoker(tool, arguments);
        const auto task_id = "dev-task-" + std::to_string(++*development_task_sequence);
        const auto task = std::make_shared<DevelopmentTask>();
        {
            std::lock_guard lock(*development_tasks_mutex);
            (*development_tasks)[task_id] = task;
        }
        std::thread([task, task_id, tool, arguments, development_invoker] {
            {
                std::lock_guard lock(task->mutex);
                task->status = "running";
            }
            try {
                auto result = development_invoker(tool, arguments, [task] {
                    std::lock_guard lock(task->mutex);
                    return task->cancel_requested;
                });
                std::lock_guard lock(task->mutex);
                task->result = std::move(result);
                task->status = task->result.value("cancelled", false) ? "cancelled" :
                    (task->result.value("exit_code", 1) == 0 ? "completed" : "failed");
            } catch (const std::exception &error) {
                std::lock_guard lock(task->mutex);
                task->result = Json{{"error", error.what()}};
                task->status = "failed";
            }
        }).detach();
        return Json{{"task_id", task_id}, {"status", "queued"},
                    {"message", "Development command started; poll dev_task_status."}};
    };
    streamfind::mcp::Session session(
        operations,
        [&dynamic_plugins] { return dynamic_plugins->dependencies(); },
        [&dynamic_plugins](const streamfind::Json &request) { return dynamic_plugins->install_dependencies(request); },
        executable.parent_path() /
#ifdef _WIN32
            "streamfind-worker.exe",
#else
            "streamfind-worker",
#endif
        configuration_path,
        [&dynamic_plugins] { return dynamic_plugins->workflow_demo_directories(); },
        task_aware_invoker);
    while (std::getline(std::cin, line)) {
            try {
                const auto request = streamfind::Json::parse(line);
                const auto response = session.handle(request);
                if (!response.is_null() && request.contains("id"))
                    std::cout << response.dump() << '\n' << std::flush;
            }
            catch (const std::exception &error) { std::cout << streamfind::Json{{"jsonrpc", "2.0"}, {"error", {{"code", -32700}, {"message", error.what()}}}}.dump() << '\n' << std::flush; }
        }
}
