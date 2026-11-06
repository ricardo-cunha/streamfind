#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/service/worker_handle_broker.hpp"
#include "streamfind/fingerprint.hpp"
#include "streamfind/service_configuration.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace streamfind::service {

namespace detail {

std::uint64_t configured_worker_memory_limit_mb(const std::filesystem::path &configuration_path) {
    return load_service_configuration(configuration_path).worker_memory_limit_mb;
}

std::string quoted_identifier(const std::string &value) {
    if (value.empty()) throw std::invalid_argument("data-proxy identifier is empty");
    for (const auto character : value)
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_'))
            throw std::invalid_argument("data-proxy identifier is invalid");
    return "\"" + value + "\"";
}

Json workflow_without_positions(Json workflow) {
    for (auto &operation : workflow["operations"]) operation.erase("position");
    return workflow;
}

std::string create_data_proxy_lease_id() {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "lease-" << std::hex << now << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

std::string create_operation_run_id() {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "operation-run-" << std::hex << now << '-'
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

std::int64_t unix_time_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Json require_typed_trace_array(const Json &value) {
    if (!value.is_array())
        throw std::invalid_argument("typed trace result column must be an array; regenerate this result");
    return value;
}

}  // namespace detail

ProjectRuntimeManager::~ProjectRuntimeManager() {
    std::vector<std::shared_ptr<WorkerProcess>> workers;
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        for (auto &[session_id, worker] : active_workers_) {
            static_cast<void>(session_id);
            if (worker) workers.push_back(worker);
        }
        for (auto &[session_id, cancellation] : workflow_cancellations_) {
            static_cast<void>(session_id);
            if (cancellation) cancellation->store(true);
        }
        for (auto &[session_id, worker] : workflow_workers_) {
            static_cast<void>(session_id);
            threads.push_back(std::move(worker));
        }
        workflow_workers_.clear();
        for (auto &[session_id, worker] : operation_workers_) {
            static_cast<void>(session_id);
            threads.push_back(std::move(worker));
        }
        operation_workers_.clear();
    }
    for (const auto &worker : workers) if (worker) worker->cancel();
    for (auto &thread : threads) if (thread.joinable()) thread.join();
}

void ProjectRuntimeManager::abort_worker_output_leases(const std::string &session_id,
                                                        const std::string &run_id) noexcept {
    try {
        std::lock_guard lock(mutex_);
        const auto project = projects_.find(session_id);
        if (project == projects_.end()) return;
        ResultStore store(project->second->get_database_path());
        const auto prefix = session_id + ":";
        for (auto iterator = data_proxy_leases_.begin(); iterator != data_proxy_leases_.end();) {
            if (iterator->first.rfind(prefix, 0) == 0 && iterator->second.run_id == run_id) {
                try { store.abort(iterator->second); } catch (...) {}
                iterator = data_proxy_leases_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    } catch (...) {
        // Worker completion must not be masked by best-effort staging cleanup.
    }
}

std::filesystem::path ProjectRuntimeManager::canonical_database_path(const std::filesystem::path &path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) result = std::filesystem::absolute(path, error).lexically_normal();
    return result.lexically_normal();
}

void ProjectRuntimeManager::ensure_database_is_not_open(const std::filesystem::path &path) const {
    const auto canonical = canonical_database_path(path);
    for (const auto &[session_id, project] : projects_) {
        if (canonical_database_path(project->get_database_path()) == canonical)
            throw std::invalid_argument("database is already open in project session " + session_id);
    }
}

ProjectSessionDto ProjectRuntimeManager::describe(const std::string &session_id,
                                                   const Project &project) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(project.get_database_path(), error);
    const ResultStore result_store(project.get_database_path());
    // Project opening must not hash every multi-gigabyte payload. Individual result
    // access performs full integrity validation; inspection still reports the
    // lightweight store size for the project card.
    const auto inspection = result_store.inspect(false);
    return ProjectSessionDto{session_id,
                             project.get_database_path().string(),
                             error ? 0 : size,
                             project.get_domains(),
                             project.get_metadata(),
                             inspection.to_json()};
}

ProjectSessionDto ProjectRuntimeManager::create(const std::string &session_id,
                                                 const ProjectOptions &options) {
    std::lock_guard lock(mutex_);
    if (projects_.contains(session_id)) throw std::invalid_argument("project session already exists");
    ensure_database_is_not_open(options.database_path);
    auto project = std::make_unique<Project>(Project::create(options));
    ResultStore(project->get_database_path()).initialize(false);
    project->set_result_cache_lookup([database_path = project->get_database_path()](const std::string &key) {
        return ResultStore(database_path).find_cached_results(key);
    });
    project->set_workflow_input_resolver([this, session_id](const std::string &operation_id, const Json &current_results) {
        return resolve_workflow_inputs(session_id, operation_id, current_results);
    });
    if (operation_log_callback_)
        project->set_operation_log_callback([this, session_id](std::string_view operation_id, std::string_view message) {
            operation_log_callback_(session_id, operation_id, message);
        });
    if (operation_event_callback_)
        project->set_operation_event_callback([this, session_id](std::string_view operation_id, std::string_view type, const Json &payload) {
            if (type == "operation.started" || type == "operation.completed") {
                std::lock_guard lock(mutex_);
                auto &progress = workflow_progress_[session_id];
                if (type == "operation.started") {
                    const auto steps = workflow_operation_steps_.find(session_id);
                    if (steps != workflow_operation_steps_.end()) {
                        const auto step = steps->second.find(std::string(operation_id));
                        if (step != steps->second.end()) progress["current_step"] = step->second;
                    }
                } else {
                    progress["completed"] = progress.value("completed", 0) + 1;
                }
            }
            operation_event_callback_(session_id, operation_id, type, payload);
        });
    const auto result = describe(session_id, *project);
    workflow_definitions_[session_id] = project->get_workflow();
    projects_.emplace(session_id, std::move(project));
    workflow_states_[session_id] = kWorkflowIdle;
    return result;
}

ProjectSessionDto ProjectRuntimeManager::open(const std::string &session_id,
                                               const ProjectOptions &options) {
    std::lock_guard lock(mutex_);
    if (projects_.contains(session_id)) throw std::invalid_argument("project session already exists");
    ensure_database_is_not_open(options.database_path);
    auto project = std::make_unique<Project>(Project::open(options));
    // Existing projects may predate the sidecar result store. Initialize it
    // while the project session is opened so the first worker can publish
    // results without failing on a missing store.
    ResultStore(project->get_database_path()).initialize(false);
    project->set_result_cache_lookup([database_path = project->get_database_path()](const std::string &key) {
        return ResultStore(database_path).find_cached_results(key);
    });
    project->set_workflow_input_resolver([this, session_id](const std::string &operation_id, const Json &current_results) {
        return resolve_workflow_inputs(session_id, operation_id, current_results);
    });
    if (operation_log_callback_)
        project->set_operation_log_callback([this, session_id](std::string_view operation_id, std::string_view message) {
            operation_log_callback_(session_id, operation_id, message);
        });
    if (operation_event_callback_)
        project->set_operation_event_callback([this, session_id](std::string_view operation_id, std::string_view type, const Json &payload) {
            if (type == "operation.started" || type == "operation.completed") {
                std::lock_guard lock(mutex_);
                auto &progress = workflow_progress_[session_id];
                if (type == "operation.started") {
                    const auto steps = workflow_operation_steps_.find(session_id);
                    if (steps != workflow_operation_steps_.end()) {
                        const auto step = steps->second.find(std::string(operation_id));
                        if (step != steps->second.end()) progress["current_step"] = step->second;
                    }
                } else {
                    progress["completed"] = progress.value("completed", 0) + 1;
                }
            }
            operation_event_callback_(session_id, operation_id, type, payload);
        });
    const auto result = describe(session_id, *project);
    workflow_definitions_[session_id] = project->get_workflow();
    projects_.emplace(session_id, std::move(project));
    workflow_states_[session_id] = kWorkflowIdle;
    return result;
}

ProjectSessionDto ProjectRuntimeManager::close(const std::string &session_id) {
    std::thread worker;
    std::thread operation_worker;
    ProjectSessionDto result;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        if (operation_cancellations_.contains(session_id))
            throw std::invalid_argument("cannot close a project while an operation is running");
        if (const auto run = operation_runs_.find(session_id); run != operation_runs_.end() &&
            (run->second.value("state", "") == "queued" || run->second.value("state", "") == "running" ||
             run->second.value("state", "") == "cancelling"))
            throw std::invalid_argument("cannot close a project while an operation is running");
        result = describe(session_id, *iterator->second);
        if (const auto cancellation = workflow_cancellations_.find(session_id); cancellation != workflow_cancellations_.end()) {
            cancellation->second->store(true);
            workflow_states_[session_id] = "cancelling";
        }
        if (const auto active = active_workers_.find(session_id); active != active_workers_.end() && active->second)
            active->second->cancel();
        if (const auto worker_iterator = workflow_workers_.find(session_id); worker_iterator != workflow_workers_.end()) {
            worker = std::move(worker_iterator->second);
            workflow_workers_.erase(worker_iterator);
        }
        if (const auto operation_iterator = operation_workers_.find(session_id);
            operation_iterator != operation_workers_.end()) {
            operation_worker = std::move(operation_iterator->second);
            operation_workers_.erase(operation_iterator);
        }
    }
    if (worker.joinable()) worker.join();
    if (operation_worker.joinable()) operation_worker.join();
    {
        std::lock_guard lock(mutex_);
        projects_.erase(session_id);
        {
            std::lock_guard cache_lock(result_cache_mutex_);
            result_row_key_cache_.clear();
            result_navigation_index_cache_.clear();
        }
        clear_decoded_trace_cache_locked();
        workflow_definitions_.erase(session_id);
        workflow_states_.erase(session_id);
        workflow_cancellations_.erase(session_id);
        workflow_progress_.erase(session_id);
        workflow_execution_modes_.erase(session_id);
        workflow_operation_steps_.erase(session_id);
        operation_runs_.erase(session_id);
    }
    return result;
}

Json ProjectRuntimeManager::workflow_definition(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto workflow = workflow_definitions_.at(session_id);
    Json diagnostics = Json::array();
    try {
        workflow.validate(*operations_);
    } catch (const std::exception &error) {
        diagnostics.push_back(Json{{"message", error.what()}});
    }
    return Json{{"session_id", session_id}, {"state", diagnostics.empty() ? "validated" : "failed"},
                {"workflow", workflow.to_json()}, {"valid", diagnostics.empty()}, {"diagnostics", diagnostics}};
}

Json ProjectRuntimeManager::set_metadata(const std::string &session_id, const Json &metadata) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->set_metadata(metadata);
    Json response;
    to_json(response, describe(session_id, *iterator->second));
    return response;
}

Json ProjectRuntimeManager::audit_trail(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    Json output = Json::array();
    for (const auto &entry : iterator->second->get_audit_trail())
        output.push_back({{"operation_type", entry.operation_type},
                          {"object_type", entry.object_type},
                          {"details", entry.details},
                          {"created_at", entry.created_at}});
    return output;
}

