#include "streamfind/service/worker_handle_broker.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace streamfind::service {
namespace detail {

std::string next_handle_grant_token() {
    std::random_device random;
    std::ostringstream token;
    token << "handle-" << std::hex << std::setfill('0');
    for (int index = 0; index < 8; ++index)
        token << std::setw(8) << random();
    return token.str();
}

std::string next_endpoint_name(const std::string &worker_id) {
    static_cast<void>(worker_id);
    return "streamfind-worker-" + next_handle_grant_token().substr(7);
}

bool same_scope(const WorkerHandleGrant &grant, const std::string &worker_id,
                std::uint64_t process_id, const std::string &session_id,
                const std::string &run_id, const std::string &operation_id,
                const std::string &result_id, const std::string &lease_id) {
    return grant.worker_id == worker_id && grant.process_id == process_id &&
           grant.session_id == session_id && grant.run_id == run_id &&
           grant.operation_id == operation_id && grant.result_id == result_id &&
           grant.lease_id == lease_id;
}

}  // namespace detail

struct WorkerHandleBrokerEndpoint::Impl {
    std::string address;
    std::string worker_id;
    std::uint64_t expected_process_id{0};
    bool peer_connected{false};
#ifdef _WIN32
    HANDLE listener{INVALID_HANDLE_VALUE};
    HANDLE peer{INVALID_HANDLE_VALUE};
#else
    int listener{-1};
    int peer{-1};
#endif
};

WorkerHandleBrokerEndpoint::WorkerHandleBrokerEndpoint(
    std::string worker_id, std::uint64_t expected_process_id)
    : impl_(std::make_unique<Impl>()) {
    if (worker_id.empty() || expected_process_id == 0)
        throw std::invalid_argument("worker broker endpoint requires a worker ID and process ID");
    impl_->expected_process_id = expected_process_id;
    impl_->worker_id = worker_id;
    const auto name = detail::next_endpoint_name(worker_id);
#ifdef _WIN32
    impl_->address = "\\\\.\\pipe\\" + name;
    const auto wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        impl_->address.data(), static_cast<int>(impl_->address.size()), nullptr, 0);
    if (wide_size <= 0) throw std::runtime_error("cannot encode worker broker pipe name");
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, impl_->address.data(),
        static_cast<int>(impl_->address.size()), wide.data(), wide_size);
    impl_->listener = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (impl_->listener == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create private worker broker pipe");
#else
    impl_->address = "@" + name;
    impl_->listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (impl_->listener < 0) throw std::runtime_error("cannot create private worker broker socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (name.size() + 1 > sizeof(address.sun_path)) {
        close(impl_->listener);
        impl_->listener = -1;
        throw std::runtime_error("worker broker socket name is too long");
    }
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    const auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (bind(impl_->listener, reinterpret_cast<sockaddr *>(&address), address_size) != 0 ||
        listen(impl_->listener, 1) != 0) {
        close(impl_->listener);
        impl_->listener = -1;
        throw std::runtime_error("cannot bind private worker broker socket");
    }
#endif
}

WorkerHandleBrokerEndpoint::~WorkerHandleBrokerEndpoint() {
    if (!impl_) return;
#ifdef _WIN32
    if (impl_->peer != INVALID_HANDLE_VALUE) CloseHandle(impl_->peer);
    if (impl_->listener != INVALID_HANDLE_VALUE) CloseHandle(impl_->listener);
#else
    if (impl_->peer >= 0) close(impl_->peer);
    if (impl_->listener >= 0) close(impl_->listener);
#endif
}

const std::string &WorkerHandleBrokerEndpoint::address() const { return impl_->address; }
bool WorkerHandleBrokerEndpoint::connected() const { return impl_->peer_connected; }

void WorkerHandleBrokerEndpoint::transfer_granted_handle(
    WorkerHandleBroker &broker, const std::string &grant_token,
    const std::string &session_id, const std::string &run_id,
    const std::string &operation_id, const std::string &result_id,
    const std::string &lease_id, WorkerHandleAccess access,
    std::uintptr_t source_handle) {
    if (!impl_->peer_connected)
        throw std::runtime_error("worker handle broker peer has not authenticated");
    static_cast<void>(broker.validate(grant_token, impl_->worker_id, impl_->expected_process_id,
        session_id, run_id, operation_id, result_id, lease_id, access));
#ifdef _WIN32
    if (grant_token.size() >= 4096)
        throw std::runtime_error("worker handle grant token is too long");
    const auto worker_handle = duplicate_worker_handle(reinterpret_cast<HANDLE>(source_handle),
        impl_->expected_process_id, access);
    struct TransferMessage { char token[4096]; std::uint64_t worker_handle; } message{};
    std::memcpy(message.token, grant_token.c_str(), grant_token.size() + 1);
    message.worker_handle = static_cast<std::uint64_t>(worker_handle);
    DWORD written = 0;
    if (!WriteFile(impl_->peer, &message, sizeof(message), &written, nullptr) ||
        written != sizeof(message))
        throw std::runtime_error("cannot send duplicated handle to worker");
#else
    send_worker_handle(impl_->peer, static_cast<int>(source_handle), grant_token);
#endif
}

