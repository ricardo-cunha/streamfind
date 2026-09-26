#include "streamfind/service/project_runtime_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace streamfind::service {

namespace detail {

Json workflow_without_positions(Json workflow) {
    for (auto &operation : workflow["operations"]) operation.erase("position");
    return workflow;
}

}  // namespace detail

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
    return ProjectSessionDto{session_id,
                             project.get_database_path().string(),
                             error ? 0 : size,
                             project.get_domains(),
                             project.get_metadata()};
}

ProjectSessionDto ProjectRuntimeManager::create(const std::string &session_id,
                                                 const ProjectOptions &options) {
    std::lock_guard lock(mutex_);
    if (projects_.contains(session_id)) throw std::invalid_argument("project session already exists");
    ensure_database_is_not_open(options.database_path);
    auto project = std::make_unique<Project>(Project::create(options));
    if (operation_log_callback_)
        project->set_operation_log_callback([this, session_id](std::string_view message) {
            operation_log_callback_(session_id, message);
        });
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
    if (operation_log_callback_)
        project->set_operation_log_callback([this, session_id](std::string_view message) {
            operation_log_callback_(session_id, message);
        });
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

Json ProjectRuntimeManager::workflow_definition(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto workflow = iterator->second->get_workflow();
    Json diagnostics = Json::array();
    try {
        workflow.validate(*operations_);
    } catch (const std::exception &error) {
        diagnostics.push_back(Json{{"message", error.what()}});
    }
    return Json{{"session_id", session_id}, {"state", diagnostics.empty() ? "validated" : "failed"},
                {"workflow", workflow.to_json()}, {"valid", diagnostics.empty()}, {"diagnostics", diagnostics}};
}

Json ProjectRuntimeManager::validate_workflow(const std::string &session_id, const Json &definition) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    auto workflow = Workflow::from_json(definition);
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
    auto workflow = Workflow::from_json(definition);
    const auto current = iterator->second->get_workflow();
    if (workflow.workflow_id.empty()) workflow.workflow_id = current.workflow_id.empty() ? "workflow" : current.workflow_id;
    const bool layout_only = detail::workflow_without_positions(workflow.to_json()) ==
                             detail::workflow_without_positions(current.to_json());
    workflow.version = layout_only ? current.version : std::max(current.version + 1, workflow.version);
    iterator->second->set_workflow(workflow, *operations_);
    return Json{{"workflow", workflow.to_json()}, {"valid", true}, {"diagnostics", Json::array()}};
}