Json ProjectRuntimeManager::copy_project(const std::string &session_id,
                                         const std::filesystem::path &destination_database_path) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    ensure_database_is_not_open(destination_database_path);
    ProjectOptions options;
    options.database_path = destination_database_path;
    auto copy = iterator->second->copy(options);
    ResultStore(copy.get_database_path()).initialize();
    Json response;
    to_json(response, describe(session_id, copy));
    response["session_id"] = nullptr;
    return response;
}

Json ProjectRuntimeManager::validate_workflow(const std::string &session_id, const Json &definition) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    auto workflow = definition.is_null() ? workflow_definitions_.at(session_id) : Workflow::from_json(definition);
    Json diagnostics = Json::array();
    try {
        workflow.validate(*operations_);
    } catch (const std::exception &error) {
        diagnostics.push_back(Json{{"message", error.what()}});
    }
    return Json{{"session_id", session_id}, {"state", diagnostics.empty() ? "validated" : "failed"},
                {"workflow", workflow.to_json()}, {"valid", diagnostics.empty()}, {"diagnostics", diagnostics}};
}

Json ProjectRuntimeManager::save_workflow(const std::string &session_id, const Json &definition) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    if (!definition.is_object() || !definition.contains("definition_revision") ||
        !definition.at("definition_revision").is_number_integer())
        throw std::invalid_argument("workflow save requires the definition_revision returned by the latest workflow read");
    auto workflow = Workflow::from_json(definition);
    const auto current = workflow_definitions_.at(session_id);
    const auto expected_revision = workflow.definition_revision;
    if (expected_revision != current.definition_revision)
        throw WorkflowRevisionConflict(expected_revision, current.definition_revision);
    if (workflow.workflow_id.empty()) workflow.workflow_id = current.workflow_id.empty() ? "workflow" : current.workflow_id;
    const bool layout_only = detail::workflow_without_positions(workflow.to_json()) ==
                             detail::workflow_without_positions(current.to_json());
    workflow.version = layout_only ? current.version : std::max(current.version + 1, workflow.version);
    workflow.definition_revision = current.definition_revision + 1;
    iterator->second->set_workflow(workflow, *operations_);
    workflow_definitions_[session_id] = workflow;
    return Json{{"workflow", workflow.to_json()}, {"valid", true}, {"diagnostics", Json::array()}};
}

Json ProjectRuntimeManager::clear_workflow_history(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_workflow_history();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_result_cache(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_result_cache();
    {
        std::lock_guard cache_lock(result_cache_mutex_);
        result_row_key_cache_.clear();
        result_navigation_index_cache_.clear();
    }
    clear_decoded_trace_cache_locked();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_all_results(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto inventory = iterator->second->get_result_inventory();
    std::vector<std::string> result_ids;
    for (const auto &entry : inventory) {
        const auto value = entry.find("result_id");
        if (value != entry.end() && value->is_string() && !value->get<std::string>().empty())
            result_ids.push_back(value->get<std::string>());
    }
    const ResultStore store(iterator->second->get_database_path());
    for (const auto &manifest : store.published_results()) {
        const auto value = manifest.find("result_id");
        if (value != manifest.end() && value->is_string() && !value->get<std::string>().empty())
            result_ids.push_back(value->get<std::string>());
    }
    iterator->second->clear_all_results();
    store.remove_published_results(result_ids);
    {
        std::lock_guard cache_lock(result_cache_mutex_);
        result_row_key_cache_.clear();
        result_navigation_index_cache_.clear();
    }
    clear_decoded_trace_cache_locked();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_node_results(const std::string &session_id, const std::string &producer_instance) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto inventory = iterator->second->get_result_inventory();
    std::vector<std::string> result_ids;
    for (const auto &entry : inventory) {
        const auto producer = entry.find("producer_instance");
        const auto result = entry.find("result_id");
        if (producer != entry.end() && producer->is_string() && producer->get<std::string>() == producer_instance &&
            result != entry.end() && result->is_string() && !result->get<std::string>().empty())
            result_ids.push_back(result->get<std::string>());
    }
    const ResultStore store(iterator->second->get_database_path());
    for (const auto &manifest : store.published_results()) {
        const auto producer = manifest.find("producer_instance");
        const auto result = manifest.find("result_id");
        if (producer != manifest.end() && producer->is_string() && producer->get<std::string>() == producer_instance &&
            result != manifest.end() && result->is_string() && !result->get<std::string>().empty())
            result_ids.push_back(result->get<std::string>());
    }
    iterator->second->clear_results_for_producer_instance(producer_instance);
    store.remove_published_results(result_ids);
    {
        std::lock_guard cache_lock(result_cache_mutex_);
        result_row_key_cache_.clear();
        result_navigation_index_cache_.clear();
    }
    clear_decoded_trace_cache_locked();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}, {"producer_instance", producer_instance}};
}

Json ProjectRuntimeManager::workflow_snapshot(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto workflow = workflow_definitions_.at(session_id);
    const auto state = workflow_states_.find(session_id);
    const std::string effective_state = state == workflow_states_.end() ? kWorkflowIdle : state->second;
    const auto progress = workflow_progress_.find(session_id);
    const auto progress_snapshot = progress == workflow_progress_.end()
        ? Json{{"completed", 0}, {"total", workflow.operations.size()}, {"current_step", 0}}
        : progress->second;
    const auto active_worker = active_workers_.find(session_id);
    if (active_worker != active_workers_.end() && active_worker->second) {
        const auto metadata = active_worker->second->metadata();
        const bool workflow_active = effective_state == "queued" || effective_state == "running" ||
                                     effective_state == "cancelling" || workflow_cancellations_.contains(session_id);
        const bool operation_active = operation_cancellations_.contains(session_id);
        if (!workflow_active && !operation_active) {
            return Json{{"session_id", session_id}, {"state", "idle"},
                        {"progress", progress_snapshot},
                        {"execution", Json::array()}, {"worker", metadata.to_json()}};
        }
        const auto worker_state = metadata.state == "cancelling" ? "cancelling" : "running";
        return Json{{"session_id", session_id}, {"state", worker_state},
                    {"progress", progress_snapshot},
                    {"execution", Json::array()}, {"worker", metadata.to_json()}};
    }
    if (effective_state == "queued" || effective_state == "running" || effective_state == "cancelling") {
        Json snapshot{{"session_id", session_id}, {"state", effective_state},
                    {"progress", progress_snapshot},
                    {"execution", Json::array()}};
        snapshot["worker"] = nullptr;
        return snapshot;
    }
    const auto summary = iterator->second->get_workflow_execution_summary();
    std::string persisted_state = effective_state;
    const auto completed = [&summary] {
        try { return static_cast<std::size_t>(std::stoull(summary.value("completed_steps", std::string{"0"}))); }
        catch (...) { return std::size_t{0}; }
    }();
    const auto current_step = [&summary] {
        try { return static_cast<std::uint64_t>(std::stoull(summary.value("current_step", std::string{"0"}))); }
        catch (...) { return std::uint64_t{0}; }
    }();
    const auto execution_status = summary.value("execution_status", std::string{"idle"});
    if (execution_status == "failed") persisted_state = "failed";
    else if (execution_status == "running") persisted_state = "running";
    else if (execution_status == "completed" || (!workflow.operations.empty() && completed >= workflow.operations.size()))
        persisted_state = "completed";
    Json snapshot{{"session_id", session_id}, {"state", persisted_state},
                {"progress", progress_snapshot},
                {"execution_available", true}};
    snapshot["worker"] = nullptr;
    return snapshot;
}

Json ProjectRuntimeManager::health_snapshot() const {
    std::lock_guard lock(mutex_);
    std::size_t active_workflows = 0;
    for (const auto &[session_id, state] : workflow_states_) {
        (void)session_id;
        if (state == "queued" || state == "running" || state == "cancelling") ++active_workflows;
    }
    const auto active_workers = active_workers_.size();
    return Json{{"alive", true}, {"ready", true},
                {"state", active_workers > 0 || active_workflows > 0 ? "busy" : "ready"},
                {"active_workers", active_workers}, {"active_workflows", active_workflows},
                {"attached_projects", projects_.size()}};
}

Json ProjectRuntimeManager::operation_snapshot(const std::string &session_id,
                                               const std::string &operation_instance) const {
    std::lock_guard lock(mutex_);
    if (!projects_.contains(session_id)) throw std::invalid_argument("project session not found");
    const auto run = operation_runs_.find(session_id);
    if (run != operation_runs_.end() &&
        run->second.value("operation_instance", std::string{}) == operation_instance) {
        auto snapshot = run->second;
        const auto worker = active_workers_.find(session_id);
        snapshot["worker"] = worker != active_workers_.end() && worker->second
            ? worker->second->metadata().to_json() : Json(nullptr);
        return snapshot;
    }
    const auto worker = active_workers_.find(session_id);
    if (worker == active_workers_.end())
        return Json{{"session_id", session_id}, {"operation_instance", operation_instance},
                    {"state", "idle"}, {"worker", nullptr}};
    auto metadata = worker->second->metadata();
    return Json{{"session_id", session_id}, {"operation_instance", operation_instance},
                {"state", metadata.state}, {"worker", metadata.to_json()}};
}

