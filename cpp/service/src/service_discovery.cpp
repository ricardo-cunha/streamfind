#include "streamfind/service/service_discovery.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif

namespace streamfind::service {

namespace detail {

std::string random_hex(std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 generator(device());
    std::string value;
    value.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
        value.push_back(digits[static_cast<std::size_t>(generator() & 0x0f)]);
    return value;
}

std::string environment_value(const char *name) {
    const auto *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

bool process_alive(std::uint64_t process_id) {
    if (process_id == 0) return false;
#ifdef _WIN32
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    static_cast<DWORD>(process_id));
    if (handle == nullptr) return false;
    DWORD status = 0;
    const auto alive = GetExitCodeProcess(handle, &status) != 0 && status == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    return kill(static_cast<pid_t>(process_id), 0) == 0 || errno == EPERM;
#endif
}

}  // namespace detail

Json ServiceDiscoveryRecord::to_json() const {
    return Json{{"service", "streamfind-service"},
                {"service_instance_id", service_instance_id},
                {"endpoint", endpoint},
                {"protocol_version", protocol_version},
                {"state", state},
                {"token", token},
                {"executable", executable},
                {"process_id", process_id},
                {"started_at", started_at}};
}

std::filesystem::path ServiceDiscovery::runtime_directory() {
#ifdef _WIN32
    auto base = detail::environment_value("LOCALAPPDATA");
    if (base.empty()) base = detail::environment_value("TEMP");
    if (base.empty()) base = std::filesystem::temp_directory_path().string();
    return std::filesystem::path(base) / "streamfind" / "runtime";
#else
    auto base = detail::environment_value("XDG_RUNTIME_DIR");
    if (base.empty()) base = detail::environment_value("HOME");
    if (base.empty()) base = std::filesystem::temp_directory_path().string();
    return std::filesystem::path(base) /
           (detail::environment_value("XDG_RUNTIME_DIR").empty() ? ".cache/streamfind/runtime" : "streamfind");
#endif
}

std::vector<std::filesystem::path> ServiceDiscovery::record_paths() {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    const auto directory = runtime_directory();
    if (!std::filesystem::is_directory(directory, error)) return paths;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json" ||
            !entry.path().filename().string().starts_with("service-")) continue;
        try {
            std::ifstream input(entry.path(), std::ios::binary);
            Json value;
            input >> value;
            if (value.value("service", "") != "streamfind-service") {
                std::filesystem::remove(entry.path(), error);
                error.clear();
                continue;
            }
            const auto record = read(entry.path());
            const auto executable = std::filesystem::path(value.value("executable", ""));
#ifdef _WIN32
            constexpr const char *expected_name = "streamfind-service.exe";
#else
            constexpr const char *expected_name = "streamfind-service";
#endif
            if (executable.empty() || executable.filename() != expected_name) {
                std::filesystem::remove(entry.path(), error);
                error.clear();
                continue;
            }
            if (!detail::process_alive(record.process_id)) {
                std::filesystem::remove(entry.path(), error);
                error.clear();
                continue;
            }
        } catch (...) {
            std::filesystem::remove(entry.path(), error);
            error.clear();
            continue;
        }
        paths.push_back(entry.path());
    }
    return paths;
}

ServiceDiscoveryRecord ServiceDiscovery::read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read service discovery record: " + path.string());
    Json value;
    input >> value;
    ServiceDiscoveryRecord record;
    record.service_instance_id = value.at("service_instance_id").get<std::string>();
    record.endpoint = value.at("endpoint").get<std::string>();
    record.protocol_version = value.at("protocol_version").get<std::string>();
    record.state = value.value("state", "unknown");
    record.token = value.value("token", std::string{});
    record.executable = value.value("executable", std::string{});
    record.process_id = value.value("process_id", std::uint64_t{0});
    record.started_at = value.value("started_at", std::string{});
    record.record_path = path;
    return record;
}

std::string ServiceDiscovery::create_id() {
    return "service-" + detail::random_hex(24);
}

std::string ServiceDiscovery::create_token() {
    return detail::random_hex(48);
}

std::string ServiceDiscovery::timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::uint64_t ServiceDiscovery::current_process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path current_executable_path() {
#ifdef _WIN32
    char path[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::filesystem::path{} : std::filesystem::path(path);
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
#endif
}

void ServiceDiscovery::write_record(const ServiceDiscoveryRecord &record) {
    std::error_code error;
    std::filesystem::create_directories(record.record_path.parent_path(), error);
    if (error) throw std::runtime_error("cannot create service discovery directory: " + error.message());
    const auto temporary = record.record_path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create service discovery record: " + record.record_path.string());
        output << record.to_json().dump(2) << '\n';
        if (!output) throw std::runtime_error("cannot write service discovery record: " + record.record_path.string());
    }
    std::filesystem::rename(temporary, record.record_path, error);
    if (error) {
        std::filesystem::remove(record.record_path, error);
        error.clear();
        std::filesystem::rename(temporary, record.record_path, error);
    }
    if (error) throw std::runtime_error("cannot publish service discovery record: " + error.message());
}

ServiceDiscoveryRecord ServiceDiscovery::publish(std::uint16_t port, const std::string &protocol_version) {
    record_ = ServiceDiscoveryRecord{};
    record_.service_instance_id = create_id();
    record_.endpoint = "http://127.0.0.1:" + std::to_string(port);
    record_.protocol_version = protocol_version;
    record_.state = "ready";
    record_.token = create_token();
    record_.executable = current_executable_path().string();
    record_.process_id = current_process_id();
    record_.started_at = timestamp();
    record_.record_path = runtime_directory() /
        ("service-" + std::to_string(record_.process_id) + "-" + record_.service_instance_id + ".json");
    write_record(record_);
    return record_;
}

void ServiceDiscovery::update_state(const std::string &state) {
    if (record_.record_path.empty()) return;
    record_.state = state;
    try { write_record(record_); } catch (...) { }
}

void ServiceDiscovery::remove() noexcept {
    if (record_.record_path.empty()) return;
    std::error_code error;
    std::filesystem::remove(record_.record_path, error);
    record_ = ServiceDiscoveryRecord{};
}

}  // namespace streamfind::service
