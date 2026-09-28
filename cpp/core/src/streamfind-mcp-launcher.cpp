#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace streamfind::mcp_launcher {

struct ProcessHandles {
    HANDLE process{};
    HANDLE thread{};
    HANDLE stdin_write{};
    HANDLE stdout_read{};
};

std::filesystem::path executable_path() {
    std::vector<char> buffer(MAX_PATH);
    for (;;) {
        const auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("unable to resolve streamfind MCP launcher path");
        if (length < buffer.size() - 1) return std::filesystem::path(buffer.data(), buffer.data() + length);
        buffer.resize(buffer.size() * 2);
    }
}

void forward_input(HANDLE destination) {
    char buffer[8192];
    DWORD count{};
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count != 0) {
        DWORD written{};
        if (!WriteFile(destination, buffer, count, &written, nullptr) || written != count) break;
    }
    CloseHandle(destination);
}

void forward_output(HANDLE source) {
    char buffer[8192];
    DWORD count{};
    while (ReadFile(source, buffer, sizeof(buffer), &count, nullptr) && count != 0) {
        DWORD written{};
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, count, &written, nullptr) || written != count) break;
    }
    CloseHandle(source);
}

ProcessHandles start_core(const std::filesystem::path &launcher) {
    const auto package_root = launcher.parent_path().parent_path();
    const auto core = launcher.parent_path() / "streamfind_mcp.exe";
    const auto mingw = package_root / "core" / "vendors" / "mingw";
    const auto duckdb = package_root / "core" / "vendors" / "duckdb";
    if (!std::filesystem::exists(core) || !std::filesystem::is_directory(mingw) ||
        !std::filesystem::is_directory(duckdb))
        throw std::runtime_error("StreamFind MCP package is incomplete");

    char *previous_path = nullptr;
    std::size_t previous_size{};
    _dupenv_s(&previous_path, &previous_size, "PATH");
    const std::string path = mingw.string() + ";" + duckdb.string() + ";" +
                             (previous_path == nullptr ? std::string{} : std::string(previous_path));
    free(previous_path);
    if (_putenv_s("PATH", path.c_str()) != 0)
        throw std::runtime_error("unable to configure the packaged MCP runtime path");

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE child_stdin_read{};
    HANDLE child_stdin_write{};
    HANDLE child_stdout_read{};
    HANDLE child_stdout_write{};
    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &security, 0) ||
        !CreatePipe(&child_stdout_read, &child_stdout_write, &security, 0))
        throw std::runtime_error("unable to create MCP stdio pipes");
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::string command = "\"" + core.string() + "\"";
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        launcher.parent_path().string().c_str(), &startup, &process))
        throw std::runtime_error("unable to start packaged streamfind_mcp.exe");
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    return {process.hProcess, process.hThread, child_stdin_write, child_stdout_read};
}

}  // namespace streamfind::mcp_launcher

int main() {
    try {
        const auto launcher = streamfind::mcp_launcher::executable_path();
        const auto child = streamfind::mcp_launcher::start_core(launcher);
        std::thread input(streamfind::mcp_launcher::forward_input,
                          child.stdin_write);
        std::thread output(streamfind::mcp_launcher::forward_output,
                           child.stdout_read);
        WaitForSingleObject(child.process, INFINITE);
        CloseHandle(child.process);
        CloseHandle(child.thread);
        input.detach();
        output.join();
        return 0;
    } catch (const std::exception &error) {
        const std::string message = std::string("streamfind-mcp-launcher: ") + error.what() + "\n";
        DWORD written{};
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        return 1;
    }
}