Json ProjectRuntimeManager::start_operation(const std::string &session_id,
                                             const std::string &operation_id,
                                             const Json &parameters,
                                             const std::string &operation_instance,
                                             const Json &inputs) {
    const auto effective_instance = operation_instance.empty() ? operation_id : operation_instance;
    const auto run_id = detail::create_operation_run_id();
    std::thread previous;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    Json accepted{{"run_id", run_id}, {"session_id", session_id},
                  {"operation_id", operation_id}, {"operation_instance", effective_instance},
                  {"state", "queued"}, {"started_at_ms", detail::unix_time_milliseconds()},
                  {"updated_at_ms", detail::unix_time_milliseconds()}, {"worker", nullptr}};
    {
        std::lock_guard lock(mutex_);
        if (!projects_.contains(session_id)) throw std::invalid_argument("project session not found");
        if (!operations_->find(operation_id)) throw std::invalid_argument("operation not found: " + operation_id);
        if (workflow_cancellations_.contains(session_id) || operation_cancellations_.contains(session_id))
            throw std::invalid_argument("another workflow or operation is already running for this project");
        if (const auto run = operation_runs_.find(session_id); run != operation_runs_.end() &&
            (run->second.value("state", "") == "queued" || run->second.value("state", "") == "running" ||
             run->second.value("state", "") == "cancelling"))
            throw std::invalid_argument("an operation is already running for this project");
        if (const auto worker = operation_workers_.find(session_id); worker != operation_workers_.end()) {
            previous = std::move(worker->second);
            operation_workers_.erase(worker);
        }
        operation_runs_[session_id] = accepted;
        operation_cancellations_[session_id] = cancellation;
    }
    if (previous.joinable()) previous.join();
    try {
        if (operation_event_callback_)
            operation_event_callback_(session_id, operation_id, "operation.started", accepted);
        std::lock_guard lock(mutex_);
        if (!projects_.contains(session_id)) throw std::invalid_argument("project session not found");
        const auto current = operation_runs_.find(session_id);
        if (current == operation_runs_.end() || current->second.value("run_id", "") != run_id)
            throw std::invalid_argument("operation run was replaced before it started");
        operation_workers_[session_id] = std::thread(
            [this, session_id, operation_id, effective_instance, parameters, inputs, run_id, cancellation] {
                {
                    std::lock_guard lock(mutex_);
                    const auto found = operation_runs_.find(session_id);
                    if (found == operation_runs_.end() || found->second.value("run_id", "") != run_id) return;
                    found->second["state"] = "running";
                    found->second["updated_at_ms"] = detail::unix_time_milliseconds();
                }
                try {
                    const auto result = run_operation(session_id, operation_id, parameters,
                                                      effective_instance, inputs, cancellation);
                    const bool cache_hit = result.is_object() && result.value("cache_hit", false);
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = operation_runs_.find(session_id);
                        if (found == operation_runs_.end() || found->second.value("run_id", "") != run_id) return;
                        found->second["state"] = cancellation->load() ? "cancelled" : "completed";
                        found->second["updated_at_ms"] = detail::unix_time_milliseconds();
                        found->second["cache_hit"] = cache_hit;
                    }
                    {
                        std::lock_guard lock(mutex_);
                        operation_cancellations_.erase(session_id);
                        active_workers_.erase(session_id);
                    }
                    if (operation_event_callback_)
                        operation_event_callback_(session_id, operation_id,
                            cancellation->load() ? "operation.cancelled" : "operation.completed",
                            Json{{"run_id", run_id}, {"operation_instance", effective_instance},
                                 {"cache_hit", cache_hit}});
                } catch (const std::exception &error) {
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = operation_runs_.find(session_id);
                        if (found == operation_runs_.end() || found->second.value("run_id", "") != run_id) return;
                        found->second["state"] = cancellation->load() ? "cancelled" : "failed";
                        found->second["updated_at_ms"] = detail::unix_time_milliseconds();
                        if (!cancellation->load()) found->second["error"] = error.what();
                        operation_cancellations_.erase(session_id);
                        active_workers_.erase(session_id);
                    }
                    if (operation_event_callback_)
                        operation_event_callback_(session_id, operation_id,
                            cancellation->load() ? "operation.cancelled" : "operation.failed",
                            Json{{"run_id", run_id}, {"operation_instance", effective_instance},
                                 {"message", error.what()}});
                } catch (...) {
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = operation_runs_.find(session_id);
                        if (found == operation_runs_.end() || found->second.value("run_id", "") != run_id) return;
                        found->second["state"] = "failed";
                        found->second["updated_at_ms"] = detail::unix_time_milliseconds();
                        found->second["error"] = "operation failed with an unknown error";
                        operation_cancellations_.erase(session_id);
                        active_workers_.erase(session_id);
                    }
                    if (operation_event_callback_)
                        operation_event_callback_(session_id, operation_id, "operation.failed",
                            Json{{"run_id", run_id}, {"operation_instance", effective_instance},
                                 {"message", "operation failed with an unknown error"}});
                }
            });
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        const auto current = operation_runs_.find(session_id);
        if (current != operation_runs_.end() && current->second.value("run_id", "") == run_id) {
            current->second["state"] = "failed";
            current->second["error"] = error.what();
            current->second["updated_at_ms"] = detail::unix_time_milliseconds();
        }
        operation_cancellations_.erase(session_id);
        throw;
    }
    return accepted;
}

Json ProjectRuntimeManager::result_inventory(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    std::unordered_map<std::string, Json> registered_results;
    for (auto metadata : iterator->second->get_result_inventory()) {
        const auto id = metadata.value("result_id", std::string{});
        if (id.empty()) continue;
        const auto revision = metadata.find("workflow_revision");
        if (revision != metadata.end() && revision->is_string()) {
            try { metadata["workflow_revision"] = std::stoi(revision->get<std::string>()); }
            catch (...) { metadata["workflow_revision"] = 0; }
        }
        registered_results[id] = std::move(metadata);
    }
    const ResultStore store(iterator->second->get_database_path());
    Json results = Json::array();
    for (const auto &manifest : store.published_results()) {
        const auto result_id = manifest.value("result_id", std::string{});
        const auto registration = registered_results.find(result_id);
        // The manifest owns payload availability; DuckDB owns project-local
        // registration and workflow provenance. Expose only committed results
        // that have both records, and never inspect a historical DuckDB table.
        if (result_id.empty() || registration == registered_results.end() ||
            registration->second.value("status", std::string{}) != "published")
            continue;
        auto result = manifest;
        for (const auto &[name, value] : registration->second.items()) {
            // File-backed JSON/table payloads live in the result store. A NULL
            // metadata payload must not replace the manifest's relative file
            // locator before the renderer resolves the JSON file or table ID.
            if (name == "payload" && value.is_null()) continue;
            result[name] = value;
        }
        result["result_id"] = result_id;
        result["status"] = "published";
        result["representation"] = manifest.value("kind", std::string{"binary"});
        if (result.value("representation", std::string{}) == "table" &&
            result.contains("schema") && result.at("schema").is_array())
            result["columns"] = result["schema"];
        if (result.value("representation", std::string{}) == "table")
            result["physical_table"] = "result:" + result_id;
        if (manifest.contains("contract_id")) result["contract_id"] = manifest.at("contract_id");
        // JSON results are immutable files in the result store.  The frontend
        // needs the bounded structured payload for JSON viewers; a payload_ref
        // alone is useful to workers but cannot render a canvas result.
        if (result.value("representation", std::string{}) == "json") {
            try {
                result["payload"] = store.read_json_payload(result.value("result_id", std::string{}));
            } catch (const std::exception &error) {
                result["status"] = "stale";
                result["availability_error"] = error.what();
            }
        }
        results.push_back(std::move(result));
    }
    return results;
}

Json ProjectRuntimeManager::current_result_inventory(const std::string &session_id) const {
    const auto results = result_inventory(session_id);
    Json current = Json::object();
    for (const auto &artifact : results) {
        if (artifact.value("status", "") != "published") continue;
        const auto key = artifact.value("producer_instance", "") + "\n" + artifact.value("contract_id", "");
        const auto existing = current.find(key);
        const auto newer = [&](const Json &candidate, const Json &previous) {
            const auto candidate_revision = candidate.value("workflow_revision", 0);
            const auto previous_revision = previous.value("workflow_revision", 0);
            return candidate_revision > previous_revision ||
                   (candidate_revision == previous_revision &&
                    std::tie(candidate["created_at"], candidate["result_id"]) >
                        std::tie(previous["created_at"], previous["result_id"]));
        };
        if (existing == current.end() || newer(artifact, *existing)) current[key] = artifact;
    }
    Json result = Json::array();
    for (const auto &[key, artifact] : current.items()) result.push_back(artifact);
    return result;
}

