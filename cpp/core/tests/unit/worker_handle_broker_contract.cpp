#include "streamfind/service/worker_handle_broker.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace streamfind::service;

int main() {
#ifdef _WIN32
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, nullptr, 0)) return 10;
    const char expected = 'x';
    DWORD written = 0;
    if (!WriteFile(pipe_write, &expected, 1, &written, nullptr) || written != 1) return 11;
    const auto worker_handle = duplicate_worker_handle(pipe_read, GetCurrentProcessId(),
                                                       WorkerHandleAccess::ReadOnly);
    HANDLE received = accept_duplicated_worker_handle(worker_handle);
    char actual = 0;
    DWORD read = 0;
    const bool transferred = ReadFile(received, &actual, 1, &read, nullptr) && read == 1 && actual == expected;
    CloseHandle(received); CloseHandle(pipe_read); CloseHandle(pipe_write);
    if (!transferred) return 12;
#else
    int channel[2]{};
    int payload_pipe[2]{};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) != 0 || pipe(payload_pipe) != 0) return 10;
    const char expected = 'x';
    if (::write(payload_pipe[1], &expected, 1) != 1) return 11;
    send_worker_handle(channel[0], payload_pipe[0], "grant-test");
    const auto received = receive_worker_handle(channel[1]);
    char actual = 0;
    const bool transferred = received.grant_token == "grant-test" &&
                             ::read(received.handle, &actual, 1) == 1 && actual == expected;
    close(received.handle); close(payload_pipe[0]); close(payload_pipe[1]);
    close(channel[0]); close(channel[1]);
    if (!transferred) return 12;
#endif
    WorkerHandleBroker broker;
    const auto endpoint_pid = static_cast<std::uint64_t>(
#ifdef _WIN32
        GetCurrentProcessId()
#else
        getpid()
#endif
    );
    WorkerHandleBrokerEndpoint endpoint("worker-endpoint-test", endpoint_pid);
    const auto endpoint_client = connect_worker_handle_broker(endpoint.address());
    if (!endpoint.accept_authenticated(std::chrono::seconds(1)) || !endpoint.connected()) return 13;
    WorkerHandleGrant endpoint_grant;
    endpoint_grant.worker_id = "worker-endpoint-test";
    endpoint_grant.process_id = endpoint_pid;
    endpoint_grant.session_id = "session-endpoint";
    endpoint_grant.run_id = "run-endpoint";
    endpoint_grant.operation_id = "operation-endpoint";
    endpoint_grant.result_id = "result-endpoint";
    endpoint_grant.lease_id = "lease-endpoint";
    endpoint_grant.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto endpoint_issued = broker.issue(endpoint_grant);
#ifdef _WIN32
    HANDLE endpoint_read = nullptr, endpoint_write = nullptr;
    if (!CreatePipe(&endpoint_read, &endpoint_write, nullptr, 0)) return 14;
    const char endpoint_expected = 'g';
    DWORD endpoint_written = 0;
    if (!WriteFile(endpoint_write, &endpoint_expected, 1, &endpoint_written, nullptr) || endpoint_written != 1)
        return 15;
    endpoint.transfer_granted_handle(broker, endpoint_issued.token, "session-endpoint", "run-endpoint",
        "operation-endpoint", "result-endpoint", "lease-endpoint", WorkerHandleAccess::ReadOnly,
        reinterpret_cast<std::uintptr_t>(endpoint_read));
    const auto endpoint_received = receive_worker_handle(endpoint_client);
    char endpoint_actual = 0;
    DWORD endpoint_read_count = 0;
    const bool endpoint_transferred = endpoint_received.grant_token == endpoint_issued.token &&
        ReadFile(endpoint_received.handle, &endpoint_actual, 1, &endpoint_read_count, nullptr) &&
        endpoint_read_count == 1 && endpoint_actual == endpoint_expected;
    CloseHandle(endpoint_received.handle); CloseHandle(endpoint_read); CloseHandle(endpoint_write);
    CloseHandle(endpoint_client);
    if (!endpoint_transferred) return 16;
