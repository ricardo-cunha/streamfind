#include "streamfind/service/worker_supervisor.hpp"
#include "streamfind/service/worker_handle_broker.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static streamfind::service::WorkerResult run_handle_worker(
    streamfind::service::WorkerRequest request, std::intptr_t handle,
    streamfind::service::WorkerHandleAccess access, const std::string &result_id) {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    request.parameters = {{"mode", access == streamfind::service::WorkerHandleAccess::WriteOnly
        ? "broker-write" : "broker-read"}};
    streamfind::service::WorkerProcess worker(request,
        [&](const streamfind::service::WorkerMetadata &, std::string_view type,
            std::string_view, const streamfind::Json &) {
            if (type == "worker.handle_broker_connected") {
                std::lock_guard lock(mutex);
                connected = true;
                condition.notify_all();
            }
        });
    streamfind::service::WorkerResult result;
    std::thread thread([&] { result = worker.run(); });
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return connected; })) {
            worker.cancel();
            thread.join();
            throw std::runtime_error("chained-result worker did not connect to its handle broker");
        }
    }
    streamfind::service::WorkerHandleGrant grant;
    grant.session_id = "session-contract";
    grant.run_id = "run-contract";
    grant.operation_id = "test";
    grant.result_id = result_id;
    grant.lease_id = "lease-" + result_id;
    grant.access = access;
    grant.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    worker.transfer_handle(std::move(grant), static_cast<std::uintptr_t>(handle));
    thread.join();
    return result;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    streamfind::service::WorkerRequest request;
    request.executable = argv[1];
    request.configuration = "worker-fixture-config.json";
    request.operation_id = "test";
    request.operation = "test.worker";

    const auto completed = streamfind::service::WorkerProcess(request).run();
    if (completed.exit_code != 0 || !completed.error.empty() ||
        !completed.result.value("ok", false)) return 2;

    request.parameters = {{"mode", "progress"}};
    const auto progressed = streamfind::service::WorkerProcess(request).run();
    if (progressed.exit_code != 0 || progressed.worker.last_progress_at.size() != 20) return 7;

    request.parameters = {{"mode", "broker-handle"}};
    std::mutex broker_mutex;
    std::condition_variable broker_condition;
    bool broker_connected = false;
    streamfind::service::WorkerProcess brokered(request,
        [&](const streamfind::service::WorkerMetadata &, std::string_view type,
            std::string_view, const streamfind::Json &) {
            if (type == "worker.handle_broker_connected") {
                std::lock_guard lock(broker_mutex);
                broker_connected = true;
                broker_condition.notify_all();
            }
        });
    streamfind::service::WorkerResult brokered_result;
    std::thread brokered_thread([&] { brokered_result = brokered.run(); });
    {
        std::unique_lock lock(broker_mutex);
        if (!broker_condition.wait_for(lock, std::chrono::seconds(5), [&] { return broker_connected; })) {
            brokered.cancel();
            brokered_thread.join();
            return 10;
        }
    }
#ifdef _WIN32
    HANDLE grant_read = nullptr, grant_write = nullptr;
    if (!CreatePipe(&grant_read, &grant_write, nullptr, 0)) {
        brokered.cancel(); brokered_thread.join(); return 11;
    }
    const char grant_byte = 'g';
    DWORD grant_written = 0;
    if (!WriteFile(grant_write, &grant_byte, 1, &grant_written, nullptr) || grant_written != 1) {
        CloseHandle(grant_read); CloseHandle(grant_write); brokered.cancel(); brokered_thread.join(); return 12;
    }
#else
    int grant_pipe[2]{};
    if (pipe(grant_pipe) != 0) { brokered.cancel(); brokered_thread.join(); return 11; }
    const char grant_byte = 'g';
    if (::write(grant_pipe[1], &grant_byte, 1) != 1) {
        close(grant_pipe[0]); close(grant_pipe[1]); brokered.cancel(); brokered_thread.join(); return 12;
    }
#endif
    streamfind::service::WorkerHandleGrant scoped_grant;
    scoped_grant.session_id = "session-contract";
    scoped_grant.run_id = "run-contract";
    scoped_grant.operation_id = "test";
    scoped_grant.result_id = "result-contract";
    scoped_grant.lease_id = "lease-contract";
    scoped_grant.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto grant_token = brokered.transfer_handle(scoped_grant,
#ifdef _WIN32
        reinterpret_cast<std::uintptr_t>(grant_read));
#else
        static_cast<std::uintptr_t>(grant_pipe[0]));
#endif
    brokered_thread.join();
#ifdef _WIN32
    CloseHandle(grant_read); CloseHandle(grant_write);
#else
    close(grant_pipe[0]); close(grant_pipe[1]);
