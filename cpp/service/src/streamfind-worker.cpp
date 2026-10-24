#include "streamfind/service/service_plugin_runtime.hpp"
#include "streamfind/vendor_runtime.hpp"
#include "streamfind/data_proxy_protocol.hpp"
#include "streamfind/data_proxy_output_sink.hpp"
#include "streamfind/operation_execution_context.hpp"

#include <chrono>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

std::string proxy_request_id() {
    static std::atomic_uint64_t counter{0};
    return "proxy-" + std::to_string(++counter) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string percent_encode_path(const std::string &value) {
    std::ostringstream result;
    result << std::hex;
    for (const auto character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) || character == '-' ||
            character == '_' || character == '.' || character == '~')
            result << character;
        else
            result << '%' << std::uppercase << std::setw(2)
                   << static_cast<int>(static_cast<unsigned char>(character)) << std::nouppercase
                   << std::setfill('0');
    }
    return result.str();
}

Json request_data_proxy(std::uint16_t port, const std::string &session_id,
                        const std::string &run_id, const std::string &operation_id,
                        const std::string &operation_instance, const std::string &lease_id,
                        const Json &payload) {
    if (port == 0 || session_id.empty())
        throw std::runtime_error("worker data-proxy endpoint is not configured");
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        throw std::runtime_error("worker data-proxy Winsock initialization failed");
    const auto cleanup = [] { WSACleanup(); };
#endif
    const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (socket_handle == INVALID_SOCKET) { cleanup(); throw std::runtime_error("worker data-proxy socket failed"); }
#else
    if (socket_handle < 0) throw std::runtime_error("worker data-proxy socket failed");
#endif
    const auto close_socket = [&] {
#ifdef _WIN32
        closesocket(socket_handle); cleanup();
#else
        close(socket_handle);
#endif
    };
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close_socket();
        throw std::runtime_error("worker could not connect to data-proxy service");
    }
    const auto envelope = data_proxy::request(proxy_request_id(), session_id, run_id, operation_id,
                                               operation_instance, lease_id, payload).to_json().dump();
    const auto path = "/projects/" + percent_encode_path(session_id) + "/data-proxy";
    const auto request = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                         "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(envelope.size()) + "\r\n\r\n" + envelope;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto count = ::send(socket_handle, request.data() + sent,
                                   static_cast<int>(request.size() - sent), 0);
        if (count <= 0) { close_socket(); throw std::runtime_error("worker data-proxy send failed"); }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[8192];
    for (;;) {
        const auto count = ::recv(socket_handle, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    close_socket();
    const auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) throw std::runtime_error("invalid data-proxy response");
    const auto status_end = response.find(' ');
    if (status_end == std::string::npos || response.substr(status_end + 1, 3) != "200")
        throw std::runtime_error("data-proxy service returned an HTTP error");
    const auto response_body = response.substr(body_start + 4);
    data_proxy::Envelope envelope_response;
    try {
        envelope_response = data_proxy::Envelope::from_json(Json::parse(response_body));
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string("invalid data-proxy response envelope: ") + error.what() +
                                 " body=" + response_body.substr(0, 512));
    }
    if (!envelope_response.error_code.empty())
        throw std::runtime_error(envelope_response.error_code + ": " + envelope_response.error_message);
    if (!envelope_response.payload.is_object())
        throw std::runtime_error("data-proxy response payload is not an object: " + envelope_response.payload.dump().substr(0, 512));
    return envelope_response.to_json();
}

}  // namespace detail

void emit_event(const Json &event) {
    std::cout << event.dump() << '\n' << std::flush;
}

}  // namespace streamfind::service

namespace streamfind::service::detail {

class WorkerExecutionContext final : public OperationExecutionContext {
public:
    WorkerExecutionContext(std::uint16_t service_port, std::string session_id,
                           std::string run_id, std::string operation_id,
                           std::string operation_instance, int workflow_revision,
                           Json inputs, std::shared_ptr<OperationOutputSink> outputs,
                           std::string computation_key)
        : service_port_(service_port), session_id_(std::move(session_id)),
          run_id_(std::move(run_id)), operation_id_(std::move(operation_id)),
          operation_instance_(std::move(operation_instance)), workflow_revision_(workflow_revision),
          inputs_(std::move(inputs)), outputs_(std::move(outputs)),
          computation_key_(std::move(computation_key)) {}

    std::string operation_instance() const override { return operation_instance_; }
    int workflow_revision() const override { return workflow_revision_; }

