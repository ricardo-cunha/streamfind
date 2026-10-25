#include "streamfind/service_runtime_client.hpp"

#include "streamfind/service/service_discovery.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace streamfind::service {
namespace detail {

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

Json request(const Endpoint &endpoint, const std::string &method,
             const std::string &path, const Json &payload = Json::object(),
             const std::vector<std::string> &headers = {}) {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        throw std::runtime_error("service connection initialization failed");
    const auto cleanup = [] { WSACleanup(); };
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) { cleanup(); throw std::runtime_error("unable to create service connection"); }
#else
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) throw std::runtime_error("unable to create service connection");
#endif
    bool socket_closed = false;
    const auto close = [&] {
        if (socket_closed) return;
        close_socket(static_cast<std::intptr_t>(socket_handle));
#ifdef _WIN32
        cleanup();
#endif
        socket_closed = true;
    };
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1 ||
        connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close();
        throw std::runtime_error("service did not respond");
    }
    const auto body = method == "POST" ? payload.dump() : std::string{};
    std::string message = method + " " + path + " HTTP/1.1\r\nHost: " + endpoint.host + ":" +
                          std::to_string(endpoint.port) +
                          "\r\nConnection: close\r\n";
    for (const auto &header : headers) message += header + "\r\n";
    if (method == "POST")
        message += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    message += "\r\n" + body;
    try {
        send_all(static_cast<std::intptr_t>(socket_handle), message);
        std::string response;
        std::array<char, 8192> buffer{};
        for (;;) {
#ifdef _WIN32
            const auto count = recv(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
            const auto count = recv(socket_handle, buffer.data(), buffer.size(), 0);
#endif
            if (count <= 0) break;
            response.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const auto separator = response.find("\r\n\r\n");
        if (separator == std::string::npos) throw std::runtime_error("invalid service response");
        const auto status_end = response.find(' ');
        const auto status = status_end == std::string::npos ? 0 : std::stoi(response.substr(status_end + 1, 3));
        const auto response_body = response.substr(separator + 4);
        if (status < 200 || status >= 300) {
            const auto error = Json::parse(response_body, nullptr, false);
            throw std::runtime_error(error.is_object() ? error.value("error", "service request failed")
                                                        : "service returned HTTP " + std::to_string(status));
        }
        const auto parsed = response_body.empty() ? Json::object() : Json::parse(response_body);
        close();
        return parsed;
    } catch (...) {
        close();
        throw;
    }
}

std::string create_session_id() {
    return "client-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace detail

Json list_services() {
    Json services = Json::array();
    for (const auto &path : ServiceDiscovery::record_paths()) {
        try {
            const auto record = ServiceDiscovery::read(path);
            Json result = record.to_json();
            result.erase("token");
            result["record_path"] = path.string();
            try {
                const auto session = detail::request(detail::parse_endpoint(record.endpoint), "GET", "/session",
                                                     Json::object(), {"X-StreamFind-Token: " + record.token});
                result["health"] = "healthy";
                result["session"] = session;
                if (session.contains("projects")) result["projects"] = session.at("projects");
            } catch (const std::exception &error) {
                result["health"] = "unreachable";
                result["diagnostic"] = error.what();
            }
            services.push_back(std::move(result));
        } catch (const std::exception &error) {
            services.push_back(Json{{"health", "invalid"}, {"record_path", path.string()},
                                    {"diagnostic", error.what()}});
        }
    }
    return services;
}

Json stop_service(const std::string &instance_id) {
    const auto paths = ServiceDiscovery::record_paths();
    std::filesystem::path selected;
    for (const auto &path : paths) {
        const auto record = ServiceDiscovery::read(path);
        if (instance_id.empty() || record.service_instance_id == instance_id) {
            if (!selected.empty()) throw std::runtime_error("multiple streamfind services are running; specify an instance ID");
            selected = path;
        }
    }
    if (selected.empty()) throw std::runtime_error("no streamfind service is running");
    const auto record = ServiceDiscovery::read(selected);
    return detail::request(detail::parse_endpoint(record.endpoint), "POST", "/service/stop",
                           Json{{"instance_id", record.service_instance_id}},
                           {"X-StreamFind-Token: " + record.token});
}

ServiceProjectAttachment attach_project(const std::filesystem::path &database_path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(database_path, error).string();
    std::string last_error;
    struct Candidate { std::string instance_id; std::string endpoint; };
    std::vector<Candidate> candidates;
    if (const auto *override_endpoint = std::getenv("STREAMFIND_SERVICE_ENDPOINT");
        override_endpoint != nullptr && *override_endpoint != '\0') {
        candidates.push_back({"explicit-endpoint", override_endpoint});
    } else {
        for (const auto &path : ServiceDiscovery::record_paths()) {
            try {
                const auto record = ServiceDiscovery::read(path);
                if (record.state == "ready")
                    candidates.push_back({record.service_instance_id, record.endpoint});
            } catch (const std::exception &caught) {
                last_error = caught.what();
            }
        }
    }
    for (const auto &candidate : candidates) {
        try {
            const auto endpoint = detail::parse_endpoint(candidate.endpoint);
            const auto projects = detail::request(endpoint, "GET", "/projects");
            for (const auto &project : projects.value("projects", Json::array())) {
                std::error_code project_error;
                if (std::filesystem::weakly_canonical(project.value("database_path", ""), project_error).string() == canonical)
                    return {candidate.instance_id, candidate.endpoint, endpoint.port,
                            project.value("session_id", std::string{})};
            }
            const auto response = detail::request(endpoint, "POST", "/projects",
                Json{{"mode", "open"}, {"session_id", detail::create_session_id()},
                     {"database_path", database_path.string()}});
            return {candidate.instance_id, candidate.endpoint, endpoint.port,
                    response.value("session_id", std::string{})};
        } catch (const std::exception &caught) {
            last_error = caught.what();
        }
    }
    throw std::runtime_error(last_error.empty()
        ? "no running streamfind service is available"
        : "unable to attach project to streamfind service: " + last_error);
}

ServiceProjectAttachment create_project(const std::filesystem::path &database_path,
                                        const data_proxy::Json &metadata) {
    std::string last_error;
    struct Candidate { std::string instance_id; std::string endpoint; };
    std::vector<Candidate> candidates;
    if (const auto *override_endpoint = std::getenv("STREAMFIND_SERVICE_ENDPOINT");
        override_endpoint != nullptr && *override_endpoint != '\0') {
        candidates.push_back({"explicit-endpoint", override_endpoint});
    } else {
        for (const auto &path : ServiceDiscovery::record_paths()) {
            try {
                const auto record = ServiceDiscovery::read(path);
                if (record.state == "ready")
                    candidates.push_back({record.service_instance_id, record.endpoint});
            } catch (const std::exception &caught) { last_error = caught.what(); }
        }
    }
    for (const auto &candidate : candidates) {
        try {
            const auto endpoint = detail::parse_endpoint(candidate.endpoint);
            const auto response = detail::request(endpoint, "POST", "/projects",
                data_proxy::Json{{"mode", "create"},
                                 {"session_id", detail::create_session_id()},
                                 {"database_path", database_path.string()},
                                 {"metadata", metadata}});
            return {candidate.instance_id, candidate.endpoint, endpoint.port,
                    response.value("session_id", std::string{})};
        } catch (const std::exception &caught) { last_error = caught.what(); }
    }
    throw std::runtime_error(last_error.empty()
        ? "no running streamfind service is available"
        : "unable to create project in streamfind service: " + last_error);
}

data_proxy::Json get_workflow(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    const auto response = detail::request(endpoint, "GET",
                                          "/projects/" + attachment.session_id + "/workflow");
    if (!response.contains("workflow"))
        throw std::runtime_error("service workflow response is missing its definition");
    return response.at("workflow");
}

data_proxy::Json resolve_workflow_inputs(const ServiceProjectAttachment &attachment,
                                         const std::string &operation_id) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    if (operation_id.empty()) throw std::invalid_argument("operation_id is required");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/workflow/inputs",
                           data_proxy::Json{{"operation_id", operation_id}});
}

