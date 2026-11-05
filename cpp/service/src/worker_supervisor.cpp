#include "streamfind/service/worker_supervisor.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace streamfind::service {
namespace detail {

std::string worker_token(const char *prefix) {
    static std::atomic_uint64_t counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + std::to_string(now) + "-" + std::to_string(++counter);
}

void update_peak_memory(std::atomic_uint64_t &peak, std::uint64_t current) {
    auto previous = peak.load(std::memory_order_relaxed);
    while (current > previous &&
           !peak.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {}
}

std::string utc_timestamp() {
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

std::string quote_json_line(const Json &value) { return value.dump() + "\n"; }

#ifndef _WIN32
std::uint64_t process_group_rss_bytes(pid_t process_group) {
    try {
        std::error_code error;
        std::uint64_t total_pages = 0;
        std::filesystem::directory_iterator iterator("/proc", error), end;
        while (!error && iterator != end) {
            const auto entry = *iterator;
            iterator.increment(error);
            const auto name = entry.path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
            std::ifstream stat(entry.path() / "stat");
            std::string line;
            if (!std::getline(stat, line)) continue;
            const auto close = line.rfind(')');
            if (close == std::string::npos || close + 1 >= line.size()) continue;
            std::istringstream fields(line.substr(close + 1));
            char state = 0;
            pid_t parent = 0;
            pid_t group = 0;
            if (!(fields >> state >> parent >> group) || group != process_group) continue;
            std::ifstream statm(entry.path() / "statm");
            std::uint64_t virtual_pages = 0;
            std::uint64_t resident_pages = 0;
            if (statm >> virtual_pages >> resident_pages &&
                resident_pages <= std::numeric_limits<std::uint64_t>::max() - total_pages)
                total_pages += resident_pages;
        }
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || total_pages > std::numeric_limits<std::uint64_t>::max() /
                                                  static_cast<std::uint64_t>(page_size))
            return 0;
        return total_pages * static_cast<std::uint64_t>(page_size);
    } catch (...) {
        return 0;
    }
}
#endif

}  // namespace detail

Json WorkerMetadata::to_json() const {
    Json value{{"id", id}, {"kind", kind}, {"operation_id", operation_id},
               {"operation", operation}, {"process_id", process_id == 0 ? Json(nullptr) : Json(process_id)},
               {"process_group_id", process_group_id == 0 ? Json(nullptr) : Json(process_group_id)},
               {"job_id", job_id.empty() ? Json(nullptr) : Json(job_id)},
               {"state", state}, {"cancellation_mode", cancellation_mode},
               {"last_progress_at", last_progress_at.empty() ? Json(nullptr) : Json(last_progress_at)},
               {"peak_memory_kind", peak_memory_kind}, {"peak_memory_bytes", peak_memory_bytes}};
    return value;
}

struct WorkerProcess::Impl {
    WorkerRequest request;
    EventCallback callback;
    mutable std::mutex mutex;
    WorkerMetadata worker;
    bool cancel_requested{false};
    std::atomic_bool memory_limit_exceeded{false};
    std::atomic_uint64_t peak_memory_bytes{0};
#ifdef _WIN32
    PROCESS_INFORMATION process{};
    HANDLE job{nullptr};
    HANDLE job_completion_port{nullptr};
    HANDLE input_write{nullptr};
    HANDLE output_read{nullptr};
#else
    pid_t pid{-1};
    int input_write{-1};
    int output_read{-1};
#endif

    Impl(WorkerRequest value, EventCallback event_callback)
        : request(std::move(value)), callback(std::move(event_callback)) {
        worker.id = detail::worker_token("worker-");
        worker.operation_id = request.operation_id;
        worker.operation = request.operation;
#ifdef _WIN32
        worker.job_id = detail::worker_token("job-");
        worker.peak_memory_kind = "job_commit";
#else
        worker.peak_memory_kind = "process_group_rss";
#endif
    }

    void emit(std::string_view type, std::string_view message, const Json &payload = Json::object()) {
        if (callback) callback(worker, type, message, payload);
    }

    void mark_progress() {
        std::lock_guard lock(mutex);
        worker.last_progress_at = detail::utc_timestamp();
    }

