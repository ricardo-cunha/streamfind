#include "streamfind/dev/command_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace streamfind::dev::detail {

std::filesystem::path executable_directory(const char *argv0) {
    return std::filesystem::absolute(std::filesystem::path(argv0)).parent_path();
}

std::filesystem::path provisioner_for(const char *argv0) {
    const auto package_script = executable_directory(argv0).parent_path() / "dev" / "streamfind-dev-setup.ps1";
    if (std::filesystem::is_regular_file(package_script)) return package_script;
    if (const auto *value = std::getenv("STREAMFIND_DEV_PROVISIONER"); value && *value)
        return std::filesystem::absolute(value);
    throw std::runtime_error("release provisioning helper is unavailable; expected dev/streamfind-dev-setup.ps1 beside the executable");
}

std::string quote_for_powershell(const std::string &value) {
    std::string result = "'";
    for (const auto character : value) { if (character == '\'') result += "''"; else result += character; }
    result += "'"; return result;
}

int setup(const Request &request, const char *argv0) {
#ifdef _WIN32
    std::filesystem::path powershell;
    if (const auto *configured = std::getenv("STREAMFIND_PWSH"); configured && *configured) {
        powershell = configured;
    } else {
        std::array<wchar_t, MAX_PATH> resolved{};
        const auto length = SearchPathW(nullptr, L"pwsh.exe", nullptr,
                                        static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        if (length == 0 || length >= resolved.size())
            throw std::runtime_error("PowerShell 7 (pwsh.exe) was not found; install it or set STREAMFIND_PWSH");
        powershell = resolved.data();
    }
#else
    const std::filesystem::path powershell = "pwsh";
#endif
    std::vector<std::string> args{"-NoProfile", "-File", provisioner_for(argv0).string(), "setup", "-Target", request.target};
    if (request.workspace) { args.push_back("-Workspace"); args.push_back(request.workspace->string()); }
    if (request.managed_home) { args.push_back("-ManagedHome"); args.push_back(request.managed_home->string()); }
    if (request.offline) args.push_back("-Offline");
    if (request.dry_run) args.push_back("-DryRun");
    if (request.requirements) { args.push_back("-Requirements"); args.push_back(request.requirements->string()); }
    if (request.viewer) { args.push_back("-Viewer"); args.push_back(request.viewer->string()); }
    const auto result = run_process(powershell, args, std::filesystem::current_path(), 3600000,
        1024 * 1024, std::nullopt, false, {});
    if (!result.output.empty()) std::cout << result.output;
    if (result.cancelled) return static_cast<int>(ErrorCode::Cancelled);
    if (result.timed_out) return static_cast<int>(ErrorCode::Timeout);
    return result.exit_code == 0 ? 0 : static_cast<int>(ErrorCode::ChildFailed);
}

} // namespace streamfind::dev::detail

