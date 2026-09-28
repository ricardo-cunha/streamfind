#pragma once

#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/service_plugin_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace streamfind::service {

class EventBroker {
public:
    void add(std::intptr_t socket);
    void remove(std::intptr_t socket);
    void publish(const Json &event);

private:
    std::mutex mutex_;
    std::vector<int> clients_;
};

class ServiceServer {
public:
    ServiceServer(std::uint16_t port, const std::filesystem::path &configuration_path,
        const std::filesystem::path &application_root = {});
    ServiceServer(const ServiceServer &) = delete;
    ServiceServer &operator=(const ServiceServer &) = delete;
    ~ServiceServer();

    void run();
    void stop();
    std::uint16_t port() const noexcept { return port_; }

private:
    void handle_client(std::intptr_t socket);
    std::uint16_t port_;
    std::filesystem::path application_root_;
    std::atomic<bool> stopping_{false};
    std::intptr_t listener_{-1};
    MethodRegistry methods_;
    OperationRegistry operations_;
    ServicePluginRuntime plugin_runtime_;
    ProjectRuntimeManager projects_;
    EventBroker events_;
};

}  // namespace streamfind::service
