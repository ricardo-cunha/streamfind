#include "streamfind/dev/command_protocol.hpp"
#include "streamfind/plugin_registry.hpp"
#include "streamfind/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace streamfind::dev {
namespace detail {

using Json = nlohmann::json;
constexpr std::size_t max_output = 1024 * 1024;
volatile std::int32_t cancellation_requested = 0;

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        cancellation_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) cancellation_requested = 1;
}
#endif

[[noreturn]] void invalid(const std::string &message) {
    throw std::invalid_argument(message);
}

std::string require_value(int &index, int argc, char **argv, const std::string &option) {
    if (index + 1 >= argc) invalid("missing value for " + option);
    return argv[++index];
}

bool is_option(const std::string &value, const char *name) { return value == name; }

std::filesystem::path home_path() {
#ifdef _WIN32
    if (const auto *value = std::getenv("USERPROFILE"); value && *value) return value;
#else
    if (const auto *value = std::getenv("HOME"); value && *value) return value;
#endif
    throw std::runtime_error("unable to resolve the current user's home directory");
}

bool is_within(const std::filesystem::path &value, const std::filesystem::path &root) {
    auto value_it = value.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++value_it) {
        if (value_it == value.end() || *value_it != *root_it) return false;
    }
    return true;
}

std::filesystem::path normalized(const std::filesystem::path &value) {
    return std::filesystem::absolute(value).lexically_normal();
}

std::string json_string(const std::optional<std::filesystem::path> &value) {
    return value ? value->string() : std::string{};
}

std::vector<std::string> isolated_environment() {
    const char *names[] = {"PATH", "HOME", "USERPROFILE", "TEMP", "TMP", "TMPDIR",
                           "SystemRoot", "ComSpec", "STREAMFIND_HOME", "STREAMFIND_MSYS2_ROOT"};
    std::vector<std::string> values;
    for (const auto *name : names) {
        if (const auto *value = std::getenv(name); value && *value)
            values.emplace_back(std::string(name) + "=" + value);
    }
    return values;
}

} // namespace detail

