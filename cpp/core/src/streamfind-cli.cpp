#include "streamfind/service/service_discovery.hpp"
#include "streamfind/service_runtime_client.hpp"

#include <cstdlib>
#include "streamfind/service_configuration.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace streamfind::service_cli::detail {

struct Endpoint {
    std::string host;
    std::uint16_t port{0};
};

Endpoint parse_endpoint(const std::string &value) {
    constexpr std::string_view prefix = "http://";
    if (!value.starts_with(prefix)) throw std::runtime_error("unsupported service endpoint: " + value);
    const auto authority = value.substr(prefix.size());
    const auto separator = authority.rfind(':');
    if (separator == std::string::npos) throw std::runtime_error("service endpoint has no port: " + value);
    const auto port = std::stoul(authority.substr(separator + 1));
    if (port > 65535) throw std::runtime_error("service endpoint has an invalid port: " + value);
    return {authority.substr(0, separator), static_cast<std::uint16_t>(port)};
}

void close_socket(std::intptr_t socket) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket));
#else
    close(static_cast<int>(socket));
#endif
}

bool process_is_running(std::uint64_t process_id) {
    if (process_id == 0) return false;
#ifdef _WIN32
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
    if (process == nullptr) return false;
    const auto running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    if (kill(static_cast<pid_t>(process_id), 0) == 0) return true;
    return errno == EPERM;
#endif
}

void set_socket_timeouts(std::intptr_t socket) {
#ifdef _WIN32
    const DWORD timeout = 2000;
    setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    const timeval timeout{2, 0};
    setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_SNDTIMEO,
               &timeout, sizeof(timeout));
    setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout));
#endif
}

void send_all(std::intptr_t socket, const std::string &value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
#ifdef _WIN32
        const auto sent = send(static_cast<SOCKET>(socket), value.data() + offset,
                               static_cast<int>(value.size() - offset), 0);
#else
        const auto sent = send(static_cast<int>(socket), value.data() + offset,
                               value.size() - offset, 0);
#endif
        if (sent <= 0) throw std::runtime_error("service connection closed while sending request");
        offset += static_cast<std::size_t>(sent);
    }
}

std::string request(const Endpoint &endpoint, const std::string &method,
                    const std::string &path, const std::vector<std::string> &headers = {}) {
#ifdef _WIN32
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) throw std::runtime_error("unable to create service connection");
#else
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) throw std::runtime_error("unable to create service connection");
#endif
    set_socket_timeouts(static_cast<std::intptr_t>(socket_handle));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(endpoint.host.c_str());
    address.sin_port = htons(endpoint.port);
    if (connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close_socket(static_cast<std::intptr_t>(socket_handle));
        throw std::runtime_error("service did not respond");
    }
    std::string message = method + " " + path + " HTTP/1.1\r\nHost: " + endpoint.host + ":" +
                          std::to_string(endpoint.port) +
                          "\r\nConnection: close\r\n";
    for (const auto &header : headers) message += header + "\r\n";
    message += "\r\n";
    try {
        send_all(static_cast<std::intptr_t>(socket_handle), message);
        std::string response;
        std::array<char, 4096> buffer{};
        for (;;) {
#ifdef _WIN32
            const auto count = recv(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
            const auto count = recv(socket_handle, buffer.data(), buffer.size(), 0);
#endif
            if (count <= 0) break;
            response.append(buffer.data(), static_cast<std::size_t>(count));
        }
        close_socket(static_cast<std::intptr_t>(socket_handle));
        const auto separator = response.find("\r\n\r\n");
        if (separator == std::string::npos) throw std::runtime_error("invalid service response");
        const auto status_end = response.find(' ');
        const auto status = status_end == std::string::npos ? 0 : std::stoi(response.substr(status_end + 1, 3));
        if (status < 200 || status >= 300) {
            const auto body = response.substr(separator + 4);
            const auto error = Json::parse(body, nullptr, false);
            if (error.is_object() && error.contains("error") && error.at("error").is_string())
                throw std::runtime_error("service returned HTTP " + std::to_string(status) + ": " +
                                         error.at("error").get<std::string>());
            throw std::runtime_error("service returned HTTP " + std::to_string(status));
        }
        return response.substr(separator + 4);
    } catch (...) {
        close_socket(static_cast<std::intptr_t>(socket_handle));
        throw;
    }
}

