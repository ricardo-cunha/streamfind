#pragma once

#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/service_plugin_runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace streamfind::service {

class EventBroker {
public:
    EventBroker();
    ~EventBroker();
    EventBroker(const EventBroker &) = delete;
    EventBroker &operator=(const EventBroker &) = delete;
    void add(std::intptr_t socket);
    void remove(std::intptr_t socket);
    void publish(const Json &event);
    Json history(const std::string &project_id, std::uint64_t after_event_id = 0,
                 std::size_t limit = 20000) const;

private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::intptr_t> clients_;
    std::deque<Json> pending_;
    std::unordered_map<std::string, std::deque<Json>> history_;
    std::thread worker_;
    bool stopping_{false};
    std::uint64_t next_event_id_{1};
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
    Json workflow_demos() const;
    void handle_client(std::intptr_t socket);
    std::uint16_t port_;
    std::filesystem::path application_root_;
    std::filesystem::path runtime_root_;
    std::atomic<bool> stopping_{false};
    std::intptr_t listener_{-1};
    MethodRegistry methods_;
    OperationRegistry operations_;
    ServicePluginRuntime plugin_runtime_;
    ProjectRuntimeManager projects_;
    EventBroker events_;
};

}  // namespace streamfind::service