    Json payload() const {
        return Json{{"configuration_path", request.configuration.string()},
                    {"service_port", request.service_port}, {"session_id", request.session_id},
                    {"run_id", request.run_id},
                    {"operation_id", request.operation_id}, {"operation", request.operation},
                    {"operation_instance", request.operation_instance}, {"project_database_path", request.project_database_path.string()},
                    {"computation_key", request.computation_key},
                    {"workflow", request.workflow},
                    {"parameters", request.parameters},
                    {"inputs", request.inputs},
                    {"persist_outputs", request.persist_outputs}};
    }

#ifdef _WIN32
    static void close_handle(HANDLE &handle) {
        if (handle) { CloseHandle(handle); handle = nullptr; }
    }

    void start_windows() {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE input_read = nullptr;
        HANDLE output_write = nullptr;
        if (!CreatePipe(&input_read, &input_write, &security, 0) ||
            !CreatePipe(&output_read, &output_write, &security, 0))
            throw std::runtime_error("could not create worker IPC pipes");
        SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
        std::wstring command = L"\"" + request.executable.wstring() + L"\" --worker-config \"" + request.configuration.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input_read;
        startup.hStdOutput = output_write;
        startup.hStdError = output_write;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, request.executable.parent_path().wstring().c_str(), &startup, &process)) {
            close_handle(input_read); close_handle(output_write);
            throw std::runtime_error("could not start streamfind_worker");
        }
        close_handle(input_read); close_handle(output_write);
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) throw std::runtime_error("could not create worker Job Object");
        job_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!job_completion_port) throw std::runtime_error("could not create worker Job Object completion port");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION notification_limits{};
        if (request.memory_limit_mb > 0) {
            constexpr std::uint64_t bytes_per_mib = 1024ull * 1024ull;
            if (request.memory_limit_mb > std::numeric_limits<std::uint64_t>::max() / bytes_per_mib)
                throw std::runtime_error("configured worker memory limit is too large");
            const auto bytes = request.memory_limit_mb * bytes_per_mib;
            const auto safety_margin = std::max<std::uint64_t>(bytes / 4, 16ull * 1024ull * 1024ull);
            if (bytes > std::numeric_limits<SIZE_T>::max() - safety_margin)
                throw std::runtime_error("configured worker memory limit is too large");
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            // The notification threshold is the configured limit. Give the supervisor
            // bounded time to terminate the job before the hard cap, which protects
            // against a delayed monitor or a failed notification path.
            notification_limits.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
            notification_limits.JobMemoryLimit = bytes;
            limits.JobMemoryLimit = static_cast<SIZE_T>(bytes + safety_margin);
        }
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT completion{};
        completion.CompletionKey = this;
        completion.CompletionPort = job_completion_port;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            (request.memory_limit_mb > 0 &&
             !SetInformationJobObject(job, JobObjectNotificationLimitInformation,
                                      &notification_limits, sizeof(notification_limits))) ||
            !SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation,
                                     &completion, sizeof(completion)) ||
            !AssignProcessToJobObject(job, process.hProcess))
            throw std::runtime_error("could not assign worker to Job Object");
        worker.process_id = process.dwProcessId;
        worker.state = "running";
        emit("worker.started", "Worker started.", worker.to_json());
        bool cancel_after_start = false;
        {
            std::lock_guard lock(mutex);
            cancel_after_start = cancel_requested;
        }
        if (cancel_after_start) {
            TerminateJobObject(job, 130);
        }
        const auto line = detail::quote_json_line(payload());
        DWORD written = 0;
        if (!WriteFile(input_write, line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
            throw std::runtime_error("could not send worker request");
        close_handle(input_write);
    }
#else
    void start_posix() {
        int input_pipe[2]{};
        int output_pipe[2]{};
        if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0)
            throw std::runtime_error("could not create worker IPC pipes");
        pid = fork();
        if (pid < 0) throw std::runtime_error("could not fork streamfind_worker");
        if (pid == 0) {
            setpgid(0, 0);
            const int oom_score = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
            if (oom_score >= 0) {
                if (::write(oom_score, "500", 3) != 3) {
                    // The OOM preference is best-effort; do not prevent a valid worker run.
                }
                close(oom_score);
            }
            dup2(input_pipe[0], STDIN_FILENO);
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(input_pipe[0]); close(input_pipe[1]); close(output_pipe[0]); close(output_pipe[1]);
            execl(request.executable.c_str(), request.executable.c_str(), "--worker-config",
                  request.configuration.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        close(input_pipe[0]); close(output_pipe[1]);
        input_write = input_pipe[1]; output_read = output_pipe[0];
        setpgid(pid, pid);
        worker.process_id = static_cast<std::uint64_t>(pid);
        worker.process_group_id = static_cast<std::uint64_t>(pid);
        worker.job_id.clear();
        worker.state = "running";
        emit("worker.started", "Worker started.", worker.to_json());
        const auto line = detail::quote_json_line(payload());
        if (::write(input_write, line.data(), line.size()) < 0)
            throw std::runtime_error("could not send worker request");
        close(input_write); input_write = -1;
    }
#endif
};

WorkerProcess::WorkerProcess(WorkerRequest request, EventCallback callback)
    : impl_(std::make_unique<Impl>(std::move(request), std::move(callback))) {}
WorkerProcess::~WorkerProcess() { cancel(); }

WorkerMetadata WorkerProcess::metadata() const {
    std::lock_guard lock(impl_->mutex);
    auto metadata = impl_->worker;
    metadata.peak_memory_bytes = impl_->peak_memory_bytes.load(std::memory_order_relaxed);
    return metadata;
}

void WorkerProcess::cancel() {
#ifdef _WIN32
    HANDLE process_handle = nullptr;
#endif
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->worker.state == "completed" || impl_->worker.state == "failed" || impl_->worker.state == "cancelled")
            return;
        if (impl_->cancel_requested) return;
        impl_->cancel_requested = true;
        impl_->worker.state = "cancelling";
#ifdef _WIN32
        if (impl_->job) TerminateJobObject(impl_->job, 130);
        else if (impl_->process.hProcess) TerminateProcess(impl_->process.hProcess, 130);
        process_handle = impl_->process.hProcess;
#else
        if (impl_->pid > 0) kill(-impl_->pid, SIGTERM);
#endif
    }
