// User-scoped external tool provisioning mirroring `bindings/r`:
// `~/.streamfind/tools/{java/jdk-*,metfrag/{MetFragCL.jar,streamfind-metfrag-fragmenter.jar}}`.
#include "utils/tools_resolver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <string_view>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace streamfind::mass_spec::tools {

namespace detail {

constexpr const char* kJdkUrlTemplate =
    "https://api.adoptium.net/v3/binary/latest/21/ga/{os}/{arch}/jdk/hotspot/normal/eclipse";
constexpr const char* kMetFragUrl =
    "https://github.com/ipb-halle/MetFragRelaunched/releases/download/v2.6.11/MetFragCommandLine-2.6.11.jar";
constexpr const char* kMetFragFragmenterUrl =
    "https://github.com/ricardo-cunha/streamfind-metfrag-tools/releases/download/v";
constexpr std::string_view kFragmenterPomEntry =
    "META-INF/maven/org.streamfind.tools/streamfind-metfrag-fragmenter/pom.properties";
constexpr std::size_t kMaximumPomSize = 64 * 1024;

uint16_t read_u16(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1] << 8);
}

uint32_t read_u32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

std::optional<std::string> read_zip_entry(const std::string& path, std::string_view wanted_entry) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto end = input.tellg();
    if (end < 22) return std::nullopt;
    const auto file_size = static_cast<std::uint64_t>(end);
    const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 65557));
    std::vector<unsigned char> tail(tail_size);
    input.seekg(static_cast<std::streamoff>(file_size - tail_size));
    input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!input) return std::nullopt;
    std::optional<std::size_t> eocd;
    for (std::size_t pos = tail.size() - 22;; --pos) {
        if (read_u32(tail, pos) == 0x06054b50u &&
            pos + 22 + read_u16(tail, pos + 20) <= tail.size()) { eocd = pos; break; }
        if (pos == 0) break;
    }
    if (!eocd) return std::nullopt;
    const auto disk = read_u16(tail, *eocd + 4);
    const auto cd_disk = read_u16(tail, *eocd + 6);
    const auto entry_count = read_u16(tail, *eocd + 10);
    const auto cd_size = read_u32(tail, *eocd + 12);
    const auto cd_offset = read_u32(tail, *eocd + 16);
    const auto absolute_eocd = file_size - tail_size + *eocd;
    if (disk != 0 || cd_disk != 0 || entry_count == 0xffff || cd_size == 0xffffffffu ||
        cd_offset == 0xffffffffu || static_cast<std::uint64_t>(cd_offset) + cd_size > absolute_eocd)
        return std::nullopt;
    input.clear();
    input.seekg(cd_offset);
    std::uint64_t cursor = cd_offset;
    for (uint16_t i = 0; i < entry_count; ++i) {
        std::array<unsigned char, 46> central{};
        if (cursor + central.size() > static_cast<std::uint64_t>(cd_offset) + cd_size) return std::nullopt;
        input.read(reinterpret_cast<char*>(central.data()), static_cast<std::streamsize>(central.size()));
        std::vector<unsigned char> fields(central.begin(), central.end());
        if (!input || read_u32(fields, 0) != 0x02014b50u) return std::nullopt;
        const auto method = read_u16(fields, 10);
        const auto crc = read_u32(fields, 16);
        const auto compressed_size = read_u32(fields, 20);
        const auto uncompressed_size = read_u32(fields, 24);
        const auto name_size = read_u16(fields, 28);
        const auto extra_size = read_u16(fields, 30);
        const auto comment_size = read_u16(fields, 32);
        const auto local_offset = read_u32(fields, 42);
        std::vector<char> name_buffer(name_size);
        input.read(name_buffer.data(), static_cast<std::streamsize>(name_buffer.size()));
        const std::string_view name(name_buffer.data(), name_buffer.size());
        if (!input || cursor + 46u + name_size + extra_size + comment_size > static_cast<std::uint64_t>(cd_offset) + cd_size)
            return std::nullopt;
        if (name == wanted_entry) {
            if (uncompressed_size > kMaximumPomSize || compressed_size > kMaximumPomSize ||
                static_cast<std::uint64_t>(local_offset) + 30 > file_size) return std::nullopt;
            input.clear();
            input.seekg(local_offset);
            std::array<unsigned char, 30> local{};
            input.read(reinterpret_cast<char*>(local.data()), static_cast<std::streamsize>(local.size()));
            std::vector<unsigned char> local_fields(local.begin(), local.end());
            if (!input || read_u32(local_fields, 0) != 0x04034b50u) return std::nullopt;
            const auto local_name = read_u16(local_fields, 26);
            const auto local_extra = read_u16(local_fields, 28);
            const auto data_offset = static_cast<std::uint64_t>(local_offset) + 30 + local_name + local_extra;
            if (data_offset + compressed_size > file_size) return std::nullopt;
            std::vector<unsigned char> compressed(compressed_size);
            input.seekg(static_cast<std::streamoff>(data_offset));
            input.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
            if (!input) return std::nullopt;
            std::string result(uncompressed_size, '\0');
            if (method == 0) {
                if (compressed_size != uncompressed_size) return std::nullopt;
                std::copy(compressed.begin(), compressed.end(), result.begin());
            } else if (method == 8) {
                z_stream stream{};
                stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
                stream.avail_in = compressed_size;
                stream.next_out = reinterpret_cast<Bytef*>(result.data());
                stream.avail_out = uncompressed_size;
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return std::nullopt;
                const auto status = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                if (status != Z_STREAM_END || stream.total_out != uncompressed_size) return std::nullopt;
            } else return std::nullopt;
            const auto actual_crc = crc32(0, reinterpret_cast<const Bytef*>(result.data()),
                                          static_cast<uInt>(result.size()));
            if (actual_crc != crc) return std::nullopt;
            return result;
        }
        input.seekg(static_cast<std::streamoff>(extra_size + comment_size), std::ios::cur);
        if (!input) return std::nullopt;
        cursor += 46u + name_size + extra_size + comment_size;
    }
    return std::nullopt;
}

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


