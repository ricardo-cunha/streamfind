#pragma once

#include "streamfind/project.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace streamfind::service {

struct WorkerMetadata {
    std::string id;
    std::string kind{"operation"};
    std::string operation_id;
    std::string operation;
    std::uint64_t process_id{0};
    std::uint64_t process_group_id{0};
    std::string job_id;
    std::string state{"starting"};
    std::string cancellation_mode{"process"};

    Json to_json() const;
};

struct WorkerRequest {
    std::filesystem::path executable;
    std::filesystem::path configuration;
    std::uint16_t service_port{0};
    std::string session_id;
    std::string run_id;
    std::string operation_id;
    std::string operation;
    std::string operation_instance;
    std::string computation_key;
    Json workflow = Json::object();
    Json parameters = Json::object();
    Json inputs = nullptr;
    bool persist_outputs{true};
};

struct WorkerResult {
    WorkerMetadata worker;
    Json result = Json::object();
    std::string error;
    int exit_code{0};
    bool cancelled{false};
};

class WorkerProcess {
public:
    using EventCallback = std::function<void(const WorkerMetadata &, std::string_view,
                                              std::string_view, const Json &)>;

    WorkerProcess(WorkerRequest request, EventCallback callback = {});
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess &) = delete;
    WorkerProcess &operator=(const WorkerProcess &) = delete;

    WorkerResult run();
    void cancel();
    WorkerMetadata metadata() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streamfind::service