    Json query(std::string_view sql) override {
        const auto response = envelope(Json{{"method", "query"}, {"sql", std::string(sql)}});
        return response.payload.value("rows", Json::array());
    }
    void execute(std::string_view sql) override {
        static_cast<void>(envelope(Json{{"method", "execute"}, {"sql", std::string(sql)}}));
    }
    void prepare_table_domain(std::string_view, std::string_view) override {}
    void log(std::string_view message) override { emit_event(Json{
        {"type", "operation.log"}, {"operation_id", operation_id_}, {"message", std::string(message)}}); }
    Json resolve_inputs(const Json &provided_inputs) override { return provided_inputs.is_null() ? inputs_ : provided_inputs; }
    Json request_data(const data_proxy::Envelope &request) override {
        const auto response = detail::request_data_proxy(service_port_, session_id_, request.run_id,
            request.operation_id.empty() ? operation_id_ : request.operation_id,
            request.operation_instance.empty() ? operation_instance_ : request.operation_instance,
            request.lease_id, request.payload);
        return response;
    }
    std::string data_proxy_session_id() const override { return session_id_; }
    std::string data_proxy_run_id() const override { return run_id_; }
    Json lookup_result_cache(const std::string &) override { return Json::array(); }
    bool has_output_sink() const override { return true; }
    std::string computation_key() const override { return computation_key_; }
    Json publish_result(const std::string &, const Json &, const Json &) override {
        throw Error(ErrorCode::MethodExecution, "direct result publication is not supported; use the output sink");
    }
    OperationOutputSink &outputs() override { return *outputs_; }
    void report_progress(double fraction, std::string_view message) override {
        if (!message.empty()) emit_event(Json{{"type", "operation.log"}, {"operation_id", operation_id_},
                                              {"message", "[" + std::to_string(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100.0)) + "%] " + std::string(message)}});
    }
    void report_event(std::string_view event, const Json &payload) override {
        emit_event(Json{{"type", "operation.event"}, {"operation_id", operation_id_},
                        {"event", std::string(event)}, {"payload", payload}});
    }
    bool cancellation_requested() const override { return false; }

private:
    data_proxy::Envelope envelope(const Json &payload) const {
        const auto response = detail::request_data_proxy(service_port_, session_id_, run_id_, operation_id_,
                                                         operation_instance_, {}, payload);
        const auto parsed = data_proxy::Envelope::from_json(response);
        if (!parsed.error_code.empty())
            throw Error(ErrorCode::DatabaseError, parsed.error_code + ": " + parsed.error_message);
        return parsed;
    }

    std::uint16_t service_port_;
    std::string session_id_, run_id_, operation_id_, operation_instance_, computation_key_;
    int workflow_revision_;
    Json inputs_;
    std::shared_ptr<OperationOutputSink> outputs_;
};

}  // namespace streamfind::service::detail

int main(int argc, char **argv) {
    using namespace streamfind;
    using namespace streamfind::service;
    try {
        std::filesystem::path configuration;
        for (int index = 1; index + 1 < argc; ++index)
            if (std::string(argv[index]) == "--worker-config") configuration = argv[index + 1];
        if (configuration.empty()) throw std::runtime_error("streamfind_worker requires --worker-config");
        const auto request_line = [&] {
            std::string line;
            if (!std::getline(std::cin, line) || line.empty()) throw std::runtime_error("worker request is missing");
            return Json::parse(line);
        }();
        // The worker is a separate process from the service.  Configure the
        // package-owned DLL search paths here as well, before loading the
        // dynamic plugins.  The service process' AddDllDirectory state is
        // process-local and cannot be inherited by this worker.
        configure_vendor_runtime_paths();
        MethodRegistry methods;
        OperationRegistry operations;
        ServicePluginRuntime runtime;
        runtime.load(configuration, methods, operations);
        int exit_code = 0;
        const auto workflow = request_line.value("workflow", Json::object());
        if (!workflow.is_object() || workflow.empty())
            throw std::runtime_error("worker workflow snapshot is missing");
        if (!workflow.contains("schema_version") || !workflow.at("schema_version").is_number_integer())
            throw std::runtime_error("worker workflow snapshot has no schema_version: " + workflow.dump());
        const auto service_port = request_line.value("service_port", static_cast<std::uint16_t>(0));
        const auto session_id = request_line.value("session_id", std::string{});
        const auto operation_id = request_line.at("operation_id").get<std::string>();
        const auto operation_instance = request_line.value("operation_instance", std::string{});
        const auto run_id = request_line.value("run_id", operation_instance);
        auto output_sink = std::make_shared<DataProxyOutputSink>(
            [service_port](const data_proxy::Envelope &request) {
                return detail::request_data_proxy(service_port, request.session_id, request.run_id,
                                                  request.operation_id, request.operation_instance,
                                                  request.lease_id, request.payload);
            }, session_id, run_id, operation_id, operation_instance,
            request_line.value("persist_outputs", true));
        const auto workflow_revision = workflow.value("version", 0);
        detail::WorkerExecutionContext context(service_port, session_id, run_id, operation_id,
            operation_instance, workflow_revision, request_line.value("inputs", Json(nullptr)),
            output_sink, request_line.value("computation_key", std::string{}));
        emit_event(Json{{"type", "worker.ready"}});
        const auto *operation_definition = operations.find(operation_id);
        if (operation_definition == nullptr)
            throw std::runtime_error("operation is not registered: " + operation_id);
        const auto result = operation_definition->run_context(context,
            request_line.value("parameters", Json::object()), operation_instance,
            request_line.value("inputs", Json(nullptr)));
        emit_event(Json{{"type", "result"}, {"result", result}});
        exit_code = 0;
        return exit_code;
    } catch (const std::exception &error) {
        emit_event(Json{{"type", "error"}, {"message", error.what()}});
        return 1;
    }
}