bool curl_revocation_bypass_requested() {
#ifdef _WIN32
    const char* value = std::getenv("STREAMFIND_CURL_SSL_NO_REVOKE");
    return value && std::string_view(value) == "1";
#else
    return false;
#endif
}

std::vector<std::string> curl_download_arguments(const std::string& destination, const std::string& url,
                                                bool allow_windows_revocation_bypass,
                                                const std::string& stderr_path) {
    std::vector<std::string> args{"--fail", "--location", "--silent", "--show-error", "--retry", "3",
                                  "--output", destination};
#ifdef _WIN32
    if (allow_windows_revocation_bypass && curl_revocation_bypass_requested()) args.push_back("--ssl-no-revoke");
#else
    (void)allow_windows_revocation_bypass;
#endif
    if (!stderr_path.empty()) {
        args.push_back("--stderr");
        args.push_back(stderr_path);
    }
    args.push_back(url);
    return args;
}

std::string read_curl_diagnostic(const std::string& stderr_path) {
    if (stderr_path.empty()) return {};
    std::ifstream input(stderr_path, std::ios::binary);
    if (!input) return {};
    std::string output(4096, '\0');
    input.read(output.data(), static_cast<std::streamsize>(output.size()));
    output.resize(static_cast<std::size_t>(input.gcount()));
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' ')) output.pop_back();
    return output;
}

std::string curl_failure_guidance() {
#ifdef _WIN32
    return " If Windows curl reports Schannel CRYPT_E_NO_REVOCATION_CHECK behind a trusted company proxy, "
           "set STREAMFIND_CURL_SSL_NO_REVOKE=1 before starting or restarting the streamfind service, then retry; "
           "this disables certificate-revocation checks for streamfind dependency downloads.";
#else
    return {};
#endif
}

std::optional<std::string> metfrag_fragmenter_jar_version(const std::string& path) {
    auto properties = read_zip_entry(path, kFragmenterPomEntry);
    if (!properties) return std::nullopt;
    std::istringstream lines(*properties);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("version=", 0) != 0) continue;
        auto version = line.substr(8);
        while (!version.empty() && (version.back() == '\r' || version.back() == ' ')) version.pop_back();
        if (!version.empty()) return version;
    }
    return std::nullopt;
}

std::string fragmenter_release_url() {
    const auto version = std::string(kMetFragFragmenterVersion);
    return std::string(kMetFragFragmenterUrl) + version + "/streamfind-metfrag-fragmenter-" + version + ".jar";
}

