#pragma once

#include "streamfind/project.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace streamfind::service {

struct WorkerHandleGrant;

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
    std::string last_progress_at;
    std::string peak_memory_kind;
    std::uint64_t peak_memory_bytes{0};

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
    std::filesystem::path project_database_path;
    std::string computation_key;
    Json workflow = Json::object();
    Json parameters = Json::object();
    Json inputs = nullptr;
    std::uint64_t memory_limit_mb{0};
};

struct WorkerResult {
    WorkerMetadata worker;
    Json result = Json::object();
    std::string error;
    int exit_code{0};
    bool cancelled{false};
    bool memory_limit_exceeded{false};
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
    std::string transfer_handle(WorkerHandleGrant grant, std::uintptr_t source_handle);
    WorkerMetadata metadata() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streamfind::service