Json ProjectRuntimeManager::result_query(const std::string &session_id, const Json &request) const {
    std::filesystem::path database_path;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        database_path = iterator->second->get_database_path();
    }
    const auto result_id = request.value("result_id", std::string{});
    if (result_id.empty()) throw std::invalid_argument("result_id is required");
    const ResultStore store(database_path);
    try {
        const auto descriptor = store.describe_result(result_id);
        if (descriptor.value("kind", std::string{}) != "table")
            throw std::invalid_argument("result is not a table");
        if (descriptor.value("kind", std::string{}) == "table" &&
            descriptor.value("format", std::string{}) == "arrow-ipc-file") {
            const auto mode = request.value("mode", std::string{"page"});
            if (mode != "page" && mode != "sample" && mode != "detail" && mode != "eic" && mode != "visualization")
                throw std::invalid_argument("result table query mode must be page, sample, detail, eic, or visualization");
            const auto offset = request.value("offset", std::uintmax_t{0});
            const auto limit = std::clamp(request.value("limit", std::uintmax_t{1000}),
                                          std::uintmax_t{1}, std::uintmax_t{250000});
            const auto format = descriptor.value("format", std::string{});
            Json columns = descriptor.value("schema", Json::array());
            std::vector<std::string> projected_columns;
            if (!request.contains("columns") && columns.is_array()) {
                for (const auto &column : columns)
                    if (column.is_object() && column.value("name", Json{}).is_string())
                        projected_columns.push_back(column.at("name").get<std::string>());
            }
            if (request.contains("columns")) {
                if (!request.at("columns").is_array())
                    throw std::invalid_argument("result query columns must be an array");
                columns = Json::array();
                for (const auto &column : request.at("columns")) {
                    if (!column.is_string() || column.get<std::string>().empty())
                        throw std::invalid_argument("result query columns must contain non-empty names");
                    const auto name = column.get<std::string>();
                    const auto schema_column = std::find_if(descriptor.at("schema").begin(), descriptor.at("schema").end(),
                        [&](const Json &candidate) { return candidate.value("name", std::string{}) == name; });
                    if (schema_column == descriptor.at("schema").end())
                        throw std::invalid_argument("result query column not found: " + name);
                    if (std::find(projected_columns.begin(), projected_columns.end(), name) != projected_columns.end())
                        continue;
                    projected_columns.push_back(name);
                    columns.push_back(*schema_column);
                }
                if (projected_columns.empty())
                    throw std::invalid_argument("result query columns must not be empty");
            }
            for (const auto &filter : request.value("filters", Json::array())) {
                if (!filter.is_object() || !filter.value("column", Json{}).is_string()) continue;
                const auto name = filter.at("column").get<std::string>();
                const auto schema = descriptor.value("schema", Json::array());
                const auto schema_column = std::find_if(schema.begin(), schema.end(),
                    [&](const Json &candidate) { return candidate.value("name", std::string{}) == name; });
                if (schema_column == schema.end())
                    throw std::invalid_argument("result query filter column not found: " + name);
                if (std::find(projected_columns.begin(), projected_columns.end(), name) == projected_columns.end())
                    projected_columns.push_back(name);
            }
            std::vector<std::string> navigation_columns;
            const auto navigation_metadata = descriptor.value("navigation_columns", Json::array());
            if (navigation_metadata.is_array()) {
                for (const auto &column : navigation_metadata)
                    if (column.is_string()) navigation_columns.push_back(column.get<std::string>());
            }
            const bool build_navigation_index = (mode == "sample" || mode == "detail") && !navigation_columns.empty();
            if (build_navigation_index) {
                for (const auto &name : navigation_columns)
                    if (std::find(projected_columns.begin(), projected_columns.end(), name) == projected_columns.end())
                        projected_columns.push_back(name);
            }
            const auto read_batch = [&](std::uintmax_t batch_offset, std::uintmax_t batch_limit) {
                auto batch = store.read_arrow_ipc_batch(result_id, batch_offset, batch_limit, projected_columns);
                // Arrow payloads contain only operation columns.  Give every
                // immutable row the same stable offset key used by the query
                // and trace APIs without storing a redundant key column.
                if (format == "arrow-ipc-file") {
                    auto &batch_rows = batch["rows"];
                    if (batch_rows.is_array()) {
                        for (std::size_t index = 0; index < batch_rows.size(); ++index)
                            if (batch_rows.at(index).is_object() && !batch_rows.at(index).contains("row_key"))
                                batch_rows[index]["row_key"] = std::to_string(batch_offset + index);
                    }
                }
                return batch;
            };
            Json rows = Json::array();
            const auto matches = [&](const Json &row) {
                if (!row.is_object()) return false;
                if (request.contains("row_keys")) {
                    const auto &requested_keys = request.at("row_keys");
                    if (!requested_keys.is_array() || requested_keys.empty()) return false;
                    const auto row_key = row.value("row_key", std::string{});
                    bool requested = false;
                    for (const auto &key : requested_keys)
                        if (key.is_string() && key.get<std::string>() == row_key) { requested = true; break; }
                    if (!requested) return false;
                }
                const auto search = request.value("search", std::string{});
                if (!search.empty()) {
                    bool found = false;
                    for (const auto &[name, value] : row.items())
                        if (value.dump().find(search) != std::string::npos) { found = true; break; }
                    if (!found) return false;
                }
                const auto filters = request.value("filters", Json::array());
                for (const auto &filter : filters) {
                    if (!filter.is_object()) continue;
                    const auto column = filter.value("column", std::string{});
                    const auto iterator = row.find(column);
                    const auto value = iterator == row.end() ? Json(nullptr) : *iterator;
                    const auto operation = filter.value("operator", std::string{"eq"});
                    if (operation == "is_null" && !value.is_null()) return false;
                    if (operation == "is_not_null" && value.is_null()) return false;
                    if (operation == "is_null" || operation == "is_not_null") continue;
                    const auto scalar_text = [](const Json &item) { return item.is_string() ? item.get<std::string>() : item.dump(); };
                    const auto left = scalar_text(value);
                    if (operation == "in") {
                        bool included = false;
                        for (const auto &candidate : filter.value("values", Json::array()))
                            if (left == scalar_text(candidate)) { included = true; break; }
                        if (!included) return false;
                        continue;
                    }
                    const auto right = filter.contains("value") ? scalar_text(filter.at("value")) : std::string{};
                    if (operation == "eq" && left != right) return false;
                    if (operation == "neq" && left == right) return false;
                    if (operation == "between") {
                        const auto minimum = scalar_text(filter.value("min", Json{}));
                        const auto maximum = scalar_text(filter.value("max", Json{}));
                        try {
                            const auto numeric = std::stod(left);
                            if (numeric < std::stod(minimum) || numeric > std::stod(maximum)) return false;
                        } catch (...) { if (left < minimum || left > maximum) return false; }
                    }
                    if (operation == "gt" || operation == "gte" || operation == "lt" || operation == "lte") {
                        try {
                            const auto lhs = std::stod(left), rhs = std::stod(right);
                            if ((operation == "gt" && !(lhs > rhs)) || (operation == "gte" && !(lhs >= rhs)) ||
                                (operation == "lt" && !(lhs < rhs)) || (operation == "lte" && !(lhs <= rhs))) return false;
                        } catch (...) { return false; }
                    }
                }
                return true;
            };
            // An unfiltered table page is a row-range read, not a filtered
            // query. Use the immutable manifest row count and let the Arrow
            // reader seek directly to the requested record batches instead
            // of scanning the whole result merely to calculate total_rows.
            // This is the common path for the table renderer and keeps its
            // latency proportional to the requested page.
            const auto filters = request.value("filters", Json::array());
            const bool has_filters = filters.is_array() && !filters.empty();
            if (mode == "page" && !has_filters && !request.contains("search") &&
                !request.contains("row_keys")) {
                const auto row_count = descriptor.value("row_count", std::uintmax_t{0});
                Json page_rows = Json::array();
                std::uintmax_t page_offset = offset;
                while (page_rows.size() < limit && page_offset < row_count) {
                    const auto remaining = limit - page_rows.size();
                    const auto batch = read_batch(page_offset, std::min<std::uintmax_t>(remaining, 65536));
                    const auto batch_rows = batch.value("rows", Json::array());
                    if (!batch_rows.is_array() || batch_rows.empty()) break;
                    for (const auto &row : batch_rows) page_rows.push_back(row);
                    page_offset += batch_rows.size();
                }
                return Json{{"result_id", result_id}, {"columns", columns}, {"rows", page_rows},
                            {"offset", offset}, {"limit", limit}, {"total_rows", row_count},
                            {"returned_rows", page_rows.size()}, {"mode", mode},
                            {"has_more", offset + page_rows.size() < row_count}};
            }
            const auto project_result_key = session_id + "\n" + result_id;
            std::shared_ptr<const NavigationIndex> cached_navigation;
            {
                std::lock_guard cache_lock(result_cache_mutex_);
                const auto cached = result_navigation_index_cache_.find(project_result_key);
                if (cached != result_navigation_index_cache_.end()) cached_navigation = cached->second;
            }
            if (!cached_navigation && !navigation_columns.empty()) {
                const auto persisted = store.read_navigation_index(result_id);
                if (persisted.is_object()) {
                    auto restored = std::make_shared<NavigationIndex>();
                    for (const auto &[column, values] : persisted.items()) {
                        if (!values.is_object()) continue;
                        for (const auto &[value, offsets] : values.items()) {
                            if (!offsets.is_array()) continue;
                            auto &destination = (*restored)[column][value];
                            for (const auto &offset : offsets)
                                if (offset.is_number_unsigned() || offset.is_number_integer())
                                    destination.push_back(offset.get<std::uintmax_t>());
                        }
                    }
                    if (!restored->empty()) {
                        cached_navigation = restored;
                        std::lock_guard cache_lock(result_cache_mutex_);
                        if (result_navigation_index_cache_.size() >= 4)
                            result_navigation_index_cache_.erase(result_navigation_index_cache_.begin());
                        result_navigation_index_cache_[project_result_key] = std::move(restored);
                    }
                }
            }
            if (!request.contains("search") && !request.contains("row_keys") &&
                cached_navigation && !cached_navigation->empty()) {
                std::vector<std::unordered_set<std::uintmax_t>> candidate_sets;
                bool indexable = true;
                for (const auto &filter : request.value("filters", Json::array())) {
                    if (!filter.is_object()) continue;
                    const auto column = filter.value("column", std::string{});
                    const auto operation = filter.value("operator", std::string{"eq"});
                    if (std::find(navigation_columns.begin(), navigation_columns.end(), column) == navigation_columns.end() ||
                        (operation != "eq" && operation != "in")) {
                        indexable = false;
                        break;
                    }
                    std::unordered_set<std::uintmax_t> candidates;
                    const auto add_value = [&](const Json &value) {
                        const auto column_index = cached_navigation->find(column);
                        if (column_index == cached_navigation->end()) return;
                        const auto values = column_index->second.find(value.dump());
                        if (values == column_index->second.end()) return;
                        candidates.insert(values->second.begin(), values->second.end());
                    };
                    if (operation == "eq") add_value(filter.value("value", Json{}));
                    else for (const auto &value : filter.value("values", Json::array())) add_value(value);
                    candidate_sets.push_back(std::move(candidates));
                }
                if (indexable && !candidate_sets.empty()) {
                    std::unordered_set<std::uintmax_t> candidates = std::move(candidate_sets.front());
                    for (std::size_t index = 1; index < candidate_sets.size(); ++index)
                        for (auto iterator = candidates.begin(); iterator != candidates.end();)
                            iterator = candidate_sets[index].contains(*iterator) ? std::next(iterator) : candidates.erase(iterator);
                    std::vector<std::uintmax_t> ordered(candidates.begin(), candidates.end());
                    std::sort(ordered.begin(), ordered.end());
                    Json indexed_rows = Json::array();
                    std::unordered_set<std::uintmax_t> pages;
                    const auto page_size = std::uintmax_t{4096};
                    for (std::size_t index = static_cast<std::size_t>(offset);
                         index < ordered.size() && indexed_rows.size() < limit; ++index)
                        pages.insert((ordered[index] / page_size) * page_size);
                    std::unordered_map<std::uintmax_t, Json> loaded_rows;
                    for (const auto page : pages) {
                        const auto batch = read_batch(page, page_size);
                        const auto source_rows = batch.value("rows", Json::array());
                        if (!source_rows.is_array()) continue;
                        for (std::size_t index = 0; index < source_rows.size(); ++index)
                            loaded_rows.emplace(page + index, source_rows.at(index));
                    }
                    for (std::size_t index = static_cast<std::size_t>(offset);
                         index < ordered.size() && indexed_rows.size() < limit; ++index) {
                        const auto row = loaded_rows.find(ordered[index]);
                        if (row != loaded_rows.end() && matches(row->second)) indexed_rows.push_back(row->second);
                    }
                    if (columns.is_array() && !columns.empty()) {
                        for (auto &row : indexed_rows) {
                            Json projected = Json::object();
                            for (const auto &column : columns) {
                                const auto name = column.value("name", std::string{});
                                if (!name.empty() && row.contains(name)) projected[name] = row.at(name);
                            }
                            row = std::move(projected);
                        }
                    }
                    return Json{{"result_id", result_id}, {"columns", columns}, {"rows", indexed_rows},
                                {"offset", offset}, {"limit", limit}, {"total_rows", ordered.size()},
                                {"returned_rows", indexed_rows.size()}, {"mode", mode},
                                {"has_more", offset + indexed_rows.size() < ordered.size()}};
                }
            }
            // Filter scans should not decode every requested output column.
            // This matters for mass-spectrometry tables where trace columns
            // are large Arrow lists but filters usually target a few scalars.
            // Scan only predicate columns, retain row offsets for the requested
            // page, then fetch/project the matching output rows by Arrow batch.
            if (has_filters && !request.contains("search") && !request.contains("row_keys")) {
                std::vector<std::string> filter_columns;
                for (const auto &filter : filters) {
                    if (!filter.is_object() || !filter.value("column", Json{}).is_string()) continue;
                    const auto column = filter.at("column").get<std::string>();
                    if (std::find(filter_columns.begin(), filter_columns.end(), column) == filter_columns.end())
                        filter_columns.push_back(column);
                }
                std::uintmax_t matched_rows = 0;
                std::vector<std::uintmax_t> selected_offsets;
                selected_offsets.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(limit, 65536)));
                Json query_stats = Json::object();
                const auto native_scan = store.scan_arrow_scalar_filters(result_id, filters, offset, limit);
                if (native_scan.is_object()) {
                    matched_rows = native_scan.value("total_rows", std::uintmax_t{0});
                    query_stats = {{"arrow_batches_scanned", native_scan.value("batches_scanned", std::uintmax_t{0})},
                                   {"arrow_batches_skipped", native_scan.value("batches_skipped", std::uintmax_t{0})}};
                    for (const auto &row_offset : native_scan.value("offsets", Json::array()))
                        if (row_offset.is_number_unsigned() || row_offset.is_number_integer())
                            selected_offsets.push_back(row_offset.get<std::uintmax_t>());
                } else {
                    std::uintmax_t scan_offset = 0;
                    constexpr std::uintmax_t scan_batch_size = 65536;
                    for (;;) {
                        const auto batch = store.read_arrow_ipc_batch(result_id, scan_offset, scan_batch_size, filter_columns);
                        const auto batch_rows = batch.value("rows", Json::array());
                        if (!batch_rows.is_array() || batch_rows.empty()) break;
                        for (std::size_t index = 0; index < batch_rows.size(); ++index) {
                            auto row = batch_rows.at(index);
                            if (row.is_object()) row["row_key"] = std::to_string(scan_offset + index);
                            if (!matches(row)) continue;
                            if (matched_rows >= offset && selected_offsets.size() < limit)
                                selected_offsets.push_back(scan_offset + index);
                            ++matched_rows;
                        }
                        scan_offset += batch_rows.size();
                        if (!batch.value("has_more", false)) break;
                    }
                }

                const auto page_size = std::uintmax_t{4096};
                std::unordered_set<std::uintmax_t> pages;
                for (const auto source_offset : selected_offsets)
                    pages.insert((source_offset / page_size) * page_size);
                std::unordered_map<std::uintmax_t, Json> loaded_rows;
                for (const auto page : pages) {
                    const auto batch = read_batch(page, page_size);
                    const auto source_rows = batch.value("rows", Json::array());
                    if (!source_rows.is_array()) continue;
                    for (std::size_t index = 0; index < source_rows.size(); ++index)
                        loaded_rows.emplace(page + index, source_rows.at(index));
                }
                Json filtered_rows = Json::array();
                for (const auto source_offset : selected_offsets) {
                    const auto row = loaded_rows.find(source_offset);
                    if (row == loaded_rows.end()) continue;
                    auto projected = row->second;
                    if (columns.is_array() && !columns.empty()) {
                        projected = Json::object();
                        for (const auto &column : columns) {
                            const auto name = column.value("name", std::string{});
                            if (!name.empty() && row->second.contains(name)) projected[name] = row->second.at(name);
                        }
                    }
                    filtered_rows.push_back(std::move(projected));
                }
                return Json{{"result_id", result_id}, {"columns", columns}, {"rows", filtered_rows},
                            {"offset", offset}, {"limit", limit}, {"total_rows", matched_rows},
                            {"returned_rows", filtered_rows.size()}, {"mode", mode},
                            {"has_more", offset + filtered_rows.size() < matched_rows},
                            {"query_stats", query_stats}};
            }
            const auto requested_row_keys = request.value("row_keys", Json::array());
            std::shared_ptr<const std::unordered_map<std::string, std::uintmax_t>> cached_row_keys;
            {
                std::lock_guard cache_lock(result_cache_mutex_);
                const auto cached = result_row_key_cache_.find(project_result_key);
                if (cached != result_row_key_cache_.end()) cached_row_keys = cached->second;
            }
            if (mode == "detail" && requested_row_keys.is_array() && !requested_row_keys.empty() &&
                cached_row_keys) {
                std::vector<std::uintmax_t> requested_offsets;
                requested_offsets.reserve(requested_row_keys.size());
                for (const auto &requested : requested_row_keys) {
                    if (!requested.is_string()) continue;
                    const auto offset = cached_row_keys->find(requested.get<std::string>());
                    if (offset != cached_row_keys->end()) requested_offsets.push_back(offset->second);
                }
                Json cached_rows = Json::array();
                const auto page_size = std::uintmax_t{4096};
                std::unordered_set<std::uintmax_t> pages;
                for (const auto source_offset : requested_offsets)
                    pages.insert((source_offset / page_size) * page_size);
                std::unordered_map<std::uintmax_t, Json> loaded_rows;
                for (const auto page : pages) {
                    const auto batch = read_batch(page, page_size);
                    const auto source_rows = batch.value("rows", Json::array());
                    if (!source_rows.is_array()) continue;
                    for (std::size_t index = 0; index < source_rows.size(); ++index)
                        loaded_rows.emplace(page + index, source_rows.at(index));
                }
                for (const auto source_offset : requested_offsets) {
                    const auto row = loaded_rows.find(source_offset);
                    if (row != loaded_rows.end()) cached_rows.push_back(row->second);
                }
                if (columns.is_array() && !columns.empty()) {
                    for (auto &row : cached_rows) {
                        if (!row.is_object()) continue;
                        Json projected = Json::object();
                        for (const auto &column : columns) {
                            const auto name = column.value("name", std::string{});
                            if (!name.empty() && row.contains(name)) projected[name] = row.at(name);
                        }
                        row = std::move(projected);
                    }
                }
                return Json{{"result_id", result_id}, {"columns", columns}, {"rows", cached_rows},
                            {"offset", 0}, {"limit", limit}, {"total_rows", cached_rows.size()},
                            {"returned_rows", cached_rows.size()}, {"mode", mode}, {"has_more", false}};
            }
            std::uintmax_t total_rows = 0;
            std::uintmax_t source_offset = 0;
            constexpr std::uintmax_t source_page_size = 65536;
            const bool build_row_key_index = mode == "sample" || mode == "detail";
            bool collect_row_key_index = build_row_key_index;
            std::size_t row_key_index_estimated_bytes = 0;
            std::unordered_map<std::string, std::uintmax_t> row_key_index;
            NavigationIndex navigation_index;
            bool collect_navigation_index = build_navigation_index;
            std::size_t navigation_index_estimated_bytes = 0;
            std::uintmax_t row_offset = source_offset;
            for (;;) {
                const auto batch = read_batch(source_offset, source_page_size);
                const auto batch_rows = batch.value("rows", Json::array());
                if (!batch_rows.is_array() || batch_rows.empty()) break;
                for (const auto &row : batch_rows) {
                    const auto current_row_offset = row_offset++;
                    if (collect_row_key_index && row.is_object()) {
                        const auto row_key = row.value("row_key", std::string{});
                        if (!row_key.empty()) {
                            row_key_index_estimated_bytes += row_key.size() + sizeof(std::uintmax_t) + 32;
                            if (row_key_index_estimated_bytes > 16 * 1024 * 1024) {
                                row_key_index.clear();
                                collect_row_key_index = false;
                            } else {
                                row_key_index.emplace(row_key, current_row_offset);
                            }
                        }
                    }
                    if (collect_navigation_index && row.is_object()) {
                        for (const auto &name : navigation_columns) {
                            if (!row.contains(name)) continue;
                            const auto value = row.at(name).dump();
                            navigation_index_estimated_bytes += name.size() + value.size() + 64;
                            if (navigation_index_estimated_bytes > 32 * 1024 * 1024) {
                                navigation_index.clear();
                                collect_navigation_index = false;
                                break;
                            }
                            navigation_index[name][value].push_back(current_row_offset);
                        }
                    }
                    if (!matches(row)) continue;
                    ++total_rows;
                    if (mode == "sample" && rows.size() >= limit) continue;
                    if (mode != "sample" && total_rows <= offset) continue;
                    if (rows.size() < limit) rows.push_back(row);
                }
                source_offset += batch_rows.size();
                row_offset = source_offset;
                if (!batch.value("has_more", false)) break;
            }
            if (collect_navigation_index && !navigation_index.empty()) {
                Json persisted_index = Json::object();
                for (const auto &[column, values] : navigation_index) {
                    auto &column_index = persisted_index[column];
                    for (const auto &[value, offsets] : values)
                        column_index[value] = offsets;
                }
                store.write_navigation_index(result_id, persisted_index);
                if (persisted_index.dump().size() <= 32 * 1024 * 1024) {
                    std::lock_guard cache_lock(result_cache_mutex_);
                    if (result_navigation_index_cache_.size() >= 4)
                        result_navigation_index_cache_.erase(result_navigation_index_cache_.begin());
                    result_navigation_index_cache_[project_result_key] =
                        std::make_shared<const NavigationIndex>(navigation_index);
                }
            }
            if (collect_row_key_index && !row_key_index.empty()) {
                std::lock_guard cache_lock(result_cache_mutex_);
                if (result_row_key_cache_.size() >= 4) result_row_key_cache_.erase(result_row_key_cache_.begin());
                result_row_key_cache_[project_result_key] =
                    std::make_shared<const std::unordered_map<std::string, std::uintmax_t>>(std::move(row_key_index));
            }
            if ((!columns.is_array() || columns.empty()) && !rows.empty() && rows.front().is_object()) {
                columns = Json::array();
                for (const auto &[name, value] : rows.front().items()) {
                    std::string type = "VARCHAR";
                    if (value.is_boolean()) type = "BOOLEAN";
                    else if (value.is_number_integer()) type = "BIGINT";
                    else if (value.is_number_float()) type = "DOUBLE";
                    else if (value.is_array() || value.is_object()) type = "JSON";
                    columns.push_back({{"name", name}, {"type", type}});
                }
            }
            if (columns.is_array() && !columns.empty() && !rows.empty()) {
                for (auto &row : rows) {
                    if (!row.is_object()) continue;
                    Json projected = Json::object();
                    for (const auto &column : columns) {
                        const auto name = column.value("name", std::string{});
                        if (!name.empty() && row.contains(name)) projected[name] = row.at(name);
                    }
                    row = std::move(projected);
                }
            }
            return Json{{"result_id", result_id},
                        {"columns", columns}, {"rows", rows}, {"offset", offset},
                        {"limit", limit}, {"total_rows", total_rows}, {"returned_rows", rows.size()},
                        {"mode", mode}, {"has_more", offset + rows.size() < total_rows}};
        }
    } catch (const std::exception &error) {
        // A published result that fails to decode must report the reader
        // error instead of being misreported as a missing result.
        const auto published = store.published_results();
        const auto found = std::any_of(published.begin(), published.end(), [&](const Json &entry) {
            return entry.value("result_id", std::string{}) == result_id;
        });
        if (found) throw std::runtime_error(std::string("result store query failed: ") + error.what());
    }
    throw std::invalid_argument("result not found in result store");
}