void promote_metfrag_fragmenter_jar(const std::string& staged, const std::string& destination) {
    const auto candidate_version = metfrag_fragmenter_jar_version(staged);
    if (!candidate_version || *candidate_version != kMetFragFragmenterVersion)
        throw std::runtime_error("Refusing to install MetFrag Fragmenter candidate version " +
            (candidate_version ? *candidate_version : std::string("unknown")) + "; expected " +
            std::string(kMetFragFragmenterVersion) + " from release v" + std::string(kMetFragFragmenterVersion));
#ifdef _WIN32
    if (!CopyFileW(fs::path(staged).c_str(), fs::path(destination).c_str(), TRUE))
        throw std::runtime_error("Could not publish the versioned MetFrag Fragmenter JAR: Windows error " +
                                 std::to_string(GetLastError()));
    const auto published_version = metfrag_fragmenter_jar_version(destination);
    if (!published_version || *published_version != kMetFragFragmenterVersion) {
        std::error_code error;
        fs::remove(destination, error);
        throw std::runtime_error("The published MetFrag Fragmenter JAR failed version verification; "
                                 "the existing versioned installation was not overwritten.");
    }
#else
    std::error_code error;
    fs::rename(staged, destination, error);
    if (error) throw std::runtime_error("Could not publish versioned MetFrag Fragmenter JAR: " + error.message());
#endif
}

} // namespace detail

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
        auto candidate = jdk / "bin" / detail::executable_name("java");
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

std::optional<std::string> resolve_metfrag_fragmenter_jar() {
    if (const char* override_path = std::getenv("STREAMFIND_METFRAG_FRAGMENTER_JAR");
        override_path && *override_path) {
        if (detail::metfrag_fragmenter_jar_version(override_path) == kMetFragFragmenterVersion)
            return std::string(override_path);
        return std::nullopt;
    }
    const auto jar = fs::path(tools_dir()) / "metfrag" /
        ("streamfind-metfrag-fragmenter-" + std::string(kMetFragFragmenterVersion) + ".jar");
    if (detail::metfrag_fragmenter_jar_version(jar.string()) == kMetFragFragmenterVersion) return jar.string();
    return std::nullopt;
}