Json status_for(const std::filesystem::path &path) {
    const auto record = streamfind::service::ServiceDiscovery::read(path);
    Json result = record.to_json();
    result.erase("token");
    result["record_path"] = path.string();
    try {
        const auto session = Json::parse(request(parse_endpoint(record.endpoint), "GET", "/health",
                                                 {"X-StreamFind-Token: " + record.token}));
        const auto serving_instance = session.value("service_instance", Json::object())
                                          .value("service_instance_id", std::string{});
        if (!serving_instance.empty() && serving_instance != record.service_instance_id)
            throw std::runtime_error("endpoint is serving service instance " + serving_instance +
                                     ", not " + record.service_instance_id);
        result["health"] = session.value("health", std::string{"healthy"});
        result["state"] = session.value("state", std::string{"ready"});
        result["active_workers"] = session.value("active_workers", std::size_t{0});
        result["active_workflows"] = session.value("active_workflows", std::size_t{0});
        result["alive"] = session.value("alive", true);
        result["ready"] = session.value("ready", true);
        Json attached_projects = Json::array();
        for (std::size_t index = 0; index < session.value("attached_projects", std::size_t{0}); ++index)
            attached_projects.push_back(Json::object());
        result["session"] = Json{{"projects", std::move(attached_projects)}};
    } catch (const std::exception &error) {
        result["health"] = "unreachable";
        result["diagnostic"] = error.what();
    }
    return result;
}

std::vector<Json> services() {
    std::vector<Json> result;
    for (const auto &path : streamfind::service::ServiceDiscovery::record_paths()) {
        try { result.push_back(status_for(path)); }
        catch (const std::exception &error) {
            result.push_back(Json{{"health", "invalid"}, {"record_path", path.string()}, {"diagnostic", error.what()}});
        }
    }
    return result;
}

std::filesystem::path package_root(const char *executable) {
    auto root = std::filesystem::absolute(executable).parent_path();
    if (root.filename() == "bin") root = root.parent_path();
    return root;
}

std::filesystem::path tray_executable(const char *executable) {
    const auto root = package_root(executable);
#ifdef _WIN32
    const auto packaged = root / "bin" / "streamfind-tray.exe";
    const auto development = root / "streamfind-tray.exe";
#else
    const auto packaged = root / "bin" / "streamfind-tray";
    const auto development = root / "streamfind-tray";
#endif
    if (std::filesystem::exists(packaged)) return packaged;
    if (std::filesystem::exists(development)) return development;
    throw std::runtime_error("streamfind-tray executable is missing");
}

