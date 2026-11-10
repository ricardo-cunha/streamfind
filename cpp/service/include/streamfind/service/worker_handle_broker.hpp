#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace streamfind::service {

enum class WorkerHandleAccess { ReadOnly, WriteOnly };

struct ReceivedWorkerHandle {
    std::string grant_token;
#ifdef _WIN32
    HANDLE handle{nullptr};
#else
    int handle{-1};
#endif
};

class WorkerHandleBroker;

// A private, per-worker control channel. The server authenticates the connected
// peer against the PID assigned by WorkerProcess before any grant can be used.
class WorkerHandleBrokerEndpoint {
public:
    WorkerHandleBrokerEndpoint(std::string worker_id, std::uint64_t expected_process_id);
    ~WorkerHandleBrokerEndpoint();
    WorkerHandleBrokerEndpoint(const WorkerHandleBrokerEndpoint &) = delete;
    WorkerHandleBrokerEndpoint &operator=(const WorkerHandleBrokerEndpoint &) = delete;

    const std::string &address() const;
    bool accept_authenticated(std::chrono::milliseconds timeout);
    bool connected() const;
    void transfer_granted_handle(WorkerHandleBroker &broker, const std::string &grant_token,
                                 const std::string &session_id, const std::string &run_id,
                                 const std::string &operation_id, const std::string &result_id,
                                 const std::string &lease_id, WorkerHandleAccess access,
                                 std::uintptr_t source_handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Called by streamfind_worker before loading plugins. The returned channel is
// private to the launched worker and remains open for its lifetime.
#ifdef _WIN32
HANDLE connect_worker_handle_broker(const std::string &address);
ReceivedWorkerHandle receive_worker_handle(HANDLE broker_pipe);
#else
int connect_worker_handle_broker(const std::string &address);
#endif

#ifdef _WIN32
std::uintptr_t duplicate_worker_handle(HANDLE source, std::uint64_t target_process_id,
                                       WorkerHandleAccess access);
HANDLE accept_duplicated_worker_handle(std::uintptr_t worker_local_handle);
#else
void send_worker_handle(int broker_socket, int source_fd, const std::string &grant_token);
ReceivedWorkerHandle receive_worker_handle(int broker_socket);
#endif

struct WorkerHandleGrant {
    std::string token;
    std::string worker_id;
    std::uint64_t process_id{0};
    std::string session_id;
    std::string run_id;
    std::string operation_id;
    std::string result_id;
    std::string lease_id;
    WorkerHandleAccess access{WorkerHandleAccess::ReadOnly};
    std::chrono::steady_clock::time_point expires_at;
};

// Service-owned authorization registry for worker file-handle transfers.
// OS handle/descriptor transfer is deliberately performed only after validate()
// succeeds, at the supervised process transport boundary.
class WorkerHandleBroker {
public:
    WorkerHandleGrant issue(WorkerHandleGrant grant);
    WorkerHandleGrant validate(const std::string &token,
                               const std::string &worker_id,
                               std::uint64_t process_id,
                               const std::string &session_id,
                               const std::string &run_id,
                               const std::string &operation_id,
                               const std::string &result_id,
                               const std::string &lease_id,
                               WorkerHandleAccess requested_access);
    void revoke(const std::string &token);
    void revoke_worker(const std::string &worker_id);
    std::size_t active_grants() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorkerHandleGrant> grants_;
};

}  // namespace streamfind::service