Request parse_request(int argc, char **argv) {
    if (argc < 2) throw std::invalid_argument("a developer command is required");
    Request request;
    request.command = argv[1];
    if (request.command == "--version") return request;
    if (request.command != "doctor" && request.command != "setup" && request.command != "run" &&
        request.command != "rebuild" &&
        request.command != "plugin" && request.command != "viewer" && request.command != "list" &&
        request.command != "inspect")
        throw std::invalid_argument("unknown developer command: " + request.command);
    if (request.command == "plugin" || request.command == "viewer") {
        if (argc < 3) throw std::invalid_argument(request.command + " requires an action");
        request.plugin_action = argv[2];
    }
    bool after_separator = false;
    const bool plugin_positional = request.command == "plugin" &&
        (request.plugin_action == "create" || request.plugin_action == "add-operation" || request.plugin_action == "uninstall" || request.plugin_action == "deactivate" || request.plugin_action == "activate");
    const bool viewer_positional = request.command == "viewer" &&
        (request.plugin_action == "create" || request.plugin_action == "activate" || request.plugin_action == "deactivate" || request.plugin_action == "uninstall");
    const int first_option = plugin_positional || viewer_positional ? 4 :
        ((request.command == "plugin" || request.command == "viewer") ? 3 : 2);
    for (int index = first_option; index < argc; ++index) {
        const std::string value = argv[index];
        if (after_separator) {
            request.program_arguments.push_back(value);
            continue;
        }
        if (value == "--") { after_separator = true; continue; }
        if (value == "--json") { request.json = true; continue; }
        if (value == "--offline") { request.offline = true; continue; }
        if (value == "--dry-run") { request.dry_run = true; continue; }
        if (value == "--workspace") { request.workspace = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--managed-home") { request.managed_home = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--sdk") { request.sdk = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--package") { request.package = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--viewer") { request.viewer = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--plugin") { request.plugin_path = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--build") { request.build_path = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--program") { request.program = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--cwd") { request.working_directory = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--output") { request.output_file = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--requirements") { request.requirements = detail::normalized(detail::require_value(index, argc, argv, value)); continue; }
        if (value == "--target") {
            request.target = detail::require_value(index, argc, argv, value);
            if (request.target != "native" && request.target != "viewer" && request.target != "python" && request.target != "all")
                throw std::invalid_argument("--target must be native, viewer, python, or all");
            continue;
        }
        if (value == "--timeout-ms") {
            const auto text = detail::require_value(index, argc, argv, value);
            std::size_t consumed = 0;
            request.timeout_ms = std::stoull(text, &consumed);
            if (consumed != text.size() || request.timeout_ms == 0 || request.timeout_ms > 3600000)
                throw std::invalid_argument("--timeout-ms must be between 1 and 3600000");
            continue;
        }
        if (value == "--config") { request.config = detail::require_value(index, argc, argv, value); continue; }
        if (value == "--version") { request.version = detail::require_value(index, argc, argv, value); continue; }
        if (value == "--contract") { request.contract = detail::require_value(index, argc, argv, value); continue; }
        throw std::invalid_argument("unknown developer option: " + value);
    }
    if (request.command == "plugin") {
        if (request.plugin_action == "create") {
            if (argc < 4 || std::string(argv[3]).starts_with("--"))
                throw std::invalid_argument("plugin create requires a plugin name");
            request.name = argv[3];
        } else if (request.plugin_action == "add-operation") {
            if (argc < 4 || std::string(argv[3]).starts_with("--"))
                throw std::invalid_argument("plugin add-operation requires an operation name");
            request.name = argv[3];
            if (!request.plugin_path) throw std::invalid_argument("plugin add-operation requires --plugin PATH");
        } else if (request.plugin_action == "build" || request.plugin_action == "test" ||
                   request.plugin_action == "validate" || request.plugin_action == "package") {
            if (!request.plugin_path) throw std::invalid_argument("plugin " + request.plugin_action + " requires --plugin PATH");
            if ((request.plugin_action == "build" || request.plugin_action == "test" || request.plugin_action == "validate" || request.plugin_action == "package") && !request.sdk)
                throw std::invalid_argument("plugin " + request.plugin_action + " requires --sdk PATH");
        } else if (request.plugin_action == "install") {
            if (!request.package) throw std::invalid_argument("plugin install requires --package PATH");
        } else if (request.plugin_action == "uninstall" || request.plugin_action == "deactivate" || request.plugin_action == "activate") {
            if (argc < 4 || std::string(argv[3]).starts_with("--"))
                throw std::invalid_argument("plugin uninstall requires a plugin id");
            request.name = argv[3];
        } else throw std::invalid_argument("plugin action must be create, add-operation, build, test, validate, package, install, activate, deactivate or uninstall");
    }
    if (request.command == "viewer") {
        if (request.plugin_action == "create") {
            if (argc < 4 || std::string(argv[3]).starts_with("--"))
                throw std::invalid_argument("viewer create requires a viewer name");
            request.name = argv[3];
            if (request.contract.empty()) throw std::invalid_argument("viewer create requires --contract IRI");
        } else if (request.plugin_action == "build" || request.plugin_action == "test" ||
                   request.plugin_action == "validate" || request.plugin_action == "package") {
            if (!request.viewer) throw std::invalid_argument("viewer " + request.plugin_action + " requires --viewer PATH");
        } else if (request.plugin_action == "install") {
            if (!request.package) throw std::invalid_argument("viewer install requires --package PATH");
        } else if (request.plugin_action == "activate" || request.plugin_action == "deactivate" || request.plugin_action == "uninstall") {
            if (argc < 4 || std::string(argv[3]).starts_with("--"))
                throw std::invalid_argument("viewer action requires a viewer ID");
            request.name = argv[3];
        } else throw std::invalid_argument("viewer action must be create, build, test, validate, package, install, activate, deactivate or uninstall");
    }
    if (request.command == "rebuild") {
        if (!request.plugin_path) throw std::invalid_argument("rebuild requires --plugin PATH");
        if (!request.sdk) throw std::invalid_argument("rebuild requires --sdk PATH");
    }
    if (request.requirements && request.command != "setup")
        throw std::invalid_argument("--requirements is only valid with setup");
    if (request.command == "run" && !request.program) throw std::invalid_argument("run requires --program PATH");
    if (request.command == "inspect" && !request.package)
        throw std::invalid_argument("inspect requires --package PATH");
    if (request.command != "run" && !request.program_arguments.empty())
        throw std::invalid_argument("positional arguments are only valid after -- for run");
    return request;
}

