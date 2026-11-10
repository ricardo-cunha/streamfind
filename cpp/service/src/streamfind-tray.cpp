#include "streamfind/service_runtime_client.hpp"
#include "streamfind/service_configuration.hpp"
#include "streamfind/service_port.hpp"
#include "../include/streamfind/service/service_discovery.hpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace streamfind::tray::detail {

std::uint16_t startup_port(const streamfind::ServiceConfiguration &configuration) {
    if (const auto *override_value = std::getenv("STREAMFIND_SERVICE_PORT")) {
        const auto parsed = std::stoul(override_value);
        if (parsed == 0 || parsed > 65535)
            throw std::runtime_error("STREAMFIND_SERVICE_PORT must be between 1 and 65535");
        return static_cast<std::uint16_t>(parsed);
    }
    return configuration.automatic_port_fallback
               ? streamfind::select_service_port(configuration.port)
               : configuration.port;
}

}  // namespace streamfind::tray::detail

#ifdef _WIN32
namespace streamfind::tray::detail {

constexpr UINT kRefreshTimer = 1;
constexpr UINT kTrayMessage = WM_USER + 1;
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT kOpenApplication = 1001;
constexpr UINT kOpenStatus = 1002;
constexpr UINT kCopyEndpoint = 1003;
constexpr UINT kOpenActivity = 1004;
constexpr UINT kStopService = 1006;
constexpr UINT kIconId = 101;

std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string &value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
    return result;
}

std::string tray_executable_path() {
    std::string path(32768, '\0');
    const auto length = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) throw std::runtime_error("unable to locate streamfind tray executable");
    path.resize(length);
    return path;
}

void stop_service(const std::string &instance) {
    streamfind::service::stop_service(instance);
}

std::string endpoint(const streamfind::data_proxy::Json &services) {
    if (!services.is_array()) return {};
    for (const auto &service : services)
        if (service.value("health", "") == "healthy") return service.value("endpoint", std::string{});
    return {};
}

std::string status_text(const streamfind::data_proxy::Json &services) {
    const auto value = endpoint(services);
    if (value.empty()) return "streamfind service: disconnected";
    for (const auto &service : services)
        if (service.value("endpoint", "") == value)
            return "streamfind service: " + service.value("state", "ready") + " (" + value + ")";
    return "streamfind service: ready";
}

const streamfind::data_proxy::Json *healthy_service(const streamfind::data_proxy::Json &services) {
    if (!services.is_array()) return nullptr;
    for (const auto &service : services)
        if (service.value("health", "") == "healthy") return &service;
    return nullptr;
}

streamfind::data_proxy::Json discovered_service_snapshot() {
    auto services = streamfind::data_proxy::Json::array();
    for (const auto &path : streamfind::service::ServiceDiscovery::record_paths()) {
        try {
            const auto record = streamfind::service::ServiceDiscovery::read(path);
            auto value = record.to_json();
            value.erase("token");
            value["record_path"] = path.string();
            value["health"] = "healthy";
            value["session"] = streamfind::data_proxy::Json{{"projects", streamfind::data_proxy::Json::array()}};
            services.push_back(std::move(value));
        } catch (...) {}
    }
    return services;
}

class TrayApplication {
public:
    ~TrayApplication() {
        stop_owned_service();
        if (mutex_ != nullptr) CloseHandle(mutex_);
    }

    int run(HINSTANCE instance) {
        mutex_ = CreateMutexW(nullptr, TRUE, L"Local\\StreamFindTrayManager");
        if (mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        ensure_service();
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &TrayApplication::window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = L"StreamFindTrayManager";
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconId));
        RegisterClassW(&window_class);
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        window_ = CreateWindowW(window_class.lpszClassName, L"streamfind service", 0, 0, 0, 0, 0,
                                nullptr, nullptr, instance, this);
        if (!window_) throw std::runtime_error("unable to create StreamFind tray window");
        refresh_thread_ = std::thread(&TrayApplication::refresh_worker, this);
        refresh();
        SetTimer(window_, kRefreshTimer, 2000, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (mutex_) CloseHandle(mutex_);
        mutex_ = nullptr;
        return 0;
    }

private:
    HWND window_{nullptr};
    HANDLE mutex_{nullptr};
    NOTIFYICONDATAW icon_{};
    bool icon_registered_{false};
    streamfind::data_proxy::Json services_ = streamfind::data_proxy::Json::array();
    UINT taskbar_created_{0};
    std::mutex refresh_mutex_;
    std::condition_variable refresh_condition_;
    std::thread refresh_thread_;
    bool refresh_requested_{false};
    bool refresh_running_{false};
    bool refresh_ready_{false};
    bool refresh_stopping_{false};
    streamfind::data_proxy::Json refreshed_services_ = streamfind::data_proxy::Json::array();
    HANDLE service_process_{nullptr};
    HANDLE service_job_{nullptr};