data_proxy::Json start_workflow(const ServiceProjectAttachment &attachment,
                                const std::string &execution_mode) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/workflow/run",
                           data_proxy::Json{{"execution_mode", execution_mode}});
}

data_proxy::Json workflow_snapshot(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "GET",
                           "/projects/" + attachment.session_id + "/workflow/state");
}

data_proxy::Json cancel_workflow(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/workflow/cancel");
}

data_proxy::Json cancel_operation(const ServiceProjectAttachment &attachment,
                                  const std::string &operation_instance) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    if (operation_instance.empty()) throw std::invalid_argument("operation_instance is required");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/operations/" + operation_instance + "/cancel");
}

data_proxy::Json close_project(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "DELETE",
                           "/projects/" + attachment.session_id);
}

data_proxy::Json run_operation(const ServiceProjectAttachment &attachment,
                               const std::string &operation_id,
                               const data_proxy::Json &parameters,
                               const std::string &operation_instance,
                               const data_proxy::Json &inputs) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    if (operation_id.empty()) throw std::invalid_argument("operation_id is required");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    data_proxy::Json payload = data_proxy::Json::object();
    if (parameters.is_object())
        payload["parameters"] = parameters;
    else
        payload["parameters"] = data_proxy::Json::object();
    if (!operation_instance.empty()) payload["operation_instance"] = operation_instance;
    if (!inputs.is_null()) payload["inputs"] = inputs;
    const auto response = detail::request(endpoint, "POST",
                                           "/projects/" + attachment.session_id + "/operations/" + operation_id,
                                           payload);
    return response.value("result", response);
}