#else
    int endpoint_pipe[2]{};
    if (pipe(endpoint_pipe) != 0) return 14;
    const char endpoint_expected = 'g';
    if (::write(endpoint_pipe[1], &endpoint_expected, 1) != 1) return 15;
    endpoint.transfer_granted_handle(broker, endpoint_issued.token, "session-endpoint", "run-endpoint",
        "operation-endpoint", "result-endpoint", "lease-endpoint", WorkerHandleAccess::ReadOnly,
        static_cast<std::uintptr_t>(endpoint_pipe[0]));
    const auto endpoint_received = receive_worker_handle(endpoint_client);
    char endpoint_actual = 0;
    const bool endpoint_transferred = endpoint_received.grant_token == endpoint_issued.token &&
        ::read(endpoint_received.handle, &endpoint_actual, 1) == 1 && endpoint_actual == endpoint_expected;
    close(endpoint_received.handle); close(endpoint_pipe[0]); close(endpoint_pipe[1]);
    close(endpoint_client);
    if (!endpoint_transferred) return 16;
#endif
    broker.revoke(endpoint_issued.token);

    WorkerHandleGrant grant;
    grant.worker_id = "worker-a";
    grant.process_id = 41;
    grant.session_id = "session-a";
    grant.run_id = "run-a";
    grant.operation_id = "node-a";
    grant.result_id = "result-a";
    grant.lease_id = "lease-a";
    grant.access = WorkerHandleAccess::ReadOnly;
    grant.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto issued = broker.issue(grant);
    const auto rejects = [&](auto mutate, WorkerHandleAccess access = WorkerHandleAccess::ReadOnly) {
        std::string worker = "worker-a", session = "session-a", run = "run-a";
        std::string operation = "node-a", result = "result-a", lease = "lease-a";
        std::uint64_t pid = 41;
        mutate(worker, pid, session, run, operation, result, lease);
        try {
            static_cast<void>(broker.validate(issued.token, worker, pid, session, run,
                operation, result, lease, access));
            return false;
        } catch (const std::invalid_argument &) { return true; }
    };
    if (!rejects([](auto &w, auto &, auto &, auto &, auto &, auto &, auto &) { w = "worker-b"; }) ||
        !rejects([](auto &, auto &pid, auto &, auto &, auto &, auto &, auto &) { pid = 42; }) ||
        !rejects([](auto &, auto &, auto &s, auto &, auto &, auto &, auto &) { s = "session-b"; }) ||
        !rejects([](auto &, auto &, auto &, auto &, auto &, auto &, auto &l) { l = "lease-b"; }) ||
        !rejects([](auto &, auto &, auto &, auto &, auto &, auto &, auto &) {}, WorkerHandleAccess::WriteOnly))
        return 2;

    const auto accepted = broker.validate(issued.token, "worker-a", 41, "session-a", "run-a",
        "node-a", "result-a", "lease-a", WorkerHandleAccess::ReadOnly);
    if (accepted.token != issued.token || broker.active_grants() != 0) return 1;
    try {
        static_cast<void>(broker.validate(issued.token, "worker-a", 41, "session-a", "run-a",
            "node-a", "result-a", "lease-a", WorkerHandleAccess::ReadOnly));
        return 6;
    } catch (const std::invalid_argument &) {}

    broker.revoke(issued.token);
    try {
        static_cast<void>(broker.validate(issued.token, "worker-a", 41, "session-a", "run-a",
            "node-a", "result-a", "lease-a", WorkerHandleAccess::ReadOnly));
        return 3;
    } catch (const std::invalid_argument &) {}

    auto expired = grant;
    expired.expires_at = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto expired_grant = broker.issue([&] { expired.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(1); return expired; }());
    // Expiry is checked at validation time; issue a grant then let its lease expire.
    auto short_lived = grant;
    short_lived.expires_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    const auto timed = broker.issue(short_lived);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    try {
        static_cast<void>(broker.validate(timed.token, "worker-a", 41, "session-a", "run-a",
            "node-a", "result-a", "lease-a", WorkerHandleAccess::ReadOnly));
        return 4;
    } catch (const std::invalid_argument &) {}
    broker.revoke_worker("worker-a");
    if (broker.active_grants() != 0 || expired_grant.token.empty()) return 5;
    return 0;
}