std::filesystem::path resolve_managed_home() {
    if (const auto *value = std::getenv("STREAMFIND_HOME"); value && *value)
        return detail::normalized(value);
    return detail::normalized(detail::home_path() / ".streamfind");
}

std::filesystem::path resolve_workspace(const std::optional<std::filesystem::path> &requested,
                                        const std::filesystem::path &managed_home) {
    if (requested) return detail::normalized(*requested);
    const auto preference = managed_home / "config" / "development.json";
    if (std::filesystem::exists(preference)) {
        try {
            const auto json = nlohmann::json::parse(std::ifstream(preference));
            if (json.contains("workspace") && json["workspace"].is_string())
                return detail::normalized(json["workspace"].get<std::string>());
        } catch (...) { }
    }
#ifdef _WIN32
    if (const auto *value = std::getenv("USERPROFILE"); value && *value)
        return detail::normalized(std::filesystem::path(value) / "Documents" / "streamfind-dev");
#endif
    return detail::normalized(detail::home_path() / "Documents" / "streamfind-dev");
}

std::filesystem::path canonicalize_user_path(const std::filesystem::path &value, bool must_exist,
                                              const std::filesystem::path &managed_home,
                                              const std::filesystem::path &workspace) {
    const auto candidate = detail::normalized(value);
    if (must_exist && !std::filesystem::exists(candidate))
        throw PathDeniedError("path does not exist: " + candidate.string());
    const auto existing = must_exist ? candidate : detail::normalized(candidate.parent_path());
    const auto user_home = detail::normalized(detail::home_path());
    if (!detail::is_within(existing, user_home) && !detail::is_within(existing, managed_home) &&
        !detail::is_within(existing, workspace))
        throw PathDeniedError("path is outside approved user/workspace roots: " + candidate.string());
    return candidate;
}

std::string doctor_json(const Request &request) {
    const auto managed_home = request.managed_home.value_or(resolve_managed_home());
    const auto workspace = resolve_workspace(request.workspace, managed_home);
    const auto home = detail::home_path();
    nlohmann::json result = {
        {"schema_version", 1}, {"command", "streamfind-dev"}, {"workspace", workspace.string()},
        {"managed_home", managed_home.string()}, {"user_home", home.string()},
        {"sdk", detail::json_string(request.sdk)}, {"package", detail::json_string(request.package)},
        {"protocol", "streamfind-dev/1"},
        {"provisioner", "dev/streamfind-dev-setup.ps1"},
        {"runtime_version", std::string(streamfind::version())},
        {"registry", nlohmann::json::object()},
        {"compatibility_findings", nlohmann::json::array()}
    };
    const auto registry_path = managed_home / "extensions" / "registry.json";
    result["registry"]["path"] = registry_path.string();
    if (std::filesystem::is_regular_file(registry_path)) {
        const auto loaded = streamfind::load_plugin_registry(registry_path);
        if (!loaded.valid) {
            result["registry"]["valid"] = false;
            result["registry"]["diagnostics"] = loaded.diagnostics;
        } else {
            result["registry"]["valid"] = true;
            result["registry"]["generation"] = loaded.registry.generation;
            for (const auto &entry : loaded.registry.installed) {
                const auto source_available = entry.source_path.empty() || std::filesystem::is_directory(entry.source_path);
                result["registry"]["installed"].push_back({
                    {"plugin_id", entry.plugin_id}, {"version", entry.version},
                    {"platform", entry.platform}, {"package_path", entry.package_path.string()},
                    {"source_path", entry.source_path.string()}, {"active", entry.active},
                    {"source_available", source_available}, {"compatibility", entry.compatibility},
                    {"diagnostics", entry.diagnostics}});
                if (entry.compatibility != "compatible")
                    result["compatibility_findings"].push_back({
                        {"plugin_id", entry.plugin_id}, {"version", entry.version},
                        {"package_path", entry.package_path.string()}, {"source_path", entry.source_path.string()},
                        {"source_available", source_available}, {"status", entry.compatibility},
                        {"diagnostics", entry.diagnostics}});
                else if (!source_available)
                    result["compatibility_findings"].push_back({
                        {"plugin_id", entry.plugin_id}, {"version", entry.version},
                        {"source_path", entry.source_path.string()}, {"source_available", false},
                        {"status", "source_missing"}, {"diagnostics", "retained source workspace is unavailable"}});
            }
        }
    } else {
        result["registry"]["valid"] = true;
        result["registry"]["installed"] = nlohmann::json::array();
    }
    return result.dump(2);
}

