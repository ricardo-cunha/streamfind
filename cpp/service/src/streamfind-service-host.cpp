#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace streamfind::service::host_detail {
PROCESS_INFORMATION child{};
HANDLE child_job = nullptr;
void stop_child();

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        stop_child();
        return TRUE;
    }
    return FALSE;
}

std::string service_path() {
    char path[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) throw std::runtime_error("unable to resolve StreamFind installation path");
    return (std::filesystem::path(path).parent_path() / "streamfind_service.exe").string();
}

void configure_child_vendor_runtime() {
    char module_path[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        throw std::runtime_error("unable to resolve StreamFind package path");

    const auto package_root = std::filesystem::path(module_path).parent_path().parent_path();
    const auto vendor_root = package_root / "core" / "vendors";
    const auto duckdb_directory = vendor_root / "duckdb";
    const auto mingw_directory = vendor_root / "mingw";
    if (!std::filesystem::is_directory(duckdb_directory) ||
        !std::filesystem::is_directory(mingw_directory))
        throw std::runtime_error("StreamFind vendor runtime directories are missing");

    const char *existing_path = std::getenv("PATH");
    std::string path = duckdb_directory.string() + ";" + mingw_directory.string();
    if (existing_path != nullptr && *existing_path != '\0')
        path += ";" + std::string(existing_path);
    if (_putenv_s("PATH", path.c_str()) != 0)
        throw std::runtime_error("unable to configure StreamFind vendor runtime path");
}

bool running() { return child.hProcess != nullptr && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT; }

void close_child() {
    if (child.hProcess) CloseHandle(child.hProcess);
    if (child.hThread) CloseHandle(child.hThread);
    child = {};
    if (child_job) CloseHandle(child_job);
    child_job = nullptr;
}

void stop_child() {
    if (running()) {
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 5000);
    }
    close_child();
}

std::uint16_t choose_port() {
    const auto probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) throw std::runtime_error("unable to allocate a local port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(probe);
        throw std::runtime_error("unable to choose a local port");
    }
    int length = sizeof(address);
    if (getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length) == SOCKET_ERROR) {
        closesocket(probe);
        throw std::runtime_error("unable to read the chosen local port");
    }
    closesocket(probe);
    return ntohs(address.sin_port);
}

void start_child(std::uint16_t port) {
    const auto executable = service_path();
    // streamfind_service imports DuckDB before its C++ main() can run. Add
    // package-owned runtime directories before CreateProcess so the Windows
    // loader can resolve DuckDB and the MinGW runtime on clean machines.
    configure_child_vendor_runtime();
    std::string command = "\"" + executable + "\" " + std::to_string(port);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        std::filesystem::path(executable).parent_path().string().c_str(), &startup, &child))
        throw std::runtime_error("unable to start streamfind_service.exe");
    child_job = CreateJobObjectA(nullptr, nullptr);
    if (!child_job) { close_child(); throw std::runtime_error("unable to create StreamFind child process job"); }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(child_job, child.hProcess)) {
        close_child();
        throw std::runtime_error("unable to attach StreamFind backend to launcher job");
    }
}

bool backend_reachable(std::uint16_t port) {
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const auto result = connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    closesocket(socket_handle);
    return result == 0;
}
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    try {
        if (!SetConsoleCtrlHandler(streamfind::service::host_detail::console_handler, TRUE))
            throw std::runtime_error("unable to install shutdown handler");
        const auto port = streamfind::service::host_detail::choose_port();
        streamfind::service::host_detail::start_child(port);
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (streamfind::service::host_detail::backend_reachable(port)) break;
            if (!streamfind::service::host_detail::running()) throw std::runtime_error("streamfind_service.exe exited during startup");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (attempt == 99) throw std::runtime_error("StreamFind backend did not become ready");
        }
        const auto url = std::string("http://127.0.0.1:") + std::to_string(port) + "/";
        std::cout << "streamfind is running\n"
                  << "  app:     " << url << "\n"
                  << "  backend: http://127.0.0.1:" << port << "\n"
                  << "Close this terminal to stop streamfind.\n" << std::flush;
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        while (streamfind::service::host_detail::running()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        streamfind::service::host_detail::close_child();
    } catch (const std::exception &error) {
        MessageBoxA(nullptr, error.what(), "streamfind startup failed", MB_OK | MB_ICONERROR);
        streamfind::service::host_detail::stop_child();
        WSACleanup();
        return 1;
    }
    WSACleanup();
    return 0;
}