    std::filesystem::path package_bin() const {
        return std::filesystem::path(tray_executable_path()).parent_path();
    }

    std::filesystem::path server_executable() const {
        return package_bin() / "streamfind-server.exe";
    }

    void ensure_service() {
        const auto current_pid = static_cast<std::uint64_t>(GetCurrentProcessId());
        for (const auto &value : streamfind::service::list_services()) {
            if (value.value("health", "") != "healthy") continue;
            if (value.value("manager_process_id", std::uint64_t{0}) == current_pid) {
                const auto pid = static_cast<DWORD>(value.value("process_id", std::uint64_t{0}));
                service_process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                               FALSE, pid);
                if (service_process_ == nullptr)
                    throw std::runtime_error("unable to attach to managed streamfind-server");
                return;
            }
            stop_service(value.value("service_instance_id", std::string{}));
            for (int attempt = 0; attempt < 150; ++attempt) {
                if (streamfind::service::ServiceDiscovery::record_paths().empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            break;
        }

        const auto server = server_executable();
        const auto manager = tray_executable_path();
        const auto config = streamfind::load_service_configuration(package_bin() / "streamfind.json");
        const auto port = startup_port(config);
        std::string command = "\"" + server.string() + "\" " + std::to_string(port) +
            " --manager-pid " + std::to_string(current_pid) + " --manager-executable \"" + manager + "\"";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        service_job_ = CreateJobObjectW(nullptr, nullptr);
        if (service_job_ == nullptr) throw std::runtime_error("unable to create streamfind service job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(service_job_, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            CloseHandle(service_job_);
            service_job_ = nullptr;
            throw std::runtime_error("unable to configure streamfind service job");
        }
        const auto path = package_bin().string();
        if (!CreateProcessA(server.string().c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
                            nullptr, path.c_str(), &startup, &process)) {
            CloseHandle(service_job_);
            service_job_ = nullptr;
            throw std::runtime_error("unable to launch streamfind-server");
        }
        if (!AssignProcessToJobObject(service_job_, process.hProcess)) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(service_job_);
            service_job_ = nullptr;
            throw std::runtime_error("unable to attach streamfind-server to its manager job");
        }
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        service_process_ = process.hProcess;

        for (int attempt = 0; attempt < 150; ++attempt) {
            if (WaitForSingleObject(service_process_, 0) == WAIT_OBJECT_0)
                throw std::runtime_error("streamfind-server exited during startup");
            for (const auto &value : streamfind::service::list_services())
                if (value.value("health", "") == "healthy" &&
                    value.value("manager_process_id", std::uint64_t{0}) == current_pid) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop_owned_service();
        throw std::runtime_error("timed out waiting for streamfind-server to become ready");
    }

    void stop_owned_service() {
        if (service_process_ != nullptr && WaitForSingleObject(service_process_, 0) != WAIT_OBJECT_0) {
            auto current_services = services_;
            try { current_services = streamfind::service::list_services(); }
            catch (...) { }
            const auto *service = healthy_service(current_services);
            if (service != nullptr) {
                try { stop_service(service->value("service_instance_id", std::string{})); }
                catch (...) { }
            }
            if (WaitForSingleObject(service_process_, 15000) != WAIT_OBJECT_0 && service_job_ != nullptr) {
                TerminateJobObject(service_job_, 1);
                WaitForSingleObject(service_process_, 5000);
            }
        }
        if (service_process_ != nullptr) CloseHandle(service_process_);
        service_process_ = nullptr;
        if (service_job_ != nullptr) CloseHandle(service_job_);
        service_job_ = nullptr;
    }

    void supervise_service() {
        if (service_process_ == nullptr || WaitForSingleObject(service_process_, 0) != WAIT_OBJECT_0) return;
        DWORD exit_code = 1;
        GetExitCodeProcess(service_process_, &exit_code);
        CloseHandle(service_process_);
        service_process_ = nullptr;
        if (service_job_ != nullptr) CloseHandle(service_job_);
        service_job_ = nullptr;
        if (exit_code == 0) {
            DestroyWindow(window_);
            return;
        }
        try { ensure_service(); }
        catch (...) { }
    }

    void refresh() {
        {
            std::lock_guard lock(refresh_mutex_);
            if (refresh_ready_) {
                services_ = std::move(refreshed_services_);
                refreshed_services_ = streamfind::data_proxy::Json::array();
                refresh_ready_ = false;
            }
            if (!refresh_requested_ && !refresh_running_) refresh_requested_ = true;
        }
        refresh_condition_.notify_one();
        if (!icon_registered_) {
            icon_.cbSize = sizeof(icon_);
            icon_.hWnd = window_;
            icon_.uID = kIconId;
            icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            icon_.uCallbackMessage = kTrayMessage;
            icon_.uVersion = NOTIFYICON_VERSION_4;
            icon_.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconId));
            if (!icon_.hIcon) icon_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            icon_registered_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;
            if (!icon_registered_) {
                icon_.cbSize = 0;
                return;
            }
            Shell_NotifyIconW(NIM_SETVERSION, &icon_);
        }
        const auto tip = wide(status_text(services_));
        wcsncpy_s(icon_.szTip, tip.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_MODIFY, &icon_)) {
            icon_registered_ = false;
            icon_.cbSize = 0;
        }
    }

    void refresh_worker() {
        for (;;) {
            {
                std::unique_lock lock(refresh_mutex_);
                refresh_condition_.wait(lock, [this] { return refresh_requested_ || refresh_stopping_; });
                if (refresh_stopping_) return;
                refresh_requested_ = false;
                refresh_running_ = true;
            }

            streamfind::data_proxy::Json result = streamfind::data_proxy::Json::array();
            try { result = streamfind::service::list_services(); }
            catch (...) {}
            if (healthy_service(result) == nullptr) {
                const auto discovered = discovered_service_snapshot();
                if (!discovered.empty()) result = discovered;
            }

            {
                std::lock_guard lock(refresh_mutex_);
                refreshed_services_ = std::move(result);
                refresh_ready_ = true;
                refresh_running_ = false;
            }
            PostMessageW(window_, kRefreshMessage, 0, 0);
        }
    }

    void stop_refresh_worker() {
        {
            std::lock_guard lock(refresh_mutex_);
            refresh_stopping_ = true;
        }
        refresh_condition_.notify_one();
        if (refresh_thread_.joinable()) refresh_thread_.join();
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<TrayApplication *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TrayApplication *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        if (message == self->taskbar_created_) {
            self->icon_.cbSize = 0;
            self->icon_registered_ = false;
            self->refresh();
        } else if (message == kRefreshMessage) self->refresh();
        else if (message == kTrayMessage && is_context_menu_event(lparam)) self->show_menu();
        else if (message == WM_CONTEXTMENU) self->show_menu();
        else if (message == WM_TIMER && wparam == kRefreshTimer) {
            self->supervise_service();
            self->refresh();
        }
        else if (message == WM_COMMAND) self->command(LOWORD(wparam));
        else if (message == WM_DESTROY) {
            KillTimer(window, kRefreshTimer);
            self->stop_refresh_worker();
            self->stop_owned_service();
            Shell_NotifyIconW(NIM_DELETE, &self->icon_);
            PostQuitMessage(0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void show_menu() {
        // Use the live service snapshot collected by the refresh worker.  The
        // discovery record identifies the process, but does not contain the
        // projects attached to that service.  Replacing the live snapshot
        // here made the tray report zero attached projects while the app had a
        // project open.
        {
            std::lock_guard lock(refresh_mutex_);
            if (refresh_ready_) {
                services_ = std::move(refreshed_services_);
                refreshed_services_ = streamfind::data_proxy::Json::array();
                refresh_ready_ = false;
            }
        }
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        HMENU menu = CreatePopupMenu();
        const auto label = wide(status_text(services_));
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, label.c_str());
        if (const auto *service = healthy_service(services_)) {
            const auto instance = wide("Instance: " + service->value("service_instance_id", "unknown"));
            const auto pid = wide("PID: " + std::to_string(service->value("process_id", std::uint64_t{0})));
            const auto projects = service->contains("session")
                ? service->at("session").value("projects", streamfind::data_proxy::Json::array()).size() : 0;
            const auto project_text = wide("Attached projects: " + std::to_string(projects));
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, instance.c_str());
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, pid.c_str());
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, project_text.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const auto has_service = healthy_service(services_) != nullptr;
        AppendMenuW(menu, MF_STRING, kOpenApplication, L"Open streamfind app");
        AppendMenuW(menu, MF_STRING, kOpenStatus, L"Open streamfind service status");
        AppendMenuW(menu, MF_STRING, kCopyEndpoint, L"Copy endpoint");
        AppendMenuW(menu, MF_STRING, kOpenActivity, L"Open activity log");
        AppendMenuW(menu, MF_STRING | (has_service ? 0 : MF_GRAYED), kStopService,
                    L"Stop service and close tray");
        const auto selected = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                             point.x, point.y, 0, window_, nullptr);
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (selected != 0) command(selected);
    }

    static bool is_context_menu_event(LPARAM value) {
        const auto event = static_cast<UINT>(LOWORD(value));
        return value == WM_RBUTTONDOWN || value == WM_RBUTTONUP || value == WM_CONTEXTMENU ||
               event == WM_RBUTTONDOWN || event == WM_RBUTTONUP || event == WM_CONTEXTMENU ||
               event == NIN_SELECT || event == NIN_KEYSELECT;
    }

    void command(UINT command_id) {
        const auto value = endpoint(services_);
        const auto url = value.empty() ? std::string("http://127.0.0.1:8787/") : value + "/";
        if (command_id == kOpenApplication) ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (command_id == kOpenStatus) ShellExecuteW(nullptr, L"open", wide(url + "session").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (command_id == kOpenActivity) ShellExecuteW(nullptr, L"open", wide(url + "events").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (command_id == kCopyEndpoint && !value.empty()) copy(value);
        else if (command_id == kStopService && !value.empty()) {
            const auto *service = healthy_service(services_);
            const auto instance = service == nullptr
                ? std::string{}
                : service->value("service_instance_id", std::string{});
            try {
                stop_service(instance);
                if (service_process_ != nullptr &&
                    WaitForSingleObject(service_process_, 15000) != WAIT_OBJECT_0 && service_job_ != nullptr)
                    TerminateJobObject(service_job_, 1);
                refresh();
            }
            catch (...) { MessageBoxW(window_, L"Unable to stop the streamfind service.", L"streamfind", MB_OK | MB_ICONERROR); }
            DestroyWindow(window_);
        } else if (command_id == kStopService) DestroyWindow(window_);
    }

    void copy(const std::string &value) {
        if (!OpenClipboard(window_)) return;
        EmptyClipboard();
        const auto text = wide(value);
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle) {
            auto *target = static_cast<wchar_t *>(GlobalLock(handle));
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(handle);
            SetClipboardData(CF_UNICODETEXT, handle);
        }
        CloseClipboard();
    }
};

}  // namespace streamfind::tray::detail

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    try { return streamfind::tray::detail::TrayApplication{}.run(instance); }
    catch (const std::exception &error) { MessageBoxA(nullptr, error.what(), "streamfind tray failed", MB_OK | MB_ICONERROR); return 1; }
}