Json ProjectRuntimeManager::clear_workflow_history(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_workflow_history();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::workflow_snapshot(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    const auto state = workflow_states_.find(session_id);
    return Json{{"session_id", session_id},
                {"state", state == workflow_states_.end() ? kWorkflowIdle : state->second},
                {"progress", workflow_progress_.contains(session_id) ? workflow_progress_.at(session_id) : Json{{"completed", 0}, {"total", 0}, {"current_step", 0}}}};
}

Json ProjectRuntimeManager::artifact_inventory(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    auto artifacts = iterator->second->get_artifact_inventory();
    for (auto &artifact : artifacts) {
        const auto table = artifact.contains("physical_table") &&
                                   artifact.at("physical_table").is_string()
                               ? artifact.at("physical_table").get<std::string>()
                               : std::string{};
        if (artifact.value("representation", std::string{}) != "table" || table.empty()) continue;
        std::string quoted = "\"";
        for (const char character : table) quoted += character == '\"' ? "\"\"" : std::string(1, character);
        quoted += "\"";
        try {
            const auto count = iterator->second->query_json("SELECT COUNT(*) AS row_count FROM " + quoted);
            if (!count.empty()) {
                const auto row_count = count.front().value("row_count", std::string{"0"});
                artifact["row_count"] = row_count.empty() ? 0 : std::stoull(row_count);
            }
            const auto columns = iterator->second->query_json("DESCRIBE " + quoted);
            artifact["columns"] = Json::array();
            for (const auto &column : columns)
                artifact["columns"].push_back({{"name", column.value("column_name", "")}, {"type", column.value("column_type", "")}});
        } catch (const std::exception &error) {
            throw std::runtime_error("artifact inventory inspection failed for " + table + ": " + error.what());
        }
    }
    return artifacts;
}

Json ProjectRuntimeManager::artifact_data(const std::string &session_id, const Json &request) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto artifact_id = request.value("artifact_id", std::string{});
    if (artifact_id.empty()) throw std::invalid_argument("artifact_id is required");
    const auto quote = [](const std::string &value) {
        std::string output = "'";
        for (const char character : value) output += character == '\'' ? "''" : std::string(1, character);
        return output + "'";
    };
    const auto metadata = iterator->second->query_json(
        "SELECT physical_table, representation FROM ARTIFACT_INVENTORY WHERE artifact_id = " + quote(artifact_id));
    if (metadata.empty()) throw std::invalid_argument("artifact not found");
    if (metadata.front().value("representation", std::string{}) != "table")
        throw std::invalid_argument("artifact is not a table");
    const auto table = metadata.front().value("physical_table", std::string{});
    if (table.empty()) throw std::invalid_argument("artifact has no physical table");
    std::string quoted_table = "\"";
    for (const char character : table) quoted_table += character == '"' ? "\"\"" : std::string(1, character);
    quoted_table += "\"";
    const auto description = iterator->second->query_json("DESCRIBE " + quoted_table);
    std::vector<std::string> columns;
    for (const auto &column : description) columns.push_back(column.value("column_name", std::string{}));
    const auto search = request.value("search", std::string{});
    std::string where;
    if (!search.empty()) {
        std::string escaped;
        for (const char character : search) escaped += character == '\'' ? "''" : std::string(1, character);
        for (const auto &column : columns) {
            if (!where.empty()) where += " OR ";
            where += "CAST(\"" + column + "\" AS VARCHAR) ILIKE '%" + escaped + "%'";
        }
        where = " WHERE " + where;
    }
    const auto sort = request.value("sort_column", std::string{});
    const bool valid_sort = std::find(columns.begin(), columns.end(), sort) != columns.end();
    const auto order = valid_sort ? (" ORDER BY \"" + sort + "\" " + (request.value("descending", false) ? "DESC" : "ASC")) : std::string{};
    const auto limit = std::clamp(request.value("limit", 100), 1, 1000);
    const auto offset = std::max(0, request.value("offset", 0));
    const auto total = iterator->second->query_json("SELECT COUNT(*) AS row_count FROM " + quoted_table + where);
    const auto rows = iterator->second->query_json("SELECT * FROM " + quoted_table + where + order +
                                                   " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));
    Json column_json = Json::array();
    for (const auto &column : description)
        column_json.push_back({{"name", column.value("column_name", std::string{})}, {"type", column.value("column_type", std::string{})}});
    return {{"artifact_id", artifact_id}, {"columns", column_json}, {"rows", rows},
            {"offset", offset}, {"limit", limit}, {"total_rows", total.empty() ? 0 : std::stoull(total.front().value("row_count", std::string{"0"}))}};
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
    std::thread completed_worker;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        if (const auto worker = workflow_workers_.find(session_id); worker != workflow_workers_.end() && worker->second.joinable()) {
            const auto state = workflow_states_.find(session_id);
            if (state != workflow_states_.end() &&
                (state->second == "queued" || state->second == "running" || state->second == "cancelling"))
                throw std::invalid_argument("workflow is already running");
            completed_worker = std::move(worker->second);
            workflow_workers_.erase(worker);
        }
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
    if (completed_worker.joinable()) completed_worker.join();
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
                                          const Json &parameters,
                                          const std::string &operation_instance) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto operation = operations_->find(operation_id);
    if (!operation) throw std::invalid_argument("operation not found: " + operation_id);
    return iterator->second->run_operation(operation_id, parameters, *operations_, operation_instance);
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
    std::function<void(const std::string &, std::string_view)> callback) {
    std::lock_guard lock(mutex_);
    operation_log_callback_ = std::move(callback);
}

}  // namespace streamfind::service
