#include "streamfind/service/worker_supervisor.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace streamfind::service {
namespace detail {

std::string worker_token(const char *prefix) {
    static std::atomic_uint64_t counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + std::to_string(now) + "-" + std::to_string(++counter);
}

std::string quote_json_line(const Json &value) { return value.dump() + "\n"; }

}  // namespace detail

Json WorkerMetadata::to_json() const {
    Json value{{"id", id}, {"kind", kind}, {"operation_id", operation_id},
               {"operation", operation}, {"process_id", process_id == 0 ? Json(nullptr) : Json(process_id)},
               {"process_group_id", process_group_id == 0 ? Json(nullptr) : Json(process_group_id)},
               {"job_id", job_id.empty() ? Json(nullptr) : Json(job_id)},
               {"state", state}, {"cancellation_mode", cancellation_mode}};
    return value;
}

struct WorkerProcess::Impl {
    WorkerRequest request;
    EventCallback callback;
    mutable std::mutex mutex;
    WorkerMetadata worker;
    bool cancel_requested{false};
#ifdef _WIN32
    PROCESS_INFORMATION process{};
    HANDLE job{nullptr};
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
#endif
    }

    void emit(std::string_view type, std::string_view message, const Json &payload = Json::object()) {
        if (callback) callback(worker, type, message, payload);
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
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
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
    return impl_->worker;
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
                        auto payload = Json{{"operation_id", event.value("operation_id", "")}};
                        impl_->emit(type, event.value("message", ""), payload);
                    } else if (type == "operation.event") {
                        auto payload = event.value("payload", Json::object());
                        payload["operation_id"] = event.value("operation_id", "");
                        impl_->emit(event.value("event", ""), event.value("message", ""), payload);
                    }
                } catch (...) {}
            }
        }
        WaitForSingleObject(impl_->process.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(impl_->process.hProcess, &code); result.exit_code = static_cast<int>(code);
        Impl::close_handle(impl_->output_read); Impl::close_handle(impl_->process.hThread); Impl::close_handle(impl_->process.hProcess); Impl::close_handle(impl_->job);
#else
        impl_->start_posix();
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
                        auto payload = Json{{"operation_id", event.value("operation_id", "")}};
                        impl_->emit(type, event.value("message", ""), payload);
                    } else if (type == "operation.event") {
                        auto payload = event.value("payload", Json::object());
                        payload["operation_id"] = event.value("operation_id", "");
                        impl_->emit(event.value("event", ""), event.value("message", ""), payload);
                    }
                } catch (...) {}
            }
        }
        int status = 1; waitpid(impl_->pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        close(impl_->output_read); impl_->output_read = -1;
#endif
    } catch (const std::exception &error) { result.error = error.what(); result.exit_code = 1; }
    {
        std::lock_guard lock(impl_->mutex);
        result.worker = impl_->worker;
        result.cancelled = impl_->cancel_requested;
        impl_->worker.state = result.cancelled ? "cancelled" : result.exit_code == 0 ? "completed" : "failed";
        result.worker = impl_->worker;
    }
    if (result.cancelled) result.error = "Worker cancelled";
    return result;
}

}  // namespace streamfind::service