Json ProjectRuntimeManager::result_data(const std::string &session_id, const Json &request) const {
    return result_query(session_id, request);
}

Json ProjectRuntimeManager::result_trace(const std::string &session_id, const Json &request) const {
    const auto trace_type = request.value("trace_type", std::string{"eic"});
    const std::unordered_map<std::string, std::pair<std::string, std::string>> trace_columns{
        {"eic", {"eic_rt", "eic_intensity"}},
        {"ms1", {"ms1_mz", "ms1_intensity"}},
        {"ms2", {"ms2_mz", "ms2_intensity"}}};
    const auto trace = trace_columns.find(trace_type);
    if (trace == trace_columns.end()) throw std::invalid_argument("unsupported trace_type");
    const auto result_id = request.value("result_id", std::string{});
    if (result_id.empty()) throw std::invalid_argument("result_id is required");
    const auto cache_key = session_id + "\n" + request.dump();
    {
        std::lock_guard lock(mutex_);
        const auto cached = decoded_trace_cache_.find(cache_key);
        if (cached != decoded_trace_cache_.end()) {
            const auto position = std::find(decoded_trace_cache_order_.begin(),
                                            decoded_trace_cache_order_.end(), cache_key);
            if (position != decoded_trace_cache_order_.end()) decoded_trace_cache_order_.erase(position);
            decoded_trace_cache_order_.push_back(cache_key);
            return cached->second;
        }
    }
    Json query = request;
    query["mode"] = "detail";
    query["columns"] = Json::array({"analysis", "name", "feature", "feature_component",
                                     trace->second.first, trace->second.second});
    const auto selected = result_query(session_id, query);
    Json traces = Json::array();
    for (const auto &row : selected.value("rows", Json::array())) {
        if (!row.is_object()) continue;
        Json rendered = Json::object();
        for (const auto &[name, value] : row.items()) {
            if (name == trace->second.first || name == trace->second.second)
                rendered[name] = detail::require_typed_trace_array(value);
            else rendered[name] = value;
        }
        traces.push_back(std::move(rendered));
    }
    Json response{{"result_id", result_id}, {"trace_type", trace_type},
                  {"traces", std::move(traces)}, {"returned_rows", selected.value("returned_rows", 0)}};
    const auto serialized_response_bytes = response.dump().size();
    if (serialized_response_bytes <= kDecodedTraceCacheBytes &&
        cache_key.size() <= kDecodedTraceCacheBytes - serialized_response_bytes) {
        const auto response_bytes = serialized_response_bytes + cache_key.size();
        std::lock_guard lock(mutex_);
        const auto existing = decoded_trace_cache_.find(cache_key);
        if (existing != decoded_trace_cache_.end()) {
            decoded_trace_cache_bytes_ -= std::min(decoded_trace_cache_bytes_,
                existing->second.dump().size() + cache_key.size());
            decoded_trace_cache_.erase(existing);
            const auto position = std::find(decoded_trace_cache_order_.begin(),
                                            decoded_trace_cache_order_.end(), cache_key);
            if (position != decoded_trace_cache_order_.end()) decoded_trace_cache_order_.erase(position);
        }
        while (!decoded_trace_cache_order_.empty() &&
               (decoded_trace_cache_.size() >= kDecodedTraceCacheEntries ||
                response_bytes > kDecodedTraceCacheBytes - decoded_trace_cache_bytes_)) {
            const auto oldest = std::move(decoded_trace_cache_order_.front());
            decoded_trace_cache_order_.pop_front();
            const auto entry = decoded_trace_cache_.find(oldest);
            if (entry == decoded_trace_cache_.end()) continue;
            decoded_trace_cache_bytes_ -= std::min(decoded_trace_cache_bytes_,
                entry->second.dump().size() + oldest.size());
            decoded_trace_cache_.erase(entry);
        }
        decoded_trace_cache_[cache_key] = response;
        decoded_trace_cache_order_.push_back(cache_key);
        decoded_trace_cache_bytes_ += response_bytes;
    }
    return response;
}