bool WorkerHandleBrokerEndpoint::accept_authenticated(std::chrono::milliseconds timeout) {
    if (impl_->peer_connected) return true;
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) throw std::runtime_error("cannot create worker broker connection event");
    const BOOL connected = ConnectNamedPipe(impl_->listener, &overlapped);
    const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    bool ready = connected != FALSE || error == ERROR_PIPE_CONNECTED;
    if (!ready && error == ERROR_IO_PENDING) {
        const auto wait = WaitForSingleObject(overlapped.hEvent,
            static_cast<DWORD>(std::max<std::int64_t>(0, timeout.count())));
        if (wait == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            ready = GetOverlappedResult(impl_->listener, &overlapped, &transferred, FALSE) != FALSE;
        } else if (wait == WAIT_TIMEOUT) {
            CancelIoEx(impl_->listener, &overlapped);
        }
    }
    CloseHandle(overlapped.hEvent);
    if (!ready) return false;
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(impl_->listener, &client_process_id) ||
        client_process_id != impl_->expected_process_id) {
        DisconnectNamedPipe(impl_->listener);
        throw std::runtime_error("worker broker peer process identity did not match the supervised worker");
    }
    impl_->peer = impl_->listener;
    impl_->peer_connected = true;
    return true;
#else
    pollfd descriptor{impl_->listener, POLLIN, 0};
    const auto wait_ms = static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, INT_MAX));
    const int ready = poll(&descriptor, 1, wait_ms);
    if (ready == 0) return false;
    if (ready < 0) {
        if (errno == EINTR) return false;
        throw std::runtime_error("worker broker connection wait failed");
    }
    const auto peer = accept(impl_->listener, nullptr, nullptr);
    if (peer < 0) throw std::runtime_error("cannot accept worker broker connection");
    const auto descriptor_flags = fcntl(peer, F_GETFD);
    if (descriptor_flags >= 0) fcntl(peer, F_SETFD, descriptor_flags | FD_CLOEXEC);
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        static_cast<std::uint64_t>(credentials.pid) != impl_->expected_process_id) {
        close(peer);
        throw std::runtime_error("worker broker peer process identity did not match the supervised worker");
    }
    impl_->peer = peer;
    impl_->peer_connected = true;
    return true;
#endif
}

#ifdef _WIN32
HANDLE connect_worker_handle_broker(const std::string &address) {
    const auto wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        address.data(), static_cast<int>(address.size()), nullptr, 0);
    if (wide_size <= 0) throw std::runtime_error("invalid worker broker pipe address");
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, address.data(),
        static_cast<int>(address.size()), wide.data(), wide_size);
    const HANDLE pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("worker cannot connect to private broker pipe");
    SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0);
    return pipe;
}
#else
int connect_worker_handle_broker(const std::string &address) {
    if (address.size() < 2 || address.front() != '@')
        throw std::runtime_error("invalid worker broker socket address");
    const auto name = address.substr(1);
    sockaddr_un endpoint{};
    if (name.size() + 1 > sizeof(endpoint.sun_path))
        throw std::runtime_error("worker broker socket address is too long");
    const int socket_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) throw std::runtime_error("worker cannot create broker socket");
    endpoint.sun_family = AF_UNIX;
    std::memcpy(endpoint.sun_path + 1, name.data(), name.size());
    const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (::connect(socket_fd, reinterpret_cast<sockaddr *>(&endpoint), size) != 0) {
        close(socket_fd);
        throw std::runtime_error("worker cannot connect to private broker socket");
    }
    return socket_fd;
}
#endif

#ifdef _WIN32
std::uintptr_t duplicate_worker_handle(HANDLE source, std::uint64_t target_process_id,
                                       WorkerHandleAccess access) {
    if (source == nullptr || target_process_id == 0)
        throw std::invalid_argument("worker handle transfer has invalid source or process");
    HANDLE target = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(target_process_id));
    if (!target) throw std::runtime_error("cannot open worker process for handle transfer");
    HANDLE remote = nullptr;
    const DWORD desired = access == WorkerHandleAccess::ReadOnly ? GENERIC_READ : GENERIC_WRITE;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), source, target, &remote,
                                    desired, FALSE, 0);
    CloseHandle(target);
    if (!ok) throw std::runtime_error("cannot duplicate authorized handle into worker");
    return reinterpret_cast<std::uintptr_t>(remote);
}