#else

namespace streamfind::tray::detail {

volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

std::filesystem::path manager_path() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) throw std::runtime_error("unable to locate streamfind tray manager");
    return path;
}

int supervise() {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    const auto manager = manager_path();
    const auto server = manager.parent_path() / "streamfind-server";
    const auto configuration = streamfind::load_service_configuration(manager.parent_path() / "streamfind.json");
    const auto runtime_directory = streamfind::service::ServiceDiscovery::runtime_directory();
    std::filesystem::create_directories(runtime_directory);
    const auto lock_path = runtime_directory / "tray-manager.lock";
    const auto lock = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock < 0) throw std::runtime_error("unable to create streamfind tray manager lock");
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        close(lock);
        return 0;
    }
    for (const auto &service : streamfind::service::list_services()) {
        if (service.value("health", "") != "healthy") continue;
        const auto manager_pid = service.value("manager_process_id", std::uint64_t{0});
        if (manager_pid != 0 && manager_pid != static_cast<std::uint64_t>(getpid())) {
            close(lock);
            return 0;
        }
        if (manager_pid == 0) {
            streamfind::service::stop_service(service.value("service_instance_id", std::string{}));
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (streamfind::service::ServiceDiscovery::record_paths().empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            break;
        }
    }
    pid_t child = -1;
    for (;;) {
        if (stop_requested != 0) {
            if (child > 0) {
                kill(child, SIGTERM);
                for (int attempt = 0; attempt < 100; ++attempt) {
                    const auto result = waitpid(child, nullptr, WNOHANG);
                    if (result == child) { child = -1; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (child > 0) {
                    kill(child, SIGKILL);
                    waitpid(child, nullptr, 0);
                    child = -1;
                }
            }
            close(lock);
            return 0;
        }
        if (child < 0) {
            const auto pid = std::to_string(static_cast<std::uint64_t>(getpid()));
    const auto port = std::to_string(startup_port(configuration));
            child = fork();
            if (child < 0) throw std::runtime_error("unable to launch streamfind-server");
            if (child == 0) {
                execl(server.c_str(), server.c_str(), port.c_str(), "--manager-pid", pid.c_str(),
                      "--manager-executable", manager.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
        }
        int status = 0;
        const auto exited = waitpid(child, &status, WNOHANG);
        if (exited == child) {
            child = -1;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                close(lock);
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace streamfind::tray::detail

int main() {
    try { return streamfind::tray::detail::supervise(); }
    catch (const std::exception &error) {
        std::cerr << "streamfind-tray: " << error.what() << '\n';
        return 1;
    }
}

#endif