#ifdef _WIN32
    if (process_handle) {
        const auto wait_result = WaitForSingleObject(process_handle, 5000);
        if (wait_result == WAIT_TIMEOUT) {
            std::lock_guard lock(impl_->mutex);
            if (impl_->job) TerminateJobObject(impl_->job, 130);
            else if (impl_->process.hProcess) TerminateProcess(impl_->process.hProcess, 130);
        }
    }
#else
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::lock_guard lock(impl_->mutex);
    if (impl_->pid > 0) {
        int status = 0;
        if (waitpid(impl_->pid, &status, WNOHANG) == 0)
            kill(-impl_->pid, SIGKILL);
    }
#endif
}

WorkerResult WorkerProcess::run() {
    WorkerResult result;
    try {
#ifdef _WIN32
        impl_->start_windows();
        std::atomic_bool stop_memory_monitor{false};
        std::jthread memory_monitor;
        {
            memory_monitor = std::jthread([this, &stop_memory_monitor](std::stop_token stop_token) {
                const auto handle_job_message = [this](DWORD message) {
                    if (message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT ||
                        message == JOB_OBJECT_MSG_NOTIFICATION_LIMIT) {
                        impl_->memory_limit_exceeded.store(true);
                        if (impl_->job) TerminateJobObject(impl_->job, 0xE001);
                    }
                };
                while (!stop_token.stop_requested() && !stop_memory_monitor.load()) {
                    DWORD message = 0;
                    ULONG_PTR completion_key = 0;
                    LPOVERLAPPED overlapped = nullptr;
                    if (impl_->job_completion_port &&
                        GetQueuedCompletionStatus(impl_->job_completion_port, &message,
                                                  &completion_key, &overlapped, 50)) {
                        handle_job_message(message);
                        continue;
                    }
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                    if (impl_->job && QueryInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                                                 &info, sizeof(info), nullptr)) {
                        const auto peak = static_cast<std::uint64_t>(info.PeakJobMemoryUsed);
                        detail::update_peak_memory(impl_->peak_memory_bytes, peak);
                    }
                }
                // The worker can exit immediately after an allocation is rejected. Drain
                // queued Job Object notifications after process exit so that this race does
                // not turn a memory-limit failure into an opaque worker crash.
                if (impl_->job_completion_port) {
                    for (;;) {
                        DWORD message = 0;
                        ULONG_PTR completion_key = 0;
                        LPOVERLAPPED overlapped = nullptr;
                        if (!GetQueuedCompletionStatus(impl_->job_completion_port, &message,
                                                       &completion_key, &overlapped, 0)) break;
                        handle_job_message(message);
                    }
                }
            });
        }
        std::string line;
        char buffer[4096]; DWORD read = 0;
        while (ReadFile(impl_->output_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            line.append(buffer, buffer + read);
            std::size_t end = 0;
            while ((end = line.find('\n')) != std::string::npos) {
                const auto text = line.substr(0, end); line.erase(0, end + 1);
                try {
                    const auto event = Json::parse(text);
                    const auto type = event.value("type", "");
                    if (type == "result") result.result = event.value("result", Json::object());
                    else if (type == "error") result.error = event.value("message", "worker failed");
                    else if (type == "operation.log") {
                        impl_->mark_progress();
                        auto payload = Json{{"operation_id", event.value("operation_id", "")}};
                        impl_->emit(type, event.value("message", ""), payload);
                    } else if (type == "operation.event") {
                        impl_->mark_progress();
                        auto payload = event.value("payload", Json::object());
                        payload["operation_id"] = event.value("operation_id", "");
                        impl_->emit(event.value("event", ""), event.value("message", ""), payload);
                    }
                } catch (...) {}
            }
        }
        WaitForSingleObject(impl_->process.hProcess, INFINITE);
        stop_memory_monitor.store(true);
        if (memory_monitor.joinable()) memory_monitor.join();
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION final_memory_info{};
        if (impl_->job && QueryInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                                     &final_memory_info, sizeof(final_memory_info), nullptr))
            detail::update_peak_memory(impl_->peak_memory_bytes,
                                       static_cast<std::uint64_t>(final_memory_info.PeakJobMemoryUsed));
        DWORD code = 1; GetExitCodeProcess(impl_->process.hProcess, &code); result.exit_code = static_cast<int>(code);
        Impl::close_handle(impl_->output_read); Impl::close_handle(impl_->process.hThread); Impl::close_handle(impl_->process.hProcess); Impl::close_handle(impl_->job_completion_port); Impl::close_handle(impl_->job);
#else
        impl_->start_posix();
        std::atomic_bool stop_memory_monitor{false};
        std::jthread memory_monitor;
        {
            const auto limit_bytes = impl_->request.memory_limit_mb * 1024ull * 1024ull;
            const auto process_group = impl_->pid;
            memory_monitor = std::jthread([this, &stop_memory_monitor, limit_bytes, process_group](std::stop_token stop_token) {
                while (!stop_token.stop_requested() && !stop_memory_monitor.load()) {
                    const auto current = detail::process_group_rss_bytes(process_group);
                    detail::update_peak_memory(impl_->peak_memory_bytes, current);
                    if (limit_bytes > 0 && current >= limit_bytes) {
                        impl_->memory_limit_exceeded.store(true);
                        kill(-process_group, SIGKILL);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        }
        std::string line; char buffer[4096];
        for (;;) {
            const auto count = ::read(impl_->output_read, buffer, sizeof(buffer));
            if (count <= 0) break;
            line.append(buffer, buffer + count);
            std::size_t end = 0;
            while ((end = line.find('\n')) != std::string::npos) {
                const auto text = line.substr(0, end); line.erase(0, end + 1);
                try {
                    const auto event = Json::parse(text);
                    const auto type = event.value("type", "");
                    if (type == "result") result.result = event.value("result", Json::object());
                    else if (type == "error") result.error = event.value("message", "worker failed");
                    else if (type == "operation.log") {
                        impl_->mark_progress();
                        auto payload = Json{{"operation_id", event.value("operation_id", "")}};
                        impl_->emit(type, event.value("message", ""), payload);
                    } else if (type == "operation.event") {
                        impl_->mark_progress();
                        auto payload = event.value("payload", Json::object());
                        payload["operation_id"] = event.value("operation_id", "");
                        impl_->emit(event.value("event", ""), event.value("message", ""), payload);
                    }
                } catch (...) {}
            }
        }
        stop_memory_monitor.store(true);
        if (memory_monitor.joinable()) memory_monitor.join();
        int status = 1; waitpid(impl_->pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        close(impl_->output_read); impl_->output_read = -1;
#endif
    } catch (const std::exception &error) { result.error = error.what(); result.exit_code = 1; }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->worker.peak_memory_bytes = impl_->peak_memory_bytes.load(std::memory_order_relaxed);
        result.cancelled = impl_->cancel_requested;
        result.memory_limit_exceeded = impl_->memory_limit_exceeded.load();
        impl_->worker.state = result.cancelled ? "cancelled" : result.exit_code == 0 ? "completed" : "failed";
        result.worker = impl_->worker;
    }
    if (result.cancelled) result.error = "Worker cancelled";
    else if (result.memory_limit_exceeded)
        result.error = "Worker process tree exceeded configured memory limit of " +
                       std::to_string(impl_->request.memory_limit_mb) + " MiB";
    else if (result.error.empty() && result.exit_code != 0)
        result.error = "Worker process exited with code " + std::to_string(result.exit_code);
    return result;
}

}  // namespace streamfind::service