Json ProjectRuntimeManager::resolve_workflow_inputs(const std::string &session_id,
                                                    const std::string &operation_id,
                                                    const Json &current_results) const {
    if (operation_id.empty()) throw std::invalid_argument("operation_id is required");
    const auto definition = workflow_definition(session_id);
    const auto workflow = Workflow::from_json(definition.at("workflow"));
    const auto inventory = current_result_inventory(session_id);
    const auto project_snapshot = [&]() {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        return iterator->second->get_database_path();
    }();
    const ResultStore store(project_snapshot);
    Json resolved = Json::object();
    for (const auto &connection : workflow.connections) {
        if (connection.target_operation != operation_id) continue;
        const auto source = current_results.find(connection.source_operation);
        if (source != current_results.end() && source->is_object()) {
            const auto output = source->find(connection.source_port);
            if (output != source->end()) {
                if (connection.target_port.rfind("parameter:", 0) == 0 && output->is_object() &&
                    output->contains("result_id")) {
                    const auto result_id = output->value("result_id", std::string{});
                    try {
                        const auto descriptor = store.describe_result(result_id);
                        if (descriptor.value("representation", std::string{}) == "table" ||
                            descriptor.value("kind", std::string{}) == "table")
                            resolved[connection.target_port] = Json{{"$result_ref", result_id}};
                        else if (descriptor.value("representation", std::string{}) == "json")
                            resolved[connection.target_port] = store.read_json_payload(result_id);
                        else
                            resolved[connection.target_port] = *output;
                    } catch (...) {
                        resolved[connection.target_port] = *output;
                    }
                } else {
                    resolved[connection.target_port] = *output;
                }
                continue;
            }
        }
        const Json *selected = nullptr;
        int selected_revision = -1;
        std::string selected_created_at;
        std::string selected_id;
        for (const auto &result : inventory) {
            if (result.value("producer_instance", std::string{}) != connection.source_operation ||
                result.value("status", std::string{}) != "published") continue;
            const auto contract = result.value("contract_id", std::string{});
            const bool port_matches = contract == connection.source_port ||
                contract == "anyTable" ||
                (contract.size() > connection.source_port.size() + 1 &&
                 contract.ends_with("#" + connection.source_port));
            if (!port_matches) continue;
            int revision = -1;
            try {
                const auto value = result.find("workflow_revision");
                if (value == result.end()) continue;
                revision = value->is_number_integer() ? value->get<int>() : std::stoi(value->get<std::string>());
            } catch (...) { continue; }
            if (revision > workflow.version) continue;
            const auto created_at = result.value("created_at", std::string{});
    const auto result_id = result.value("result_id", std::string{});
            if (revision < selected_revision ||
                (revision == selected_revision &&
                 (created_at < selected_created_at ||
                  (created_at == selected_created_at && result_id <= selected_id)))) continue;
            selected = &result;
            selected_revision = revision;
            selected_created_at = created_at;
            selected_id = result_id;
        }
        if (!selected)
            throw Error(ErrorCode::WorkflowValidation,
                        "No published result for connection " +
                        connection.source_operation + "." + connection.source_port);
        if (connection.target_port.rfind("parameter:", 0) == 0) {
            const auto payload = selected->find("payload");
            if (payload != selected->end()) {
                resolved[connection.target_port] = payload->is_string()
                    ? Json::parse(payload->get<std::string>()) : *payload;
            } else if (selected->value("representation", std::string{}) == "json") {
                resolved[connection.target_port] = store.read_json_payload(
                    selected->value("result_id", std::string{}));
            } else if (selected->value("kind", std::string{}) == "table") {
                resolved[connection.target_port] = Json{{"$result_ref", selected->value("result_id", std::string{})}};
            } else {
                resolved[connection.target_port] = nullptr;
            }
        } else {
            resolved[connection.target_port] = *selected;
        }
    }
    return resolved;
}

Json ProjectRuntimeManager::data_proxy(const std::string &session_id,
                                       const data_proxy::Envelope &request) const {
    request.validate();
    if (request.session_id != session_id)
        throw std::invalid_argument("data-proxy session does not match the request path");
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto method = request.payload.value("method", std::string{});
    ResultStore store(iterator->second->get_database_path());
    if (method == "describe_result") {
        const auto result_id = request.payload.value("result_id", std::string{});
        if (result_id.empty()) throw std::invalid_argument("describe_result requires result_id");
        return data_proxy::response(request, store.describe_result(result_id)).to_json();
    }
    if (method == "read_result_range") {
        const auto result_id = request.payload.value("result_id", std::string{});
        if (result_id.empty()) throw std::invalid_argument("read_result_range requires result_id");
        const auto offset = request.payload.value("offset", std::uintmax_t{0});
        const auto length = request.payload.value("length", std::uintmax_t{0});
        return data_proxy::response(request, store.read_result_range(result_id, offset, length)).to_json();
    }
    const auto lease_key = session_id + ":" + request.lease_id;
    if (method == "allocate_result") {
        const auto result_id = request.payload.value("result_id", std::string{});
        const auto run_id = request.payload.value("run_id", request.run_id);
        const auto format = request.payload.value("format", std::string{});
        const auto extension = request.payload.value("extension", format);
        if (result_id.empty() || run_id.empty() || format.empty())
            throw std::invalid_argument("allocate_result requires result_id, run_id, and format");
        if (run_id != request.run_id)
            throw std::invalid_argument("result lease run_id does not match the active worker");
        const auto worker_iterator = active_workers_.find(session_id);
        if (worker_iterator == active_workers_.end() || !worker_iterator->second)
            throw std::runtime_error("no active worker owns this result lease");
        const auto worker = worker_iterator->second->metadata();
        if (worker.state != "running" || worker.operation_id != request.operation_id)
            throw std::invalid_argument("result lease request does not match the active worker");
        const auto lease_id = detail::create_data_proxy_lease_id();
        auto lease = store.allocate(run_id, result_id, format, extension);
        lease.owner_session_id = session_id;
        lease.owner_operation_id = request.operation_id;
        lease.owner_worker_id = worker.id;
        data_proxy_leases_[session_id + ":" + lease_id] = lease;
        WorkerHandleGrant grant;
        grant.session_id = session_id;
        grant.run_id = run_id;
        grant.operation_id = request.operation_id;
        grant.result_id = result_id;
        grant.lease_id = lease_id;
        grant.access = WorkerHandleAccess::WriteOnly;
        grant.expires_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        try {
            worker_iterator->second->transfer_handle(std::move(grant),
                static_cast<std::uintptr_t>(lease.writer->native_handle()));
        } catch (...) {
            data_proxy_leases_.erase(session_id + ":" + lease_id);
            store.abort(lease);
            throw;
        }
        return data_proxy::response(request, { {"lease_id", lease_id}, {"result_id", result_id},
                                                {"run_id", run_id}, {"format", format},
                                                {"storage", "published"},
                                                {"capabilities", {"write_handle", "publish"}} }).to_json();
    }
    if (method == "publish_result") {
        if (request.lease_id.empty()) throw std::invalid_argument("publish_result requires lease_id");
        const auto lease_iterator = data_proxy_leases_.find(lease_key);
        if (lease_iterator == data_proxy_leases_.end()) throw std::invalid_argument("result lease not found");
        const auto active_worker = active_workers_.find(session_id);
        if (lease_iterator->second.run_id != request.run_id ||
            lease_iterator->second.owner_operation_id != request.operation_id ||
            active_worker == active_workers_.end() || !active_worker->second ||
            active_worker->second->metadata().id != lease_iterator->second.owner_worker_id)
            throw std::invalid_argument("result publication request does not own this lease");
        const auto metadata = request.payload.value("metadata", Json::object());
        if (!metadata.is_object()) throw std::invalid_argument("result metadata must be an object");
        const auto manifest = store.publish(lease_iterator->second, metadata);
        data_proxy_leases_.erase(lease_iterator);
        return data_proxy::response(request, manifest).to_json();
    }
    if (method == "abort_result") {
        if (request.lease_id.empty()) throw std::invalid_argument("abort_result requires lease_id");
        const auto lease_iterator = data_proxy_leases_.find(lease_key);
        if (lease_iterator == data_proxy_leases_.end()) throw std::invalid_argument("result lease not found");
        const auto active_worker = active_workers_.find(session_id);
        if (lease_iterator->second.run_id != request.run_id ||
            lease_iterator->second.owner_operation_id != request.operation_id ||
            active_worker == active_workers_.end() || !active_worker->second ||
            active_worker->second->metadata().id != lease_iterator->second.owner_worker_id)
            throw std::invalid_argument("result abort request does not own this lease");
        store.abort(lease_iterator->second);
        data_proxy_leases_.erase(lease_iterator);
        return data_proxy::response(request, {{"lease_id", request.lease_id}, {"state", "aborted"}}).to_json();
    }
    if (method == "lease_result_input") {
        const auto result_id = request.payload.value("result_id", std::string{});
        if (result_id.empty()) throw std::invalid_argument("lease_result_input requires result_id");
        const auto worker_iterator = active_workers_.find(session_id);
        if (worker_iterator == active_workers_.end() || !worker_iterator->second)
            throw std::runtime_error("no active worker can receive the result input");
        const auto worker = worker_iterator->second->metadata();
        if (worker.state != "running" || worker.operation_id != request.operation_id ||
            request.run_id.empty())
            throw std::invalid_argument("result input request does not match the active worker");
        auto reader = store.open_published_result(result_id);
        const auto read_lease_id = detail::create_data_proxy_lease_id();
        WorkerHandleGrant grant;
        grant.session_id = session_id;
        grant.run_id = request.run_id;
        grant.operation_id = request.operation_id;
        grant.result_id = result_id;
        grant.lease_id = read_lease_id;
        grant.access = WorkerHandleAccess::ReadOnly;
        grant.expires_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        worker_iterator->second->transfer_handle(std::move(grant),
            static_cast<std::uintptr_t>(reader->native_handle()));
        return data_proxy::response(request, {{"result_id", result_id},
            {"lease_id", read_lease_id}, {"format", "arrow-ipc-file"},
            {"capabilities", {"read_handle"}}}).to_json();
    }
    const auto table = request.payload.value("table", std::string{});
    if (table.empty()) throw std::invalid_argument("data-proxy request requires table");
    if (method == "count_rows" && table.rfind("result:", 0) == 0) {
        const auto result_id = table.substr(std::string("result:").size());
        if (result_id.empty()) throw std::invalid_argument("result reference is empty");
        const auto descriptor = store.describe_result(result_id);
        return data_proxy::response(
            request, {{"row_count", descriptor.value("row_count", std::uint64_t{0})}})
            .to_json();
    }
    const auto quoted_table = detail::quoted_identifier(table);
    if (method == "has_table") {
        const auto tables = iterator->second->list_tables();
        return data_proxy::response(request, {{"exists", std::find(tables.begin(), tables.end(), table) != tables.end()}}).to_json();
    }
    if (method == "describe") {
        return data_proxy::response(request, {{"columns", iterator->second->query_json("DESCRIBE " + quoted_table)}}).to_json();
    }
    if (method == "count_rows") {
        const auto rows = iterator->second->query_json("SELECT COUNT(*) AS row_count FROM " + quoted_table);
        return data_proxy::response(request, {{"row_count", rows.empty() ? 0 : std::stoull(rows.front().value("row_count", std::string{"0"}))}}).to_json();
    }
    if (method == "append_rows") {
        const auto columns = request.payload.value("columns", Json::array());
        const auto rows = request.payload.value("rows", Json::array());
        if (!columns.is_array() || columns.empty() || !rows.is_array())
            throw std::invalid_argument("data-proxy append_rows requires columns and rows");
        std::vector<std::string> column_names;
        column_names.reserve(columns.size());
        for (const auto &column : columns) {
            if (!column.is_string() || column.get<std::string>().empty())
                throw std::invalid_argument("data-proxy append_rows has an invalid column");
            column_names.push_back(column.get<std::string>());
        }
        std::vector<std::vector<std::optional<std::string>>> values;
        values.reserve(rows.size());
        for (const auto &row : rows) {
            if (!row.is_array() || row.size() != column_names.size())
                throw std::invalid_argument("data-proxy append_rows row width does not match columns");
            std::vector<std::optional<std::string>> value;
            value.reserve(row.size());
            for (const auto &cell : row) {
                if (cell.is_null()) value.push_back(std::nullopt);
                else if (cell.is_string()) value.push_back(cell.get<std::string>());
                else value.push_back(cell.dump());
            }
            values.push_back(std::move(value));
        }
        iterator->second->append_rows(table, column_names, values);
        return data_proxy::response(request, {{"appended_rows", values.size()}}).to_json();
    }
    if (method == "query") {
        const auto sql = request.payload.value("sql", std::string{});
        if (sql.empty()) throw std::invalid_argument("data-proxy query requires sql");
        return data_proxy::response(request, {{"rows", iterator->second->query_json(sql)}}).to_json();
    }
    if (method == "execute") {
        const auto sql = request.payload.value("sql", std::string{});
        if (sql.empty()) throw std::invalid_argument("data-proxy execute requires sql");
        iterator->second->execute_sql(sql);
        return data_proxy::response(request, {{"executed", true}}).to_json();
    }
    throw std::invalid_argument("unsupported data-proxy method: " + method);
}

