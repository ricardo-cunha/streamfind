#pragma once

#include "streamfind/service/service_protocol.hpp"
#include "streamfind/service/result_store.hpp"
#include "streamfind/service/worker_supervisor.hpp"
#include "streamfind/data_proxy_protocol.hpp"

#include <memory>
#include <filesystem>
#include <mutex>
#include <set>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>

namespace streamfind::service {

class ProjectRuntimeManager {
public:
    struct TransientResult {
        std::string result_id;
        std::string run_id;
        std::string format;
        Json descriptor = Json::object();
        std::vector<unsigned char> bytes;
        bool published{false};
    };
    static constexpr const char *kWorkflowIdle = "idle";
    explicit ProjectRuntimeManager(OperationRegistry &operations,
                                   std::filesystem::path configuration_path = {},
                                   std::uint16_t service_port = 0)
        : operations_(&operations), configuration_path_(std::move(configuration_path)), service_port_(service_port) {}
    ~ProjectRuntimeManager();
    ProjectRuntimeManager(const ProjectRuntimeManager &) = delete;
    ProjectRuntimeManager &operator=(const ProjectRuntimeManager &) = delete;

    ProjectSessionDto create(const std::string &session_id, const ProjectOptions &options);
    ProjectSessionDto open(const std::string &session_id, const ProjectOptions &options);
    ProjectSessionDto close(const std::string &session_id);
    Json workflow_definition(const std::string &session_id) const;
    Json set_metadata(const std::string &session_id, const Json &metadata);
    Json audit_trail(const std::string &session_id) const;
    Json copy_project(const std::string &session_id, const std::filesystem::path &destination_database_path);
    Json resolve_workflow_inputs(const std::string &session_id, const std::string &operation_id,
                                 const Json &current_results = Json::object()) const;
    Json validate_workflow(const std::string &session_id, const Json &definition) const;
    Json save_workflow(const std::string &session_id, const Json &definition);
    Json clear_workflow_history(const std::string &session_id);
    Json clear_result_cache(const std::string &session_id);
    Json clear_all_results(const std::string &session_id);
    Json clear_node_results(const std::string &session_id, const std::string &producer_instance);
    Json workflow_snapshot(const std::string &session_id) const;
    Json result_inventory(const std::string &session_id) const;
    Json current_result_inventory(const std::string &session_id) const;
    Json result_query(const std::string &session_id, const Json &request) const;
    Json result_trace(const std::string &session_id, const Json &request) const;
    Json result_data(const std::string &session_id, const Json &request) const;
    Json data_proxy(const std::string &session_id, const data_proxy::Envelope &request) const;
    Json result_cache_lookup(const std::string &session_id, const Json &request) const;
    std::string set_workflow_state(const std::string &session_id, const std::string &state);
    std::string start_workflow(const std::string &session_id,
                               const std::string &execution_mode = "detailed");
    std::string cancel_workflow(const std::string &session_id);
    std::string cancel_operation(const std::string &session_id, const std::string &operation_instance);
    Json operation_snapshot(const std::string &session_id, const std::string &operation_instance) const;
    Json run_operation(const std::string &session_id, const std::string &operation_id,
                       const Json &parameters, const std::string &operation_instance = {},
                       const Json &inputs = Json(nullptr));
    std::vector<ProjectSessionDto> list() const;
    bool contains(const std::string &session_id) const;
    void set_operation_log_callback(std::function<void(const std::string &, std::string_view, std::string_view)> callback);
    void set_operation_event_callback(std::function<void(const std::string &, std::string_view, std::string_view, const Json &)> callback);

private:
    ProjectSessionDto describe(const std::string &session_id, const Project &project) const;
    static std::filesystem::path canonical_database_path(const std::filesystem::path &path);
    void ensure_database_is_not_open(const std::filesystem::path &path) const;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Project>> projects_;
    std::unordered_map<std::string, Workflow> workflow_definitions_;
    std::unordered_map<std::string, std::string> workflow_states_;
    std::unordered_map<std::string, std::thread> workflow_workers_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> workflow_cancellations_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> operation_cancellations_;
    std::unordered_map<std::string, std::shared_ptr<WorkerProcess>> active_workers_;
    std::unordered_map<std::string, Json> workflow_progress_;
    std::unordered_map<std::string, std::string> workflow_execution_modes_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> workflow_operation_steps_;
    OperationRegistry *operations_;
    std::filesystem::path configuration_path_;
    std::uint16_t service_port_{0};
    mutable std::unordered_map<std::string, ResultLease> data_proxy_leases_;
    mutable std::unordered_map<std::string, TransientResult> transient_results_;
    // Feature maps and their EIC/MS traces are read repeatedly by the
    // explorer. Keep a bounded, session-scoped copy after the first map scan
    // so selecting a point does not rescan the Arrow payload.
    mutable std::unordered_map<std::string, Json> feature_result_cache_;
    // Row-key offsets are prepared while the service reads a sampled/detail
    // result. Subsequent app and MCP detail requests can jump directly to the
    // Arrow frame containing the requested rows instead of rescanning the
    // complete immutable result file.
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::uintmax_t>> result_row_key_cache_;
    using NavigationIndex = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::uintmax_t>>>;
    mutable std::unordered_map<std::string, NavigationIndex> result_navigation_index_cache_;
    mutable std::unordered_map<std::string, Json> decoded_trace_cache_;
    std::function<void(const std::string &, std::string_view, std::string_view)> operation_log_callback_;
    std::function<void(const std::string &, std::string_view, std::string_view, const Json &)> operation_event_callback_;
};

}  // namespace streamfind::service
