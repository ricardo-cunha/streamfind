#include "streamfind/service_runtime_client.hpp"
#include "../include/streamfind/service/service_discovery.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

namespace streamfind::tray::detail {

constexpr UINT kRefreshTimer = 1;
constexpr UINT kTrayMessage = WM_USER + 1;
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT kOpenApplication = 1001;
constexpr UINT kOpenStatus = 1002;
constexpr UINT kCopyEndpoint = 1003;
constexpr UINT kOpenActivity = 1004;
constexpr UINT kStartService = 1005;
constexpr UINT kStopService = 1006;
constexpr UINT kExitTray = 1007;
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

void start_service() {
    const auto tray_path = tray_executable_path();
    const auto bin_end = tray_path.find_last_of("\\/");
    if (bin_end == std::string::npos) throw std::runtime_error("unable to locate streamfind package");
    const auto bin = tray_path.substr(0, bin_end);
    const auto package_end = bin.find_last_of("\\/");
    if (package_end == std::string::npos) throw std::runtime_error("unable to locate streamfind package");
    const auto cli = bin.substr(0, package_end + 1) + "bin\\streamfind.exe";
    std::string command_line = "\"" + cli + "\" service start";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, bin.c_str(), &startup, &process)) {
        throw std::runtime_error("unable to start streamfind service");
    }
    CloseHandle(process.hThread);
    const auto wait_result = WaitForSingleObject(process.hProcess, 15000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        throw std::runtime_error("timed out while starting streamfind service");
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0) throw std::runtime_error("streamfind service start failed");
}

void stop_service(const std::string &instance) {
    const auto tray_path = tray_executable_path();
    const auto bin_end = tray_path.find_last_of("\\/");
    if (bin_end == std::string::npos) throw std::runtime_error("unable to locate streamfind package");
    const auto bin = tray_path.substr(0, bin_end);
    const auto package_end = bin.find_last_of("\\/");
    if (package_end == std::string::npos) throw std::runtime_error("unable to locate streamfind package");
    const auto cli = bin.substr(0, package_end + 1) + "bin\\streamfind.exe";
    std::string command_line = "\"" + cli + "\" service stop --instance \"" + instance + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, bin.c_str(), &startup, &process)) {
        throw std::runtime_error("unable to stop streamfind service");
    }
    CloseHandle(process.hThread);
    const auto wait_result = WaitForSingleObject(process.hProcess, 15000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        throw std::runtime_error("timed out while stopping streamfind service");
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0) throw std::runtime_error("streamfind service stop failed");
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
    int run(HINSTANCE instance) {
        mutex_ = CreateMutexW(nullptr, TRUE, L"Local\\StreamFindTrayManager");
        if (mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
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
        else if (message == WM_TIMER && wparam == kRefreshTimer) self->refresh();
        else if (message == WM_COMMAND) self->command(LOWORD(wparam));
        else if (message == WM_DESTROY) {
            KillTimer(window, kRefreshTimer);
            self->stop_refresh_worker();
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
        AppendMenuW(menu, MF_STRING, kOpenApplication, L"Open streamfind");
        AppendMenuW(menu, MF_STRING, kOpenStatus, L"Open streamfind service status");
        AppendMenuW(menu, MF_STRING, kCopyEndpoint, L"Copy endpoint");
        AppendMenuW(menu, MF_STRING, kOpenActivity, L"Open activity log");
        AppendMenuW(menu, MF_STRING | (has_service ? MF_GRAYED : 0), kStartService, L"Start streamfind service");
        AppendMenuW(menu, MF_STRING | (has_service ? 0 : MF_GRAYED), kStopService, L"Stop streamfind service");
        AppendMenuW(menu, MF_STRING, kExitTray, L"Exit tray manager");
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
        if (command_id == kStartService) {
            try { start_service(); refresh(); }
            catch (...) { MessageBoxW(window_, L"Unable to start the streamfind service.", L"streamfind", MB_OK | MB_ICONERROR); }
            return;
        }
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
            try { stop_service(instance); refresh(); }
            catch (...) { MessageBoxW(window_, L"Unable to stop the streamfind service.", L"streamfind", MB_OK | MB_ICONERROR); }
        } else if (command_id == kExitTray) DestroyWindow(window_);
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

int main() {
    std::cout << "streamfind tray manager: no StatusNotifier/AppIndicator host is available in this build.\n"
              << "Use 'streamfind service status' or the service web status page.\n" << std::flush;
    try {
        for (;;) {
            const auto services = streamfind::service::list_services();
            if (!services.empty()) std::cout << services.dump() << '\n' << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    } catch (const std::exception &error) {
        std::cerr << "streamfind-tray: " << error.what() << '\n';
        return 1;
    }
}

#endif
