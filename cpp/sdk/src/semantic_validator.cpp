#include "streamfind/sdk/semantic_validator.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace streamfind::sdk::detail {

std::optional<std::filesystem::path> find_java() {
    const auto executable_name =
#ifdef _WIN32
        std::string("java.exe");
#else
        std::string("java");
#endif
    if (const char *raw_path = std::getenv("PATH"); raw_path && *raw_path) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream paths(raw_path);
        std::string directory;
        while (std::getline(paths, directory, separator)) {
            if (directory.empty()) continue;
            const auto candidate = std::filesystem::path(directory) / executable_name;
            if (std::filesystem::is_regular_file(candidate)) return candidate;
        }
    }
    if (const char *java_home = std::getenv("JAVA_HOME"); java_home && *java_home) {
        const auto candidate = std::filesystem::path(java_home) / "bin" / executable_name;
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif
    if (!home || !*home) return std::nullopt;
    const auto java_root = std::filesystem::path(home) / ".streamfind" / "tools" / "java";
    if (!std::filesystem::is_directory(java_root)) return std::nullopt;
    for (const auto &entry : std::filesystem::directory_iterator(java_root)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "bin" / executable_name;
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

#ifdef _WIN32
std::wstring widen(const std::string &value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring windows_argument(const std::wstring &value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

int run_process(const std::filesystem::path &executable, const std::vector<std::string> &arguments) {
    std::wstring command = windows_argument(executable.wstring());
    for (const auto &argument : arguments) command += L" " + windows_argument(widen(argument));
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process))
        return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}
#else
int run_process(const std::filesystem::path &executable, const std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    std::vector<std::string> values;
    values.reserve(arguments.size() + 1);
    values.push_back(executable.string());
    values.insert(values.end(), arguments.begin(), arguments.end());
    for (auto &value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

bool ensure_jena(const std::filesystem::path &home, std::string &diagnostics) {
    if (std::filesystem::exists(home / "lib")) return true;
    diagnostics = "Apache Jena SHACL validator is not available at " + home.string() +
                  ". The SDK package should contain Jena 6.2.0; reinstall the SDK package, then rerun catalogue generation.";
    return false;
}

}  // namespace streamfind::sdk::detail

namespace streamfind::sdk {

SemanticValidationResult validate_semantics_with_jena(const SemanticResourceSet &resources,
                                                       const std::filesystem::path &jena_home) {
    std::string diagnostics;
    if (!detail::ensure_jena(jena_home, diagnostics)) return {false, diagnostics};
    const auto java = detail::find_java();
    if (!java) {
        return {false, "Java is required to run Apache Jena semantic validation. Install it with "
                       "streamfind-cli tools install java into the user-scoped .streamfind/tools/java directory."};
    }
    const auto shapes = resources.core_directory / "shapes.ttl";
    if (!std::filesystem::exists(shapes)) return {false, "core semantic shapes.ttl is missing"};

    const auto &executable = *java;
    std::vector<std::string> arguments = {
        "-cp", (jena_home / "lib" / "*").generic_string(), "shacl.shacl", "validate", "--text", "--shapes", shapes.generic_string()};
    for (const auto &file : semantic_files(resources.core_directory)) {
        if (file.filename() == "shapes.ttl") continue;
        arguments.push_back("--data");
        arguments.push_back(file.generic_string());
    }
    for (const auto &directory : resources.plugin_directories)
        for (const auto &file : semantic_files(directory))
            arguments.insert(arguments.end(), {"--data", file.generic_string()});
    const int status = detail::run_process(executable, arguments);
    if (status != 0) return {false, "Apache Jena SHACL validation failed"};
    return {true, {}};
}

}  // namespace streamfind::sdk
