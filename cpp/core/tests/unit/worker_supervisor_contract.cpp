#include "streamfind/service/worker_supervisor.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

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