std::string install_metfrag_fragmenter() {
    if (auto jar = resolve_metfrag_fragmenter_jar()) return *jar;
    if (const char* override_path = std::getenv("STREAMFIND_METFRAG_FRAGMENTER_JAR");
        override_path && *override_path) {
        const auto installed_version = detail::metfrag_fragmenter_jar_version(override_path);
        throw std::runtime_error("STREAMFIND_METFRAG_FRAGMENTER_JAR points to a missing, unreadable, or incompatible JAR at " +
            std::string(override_path) + (installed_version ? "; found version " + *installed_version : "; embedded version metadata was not found") +
            "; expected " + std::string(kMetFragFragmenterVersion) + ". Remove the override or point it to the pinned release.");
    }
    const auto metfrag_root = fs::path(tools_dir()) / "metfrag";
    const auto staging = fs::path(tools_dir()) / ".metfrag-fragmenter-installing";
    const auto staged_jar = staging / ("streamfind-metfrag-fragmenter-" + std::string(kMetFragFragmenterVersion) + ".jar");
    std::error_code error;
    fs::remove_all(staging, error);
    error.clear();
    fs::create_directories(staging, error);
    if (error) throw std::runtime_error("Could not create MetFrag Fragmenter staging directory: " + error.message());
    const auto curl_stderr = staging / "curl.stderr";
    const auto download_status = detail::run_process("curl", detail::curl_download_arguments(
        staged_jar.generic_string(), detail::fragmenter_release_url(), true, curl_stderr.generic_string()));
    if (download_status != 0) {
        const auto curl_diagnostic = detail::read_curl_diagnostic(curl_stderr.generic_string());
        fs::remove_all(staging, error);
        std::string proxy_hint;
#ifdef _WIN32
        proxy_hint = detail::curl_failure_guidance();
#endif
        throw std::runtime_error("MetFrag Fragmenter download failed (curl exit " + std::to_string(download_status) +
                                 ") from release v" + std::string(kMetFragFragmenterVersion) +
                                 "; check network/proxy access or install the JAR manually under " +
                                 (metfrag_root / staged_jar.filename()).string() +
                                 (curl_diagnostic.empty() ? std::string{} : "; curl: " + curl_diagnostic) + proxy_hint);
    }
    const auto size = fs::file_size(staged_jar, error);
    if (error || size < 1024 * 1024) {
        fs::remove_all(staging, error);
        throw std::runtime_error("MetFrag Fragmenter download did not produce a usable JAR");
    }
    std::ifstream input(staged_jar, std::ios::binary);
    char signature[4]{};
    input.read(signature, sizeof(signature));
    if (input.gcount() != sizeof(signature) || signature[0] != 'P' || signature[1] != 'K' ||
        signature[2] != 3 || signature[3] != 4) {
        fs::remove_all(staging, error);
        throw std::runtime_error("MetFrag Fragmenter download is not a valid ZIP/JAR archive");
    }
    fs::create_directories(metfrag_root, error);
    if (error) {
        fs::remove_all(staging, error);
        throw std::runtime_error("Could not create MetFrag tools directory: " + error.message());
    }
    const auto installed = metfrag_root / staged_jar.filename();
    try {
        detail::promote_metfrag_fragmenter_jar(staged_jar.string(), installed.string());
    } catch (...) {
        fs::remove_all(staging, error);
        throw;
    }
    fs::remove_all(staging, error);
    const auto installed_version = detail::metfrag_fragmenter_jar_version(installed.string());
    if (!installed_version || *installed_version != kMetFragFragmenterVersion)
        throw std::runtime_error("MetFrag Fragmenter installation verification failed at " + installed.string());
    return installed.string();
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
    const auto curl_stderr = staging / "curl.stderr";
    const auto download_status = detail::run_process("curl", detail::curl_download_arguments(
        archive.generic_string(), detail::kMetFragUrl, true, curl_stderr.generic_string()));
    if (download_status != 0) {
        const auto curl_diagnostic = detail::read_curl_diagnostic(curl_stderr.generic_string());
        fs::remove_all(staging);
        throw std::runtime_error("MetFragCL download failed (curl exit " + std::to_string(download_status) +
                                 "); install MetFragCL 2.6.11 manually into " +
                                 (metfrag_root / "MetFragCL.jar").string() +
                                 (curl_diagnostic.empty() ? std::string{} : "; curl: " + curl_diagnostic) +
                                 detail::curl_failure_guidance());
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
    std::string url = detail::kJdkUrlTemplate;
    url.replace(url.find("{os}"), 4, os);
    url.replace(url.find("{arch}"), 6, "x64");
    const auto archive = staging / archive_name;
    const auto archive_text = archive.generic_string();
    const auto curl_stderr = staging / "curl.stderr";
    const auto download_status = detail::run_process("curl", detail::curl_download_arguments(
        archive_text, url, true, curl_stderr.generic_string()));
    if (download_status != 0) {
        const auto curl_diagnostic = detail::read_curl_diagnostic(curl_stderr.generic_string());
        fs::remove_all(staging);
        throw std::runtime_error("Java download failed (curl exit " + std::to_string(download_status) +
                                 "); install a JDK manually under " + java_root.string() +
                                 (curl_diagnostic.empty() ? std::string{} : "; curl: " + curl_diagnostic) +
                                 detail::curl_failure_guidance());
    }
    const auto extract_dir = staging / "extracted";
    fs::create_directories(extract_dir);
    if (detail::run_process("tar", {"-xf", archive.generic_string(), "-C", extract_dir.generic_string()}) != 0) {
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
    if (auto fragmenter = resolve_metfrag_fragmenter_jar())
        out << "metfrag_fragmenter: " << *fragmenter << " (version " << *detail::metfrag_fragmenter_jar_version(*fragmenter) << ")\n";
    else {
        const auto path = std::getenv("STREAMFIND_METFRAG_FRAGMENTER_JAR");
        const auto managed = fs::path(tools_dir()) / "metfrag" /
            ("streamfind-metfrag-fragmenter-" + std::string(kMetFragFragmenterVersion) + ".jar");
        const auto candidate = path && *path ? fs::path(path) : managed;
        const auto found_version = detail::metfrag_fragmenter_jar_version(candidate.string());
        if (found_version)
            out << "metfrag_fragmenter: outdated version " << *found_version << " at " << candidate.string()
                << " (expected release v" << kMetFragFragmenterVersion << ", embedded "
                << kMetFragFragmenterVersion << ")\n";
        else {
            const auto legacy_path = fs::path(tools_dir()) / "metfrag" / "streamfind-metfrag-fragmenter.jar";
            const auto legacy_version = detail::metfrag_fragmenter_jar_version(legacy_path.string());
            if (legacy_version)
                out << "metfrag_fragmenter: outdated legacy version " << *legacy_version << " at " << legacy_path.string()
                    << " (expected release v" << kMetFragFragmenterVersion << ", embedded "
                    << kMetFragFragmenterVersion << ")\n";
            else
                out << "metfrag_fragmenter: not found (expected release v" << kMetFragFragmenterVersion
                    << ", embedded " << kMetFragFragmenterVersion << ")\n";
        }
    }
    return out.str();
}

} // namespace streamfind::mass_spec::tools