#ifdef _WIN32
namespace detail_process {
std::wstring quote_windows(const std::wstring &value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') { result.append(slashes * 2 + 1, L'\\'); result += L'"'; slashes = 0; continue; }
        result.append(slashes, L'\\'); slashes = 0; result += character;
    }
    result.append(slashes * 2, L'\\'); result += L'"';
    return result;
}
}
#endif

void install_cancellation_handler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(detail::console_handler, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = detail::signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

ProcessResult run_process(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                          const std::filesystem::path &working_directory, std::size_t timeout_ms,
                          std::size_t output_limit, const std::optional<std::filesystem::path> &output_file,
                          bool isolate_environment, const std::vector<std::string> &environment_overrides,
                          const std::function<bool()> &cancel_requested) {
    if (program.has_parent_path() && !std::filesystem::is_regular_file(program))
        throw std::runtime_error("program is not a file: " + program.string());
    if (!std::filesystem::is_directory(working_directory)) throw std::runtime_error("working directory is not a directory: " + working_directory.string());
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_handle = nullptr, write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0)) throw std::runtime_error("unable to create process output pipe");
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = detail_process::quote_windows(program.wstring());
    for (const auto &argument : arguments) command += L" " + detail_process::quote_windows(std::filesystem::path(argument).wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = write_handle; startup.hStdError = write_handle; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment;
    if (isolate_environment) {
        auto values = detail::isolated_environment();
        values.insert(values.end(), environment_overrides.begin(), environment_overrides.end());
        for (const auto &entry : values) {
            const auto wide = std::filesystem::path(entry).wstring();
            environment.insert(environment.end(), wide.begin(), wide.end());
            environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
    }
    auto command_buffer = std::vector<wchar_t>(command.begin(), command.end()); command_buffer.push_back(L'\0');
    const auto creation_flags = CREATE_NO_WINDOW | (isolate_environment ? CREATE_UNICODE_ENVIRONMENT : 0);
    if (!CreateProcessW(program.wstring().c_str(), command_buffer.data(), nullptr, nullptr, TRUE, creation_flags,
                        isolate_environment ? environment.data() : nullptr,
                        working_directory.wstring().c_str(), &startup, &process)) { CloseHandle(read_handle); CloseHandle(write_handle); throw std::runtime_error("unable to start program: " + program.string()); }
    CloseHandle(write_handle);
    ProcessResult result;
    const auto start = GetTickCount64();
    std::ofstream file; if (output_file) { file.open(*output_file, std::ios::binary); if (!file) throw std::runtime_error("unable to open output file: " + output_file->string()); }
    char buffer[4096]; DWORD available = 0, read = 0;
    while (true) {
        while (PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr) && available) {
            if (!ReadFile(read_handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
            const auto accepted = std::min<std::size_t>(read, output_limit > result.output.size() ? output_limit - result.output.size() : 0);
            result.output.append(buffer, accepted); if (file) file.write(buffer, static_cast<std::streamsize>(read));
            if (accepted < read) result.output_truncated = true;
        }
        const auto status = WaitForSingleObject(process.hProcess, 25);
        if (detail::cancellation_requested || (cancel_requested && cancel_requested())) { TerminateProcess(process.hProcess, 130); result.cancelled = true; break; }
        if (status == WAIT_OBJECT_0) break;
        if (GetTickCount64() - start > timeout_ms) { TerminateProcess(process.hProcess, 124); result.timed_out = true; break; }
    }
    while (ReadFile(read_handle, buffer, sizeof(buffer), &read, nullptr) && read) { const auto accepted = std::min<std::size_t>(read, output_limit > result.output.size() ? output_limit - result.output.size() : 0); result.output.append(buffer, accepted); if (accepted < read) result.output_truncated = true; }
    DWORD exit_status = 1; GetExitCodeProcess(process.hProcess, &exit_status); result.exit_code = static_cast<int>(exit_status);
    CloseHandle(process.hProcess); CloseHandle(process.hThread); CloseHandle(read_handle); return result;
#else
    int pipe_fds[2]; if (pipe(pipe_fds) != 0) throw std::runtime_error("unable to create process output pipe");
    const auto pid = fork();
    if (pid < 0) { close(pipe_fds[0]); close(pipe_fds[1]); throw std::runtime_error("unable to fork process"); }
    auto environment = isolate_environment ? detail::isolated_environment() : std::vector<std::string>{};
    environment.insert(environment.end(), environment_overrides.begin(), environment_overrides.end());
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO); dup2(pipe_fds[1], STDERR_FILENO); close(pipe_fds[0]); close(pipe_fds[1]); chdir(working_directory.c_str());
        if (isolate_environment) {
            clearenv();
            for (const auto &entry : environment) {
                const auto separator = entry.find('=');
                if (separator != std::string::npos)
                    setenv(entry.substr(0, separator).c_str(), entry.substr(separator + 1).c_str(), 1);
            }
        }
        std::vector<char *> child_argv; child_argv.push_back(const_cast<char *>(program.c_str())); std::vector<std::string> values = arguments; for (auto &value : values) child_argv.push_back(value.data()); child_argv.push_back(nullptr); if (program.has_parent_path()) execv(program.c_str(), child_argv.data()); else execvp(program.c_str(), child_argv.data()); _exit(127);
    }
    close(pipe_fds[1]); fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK); ProcessResult result; std::ofstream file; if (output_file) { file.open(*output_file, std::ios::binary); if (!file) throw std::runtime_error("unable to open output file: " + output_file->string()); } const auto started = std::chrono::steady_clock::now(); char buffer[4096]; bool done = false;
    while (!done) { pollfd descriptor{pipe_fds[0], POLLIN, 0}; poll(&descriptor, 1, 25); const auto count = read(pipe_fds[0], buffer, sizeof(buffer)); if (count > 0) { const auto accepted = std::min<std::size_t>(count, output_limit > result.output.size() ? output_limit - result.output.size() : 0); result.output.append(buffer, accepted); if (file) file.write(buffer, count); if (accepted < static_cast<std::size_t>(count)) result.output_truncated = true; } int status = 0; if (waitpid(pid, &status, WNOHANG) == pid) { done = true; result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1; } if (detail::cancellation_requested || (cancel_requested && cancel_requested())) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); result.cancelled = true; result.exit_code = 130; done = true; } else if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() > static_cast<long long>(timeout_ms)) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); result.timed_out = true; result.exit_code = 124; done = true; } }
    close(pipe_fds[0]); return result;
