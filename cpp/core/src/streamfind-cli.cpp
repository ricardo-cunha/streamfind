#include "streamfind/service/service_discovery.hpp"

#include <cstdlib>
#include "streamfind/service_configuration.hpp"
#include <array>
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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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

void set_socket_timeouts(std::intptr_t socket) {
#ifdef _WIN32
    const DWORD timeout = 500;
    setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    const timeval timeout{0, 500000};
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
        if (status < 200 || status >= 300)
            throw std::runtime_error("service returned HTTP " + std::to_string(status));
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
        const auto session = Json::parse(request(parse_endpoint(record.endpoint), "GET", "/session"));
        result["health"] = "healthy";
        result["session"] = session;
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

std::filesystem::path service_executable(const char *executable) {
    auto root = package_root(executable);
#ifdef _WIN32
    const auto packaged = root / "bin" / "streamfind-service.exe";
    const auto development = root / "streamfind-service.exe";
#else
    const auto packaged = root / "bin" / "streamfind-service";
    const auto development = root / "streamfind-service";
#endif
    if (std::filesystem::exists(packaged)) return packaged;
    if (std::filesystem::exists(development)) return development;
    throw std::runtime_error("streamfind-service executable is missing");
}

std::uint16_t configured_service_port(const char *executable) {
    const auto root = package_root(executable);
    const auto packaged = root / "bin" / "streamfind.json";
    const auto development = root / "streamfind.json";
    const auto configuration = std::filesystem::exists(packaged) ? packaged : development;
    return streamfind::load_service_configuration(configuration).port;
}

void start_service_process(const char *executable, std::uint16_t port) {
    const auto service = service_executable(executable);
#ifdef _WIN32
    const auto root = package_root(executable);
    const auto runtime_root = root / "bin";
    const auto vendor_path = runtime_root.string();
    const char *existing_path = std::getenv("PATH");
    std::string process_path = vendor_path;
    if (existing_path != nullptr && *existing_path != '\0') process_path += ";" + std::string(existing_path);
    if (_putenv_s("PATH", process_path.c_str()) != 0)
        throw std::runtime_error("unable to configure streamfind runtime paths");
    std::string command = "\"" + service.string() + "\" " + std::to_string(port);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, service.parent_path().string().c_str(), &startup, &process))
        throw std::runtime_error("unable to start streamfind-service");
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
#else
    const auto child = fork();
    if (child < 0) throw std::runtime_error("unable to start streamfind-service");
    if (child == 0) {
        setsid();
        const auto port_text = std::to_string(port);
        execl(service.c_str(), service.c_str(), port_text.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
#endif
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
    throw std::runtime_error("streamfind-service did not become ready");
}

void print_services(const std::vector<Json> &values, bool json) {
    if (json) {
        Json output = Json::array();
        for (const auto &value : values) output.push_back(value);
        std::cout << output.dump(2) << '\n';
        return;
    }
    if (values.empty()) {
        std::cout << "StreamFind service: not running\n";
        return;
    }
    for (const auto &value : values) {
        std::cout << "StreamFind service: " << value.value("health", "unknown") << '\n'
                  << "  Instance: " << value.value("service_instance_id", "unknown") << '\n'
                  << "  PID: " << value.value("process_id", std::uint64_t{0}) << '\n'
                  << "  Endpoint: " << value.value("endpoint", "unknown") << '\n'
                  << "  State: " << value.value("state", "unknown") << '\n';
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
                         "       streamfind service stop [--instance <id>]\n";
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
                    streamfind::service_cli::detail::print_services(values, json);
                    return finish(0);
                }
            }
            streamfind::service_cli::detail::start_service_process(argv[0], port);
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
                if (paths.empty()) throw std::runtime_error("no streamfind service is running");
                if (paths.size() > 1)
                    throw std::runtime_error("multiple streamfind services are running; use --instance <id>");
                instance = streamfind::service::ServiceDiscovery::read(paths.front()).service_instance_id;
            }
            for (const auto &path : paths) {
                const auto record = streamfind::service::ServiceDiscovery::read(path);
                if (record.service_instance_id != instance) continue;
                const auto response = streamfind::service_cli::detail::request(
                    streamfind::service_cli::detail::parse_endpoint(record.endpoint), "POST", "/service/stop",
                    {"X-StreamFind-Token: " + record.token});
                std::cout << response << '\n';
                return finish(0);
            }
            throw std::runtime_error("service instance not found");
        }
        throw std::runtime_error("unknown service command: " + command);
    } catch (const std::exception &error) {
        std::cerr << "streamfind service: " << error.what() << '\n';
        return finish(1);
    }
}