#endif
    if (brokered_result.exit_code != 0 || !brokered_result.result.value("handle_received", false) ||
        brokered_result.result.value("grant_token", std::string{}) != grant_token) return 13;

    const auto chain_path = std::filesystem::path(STREAMFIND_TEST_REPO_ROOT) /
        "tmp" / "projects" / "worker-chained-result-contract.bin";
    std::filesystem::create_directories(chain_path.parent_path());
#ifdef _WIN32
    const auto chain_writer = CreateFileW(chain_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (chain_writer == INVALID_HANDLE_VALUE) return 14;
#else
    const auto chain_writer = ::open(chain_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (chain_writer < 0) return 14;
#endif
    const auto first_node = run_handle_worker(request,
#ifdef _WIN32
        reinterpret_cast<std::intptr_t>(chain_writer),
#else
        static_cast<std::intptr_t>(chain_writer),
#endif
        streamfind::service::WorkerHandleAccess::WriteOnly, "result-chain-node-1");
#ifdef _WIN32
    CloseHandle(chain_writer);
    const auto chain_reader = CreateFileW(chain_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (chain_reader == INVALID_HANDLE_VALUE) return 15;
#else
    close(chain_writer);
    const auto chain_reader = ::open(chain_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (chain_reader < 0) return 15;
#endif
    const auto second_node = run_handle_worker(request,
#ifdef _WIN32
        reinterpret_cast<std::intptr_t>(chain_reader),
#else
        static_cast<std::intptr_t>(chain_reader),
#endif
        streamfind::service::WorkerHandleAccess::ReadOnly, "result-chain-node-2");
#ifdef _WIN32
    CloseHandle(chain_reader);
#else
    close(chain_reader);
#endif
    std::filesystem::remove(chain_path);
    if (first_node.exit_code != 0 || second_node.exit_code != 0 ||
        !second_node.result.value("handle_received", false)) return 16;

#if defined(__linux__)
    request.parameters = {{"mode", "oom-score"}};
    const auto oom_preference = streamfind::service::WorkerProcess(request).run();
    if (oom_preference.exit_code != 0 || oom_preference.result.value("oom_score_adj", -1) != 500)
        return 6;
#endif

    request.parameters = {{"mode", "crash"}};
    const auto crashed = streamfind::service::WorkerProcess(request).run();
    if (crashed.exit_code == 0 || crashed.error.find("exited with code 23") == std::string::npos)
        return 3;

    request.parameters = {{"mode", "block"}};
    std::mutex started_mutex;
    std::condition_variable started_condition;
    bool started = false;
    streamfind::service::WorkerProcess blocking(request,
        [&](const streamfind::service::WorkerMetadata &, std::string_view type,
            std::string_view, const streamfind::Json &) {
            if (type == "worker.started") {
                std::lock_guard lock(started_mutex);
                started = true;
                started_condition.notify_all();
            }
        });
    streamfind::service::WorkerResult cancelled;
    std::thread blocking_thread([&] { cancelled = blocking.run(); });
    {
        std::unique_lock lock(started_mutex);
        if (!started_condition.wait_for(lock, std::chrono::seconds(5), [&] { return started; })) {
            blocking.cancel();
            blocking_thread.join();
            return 8;
        }
    }
    blocking.cancel();
    blocking_thread.join();
    if (!cancelled.cancelled || cancelled.error != "Worker cancelled") return 9;

    request.parameters = {{"mode", "memory"}};
    request.memory_limit_mb = 64;
    const auto limited = streamfind::service::WorkerProcess(request).run();
    if (limited.exit_code == 0 ||
        limited.error.find("exceeded configured memory limit of 64 MiB") == std::string::npos ||
        limited.worker.peak_memory_bytes == 0 ||
#ifdef _WIN32
        limited.worker.peak_memory_kind != "job_commit" ||
        limited.worker.peak_memory_bytes < 64ull * 1024ull * 1024ull ||
        limited.worker.peak_memory_bytes >= 80ull * 1024ull * 1024ull) {
#else
        limited.worker.peak_memory_kind != "process_group_rss") {
#endif
        std::cerr << "memory-limited worker contract failed: exit=" << limited.exit_code
                  << ", peak_bytes=" << limited.worker.peak_memory_bytes
                  << ", error=" << limited.error << '\n';
        return 4;
    }

    // A worker crash and a memory-limited worker must not terminate this
    // supervising process; this final result proves the supervisor continued.
    request.parameters = {{"mode", "success"}};
    request.memory_limit_mb = 0;
    const auto recovered = streamfind::service::WorkerProcess(request).run();
    if (recovered.exit_code != 0 || !recovered.result.value("ok", false)) return 5;
    return 0;
}
