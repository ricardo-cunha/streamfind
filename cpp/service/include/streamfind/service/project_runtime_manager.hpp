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
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>
#include <deque>
#include <stdexcept>

namespace streamfind::service {

class WorkflowRevisionConflict final : public std::runtime_error {
public:
    WorkflowRevisionConflict(int expected, int current)
        : std::runtime_error("workflow definition revision conflict: expected " +
                             std::to_string(expected) + ", current revision is " +
                             std::to_string(current) + "; reload before saving"),
          expected_(expected), current_(current) {}
    int expected() const noexcept { return expected_; }
    int current() const noexcept { return current_; }
private:
    int expected_;
    int current_;
};

class ProjectRuntimeManager {
public:
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
    Json health_snapshot() const;
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
    Json start_operation(const std::string &session_id, const std::string &operation_id,
                         const Json &parameters, const std::string &operation_instance = {},
                         const Json &inputs = Json(nullptr));
    Json run_operation(const std::string &session_id, const std::string &operation_id,
                       const Json &parameters, const std::string &operation_instance = {},
                       const Json &inputs = Json(nullptr),
                       std::shared_ptr<std::atomic_bool> cancellation = {});
    std::vector<ProjectSessionDto> list() const;
    bool contains(const std::string &session_id) const;
    void set_operation_log_callback(std::function<void(const std::string &, std::string_view, std::string_view)> callback);
    void set_operation_event_callback(std::function<void(const std::string &, std::string_view, std::string_view, const Json &)> callback);

private:
    ProjectSessionDto describe(const std::string &session_id, const Project &project) const;
    static std::filesystem::path canonical_database_path(const std::filesystem::path &path);
    void ensure_database_is_not_open(const std::filesystem::path &path) const;
    void abort_worker_output_leases(const std::string &session_id, const std::string &run_id) noexcept;
    mutable std::mutex mutex_;
    // Result file queries may outlive the session-map lookup. Keep result
    // cache access independent so immutable file reads never hold mutex_.
    mutable std::mutex result_cache_mutex_;
    std::unordered_map<std::string, std::unique_ptr<Project>> projects_;
    std::unordered_map<std::string, Workflow> workflow_definitions_;
    std::unordered_map<std::string, std::string> workflow_states_;
    std::unordered_map<std::string, std::thread> workflow_workers_;
    std::unordered_map<std::string, std::thread> operation_workers_;
    std::unordered_map<std::string, Json> operation_runs_;
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
    // Row-key offsets are prepared while the service reads a sampled/detail
    // result. Subsequent app and MCP detail requests can jump directly to the
    // Arrow frame containing the requested rows instead of rescanning the
    // complete immutable result file.
    mutable std::unordered_map<std::string, std::shared_ptr<const std::unordered_map<std::string, std::uintmax_t>>> result_row_key_cache_;
    using NavigationIndex = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::uintmax_t>>>;
    mutable std::unordered_map<std::string, std::shared_ptr<const NavigationIndex>> result_navigation_index_cache_;
    mutable std::unordered_map<std::string, Json> decoded_trace_cache_;
    mutable std::deque<std::string> decoded_trace_cache_order_;
    mutable std::size_t decoded_trace_cache_bytes_{0};
    static constexpr std::size_t kDecodedTraceCacheBytes = 32 * 1024 * 1024;
    static constexpr std::size_t kDecodedTraceCacheEntries = 128;
    void clear_decoded_trace_cache_locked() const noexcept {
        decoded_trace_cache_.clear();
        decoded_trace_cache_order_.clear();
        decoded_trace_cache_bytes_ = 0;
    }
    std::function<void(const std::string &, std::string_view, std::string_view)> operation_log_callback_;
    std::function<void(const std::string &, std::string_view, std::string_view, const Json &)> operation_event_callback_;
};

}  // namespace streamfind::service
