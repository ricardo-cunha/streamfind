#include <windows.h>
#include <shellapi.h>
#include "streamfind/service_configuration.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace streamfind::service::host_detail {

std::filesystem::path package_root() {
    char module_path[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        throw std::runtime_error("unable to resolve streamfind package path");
    auto root = std::filesystem::path(module_path).parent_path();
    if (root.filename() == "bin") root = root.parent_path();
    return root;
}

std::string quote_argument(const std::string &value) {
    return "\"" + value + "\"";
}

std::filesystem::path cli_path() {
    const auto cli = package_root() / "bin" / "streamfind.exe";
    if (!std::filesystem::exists(cli)) throw std::runtime_error("streamfind CLI is missing");
    return cli;
}

int run_service_command(int argc, char **argv) {
    const auto cli = cli_path();
    std::string command = quote_argument(cli.string());
    for (int index = 1; index < argc; ++index)
        command += " " + quote_argument(argv[index]);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        package_root().string().c_str(), &startup, &process))
        throw std::runtime_error("unable to start streamfind CLI");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<int>(exit_code);
}

void start_service() {
    const auto cli = cli_path();
    std::string command = quote_argument(cli.string()) + " service start";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        package_root().string().c_str(), &startup, &process))
        throw std::runtime_error("unable to start streamfind service command");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (exit_code != 0) throw std::runtime_error("streamfind service did not become ready");
    const auto configuration = streamfind::load_service_configuration(package_root() / "bin" / "streamfind.json");
    if (configuration.tray_enabled && configuration.tray_start_with_app) {
        auto tray = package_root() / "bin" / "streamfind-tray.exe";
        if (!std::filesystem::exists(tray)) tray = package_root() / "streamfind-tray.exe";
        if (!std::filesystem::exists(tray)) return;
        const auto result = ShellExecuteA(nullptr, "open", tray.string().c_str(), nullptr,
                                          tray.parent_path().string().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
            throw std::runtime_error("unable to start streamfind tray");
    }
}

std::uint16_t service_port() {
    return streamfind::load_service_configuration(package_root() / "bin" / "streamfind.json").port;
}

}  // namespace streamfind::service::host_detail

int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "service")
            return streamfind::service::host_detail::run_service_command(argc, argv);

        streamfind::service::host_detail::start_service();
        const auto url = "http://127.0.0.1:" + std::to_string(streamfind::service::host_detail::service_port()) + "/";
        std::cout << "\nstreamfind is ready.\n"
                  << "The background service remains running when this window or browser closes.\n"
                  << "Open the application: " << url << "\n"
                  << "Stop the service explicitly with: streamfind service stop --instance <id>\n"
                  << std::flush;
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    } catch (const std::exception &error) {
        MessageBoxA(nullptr, error.what(), "streamfind-app startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }
}
