#include "streamfind/service/service_server.hpp"
#include "streamfind/service/service_discovery.hpp"
#include "streamfind/service_configuration.hpp"
#include "streamfind/vendor_runtime.hpp"

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace streamfind::service::process_detail {

#ifndef _WIN32
volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }
#endif

}

namespace streamfind::service::process_detail {

bool process_alive(const std::uint64_t process_id) {
    if (process_id == 0) return true;
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
    if (process == nullptr) return false;
    const auto state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
#else
    return kill(static_cast<pid_t>(process_id), 0) == 0 || errno == EPERM;
#endif
}

}  // namespace streamfind::service::process_detail

int main(int argc, char **argv) {
    streamfind::configure_vendor_runtime_paths();
    const auto executable_path = argc > 0
                                     ? std::filesystem::absolute(argv[0])
                                     : std::filesystem::current_path() / "streamfind-server";
    std::uint16_t port = 0;
    std::uint64_t manager_process_id = 0;
    std::filesystem::path manager_executable;
    std::filesystem::path configuration_path;
    streamfind::ServiceConfiguration configuration;
    try {
        configuration_path = executable_path.parent_path() / "streamfind.json";
        configuration = streamfind::load_service_configuration(configuration_path);
        port = configuration.port;
    } catch (const std::exception &error) {
        std::cerr << "streamfind-server: " << error.what() << '\n';
        return 2;
    }
    if (argc > 1) {
        const auto parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed > 65535) {
            std::cerr << "streamfind-server: invalid port\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--manager-pid" && index + 1 < argc) {
            manager_process_id = std::stoull(argv[++index]);
        } else if (argument == "--manager-executable" && index + 1 < argc) {
            manager_executable = std::filesystem::path(argv[++index]);
        } else {
            std::cerr << "streamfind-server: unknown argument " << argument << '\n';
            return 2;
        }
    }
    if ((manager_process_id == 0) != manager_executable.empty()) {
        std::cerr << "streamfind-server: manager PID and executable must be supplied together\n";
        return 2;
    }
#ifndef _WIN32
    std::signal(SIGINT, streamfind::service::process_detail::request_stop);
    std::signal(SIGTERM, streamfind::service::process_detail::request_stop);
#endif
    streamfind::service::ServiceDiscovery discovery;
    try {
        const auto application_root = executable_path.parent_path().parent_path() / "app";
        const auto record = discovery.publish(port, streamfind::service::kProtocolVersion,
                                              manager_process_id, manager_executable);
        auto service_identity = record.to_json();
        service_identity.erase("token");
        streamfind::service::ServiceServer server(
            port, configuration_path, application_root,
            service_identity, record.token);
        std::atomic<bool> monitor_stopping{false};
        std::thread manager_monitor;
        bool monitor_required = record.manager_process_id != 0;
#ifndef _WIN32
        monitor_required = true;
#endif
        if (monitor_required) {
            manager_monitor = std::thread([&server, &monitor_stopping, manager_pid = record.manager_process_id] {
                while (!monitor_stopping.load()) {
                    if (!streamfind::service::process_detail::process_alive(manager_pid)) {
                        server.stop();
                        return;
                    }
#ifndef _WIN32
                    if (streamfind::service::process_detail::stop_requested != 0) {
                        server.stop();
                        return;
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            });
        }
        std::cout << "streamfind-server listening on port " << port << '\n' << std::flush;
        std::cout << "streamfind service instance " << record.service_instance_id
                  << " (PID " << record.process_id << ")\n" << std::flush;
        server.run();
        monitor_stopping.store(true);
        if (manager_monitor.joinable()) manager_monitor.join();
        discovery.update_state("stopped");
        discovery.remove();
    } catch (const std::exception &error) {
        discovery.update_state("failed");
        discovery.remove();
        std::cerr << "streamfind-server: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