Json ProjectRuntimeManager::result_cache_lookup(const std::string &session_id, const Json &request) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto computation_key = request.value("computation_key", std::string{});
    if (computation_key.empty()) throw std::invalid_argument("computation_key is required");
    const ResultStore store(iterator->second->get_database_path());
    const auto inspection = store.inspect();
    const auto results = inspection.status == ResultStoreStatus::ready
                             ? store.find_cached_results(computation_key)
                             : Json::array();
    return Json{{"hit", !results.empty()},
                {"computation_key", computation_key},
                {"result_store", inspection.to_json()},
                {"results", results},
                {"result", results.empty() ? Json(nullptr) : results.front()}};
}

std::string ProjectRuntimeManager::set_workflow_state(const std::string &session_id, const std::string &state) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    static const std::set<std::string> valid_states{"idle", "validated", "queued", "running", "cancelling", "completed", "failed", "cancelled"};
    if (!valid_states.contains(state)) throw std::invalid_argument("invalid workflow state");
    workflow_states_[session_id] = state;
    return state;
}

std::string ProjectRuntimeManager::start_workflow(const std::string &session_id,
                                                  const std::string &execution_mode) {
    if (execution_mode != "detailed" && execution_mode != "pipeline")
        throw std::invalid_argument("execution_mode must be detailed or pipeline");
    Project *project = nullptr;
    std::thread completed_worker;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        if (operation_cancellations_.contains(session_id))
            throw std::invalid_argument("an operation is already running for this project");
        if (const auto operation = operation_runs_.find(session_id); operation != operation_runs_.end() &&
            (operation->second.value("state", "") == "queued" || operation->second.value("state", "") == "running" ||
             operation->second.value("state", "") == "cancelling"))
            throw std::invalid_argument("an operation is already running for this project");
        if (const auto worker = workflow_workers_.find(session_id); worker != workflow_workers_.end() && worker->second.joinable()) {
            const auto state = workflow_states_.find(session_id);
            if (state != workflow_states_.end() &&
                (state->second == "queued" || state->second == "running" || state->second == "cancelling"))
                throw std::invalid_argument("workflow is already running");
            completed_worker = std::move(worker->second);
            workflow_workers_.erase(worker);
        }
        workflow_states_[session_id] = "queued";
        {
            std::lock_guard cache_lock(result_cache_mutex_);
            result_row_key_cache_.clear();
            result_navigation_index_cache_.clear();
        }
        clear_decoded_trace_cache_locked();
        workflow_execution_modes_[session_id] = execution_mode;
        const auto workflow = workflow_definitions_.at(session_id);
        workflow_progress_[session_id] = Json{{"completed", 0}, {"total", workflow.operations.size()}, {"current_step", 0}};
        workflow_operation_steps_[session_id].clear();
        for (std::size_t index = 0; index < workflow.operations.size(); ++index)
            workflow_operation_steps_[session_id][workflow.operations[index].id] = index;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        workflow_cancellations_[session_id] = cancellation;
        project = iterator->second.get();
        workflow_workers_[session_id] = std::thread([this, session_id, project, cancellation, workflow, execution_mode] {
            std::vector<std::string> run_result_ids;
            const auto register_pipeline_results = [&]() {
                std::string errors;
                if (execution_mode != "pipeline") return errors;
                try {
                    const ResultStore result_store(project->get_database_path());
                    for (const auto &result_id : run_result_ids) {
                        try {
                            project->register_result_manifest(result_store.describe_result(result_id));
                        } catch (const std::exception &error) {
                            if (!errors.empty()) errors += "; ";
                            errors += result_id + ": " + error.what();
                        }
                    }
                } catch (const std::exception &error) {
                    errors = error.what();
                }
                return errors;
            };
            try {
                if (cancellation->load()) {
                    set_workflow_state(session_id, "cancelled");
                    std::lock_guard lock(mutex_);
                    workflow_cancellations_.erase(session_id);
                    return;
                }
                set_workflow_state(session_id, "running");
                WorkflowExecutionManager execution_manager(*project);
                execution_manager.create(Json{{"workflow_revision", workflow.version},
                                              {"progress", Json{{"completed", 0}, {"total", workflow.operations.size()}, {"current_step", 0}}}});
                OperationRunner runner = [this, session_id, project, execution_mode,
                                          &run_result_ids](const std::string &operation,
                                                                        const Json &parameters,
                                                                        const std::string &instance,
                                                                        const Json &inputs,
                                                                        const std::string &computation_key,
                                                                        bool) {
                    WorkerRequest request;
                    request.executable = configuration_path_.parent_path() /
#ifdef _WIN32
                        "streamfind-worker.exe";
#else
                        "streamfind-worker";
#endif
                    request.configuration = configuration_path_;
                    request.memory_limit_mb = detail::configured_worker_memory_limit_mb(configuration_path_);
                    request.service_port = service_port_;
                    request.session_id = session_id;
                    request.run_id = instance;
                    request.operation_id = operation;
                    request.operation_instance = instance;
                    request.computation_key = computation_key;
                    request.operation = operation;
                    // Send the canonical workflow snapshot held by the project.
                    // The project is updated by save_workflow before a run is
                    // scheduled, so this also prevents a stale manager copy
                    // from losing the required workflow schema metadata.
                    auto worker_workflow = project->get_workflow().to_json();
                    // Workflow is a v2-only contract at the worker boundary.
                    // Keep the serialized request explicit even if a project
                    // was opened from an older in-memory snapshot.
                    worker_workflow["schema_version"] = 2;
                    request.workflow = std::move(worker_workflow);
                    request.parameters = parameters;
                    request.inputs = inputs;
                    auto worker = std::make_shared<WorkerProcess>(std::move(request),
                        [this, session_id](const WorkerMetadata &metadata, std::string_view type,
                                           std::string_view message, const Json &payload) {
                            if (type == "worker.started") {
                                if (operation_event_callback_) operation_event_callback_(session_id, metadata.operation_id, type, metadata.to_json());
                            } else if (type == "operation.log") {
                                if (operation_log_callback_) operation_log_callback_(session_id, payload.value("operation_id", ""), message);
                            } else if (operation_event_callback_ && type != "operation.started" &&
                                       type != "operation.completed" && type != "operation.failed") {
                                operation_event_callback_(session_id, payload.value("operation_id", ""), type, payload);
                            }
                        });
                    {
                        std::lock_guard lock(mutex_);
                        active_workers_[session_id] = worker;
                    }
                    WorkerResult worker_result;
                    try {
                        worker_result = worker->run();
                    } catch (...) {
                        abort_worker_output_leases(session_id, instance);
                        std::lock_guard lock(mutex_);
                        active_workers_.erase(session_id);
                        throw;
                    }
                    abort_worker_output_leases(session_id, instance);
                    if (operation_event_callback_) {
                        const auto worker_event = worker_result.cancelled
                            ? "worker.cancelled"
                            : worker_result.error.empty() && worker_result.exit_code == 0
                                ? "worker.completed"
                                : "worker.failed";
                        operation_event_callback_(session_id, worker_result.worker.operation_id,
                                                  worker_event, worker_result.worker.to_json());
                    }
                    {
                        std::lock_guard lock(mutex_);
                        active_workers_.erase(session_id);
                    }
                    if (worker_result.cancelled) throw Error(ErrorCode::Cancelled, "Workflow worker cancelled");
                    if (!worker_result.error.empty()) throw Error(ErrorCode::MethodExecution, worker_result.error);
                    const auto emitted = worker_result.result.value("emitted_results", Json::object());
                    if (emitted.is_object()) {
                        const ResultStore result_store(project->get_database_path());
                        for (const auto &[output_port, reference] : emitted.items()) {
                            static_cast<void>(output_port);
                            if (!reference.is_object()) continue;
                            const auto result_id = reference.value("result_id", std::string{});
                            if (result_id.empty()) continue;
                            run_result_ids.push_back(result_id);
                            if (execution_mode == "detailed")
                                project->register_result_manifest(result_store.describe_result(result_id));
                        }
                    }
                    return worker_result.result;
                };
                const auto result = project->run_worker(session_id, *operations_, std::move(runner));
                if (execution_mode == "pipeline") {
                    const auto registration_errors = register_pipeline_results();
                    if (!registration_errors.empty())
                        throw std::runtime_error("pipeline results were retained but some could not be registered: " +
                                                 registration_errors);
                }
                set_workflow_state(session_id, result.value("status", std::string{"failed"}));
                if (operation_event_callback_) {
                    const auto status = result.value("status", std::string{"failed"});
                    const auto event_type = status == "completed" ? "workflow.completed"
                                           : status == "cancelled" ? "workflow.cancelled"
                                           : "workflow.failed";
                    operation_event_callback_(session_id, {}, event_type, Json{{"message", "Workflow execution finished."}});
                }
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
            } catch (const Error &error) {
                const auto registration_errors = register_pipeline_results();
                try { set_workflow_state(session_id, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
                if (!registration_errors.empty())
                    workflow_progress_[session_id]["result_registration_error"] = registration_errors;
            } catch (const std::exception &error) {
                const auto registration_errors = register_pipeline_results();
                try { set_workflow_state(session_id, cancellation->load() ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, cancellation->load() ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
                if (!registration_errors.empty())
                    workflow_progress_[session_id]["result_registration_error"] = registration_errors;
            } catch (...) {
                const auto registration_errors = register_pipeline_results();
                try { set_workflow_state(session_id, "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, "workflow.failed", Json{{"message", "unknown workflow execution failure"}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = "unknown workflow execution failure";
                if (!registration_errors.empty())
                    workflow_progress_[session_id]["result_registration_error"] = registration_errors;
            }
        });
    }
    if (completed_worker.joinable()) completed_worker.join();
    return "queued";
}

std::string ProjectRuntimeManager::cancel_workflow(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    const auto cancellation = workflow_cancellations_.find(session_id);
    if (cancellation == workflow_cancellations_.end()) throw std::invalid_argument("workflow is not running");
    cancellation->second->store(true);
    if (const auto worker = active_workers_.find(session_id); worker != active_workers_.end())
        worker->second->cancel();
    workflow_states_[session_id] = "cancelling";
    return "cancelling";
}

std::string ProjectRuntimeManager::cancel_operation(const std::string &session_id,
                                                     const std::string &operation_instance) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end())
        throw std::invalid_argument("project session not found");
    const auto run = operation_runs_.find(session_id);
    if (run == operation_runs_.end() ||
        run->second.value("operation_instance", std::string{}) != operation_instance)
        throw std::invalid_argument("operation instance is not the active service run");
    const auto cancellation = operation_cancellations_.find(session_id);
    if (cancellation == operation_cancellations_.end())
        throw std::invalid_argument("operation is not running");
    cancellation->second->store(true);
    run->second["state"] = "cancelling";
    run->second["updated_at_ms"] = detail::unix_time_milliseconds();
    if (const auto worker = active_workers_.find(session_id); worker != active_workers_.end())
        worker->second->cancel();
    return "cancelling";
}

Json ProjectRuntimeManager::run_operation(const std::string &session_id,
                                          const std::string &operation_id,
                                          const Json &parameters,
                                          const std::string &operation_instance,
                                          const Json &inputs,
                                          std::shared_ptr<std::atomic_bool> cancellation) {
    const auto effective_instance = operation_instance.empty() ? operation_id : operation_instance;
    Project *project = nullptr;
    const Operation *operation = nullptr;
    Json effective_inputs = inputs;
    if (!cancellation) cancellation = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        operation = operations_->find(operation_id);
        if (!operation) throw std::invalid_argument("operation not found: " + operation_id);
        if (workflow_cancellations_.contains(session_id))
            throw std::invalid_argument("workflow is already running");
        if (const auto running = operation_cancellations_.find(session_id);
            running != operation_cancellations_.end() && running->second != cancellation)
            throw std::invalid_argument("an operation is already running for this project");
        project = iterator->second.get();
        {
            std::lock_guard cache_lock(result_cache_mutex_);
            result_row_key_cache_.clear();
            result_navigation_index_cache_.clear();
        }
        clear_decoded_trace_cache_locked();
    }

    if (cancellation->load()) throw Error(ErrorCode::Cancelled, "Operation cancelled");

    if (effective_inputs.is_null())
        effective_inputs = resolve_workflow_inputs(session_id, effective_instance);
    if (!effective_inputs.is_object()) effective_inputs = Json::object();
    const auto resolved_parameters = operation->resolve_parameters(parameters);
    const auto computation_key = fingerprint::operation(
        operation_id, effective_instance, operation->definition().version,
        resolved_parameters, effective_inputs);
    const ResultStore store(project->get_database_path());
    const auto cached_results = store.find_cached_results(computation_key);
    if (cached_results.is_array() && !cached_results.empty()) {
        bool complete = true;
        for (const auto &port : operation->definition().output_ports) {
            // Success signals are run-local notifications, not reusable result
            // payloads. They are emitted again when a cached operation is reused.
            if (port.optional || port.id == "operationSuccessSignal" ||
                port.semantic_contract == "operationSuccessSignal") continue;
            const auto found = std::any_of(cached_results.begin(), cached_results.end(), [&](const Json &result) {
                const auto output_port = result.value("output_port_id", result.value("contract_id", std::string{}));
                return output_port == port.id || output_port == port.semantic_contract;
            });
            if (!found) { complete = false; break; }
        }
        if (complete) {
            Json emitted_results = Json::object();
            for (const auto &result : cached_results) {
                const auto output_port = result.value("output_port_id", result.value("contract_id", std::string{}));
                Json reference{{"result_id", result.value("result_id", std::string{})},
                               {"payload_ref", result.value("payload_ref", std::string{})},
                               {"contract_id", result.value("contract_id", output_port)},
                               {"representation", result.value("representation", result.value("kind", std::string{}))}};
                if (reference.value("representation", std::string{}) == "json")
                    reference["payload"] = store.read_json_payload(reference.value("result_id", std::string{}));
                if (reference.value("representation", std::string{}) == "table")
                    reference["physical_table"] = "result:" + reference.value("result_id", std::string{});
                emitted_results[output_port] = std::move(reference);
            }
            if (operation_log_callback_)
                operation_log_callback_(session_id, operation_id,
                                       "operation.cache_reused (" + operation_id + "): Reused service results; operation execution skipped.");
            return Json{{"emitted_results", std::move(emitted_results)}, {"cache_hit", true}};
        }
    }

    if (cancellation->load()) throw Error(ErrorCode::Cancelled, "Operation cancelled");

    {
        std::lock_guard lock(mutex_);
        if (workflow_cancellations_.contains(session_id))
            throw std::invalid_argument("workflow is already running");
        if (const auto running = operation_cancellations_.find(session_id);
            running != operation_cancellations_.end() && running->second != cancellation)
            throw std::invalid_argument("an operation is already running for this project");
        operation_cancellations_[session_id] = cancellation;
    }

    WorkerRequest request;
    request.executable = configuration_path_.parent_path() /
#ifdef _WIN32
        "streamfind-worker.exe";
#else
        "streamfind-worker";
#endif
    request.configuration = configuration_path_;
    request.memory_limit_mb = detail::configured_worker_memory_limit_mb(configuration_path_);
    request.service_port = service_port_;
    request.session_id = session_id;
    request.run_id = effective_instance;
    request.operation_id = operation_id;
    request.operation_instance = effective_instance;
    request.operation = operations_->find(operation_id)->definition().id;
    request.project_database_path = project->get_database_path();
    request.computation_key = computation_key;
    request.workflow = project->get_workflow().to_json();
    request.parameters = parameters;
    request.inputs = effective_inputs;
    auto worker = std::make_shared<WorkerProcess>(std::move(request),
        [this, session_id](const WorkerMetadata &metadata, std::string_view type,
                           std::string_view message, const Json &payload) {
            if (type == "worker.started") {
                if (operation_event_callback_) operation_event_callback_(session_id, metadata.operation_id, "worker.started", metadata.to_json());
            } else if (type == "operation.log") {
                if (operation_log_callback_) operation_log_callback_(session_id, payload.value("operation_id", ""), message);
            } else if (type != "worker.started" && operation_event_callback_ &&
                       type != "operation.started" && type != "operation.completed" &&
                       type != "operation.failed") {
                operation_event_callback_(session_id, payload.value("operation_id", ""), type, payload);
            }
        });
    {
        std::lock_guard lock(mutex_);
        active_workers_[session_id] = worker;
    }
    try {
        WorkerResult worker_result;
        try {
            worker_result = worker->run();
        } catch (...) {
            abort_worker_output_leases(session_id, effective_instance);
            std::lock_guard lock(mutex_);
            operation_cancellations_.erase(session_id);
            active_workers_.erase(session_id);
            throw;
        }
        abort_worker_output_leases(session_id, effective_instance);
        if (operation_event_callback_) {
            const auto worker_event = worker_result.cancelled
                ? "worker.cancelled"
                : worker_result.error.empty() && worker_result.exit_code == 0
                    ? "worker.completed"
                    : "worker.failed";
            operation_event_callback_(session_id, worker_result.worker.operation_id,
                                      worker_event, worker_result.worker.to_json());
        }
        if (worker_result.cancelled) throw Error(ErrorCode::Cancelled, "Operation cancelled");
        if (!worker_result.error.empty()) throw Error(ErrorCode::MethodExecution, worker_result.error);
        const auto emitted = worker_result.result.value("emitted_results", Json::object());
        if (emitted.is_object()) {
            for (const auto &[output_port, reference] : emitted.items()) {
                static_cast<void>(output_port);
                if (!reference.is_object()) continue;
                const auto result_id = reference.value("result_id", std::string{});
                if (result_id.empty()) continue;
                project->register_result_manifest(store.describe_result(result_id));
            }
        }
        {
            std::lock_guard lock(mutex_);
            operation_cancellations_.erase(session_id);
            active_workers_.erase(session_id);
        }
        return worker_result.result;
    } catch (...) {
        std::lock_guard lock(mutex_);
        operation_cancellations_.erase(session_id);
        active_workers_.erase(session_id);
        throw;
    }
}

std::vector<ProjectSessionDto> ProjectRuntimeManager::list() const {
    std::lock_guard lock(mutex_);
    std::vector<ProjectSessionDto> result;
    result.reserve(projects_.size());
    for (const auto &[session_id, project] : projects_) result.push_back(describe(session_id, *project));
    return result;
}

bool ProjectRuntimeManager::contains(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    return projects_.contains(session_id);
}

void ProjectRuntimeManager::set_operation_log_callback(
    std::function<void(const std::string &, std::string_view, std::string_view)> callback) {
    std::lock_guard lock(mutex_);
    operation_log_callback_ = std::move(callback);
}

void ProjectRuntimeManager::set_operation_event_callback(
    std::function<void(const std::string &, std::string_view, std::string_view, const Json &)> callback) {
    std::lock_guard lock(mutex_);
    operation_event_callback_ = std::move(callback);
}

}  // namespace streamfind::service
