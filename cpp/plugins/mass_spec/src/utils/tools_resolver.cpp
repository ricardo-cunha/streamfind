// User-scoped external tool provisioning mirroring `bindings/r`:
// `~/.streamfind/tools/{java/jdk-*,metfrag/MetFragCL.jar}`.
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace streamfind::mass_spec::tools {

namespace {

constexpr const char* kJdkUrlTemplate =
    "https://api.adoptium.net/v3/binary/latest/21/ga/{os}/{arch}/jdk/hotspot/normal/eclipse";

#ifdef _WIN32
std::wstring widen(const std::string& value) { return {value.begin(), value.end()}; }

std::wstring quote_windows(const std::wstring& value) {
    std::wstring result = L"\"";
    for (const wchar_t ch : value) result += (ch == L'\"') ? L"\\\"" : std::wstring(1, ch);
    return result + L"\"";
}

int run_process(const std::string& executable, const std::vector<std::string>& arguments) {
    std::wstring command = quote_windows(widen(executable));
    for (const auto& argument : arguments) command += L" " + quote_windows(widen(argument));
    std::vector<wchar_t> command_line(command.begin(), command.end()); command_line.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) return -1;
    WaitForSingleObject(process.hProcess, INFINITE); DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
#else
int run_process(const std::string& executable, const std::vector<std::string>& arguments) {
    std::vector<std::string> values{executable}; values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv; for (auto& value : values) argv.push_back(value.data()); argv.push_back(nullptr);
    pid_t pid = 0; if (posix_spawnp(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0; if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif


std::string executable_name(const char* name) {
#ifdef _WIN32
    return std::string(name) + ".exe";
#else
    return name;
#endif
}


std::optional<std::string> find_on_path(const std::string& name) {
    const char* raw_path = std::getenv("PATH");
    if (!raw_path) return std::nullopt;
#ifdef _WIN32
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::istringstream stream(raw_path);
    std::string directory;
    while (std::getline(stream, directory, kSep)) {
        if (directory.empty()) continue;
        auto candidate = fs::path(directory) / name;
        if (fs::is_regular_file(candidate)) return candidate.string();
    }
    return std::nullopt;
}

} // namespace

std::string streamfind_home() {
#ifdef _WIN32
    const char* base = std::getenv("USERPROFILE");
#else
    const char* base = std::getenv("HOME");
#endif
    if (!base || !*base) base = ".";
    return (fs::path(base) / ".streamfind").string();
}

std::string tools_dir() { return (fs::path(streamfind_home()) / "tools").string(); }

std::optional<std::string> resolve_java() {
    if (auto java = find_on_path(executable_name("java"))) return java;
    if (const char* java_home = std::getenv("JAVA_HOME"); java_home && *java_home) {
        auto candidate = fs::path(java_home) / "bin" / executable_name("java");
        if (fs::is_regular_file(candidate)) return candidate.string();
    }
    auto java_root = fs::path(tools_dir()) / "java";
    if (!fs::is_directory(java_root)) return std::nullopt;
    std::vector<fs::path> jdks;
    for (const auto& entry : fs::directory_iterator(java_root))
        if (entry.is_directory() && entry.path().filename().string().rfind("jdk", 0) == 0)
            jdks.push_back(entry.path());
    std::sort(jdks.begin(), jdks.end());
    for (const auto& jdk : jdks) {
        auto candidate = jdk / "bin" / executable_name("java");
        if (fs::is_regular_file(candidate)) return candidate.string();
    }
    return std::nullopt;
}

std::optional<std::string> resolve_metfrag_jar() {
    auto jar = fs::path(tools_dir()) / "metfrag" / "MetFragCL.jar";
    if (fs::is_regular_file(jar)) return jar.string();
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> resolve_metfrag() {
    auto java = resolve_java();
    auto jar = resolve_metfrag_jar();
    if (java && jar) return std::pair{*java, *jar};
    return std::nullopt;
}

std::string install_metfrag() {
    throw std::runtime_error("MetFrag is not installed. Install MetFragCL 2.6.11 into " +
                             (fs::path(tools_dir()) / "metfrag" / "MetFragCL.jar").string() +
                             " and rerun the operation.");
}

std::string install_java() {
    if (auto java = resolve_java()) return *java;
    const auto java_root = fs::path(tools_dir()) / "java";
    const auto staging = fs::path(tools_dir()) / ".java-installing";
    fs::remove_all(staging);
    fs::create_directories(staging);
#ifdef _WIN32
    const std::string os = "windows";
    const std::string archive_name = "temurin21.zip";
#elif defined(__APPLE__)
    const std::string os = "mac";
    const std::string archive_name = "temurin21.tar.gz";
#else
    const std::string os = "linux";
    const std::string archive_name = "temurin21.tar.gz";
#endif
    std::string url = kJdkUrlTemplate;
    url.replace(url.find("{os}"), 4, os);
    url.replace(url.find("{arch}"), 6, "x64");
    const auto archive = staging / archive_name;
    const auto archive_text = archive.generic_string();
    if (run_process("curl", {"--fail", "--location", "--silent", "--show-error", "--output", archive_text, url}) != 0) {
        fs::remove_all(staging);
        throw std::runtime_error("Java download failed; install a JDK manually under " + java_root.string());
    }
    const auto extract_dir = staging / "extracted";
    fs::create_directories(extract_dir);
    if (run_process("tar", {"-xf", archive.generic_string(), "-C", extract_dir.generic_string()}) != 0) {
        fs::remove_all(staging);
        throw std::runtime_error("Java archive extraction failed; install a JDK manually under " + java_root.string());
    }
    fs::path installed;
    for (const auto& entry : fs::directory_iterator(extract_dir)) {
        if (!entry.is_directory()) continue;
        installed = entry.path();
        break;
    }
    if (installed.empty()) { fs::remove_all(staging); throw std::runtime_error("Java archive contained no JDK directory"); }
    fs::create_directories(java_root.parent_path());
    const auto backup = java_root.parent_path() / ".java-previous";
    fs::remove_all(backup);
    if (fs::exists(java_root)) fs::rename(java_root, backup);
    fs::rename(installed, java_root);
    fs::remove_all(backup);
    fs::remove_all(staging);
    if (auto java = resolve_java()) return *java;
    throw std::runtime_error("Java installation completed but no executable was found under " + java_root.string());
}

std::string tool_status() {
    std::ostringstream out;
    out << "home: " << streamfind_home() << "\n";
    if (auto java = resolve_java())
        out << "java: " << *java << "\n";
    else
        out << "java: not found\n";
    if (auto jar = resolve_metfrag_jar())
        out << "metfrag: " << *jar << "\n";
    else
        out << "metfrag: not found\n";
    return out.str();
}

} // namespace streamfind::mass_spec::tools