#pragma once

#include "streamfind/service/service_protocol.hpp"

#include <memory>
#include <filesystem>
#include <mutex>
#include <set>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace streamfind::service {

class ProjectRuntimeManager {
public:
    static constexpr const char *kWorkflowIdle = "idle";
    explicit ProjectRuntimeManager(MethodRegistry &methods, OperationRegistry &operations)
        : methods_(&methods), operations_(&operations) {}
    ProjectRuntimeManager(const ProjectRuntimeManager &) = delete;
    ProjectRuntimeManager &operator=(const ProjectRuntimeManager &) = delete;

    ProjectSessionDto create(const std::string &session_id, const ProjectOptions &options);
    ProjectSessionDto open(const std::string &session_id, const ProjectOptions &options);
    ProjectSessionDto close(const std::string &session_id);
    Json workflow_definition(const std::string &session_id) const;
    Json validate_workflow(const std::string &session_id, const Json &definition) const;
    Json save_workflow(const std::string &session_id, const Json &definition);
    Json clear_workflow_history(const std::string &session_id);
    Json workflow_snapshot(const std::string &session_id) const;
    Json artifact_inventory(const std::string &session_id) const;
    Json artifact_data(const std::string &session_id, const Json &request) const;
    std::string set_workflow_state(const std::string &session_id, const std::string &state);
    std::string start_workflow(const std::string &session_id);
    std::string cancel_workflow(const std::string &session_id);
    Json run_operation(const std::string &session_id, const std::string &operation_id,
                       const Json &parameters, const std::string &operation_instance = {});
    std::vector<ProjectSessionDto> list() const;
    bool contains(const std::string &session_id) const;

private:
    ProjectSessionDto describe(const std::string &session_id, const Project &project) const;
    static std::filesystem::path canonical_database_path(const std::filesystem::path &path);
    void ensure_database_is_not_open(const std::filesystem::path &path) const;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Project>> projects_;
    std::unordered_map<std::string, std::string> workflow_states_;
    std::unordered_map<std::string, std::thread> workflow_workers_;
    std::unordered_map<std::string, std::shared_ptr<CancellationToken>> workflow_cancellations_;
    std::unordered_map<std::string, Json> workflow_progress_;
    MethodRegistry *methods_;
    OperationRegistry *operations_;
};

}  // namespace streamfind::service
