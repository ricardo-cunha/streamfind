#include "streamfind/service/project_runtime_manager.hpp"

#include <stdexcept>
#include <system_error>

namespace streamfind::service {

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
    return ProjectSessionDto{session_id,
                             project.get_database_path().string(),
                             project.get_domain(),
                             project.get_metadata()};
}

ProjectSessionDto ProjectRuntimeManager::create(const std::string &session_id,
                                                 const ProjectOptions &options) {
    std::lock_guard lock(mutex_);
    if (projects_.contains(session_id)) throw std::invalid_argument("project session already exists");
    ensure_database_is_not_open(options.database_path);
    auto project = std::make_unique<Project>(Project::create(options));
    const auto result = describe(session_id, *project);
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
    const auto result = describe(session_id, *project);
    projects_.emplace(session_id, std::move(project));
    workflow_states_[session_id] = kWorkflowIdle;
    return result;
}

ProjectSessionDto ProjectRuntimeManager::close(const std::string &session_id) {
    std::thread worker;
    ProjectSessionDto result;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        result = describe(session_id, *iterator->second);
        if (const auto worker_iterator = workflow_workers_.find(session_id); worker_iterator != workflow_workers_.end()) {
            worker = std::move(worker_iterator->second);
            workflow_workers_.erase(worker_iterator);
        }
    }
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(mutex_);
        projects_.erase(session_id);
        workflow_states_.erase(session_id);
        workflow_cancellations_.erase(session_id);
        workflow_progress_.erase(session_id);
    }
    return result;
}

Json ProjectRuntimeManager::workflow_snapshot(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    const auto state = workflow_states_.find(session_id);
    return Json{{"session_id", session_id},
                {"state", state == workflow_states_.end() ? kWorkflowIdle : state->second},
                {"progress", workflow_progress_.contains(session_id) ? workflow_progress_.at(session_id) : Json{{"completed", 0}, {"total", 0}, {"current_step", 0}}}};
}

std::string ProjectRuntimeManager::set_workflow_state(const std::string &session_id, const std::string &state) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    static const std::set<std::string> valid_states{"idle", "validated", "queued", "running", "paused", "cancelling", "completed", "failed", "cancelled"};
    if (!valid_states.contains(state)) throw std::invalid_argument("invalid workflow state");
    workflow_states_[session_id] = state;
    return state;
}

std::string ProjectRuntimeManager::start_workflow(const std::string &session_id) {
    Project *project = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        if (const auto worker = workflow_workers_.find(session_id); worker != workflow_workers_.end() && worker->second.joinable())
            throw std::invalid_argument("workflow is already running");
        workflow_states_[session_id] = "queued";
        workflow_progress_[session_id] = Json{{"completed", 0}, {"total", 0}, {"current_step", 0}};
        auto cancellation = std::make_shared<CancellationToken>();
        workflow_cancellations_[session_id] = cancellation;
        project = iterator->second.get();
        workflow_workers_[session_id] = std::thread([this, session_id, project, cancellation] {
            try {
                if (cancellation->is_cancelled()) {
                    set_workflow_state(session_id, "cancelled");
                    std::lock_guard lock(mutex_);
                    workflow_cancellations_.erase(session_id);
                    return;
                }
                set_workflow_state(session_id, "running");
                const auto result = project->run_operation_graph(*operations_);
                set_workflow_state(session_id, result.value("status", "failed") == "completed" ? "completed" : "failed");
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
            } catch (const std::exception &error) {
                try { set_workflow_state(session_id, "failed"); } catch (...) {}
                std::lock_guard lock(mutex_);
                workflow_progress_[session_id] = Json{{"completed", 0}, {"total", 0}, {"current_step", 0}, {"error", error.what()}};
            } catch (...) {
                try { set_workflow_state(session_id, "failed"); } catch (...) {}
                std::lock_guard lock(mutex_);
                workflow_progress_[session_id] = Json{{"completed", 0}, {"total", 0}, {"current_step", 0}, {"error", "unknown workflow execution failure"}};
            }
        });
    }
    return "queued";
}

std::string ProjectRuntimeManager::cancel_workflow(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    const auto cancellation = workflow_cancellations_.find(session_id);
    if (cancellation == workflow_cancellations_.end()) throw std::invalid_argument("workflow is not running");
    cancellation->second->cancel();
    workflow_states_[session_id] = "cancelling";
    return "cancelling";
}

Json ProjectRuntimeManager::run_operation(const std::string &session_id,
                                          const std::string &operation_id,
                                          const Json &parameters) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto operation = operations_->find(operation_id);
    if (!operation) throw std::invalid_argument("operation not found: " + operation_id);
    if (operation->definition().domain != iterator->second->get_domain())
        throw std::invalid_argument("operation is not available in the project domain");
    return iterator->second->run_operation(operation_id, parameters, *operations_);
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

}  // namespace streamfind::service