int main(int argc, char **argv) {
    const bool json_requested = std::find(argv, argv + argc, std::string("--json")) != argv + argc;
    try {
        streamfind::dev::install_cancellation_handler();
        const auto request = streamfind::dev::parse_request(argc, argv);
        if (request.command == "--version") { std::cout << "streamfind-dev/1\n"; return 0; }
        if (request.command == "doctor") { std::cout << streamfind::dev::doctor_json(request) << '\n'; return 0; }
        if (request.command == "rebuild") return streamfind::dev::rebuild_plugin(request);
        if (request.command == "list") return streamfind::dev::list_extensions(request);
        if (request.command == "inspect") return streamfind::dev::inspect_extension(request);
        if (request.command == "setup") return streamfind::dev::detail::setup(request, argv[0]);
        if (request.command == "plugin") {
            if (request.plugin_action == "create") return streamfind::dev::scaffold_plugin(request);
            if (request.plugin_action == "add-operation") return streamfind::dev::add_plugin_operation(request);
            if (request.plugin_action == "build") return streamfind::dev::build_plugin(request);
            if (request.plugin_action == "test") return streamfind::dev::test_plugin(request);
            if (request.plugin_action == "validate") return streamfind::dev::validate_plugin(request);
            if (request.plugin_action == "package") return streamfind::dev::package_plugin(request);
            if (request.plugin_action == "install") return streamfind::dev::install_plugin(request);
            if (request.plugin_action == "deactivate") return streamfind::dev::deactivate_plugin(request);
            if (request.plugin_action == "activate") return streamfind::dev::activate_plugin(request);
            return streamfind::dev::uninstall_plugin(request);
        }
        if (request.command == "viewer") {
            if (request.plugin_action == "create") return streamfind::dev::scaffold_viewer(request);
            if (request.plugin_action == "build") return streamfind::dev::build_viewer(request);
            if (request.plugin_action == "test") return streamfind::dev::test_viewer(request);
            if (request.plugin_action == "validate") return streamfind::dev::validate_viewer(request);
            if (request.plugin_action == "package") return streamfind::dev::package_viewer(request);
            if (request.plugin_action == "install") return streamfind::dev::install_viewer(request);
            if (request.plugin_action == "activate") return streamfind::dev::activate_viewer(request);
            if (request.plugin_action == "deactivate") return streamfind::dev::deactivate_viewer(request);
            return streamfind::dev::uninstall_viewer(request);
        }
        const auto managed = request.managed_home.value_or(streamfind::dev::resolve_managed_home());
        const auto workspace = streamfind::dev::resolve_workspace(request.workspace, managed);
        const auto program = streamfind::dev::canonicalize_user_path(*request.program, true, managed, workspace);
        const auto cwd = streamfind::dev::canonicalize_user_path(
            request.working_directory.value_or(std::filesystem::current_path()), true, managed, workspace);
        const auto output = request.output_file
            ? std::optional<std::filesystem::path>(streamfind::dev::canonicalize_user_path(
                *request.output_file, false, managed, workspace))
            : std::nullopt;
        const auto result = streamfind::dev::run_process(program, request.program_arguments, cwd,
            request.timeout_ms, 1024 * 1024, output, true, {});
        nlohmann::json response{{"exit_code", result.exit_code}, {"timed_out", result.timed_out},
            {"cancelled", result.cancelled}, {"output_truncated", result.output_truncated},
            {"output", result.output}};
        if (request.json) std::cout << response.dump(2) << '\n'; else std::cout << result.output;
        if (result.cancelled) return static_cast<int>(streamfind::dev::ErrorCode::Cancelled);
        if (result.timed_out) return static_cast<int>(streamfind::dev::ErrorCode::Timeout);
        return result.exit_code == 0 ? 0 : static_cast<int>(streamfind::dev::ErrorCode::ChildFailed);
    } catch (const streamfind::dev::PathDeniedError &error) {
        if (json_requested) std::cout << nlohmann::json{{"error", {{"code", "E_PATH_DENIED"}, {"message", error.what()}}}}.dump() << '\n';
        else std::cerr << "streamfind-dev [E_PATH_DENIED]: " << error.what() << '\n';
        return static_cast<int>(streamfind::dev::ErrorCode::PathDenied);
    } catch (const std::invalid_argument &error) {
        if (json_requested) std::cout << nlohmann::json{{"error", {{"code", "E_INVALID_ARGUMENT"}, {"message", error.what()}}}}.dump() << '\n';
        else std::cerr << "streamfind-dev [E_INVALID_ARGUMENT]: " << error.what() << '\n' << streamfind::dev::usage() << '\n';
        return static_cast<int>(streamfind::dev::ErrorCode::InvalidArguments);
    } catch (const std::exception &error) {
        if (json_requested) std::cout << nlohmann::json{{"error", {{"code", "E_FAILURE"}, {"message", error.what()}}}}.dump() << '\n';
        else std::cerr << "streamfind-dev [E_FAILURE]: " << error.what() << '\n';
        return 1;
    }
}
