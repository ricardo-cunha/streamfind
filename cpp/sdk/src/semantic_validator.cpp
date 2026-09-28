#include "streamfind/sdk/semantic_validator.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
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

constexpr const char *kJavaUrlTemplate =
    "https://api.adoptium.net/v3/binary/latest/21/ga/{os}/{arch}/jdk/hotspot/normal/eclipse";

std::optional<std::filesystem::path> find_java() {
    const auto executable_name =
#ifdef _WIN32
        std::string("java.exe");
#else
        std::string("java");
#endif
    const auto java_root = streamfind_home() / "tools" / "java";
    if (!std::filesystem::is_directory(java_root)) return std::nullopt;
    for (const auto &entry : std::filesystem::directory_iterator(java_root)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "bin" / executable_name;
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

int run_process(const std::filesystem::path &executable, const std::vector<std::string> &arguments);

std::optional<std::filesystem::path> install_java(std::string &diagnostics) {
#ifdef _WIN32
    const std::string java_name = "java.exe";
#else
    const std::string java_name = "java";
#endif
    const auto java_root = streamfind_home() / "tools" / "java";
    const auto staging = streamfind_home() / "tools" / ".java-installing";
    const auto lock = streamfind_home() / "tools" / ".java-install.lock";
    std::error_code lock_error;
    std::filesystem::create_directories(lock.parent_path(), lock_error);
    if (!std::filesystem::create_directory(lock, lock_error)) {
        for (int attempt = 0; attempt < 120; ++attempt) {
            if (const auto java = find_java()) return java;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        diagnostics = "Timed out waiting for another StreamFind process to install Java under " +
                      java_root.string() + ".";
        return std::nullopt;
    }
    const auto release_lock = [&]() { std::error_code ignored; std::filesystem::remove_all(lock, ignored); };
    const auto archive = staging / (
#ifdef _WIN32
        "temurin21.zip"
#else
        "temurin21.tar.gz"
#endif
    );
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging, error);
    if (error) {
        diagnostics = "Unable to create StreamFind Java tool directory " + staging.string() + ": " + error.message();
        release_lock();
        return std::nullopt;
    }
    std::string url = kJavaUrlTemplate;
#ifdef _WIN32
    const std::string os = "windows";
#elif defined(__APPLE__)
    const std::string os = "mac";
#else
    const std::string os = "linux";
#endif
    url.replace(url.find("{os}"), 4, os);
    url.replace(url.find("{arch}"), 6, "x64");
    if (run_process("curl", {"--fail", "--location", "--silent", "--show-error", "--output",
                              archive.string(), url}) != 0) {
        std::filesystem::remove_all(staging, error);
        diagnostics = "Unable to download Temurin JDK 21 into " + java_root.string() +
                      ". Install curl or check network access.";
        release_lock();
        return std::nullopt;
    }
    const auto extracted = staging / "extracted";
    std::filesystem::create_directories(extracted, error);
    if (run_process("tar", {"-xf", archive.string(), "-C", extracted.string()}) != 0) {
        std::filesystem::remove_all(staging, error);
        diagnostics = "Unable to extract the Temurin JDK archive in " + staging.string() + ".";
        release_lock();
        return std::nullopt;
    }
    std::filesystem::path installed;
    for (const auto &entry : std::filesystem::directory_iterator(extracted)) {
        if (entry.is_directory()) {
            installed = entry.path();
            break;
        }
    }
    if (installed.empty()) {
        std::filesystem::remove_all(staging, error);
        diagnostics = "The downloaded Temurin JDK archive did not contain a top-level JDK directory.";
        release_lock();
        return std::nullopt;
    }
    std::filesystem::create_directories(java_root, error);
    const auto previous = java_root.parent_path() / ".java-previous";
    std::filesystem::remove_all(previous, error);
    const auto installed_jdk = java_root / installed.filename();
    if (!error && std::filesystem::exists(installed_jdk))
        std::filesystem::remove_all(installed_jdk, error);
    if (!error) std::filesystem::rename(installed, installed_jdk, error);
    std::filesystem::remove_all(previous, error);
    std::filesystem::remove_all(staging, error);
    if (error || !find_java()) {
        diagnostics = "Temurin JDK installation did not produce " +
                      (java_root / "bin" / java_name).string() + ".";
        release_lock();
        return std::nullopt;
    }
    const auto java = find_java();
    release_lock();
    return java;
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
    std::string java_diagnostics;
    auto java = detail::find_java();
    if (!java) java = detail::install_java(java_diagnostics);
    if (!java) return {false, java_diagnostics};
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
