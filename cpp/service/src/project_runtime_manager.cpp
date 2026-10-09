#include "streamfind/service/project_runtime_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <tuple>

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
        if (const auto cancellation = workflow_cancellations_.find(session_id); cancellation != workflow_cancellations_.end()) {
            cancellation->second->store(true);
            workflow_states_[session_id] = "cancelling";
        }
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
        workflow_operation_steps_.erase(session_id);
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
    auto workflow = definition.is_null() ? iterator->second->get_workflow() : Workflow::from_json(definition);
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

Json ProjectRuntimeManager::clear_artifact_cache(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_artifact_cache();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_all_artifacts(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_all_artifacts();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::workflow_snapshot(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto workflow = iterator->second->get_workflow();
    const auto state = workflow_states_.find(session_id);
    const std::string effective_state = state == workflow_states_.end() ? kWorkflowIdle : state->second;
    const auto progress = workflow_progress_.find(session_id);
    if (effective_state == "queued" || effective_state == "running" || effective_state == "cancelling") {
        return Json{{"session_id", session_id}, {"state", effective_state},
                    {"progress", progress == workflow_progress_.end()
                                      ? Json{{"completed", 0}, {"total", workflow.operations.size()}, {"current_step", 0}}
                                      : progress->second},
                    {"execution", Json::array()}};
    }
    const auto execution = iterator->second->get_workflow_execution();
    std::string persisted_state = effective_state;
    std::size_t completed = 0;
    std::size_t current_step = 0;
    bool has_running = false;
    bool has_failed = false;
    for (const auto &step : execution) {
        const auto status = step.value("status", std::string{});
        if (status == "completed") ++completed;
        if (status == "running") { has_running = true; current_step = step.value("step_index", 0); }
        if (status == "failed") { has_failed = true; current_step = step.value("step_index", 0); }
    }
    if (has_failed) persisted_state = "failed";
    else if (has_running) persisted_state = "running";
    else if (!workflow.operations.empty() && completed >= workflow.operations.size()) persisted_state = "completed";
    return Json{{"session_id", session_id}, {"state", persisted_state},
                {"progress", Json{{"completed", completed}, {"total", workflow.operations.size()}, {"current_step", current_step}}},
                {"execution", execution}};
}

Json ProjectRuntimeManager::artifact_inventory(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    auto artifacts = iterator->second->get_artifact_inventory();
    for (auto &artifact : artifacts) {
        const auto revision = artifact.find("workflow_revision");
        if (revision != artifact.end() && revision->is_string()) {
            try { artifact["workflow_revision"] = std::stoi(revision->get<std::string>()); }
            catch (...) { artifact["workflow_revision"] = 0; }
        }
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
            // A stale historical table must not hide valid current artifacts after reopening a project.
            // Keep the inventory row visible for provenance, but exclude it from current selection.
            artifact["status"] = "stale";
            artifact["availability_error"] = error.what();
        }
    }
    return artifacts;
}

Json ProjectRuntimeManager::current_artifact_inventory(const std::string &session_id) const {
    const auto artifacts = artifact_inventory(session_id);
    Json current = Json::object();
    for (const auto &artifact : artifacts) {
        if (artifact.value("status", "") != "published") continue;
        const auto key = artifact.value("producer_instance", "") + "\n" + artifact.value("contract_id", "");
        const auto existing = current.find(key);
        const auto newer = [&](const Json &candidate, const Json &previous) {
            const auto candidate_revision = candidate.value("workflow_revision", 0);
            const auto previous_revision = previous.value("workflow_revision", 0);
            return candidate_revision > previous_revision ||
                   (candidate_revision == previous_revision &&
                    std::tie(candidate["created_at"], candidate["artifact_id"]) >
                        std::tie(previous["created_at"], previous["artifact_id"]));
        };
        if (existing == current.end() || newer(artifact, *existing)) current[key] = artifact;
    }
    Json result = Json::array();
    for (const auto &[key, artifact] : current.items()) result.push_back(artifact);
    return result;
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
    for (const auto &column : description)
        columns.push_back(column.value("column_name", std::string{}));
    const auto quote_identifier = [](const std::string &value) {
        std::string output = "\"";
        for (const char character : value) output += character == '"' ? "\"\"" : std::string(1, character);
        return output + "\"";
    };
    const auto escape = [](const std::string &value) {
        std::string output;
        for (const char character : value) output += character == '\'' ? "''" : std::string(1, character);
        return output;
    };
    std::string where;
    const auto append_condition = [&where](const std::string &condition) {
        where += where.empty() ? " WHERE " + condition : " AND " + condition;
    };
    const auto search = request.value("search", std::string{});
    if (!search.empty()) {
        std::string search_condition;
        for (const auto &column : columns) {
            if (!search_condition.empty()) search_condition += " OR ";
            search_condition += "CAST(" + quote_identifier(column) + " AS VARCHAR) ILIKE '%" + escape(search) + "%'";
        }
        append_condition("(" + search_condition + ")");
    }
    const auto sort = request.value("sort_column", std::string{});
    const bool valid_sort = std::find(columns.begin(), columns.end(), sort) != columns.end();
    const auto order = valid_sort ? (" ORDER BY " + quote_identifier(sort) + " " + (request.value("descending", false) ? "DESC" : "ASC")) : std::string{};
    const auto limit = std::clamp(request.value("limit", 100), 1, 1000);
    const auto offset = std::max(0, request.value("offset", 0));
    auto rows = iterator->second->query_json(
        "SELECT *, COUNT(*) OVER() AS \"__streamfind_total_rows\" FROM " + quoted_table + where + order +
        " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));
    std::size_t total_rows = 0;
    if (!rows.empty()) {
        total_rows = std::stoull(rows.front().value("__streamfind_total_rows", std::string{"0"}));
        for (auto &row : rows) row.erase("__streamfind_total_rows");
    } else {
        const auto count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + where);
        if (!count.empty()) total_rows = std::stoull(count.front().value("__streamfind_total_rows", std::string{"0"}));
    }
    Json column_json = Json::array();
    for (const auto &column : description)
        column_json.push_back({{"name", column.value("column_name", std::string{})}, {"type", column.value("column_type", std::string{})}});
    return {{"artifact_id", artifact_id}, {"columns", column_json}, {"rows", rows},
            {"offset", offset}, {"limit", limit}, {"total_rows", total_rows}};
}

std::string ProjectRuntimeManager::set_workflow_state(const std::string &session_id, const std::string &state) {
    std::lock_guard lock(mutex_);
    if (projects_.find(session_id) == projects_.end()) throw std::invalid_argument("project session not found");
    static const std::set<std::string> valid_states{"idle", "validated", "queued", "running", "cancelling", "completed", "failed", "cancelled"};
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
        workflow_progress_[session_id] = Json{{"completed", 0}, {"total", iterator->second->get_workflow().operations.size()}, {"current_step", 0}};
        workflow_operation_steps_[session_id].clear();
        const auto workflow = iterator->second->get_workflow();
        for (std::size_t index = 0; index < workflow.operations.size(); ++index)
            workflow_operation_steps_[session_id][workflow.operations[index].id] = index;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        workflow_cancellations_[session_id] = cancellation;
        project = iterator->second.get();
        workflow_workers_[session_id] = std::thread([this, session_id, project, cancellation] {
            try {
                project->set_cancellation_flag(cancellation.get());
                if (cancellation->load()) {
                    set_workflow_state(session_id, "cancelled");
                    project->set_cancellation_flag(nullptr);
                    std::lock_guard lock(mutex_);
                    workflow_cancellations_.erase(session_id);
                    return;
                }
                set_workflow_state(session_id, "running");
                WorkflowExecutionManager execution_manager(*project);
                execution_manager.create(Json{{"workflow_revision", project->get_workflow().version},
                                              {"progress", Json{{"completed", 0}, {"total", project->get_workflow().operations.size()}, {"current_step", 0}}}});
                const auto result = project->run_worker(session_id, *operations_);
                project->set_cancellation_flag(nullptr);
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
                project->set_cancellation_flag(nullptr);
                try { set_workflow_state(session_id, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
            } catch (const std::exception &error) {
                project->set_cancellation_flag(nullptr);
                try { set_workflow_state(session_id, cancellation->load() ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, cancellation->load() ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
            } catch (...) {
                project->set_cancellation_flag(nullptr);
                try { set_workflow_state(session_id, "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, "workflow.failed", Json{{"message", "unknown workflow execution failure"}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = "unknown workflow execution failure";
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