HANDLE accept_duplicated_worker_handle(std::uintptr_t worker_local_handle) {
    if (worker_local_handle == 0) throw std::invalid_argument("worker-local handle is invalid");
    return reinterpret_cast<HANDLE>(worker_local_handle);
}

ReceivedWorkerHandle receive_worker_handle(HANDLE broker_pipe) {
    struct TransferMessage { char token[4096]; std::uint64_t worker_handle; } message{};
    DWORD received = 0;
    if (!ReadFile(broker_pipe, &message, sizeof(message), &received, nullptr) ||
        received != sizeof(message) || message.token[0] == '\0' || message.worker_handle == 0)
        throw std::runtime_error("worker handle grant message is incomplete");
    ReceivedWorkerHandle result;
    result.grant_token = message.token;
    result.handle = accept_duplicated_worker_handle(static_cast<std::uintptr_t>(message.worker_handle));
    return result;
}
#else
void send_worker_handle(int broker_socket, int source_fd, const std::string &grant_token) {
    if (broker_socket < 0 || source_fd < 0 || grant_token.empty() || grant_token.size() > 4096)
        throw std::invalid_argument("worker handle transfer metadata is invalid");
    iovec payload{const_cast<char *>(grant_token.data()), grant_token.size()};
    std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    auto *control = CMSG_FIRSTHDR(&message);
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(control), &source_fd, sizeof(source_fd));
    if (sendmsg(broker_socket, &message, 0) != static_cast<ssize_t>(grant_token.size()))
        throw std::runtime_error("cannot pass worker file descriptor");
}

ReceivedWorkerHandle receive_worker_handle(int broker_socket) {
    if (broker_socket < 0) throw std::invalid_argument("worker broker socket is invalid");
    std::array<char, 4096> payload{};
    std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    iovec data{payload.data(), payload.size()};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    const auto received = recvmsg(broker_socket, &message, MSG_CMSG_CLOEXEC);
    if (received <= 0 || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
        throw std::runtime_error("worker handle grant message is incomplete");
    auto *control = CMSG_FIRSTHDR(&message);
    if (!control || control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
        control->cmsg_len != CMSG_LEN(sizeof(int)))
        throw std::runtime_error("worker handle grant did not contain one descriptor");
    ReceivedWorkerHandle result;
    result.grant_token.assign(payload.data(), static_cast<std::size_t>(received));
    std::memcpy(&result.handle, CMSG_DATA(control), sizeof(result.handle));
    return result;
}
#endif

WorkerHandleGrant WorkerHandleBroker::issue(WorkerHandleGrant grant) {
    if (grant.worker_id.empty() || grant.process_id == 0 || grant.session_id.empty() ||
        grant.run_id.empty() || grant.operation_id.empty() || grant.result_id.empty() ||
        grant.lease_id.empty() || grant.expires_at <= std::chrono::steady_clock::now())
        throw std::invalid_argument("worker handle grant has incomplete or expired scope");
    grant.token = detail::next_handle_grant_token();
    std::lock_guard lock(mutex_);
    grants_.emplace(grant.token, grant);
    return grant;
}

WorkerHandleGrant WorkerHandleBroker::validate(
    const std::string &token, const std::string &worker_id, std::uint64_t process_id,
    const std::string &session_id, const std::string &run_id,
    const std::string &operation_id, const std::string &result_id,
    const std::string &lease_id, WorkerHandleAccess requested_access) {
    std::lock_guard lock(mutex_);
    const auto found = grants_.find(token);
    if (found == grants_.end()) throw std::invalid_argument("worker handle grant is unknown or revoked");
    const auto grant = found->second;
    if (grant.expires_at <= std::chrono::steady_clock::now()) {
        grants_.erase(found);
        throw std::invalid_argument("worker handle grant has expired");
    }
    if (!detail::same_scope(grant, worker_id, process_id, session_id, run_id,
                            operation_id, result_id, lease_id))
        throw std::invalid_argument("worker handle grant scope does not match the request");
    if (grant.access != requested_access)
        throw std::invalid_argument("worker handle grant access mode does not match the request");
    grants_.erase(found);
    return grant;
}

void WorkerHandleBroker::revoke(const std::string &token) {
    std::lock_guard lock(mutex_);
    grants_.erase(token);
}

void WorkerHandleBroker::revoke_worker(const std::string &worker_id) {
    std::lock_guard lock(mutex_);
    for (auto iterator = grants_.begin(); iterator != grants_.end();) {
        if (iterator->second.worker_id == worker_id) iterator = grants_.erase(iterator);
        else ++iterator;
    }
}

std::size_t WorkerHandleBroker::active_grants() const {
    std::lock_guard lock(mutex_);
    return grants_.size();
}

}  // namespace streamfind::service

