// User-scoped external tool provisioning mirroring `bindings/r`:
// `~/.streamfind/tools/{java/jdk-*,metfrag/MetFragCL.jar}`.
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

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
constexpr const char* kMetFragUrl =
    "https://github.com/ipb-halle/MetFragRelaunched/releases/download/v2.6.11/MetFragCommandLine-2.6.11.jar";

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


} // namespace

std::string streamfind_home() {
    if (const char* override_home = std::getenv("STREAMFIND_HOME"); override_home && *override_home)
        return override_home;
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
    std::error_code error;
    if (fs::is_regular_file(jar, error) && fs::file_size(jar, error) > 0) return jar.string();
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> resolve_metfrag() {
    auto java = resolve_java();
    auto jar = resolve_metfrag_jar();
    if (java && jar) return std::pair{*java, *jar};
    return std::nullopt;
}

std::string install_metfrag() {
    if (auto jar = resolve_metfrag_jar()) return *jar;
    const auto metfrag_root = fs::path(tools_dir()) / "metfrag";
    const auto staging = fs::path(tools_dir()) / ".metfrag-installing";
    const auto archive = staging / "MetFragCommandLine-2.6.11.jar";
    fs::remove_all(staging);
    fs::create_directories(staging);
    if (run_process("curl", {"--fail", "--location", "--silent", "--show-error", "--output",
                               archive.generic_string(), kMetFragUrl}) != 0) {
        fs::remove_all(staging);
        throw std::runtime_error("MetFragCL download failed; install MetFragCL 2.6.11 manually into " +
                                 (metfrag_root / "MetFragCL.jar").string());
    }
    std::error_code error;
    if (!fs::is_regular_file(archive, error) || fs::file_size(archive, error) == 0) {
        fs::remove_all(staging);
        throw std::runtime_error("MetFragCL download produced no usable JAR");
    }
    fs::create_directories(metfrag_root);
    const auto installed = metfrag_root / "MetFragCL.jar";
    fs::remove(installed, error);
    fs::rename(archive, installed, error);
    fs::remove_all(staging);
    if (error || !resolve_metfrag_jar())
        throw std::runtime_error("MetFragCL installation completed but the managed JAR was not found at " + installed.string());
    return installed.string();
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
    fs::create_directories(java_root);
    const auto installed_jdk = java_root / installed.filename();
    fs::remove_all(installed_jdk);
    fs::rename(installed, installed_jdk);
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