data_proxy::Json validate_workflow(const ServiceProjectAttachment &attachment,
                                   const data_proxy::Json &workflow) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    data_proxy::Json payload = data_proxy::Json::object();
    if (!workflow.is_null()) payload["workflow"] = workflow;
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/workflow/validate", payload);
}

data_proxy::Json save_workflow(const ServiceProjectAttachment &attachment,
                               const data_proxy::Json &workflow) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/workflow", workflow);
}

data_proxy::Json get_project(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    const auto response = detail::request(endpoint, "GET", "/projects");
    for (const auto &project : response.value("projects", data_proxy::Json::array()))
        if (project.value("session_id", std::string{}) == attachment.session_id) return project;
    throw std::runtime_error("service project session disappeared");
}

data_proxy::Json set_metadata(const ServiceProjectAttachment &attachment,
                              const data_proxy::Json &metadata) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/metadata", metadata);
}

data_proxy::Json get_audit_trail(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "GET",
                           "/projects/" + attachment.session_id + "/audit");
}

data_proxy::Json copy_project(const ServiceProjectAttachment &attachment,
                              const std::filesystem::path &destination_database_path) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/copy",
                           data_proxy::Json{{"destination_database_path", destination_database_path.string()}});
}

data_proxy::Json get_results(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "GET", "/projects/" + attachment.session_id + "/results")
        .value("results", data_proxy::Json::array());
}

data_proxy::Json get_current_results(const ServiceProjectAttachment &attachment) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "GET", "/projects/" + attachment.session_id + "/results/current")
        .value("results", data_proxy::Json::array());
}

data_proxy::Json get_result_data(const ServiceProjectAttachment &attachment,
                                 const data_proxy::Json &request) {
    if (attachment.port == 0 || attachment.session_id.empty())
        throw std::invalid_argument("service project attachment is incomplete");
    const auto endpoint = detail::parse_endpoint(attachment.endpoint);
    return detail::request(endpoint, "POST",
                           "/projects/" + attachment.session_id + "/results/data", request);
}

}  // namespace streamfind::service