void start_tray_process(const char *executable) {
    const auto tray = tray_executable(executable);
#ifdef _WIN32
    const auto result = ShellExecuteA(nullptr, "open", tray.string().c_str(), nullptr,
                                      tray.parent_path().string().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32)
        throw std::runtime_error("unable to start streamfind-tray");
#else
    const auto child = fork();
    if (child < 0) throw std::runtime_error("unable to start streamfind-tray");
    if (child == 0) {
        setsid();
        execl(tray.c_str(), tray.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
#endif
}

std::uint16_t configured_service_port(const char *executable) {
    const auto root = package_root(executable);
    const auto packaged = root / "bin" / "streamfind.json";
    const auto development = root / "streamfind.json";
    const auto configuration = std::filesystem::exists(packaged) ? packaged : development;
    return streamfind::load_service_configuration(configuration).port;
}

std::vector<Json> wait_for_service(const char *executable, std::uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        try {
            const auto session = Json::parse(request(
                parse_endpoint("http://127.0.0.1:" + std::to_string(port)), "GET", "/session"));
            if (session.value("state", "") == "ready") return services();
        } catch (...) { }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("streamfind-server did not become ready");
}

void print_services(const std::vector<Json> &values, bool json) {
    if (json) {
        Json output = Json::array();
        for (const auto &value : values) output.push_back(value);
        std::cout << output.dump(2) << '\n';
        return;
    }
    if (values.empty()) {
        std::cout << "streamfind service: not running\n";
        return;
    }
    for (const auto &value : values) {
        std::cout << "streamfind service: " << value.value("health", "unknown") << '\n'
                  << "  Instance: " << value.value("service_instance_id", "unknown") << '\n'
                  << "  PID: " << value.value("process_id", std::uint64_t{0}) << '\n'
                  << "  Manager PID: " << value.value("manager_process_id", std::uint64_t{0}) << '\n'
                  << "  Endpoint: " << value.value("endpoint", "unknown") << '\n'
                  << "  State: " << value.value("state", "unknown") << '\n'
                  << "  Active workers: " << value.value("active_workers", std::size_t{0}) << '\n'
                  << "  Active workflows: " << value.value("active_workflows", std::size_t{0}) << '\n';
    }
}

}  // namespace streamfind::service_cli::detail

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    const auto finish = [](int code) {
#ifdef _WIN32
        WSACleanup();
#endif
        return code;
    };
    try {
        if (argc < 3 || std::string(argv[1]) != "service") {
            std::cout << "Usage: streamfind service start [--port <port>] [--json]\n"
                         "       streamfind service list|status [--json]\n"
                         "       streamfind service stop [--instance <id>]\n"
                         "       streamfind service restart [--instance <id>]\n"
                         "       streamfind service tray\n";
            return finish(argc < 2 ? 0 : 2);
        }
        const std::string command = argv[2];
        bool json = false;
        std::string instance;
        std::uint16_t port = streamfind::service_cli::detail::configured_service_port(argv[0]);
        for (int index = 3; index < argc; ++index) {
            if (std::string(argv[index]) == "--json") json = true;
            else if (std::string(argv[index]) == "--instance" && index + 1 < argc) instance = argv[++index];
            else if (std::string(argv[index]) == "--port" && index + 1 < argc) {
                const auto parsed = std::stoul(argv[++index]);
                if (parsed > 65535 || parsed == 0) throw std::runtime_error("service start has an invalid port");
                port = static_cast<std::uint16_t>(parsed);
            }
            else throw std::runtime_error("unknown service option");
        }
        const auto values = streamfind::service_cli::detail::services();
        if (command == "start") {
            for (const auto &value : values) {
                if (value.value("health", "") == "healthy") {
                    if (value.value("endpoint", "") !=
                        "http://127.0.0.1:" + std::to_string(port))
                        throw std::runtime_error("a streamfind service is already running on another endpoint");
                    if (value.value("manager_process_id", std::uint64_t{0}) != 0) {
                        streamfind::service_cli::detail::start_tray_process(argv[0]);
                        const auto refreshed = streamfind::service_cli::detail::wait_for_service(argv[0], port);
                        streamfind::service_cli::detail::print_services(refreshed, json);
                        return finish(0);
                    }
                    streamfind::service::stop_service(value.value("service_instance_id", std::string{}));
                    for (int attempt = 0; attempt < 100 &&
                         !streamfind::service::ServiceDiscovery::record_paths().empty(); ++attempt)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }
            }
            streamfind::service_cli::detail::start_tray_process(argv[0]);
            const auto ready = streamfind::service_cli::detail::wait_for_service(argv[0], port);
            streamfind::service_cli::detail::print_services(ready, json);
            return finish(0);
        }
        if (command == "list" || command == "status") {
            if (command == "status" && !instance.empty()) {
                std::vector<streamfind::Json> selected;
                for (const auto &value : values)
                    if (value.value("service_instance_id", "") == instance) selected.push_back(value);
                streamfind::service_cli::detail::print_services(selected, json);
                return finish(selected.empty() ? 1 : 0);
            }
            streamfind::service_cli::detail::print_services(values, json);
            return finish(0);
        }
        if (command == "stop") {
            const auto paths = streamfind::service::ServiceDiscovery::record_paths();
            if (instance.empty()) {
                std::vector<streamfind::Json> healthy;
                for (const auto &value : values)
                    if (value.value("health", "") == "healthy") healthy.push_back(value);
                if (healthy.empty()) throw std::runtime_error("no healthy streamfind service is running");
                if (healthy.size() > 1)
                    throw std::runtime_error("multiple streamfind services are running; use --instance <id>");
                instance = healthy.front().value("service_instance_id", "");
            }
            for (const auto &path : paths) {
                const auto record = streamfind::service::ServiceDiscovery::read(path);
                if (record.service_instance_id != instance) continue;
                const auto matching = std::find_if(values.begin(), values.end(), [&](const auto &value) {
                    return value.value("service_instance_id", "") == instance &&
                           value.value("health", "") == "healthy";
                });
                if (matching == values.end())
                    throw std::runtime_error("service instance is not healthy or its discovery token is stale");
                const auto response = streamfind::service_cli::detail::request(
                    streamfind::service_cli::detail::parse_endpoint(record.endpoint), "POST", "/service/stop",
                    {"X-StreamFind-Token: " + record.token});
                const auto manager_process_id = record.manager_process_id;
                bool stopped = false;
                for (int attempt = 0; attempt < 200; ++attempt) {
                    const auto current_paths = streamfind::service::ServiceDiscovery::record_paths();
                    const auto still_running = std::any_of(current_paths.begin(), current_paths.end(),
                        [&](const auto &current_path) {
                            try {
                                return streamfind::service::ServiceDiscovery::read(current_path)
                                           .service_instance_id == instance;
                            } catch (...) { return false; }
                        });
                    if (!still_running) { stopped = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!stopped)
                    throw std::runtime_error("streamfind service did not stop within 20 seconds");
                for (int attempt = 0; manager_process_id != 0 && attempt < 50; ++attempt) {
                    if (!streamfind::service_cli::detail::process_is_running(manager_process_id)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (manager_process_id != 0 &&
                    streamfind::service_cli::detail::process_is_running(manager_process_id))
                    throw std::runtime_error("streamfind tray manager did not exit after service shutdown");
                std::cout << response << '\n';
                return finish(0);
            }
            throw std::runtime_error("service instance not found");
        }
        if (command == "restart") {
            if (instance.empty()) {
                if (values.size() != 1) throw std::runtime_error(values.empty()
                    ? "no streamfind service is running"
                    : "multiple streamfind services are running; use --instance <id>");
                instance = values.front().value("service_instance_id", "");
            }
            streamfind::service::stop_service(instance);
            for (int attempt = 0; attempt < 50 && !streamfind::service::ServiceDiscovery::record_paths().empty(); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            streamfind::service_cli::detail::start_tray_process(argv[0]);
            const auto ready = streamfind::service_cli::detail::wait_for_service(argv[0], port);
            streamfind::service_cli::detail::print_services(ready, json);
            return finish(0);
        }
        if (command == "tray") {
            if (!instance.empty()) throw std::runtime_error("service tray does not accept --instance");
            streamfind::service_cli::detail::start_tray_process(argv[0]);
            const auto ready = streamfind::service_cli::detail::wait_for_service(argv[0], port);
            streamfind::service_cli::detail::print_services(ready, json);
            std::cout << "streamfind tray manager started.\n";
            return finish(0);
        }
        throw std::runtime_error("unknown service command: " + command);
    } catch (const std::exception &error) {
        std::cerr << "streamfind service: " << error.what() << '\n';
        return finish(1);
    }
}