#endif
}

std::string usage() {
    return "Usage: streamfind-dev doctor [--json] [--workspace PATH] [--managed-home PATH]\n"
           "       streamfind-dev rebuild --plugin PATH --sdk PATH [--build PATH]\n"
           "       streamfind-dev list [--target native|viewer|python|all] [--managed-home PATH]\n"
           "       streamfind-dev inspect --package PATH [--target native|viewer]\n"
           "       streamfind-dev setup [--target native|viewer|python|all] [--workspace PATH] [--offline] [--dry-run]\n"
           "       streamfind-dev plugin create NAME [--workspace PATH]\n"
           "       streamfind-dev plugin add-operation NAME --plugin PATH\n"
           "       streamfind-dev plugin build|test|validate|package --plugin PATH [--sdk PATH] [--build PATH]\n"
           "       streamfind-dev plugin install --package PATH [--managed-home PATH]\n"
           "       streamfind-dev plugin deactivate PLUGIN_ID [--version VERSION]\n"
           "       streamfind-dev plugin activate PLUGIN_ID [--version VERSION]\n"
           "       streamfind-dev plugin uninstall PLUGIN_ID [--version VERSION]\n"
           "       streamfind-dev viewer create NAME --contract IRI [--workspace PATH]\n"
           "       streamfind-dev viewer build|test|validate|package --viewer PATH\n"
           "       streamfind-dev viewer install --package PATH [--managed-home PATH]\n"
           "       streamfind-dev viewer deactivate|activate|uninstall VIEWER_ID [--version VERSION]\n"
           "       streamfind-dev run --program PATH [--cwd PATH] [--timeout-ms N] [--output PATH] -- ARGS...";
}

} // namespace streamfind::dev
