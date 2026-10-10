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

Json ProjectRuntimeManager::clear_node_artifacts(const std::string &session_id, const std::string &producer_instance) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_artifacts_for_producer_instance(producer_instance);
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}, {"producer_instance", producer_instance}};
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

Json ProjectRuntimeManager::artifact_query(const std::string &session_id, const Json &request) const {
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
    const auto mode = request.value("mode", std::string{"page"});
    if (mode != "page" && mode != "sample" && mode != "detail")
        throw std::invalid_argument("artifact query mode must be page, sample, or detail");
    std::vector<std::string> selected_columns;
    if (request.contains("columns")) {
        if (!request.at("columns").is_array()) throw std::invalid_argument("artifact query columns must be an array");
        for (const auto &column : request.at("columns")) {
            if (!column.is_string()) throw std::invalid_argument("artifact query column names must be strings");
            const auto name = column.get<std::string>();
            if (std::find(columns.begin(), columns.end(), name) == columns.end())
                throw std::invalid_argument("artifact query column not found: " + name);
            selected_columns.push_back(name);
        }
    }
    if (selected_columns.empty()) selected_columns = columns;
    if (selected_columns.size() > 128) throw std::invalid_argument("artifact query selects too many columns");
    std::string projection;
    for (const auto &column : selected_columns) {
        if (!projection.empty()) projection += ", ";
        projection += quote_identifier(column);
    }
    if (std::find(columns.begin(), columns.end(), "row_key") != columns.end())
        throw std::invalid_argument("artifact query cannot expose a source column named row_key");
    const auto projection_with_key = projection + ", CAST(rowid AS VARCHAR) AS \"row_key\"";
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
        for (const auto &column : selected_columns) {
            if (!search_condition.empty()) search_condition += " OR ";
            search_condition += "CAST(" + quote_identifier(column) + " AS VARCHAR) ILIKE '%" + escape(search) + "%'";
        }
        append_condition("(" + search_condition + ")");
    }
    const auto filters = request.value("filters", Json::array());
    if (!filters.is_array()) throw std::invalid_argument("artifact query filters must be an array");
    const auto scalar_sql = [](const Json &value) {
        if (value.is_null()) return std::string("NULL");
        if (value.is_boolean()) return value.get<bool>() ? std::string("TRUE") : std::string("FALSE");
        if (value.is_number()) return value.dump();
        if (value.is_string()) {
            std::string escaped = "'";
            for (const char character : value.get<std::string>()) escaped += character == '\'' ? "''" : std::string(1, character);
            return escaped + "'";
        }
        throw std::invalid_argument("artifact query filter value must be scalar");
    };
    for (const auto &filter : filters) {
        if (!filter.is_object() || !filter.contains("column") || !filter.at("column").is_string())
            throw std::invalid_argument("artifact query filter requires a column");
        const auto column = filter.at("column").get<std::string>();
        if (std::find(columns.begin(), columns.end(), column) == columns.end())
            throw std::invalid_argument("artifact query filter column not found: " + column);
        const auto operation = filter.value("operator", std::string{});
        const auto identifier = quote_identifier(column);
        if (operation == "is_null" || operation == "is_not_null") {
            append_condition(identifier + (operation == "is_null" ? " IS NULL" : " IS NOT NULL"));
        } else if (operation == "between") {
            if (!filter.contains("min") || !filter.contains("max")) throw std::invalid_argument("between filter requires min and max");
            append_condition(identifier + " BETWEEN " + scalar_sql(filter.at("min")) + " AND " + scalar_sql(filter.at("max")));
        } else if (operation == "in") {
            if (!filter.contains("values") || !filter.at("values").is_array() || filter.at("values").empty())
                throw std::invalid_argument("in filter requires a non-empty values array");
            std::string values;
            for (const auto &value : filter.at("values")) {
                if (!values.empty()) values += ", ";
                values += scalar_sql(value);
            }
            append_condition(identifier + " IN (" + values + ")");
        } else {
            static const std::set<std::string> operators{"eq", "neq", "gt", "gte", "lt", "lte"};
            if (!operators.contains(operation) || !filter.contains("value"))
                throw std::invalid_argument("unsupported artifact query filter");
            const auto sql_operator = operation == "eq" ? "=" : operation == "neq" ? "<>" : operation == "gt" ? ">" :
                                      operation == "gte" ? ">=" : operation == "lt" ? "<" : "<=";
            append_condition(identifier + " " + sql_operator + " " + scalar_sql(filter.at("value")));
        }
    }
    if (request.contains("row_keys")) {
        if (!request.at("row_keys").is_array() || request.at("row_keys").empty())
            throw std::invalid_argument("row_keys must be a non-empty array");
        std::string keys;
        for (const auto &key : request.at("row_keys")) {
            if (!key.is_string()) throw std::invalid_argument("row_keys must contain strings");
            if (!keys.empty()) keys += ", ";
            keys += scalar_sql(key);
        }
        append_condition("CAST(rowid AS VARCHAR) IN (" + keys + ")");
    }
    const auto sort = request.value("sort_column", std::string{});
    const bool valid_sort = std::find(columns.begin(), columns.end(), sort) != columns.end();
    std::string order = valid_sort ? (" ORDER BY " + quote_identifier(sort) + " " + (request.value("descending", false) ? "DESC" : "ASC")) : std::string{};
    std::string x_column;
    std::string y_column;
    bool requested_grid_sampling = false;
    bool grid_sampling = false;
    std::string sample_where = where;
    if (mode == "sample") {
        x_column = request.value("x_column", std::string{});
        y_column = request.value("y_column", std::string{});
        if (std::find(columns.begin(), columns.end(), x_column) == columns.end() ||
            std::find(columns.begin(), columns.end(), y_column) == columns.end())
            throw std::invalid_argument("sample mode requires valid x_column and y_column");
        order = " ORDER BY " + quote_identifier(x_column) + ", " + quote_identifier(y_column);
        requested_grid_sampling = request.value("sampling_strategy", std::string{"ordered"}) == "grid";
    }
    const auto limit = std::clamp(request.value("limit", mode == "sample" ? 50000 : 100), 1, mode == "sample" ? 50000 : 1000);
    const auto offset = std::max(0, request.value("offset", 0));
    std::size_t total_rows = 0;
    if (mode == "sample") {
        const auto count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + where);
        if (!count.empty()) total_rows = std::stoull(count.front().value("__streamfind_total_rows", std::string{"0"}));
        grid_sampling = requested_grid_sampling && total_rows > static_cast<std::size_t>(limit);
        if (grid_sampling) {
            sample_where += sample_where.empty() ? " WHERE " : " AND ";
            sample_where += quote_identifier(x_column) + " IS NOT NULL AND " + quote_identifier(y_column) + " IS NOT NULL";
            const auto coordinate_count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + sample_where);
            if (!coordinate_count.empty()) total_rows = std::stoull(coordinate_count.front().value("__streamfind_total_rows", std::string{"0"}));
        }
    }
    const auto x_bins = std::clamp(request.value("x_bins", 256), 1, 1024);
    const auto y_bins = std::clamp(request.value("y_bins", 256), 1, 1024);
    const auto bounds = grid_sampling
        ? iterator->second->query_json("SELECT MIN(" + quote_identifier(x_column) + ") AS \"min_x\", MAX(" + quote_identifier(x_column) +
          ") AS \"max_x\", MIN(" + quote_identifier(y_column) + ") AS \"min_y\", MAX(" + quote_identifier(y_column) +
          ") AS \"max_y\" FROM " + quoted_table + sample_where)
        : Json::array();
    const auto min_x = !bounds.empty() ? std::stod(bounds.front().value("min_x", std::string{"0"})) : 0.0;
    const auto max_x = !bounds.empty() ? std::stod(bounds.front().value("max_x", std::string{"0"})) : 0.0;
    const auto min_y = !bounds.empty() ? std::stod(bounds.front().value("min_y", std::string{"0"})) : 0.0;
    const auto max_y = !bounds.empty() ? std::stod(bounds.front().value("max_y", std::string{"0"})) : 0.0;
    const auto x_range = max_x - min_x;
    const auto y_range = max_y - min_y;
    const auto x_bin = "CASE WHEN " + std::to_string(x_range) + " = 0 THEN 0 ELSE LEAST(" + std::to_string(x_bins - 1) +
        ", GREATEST(0, CAST(FLOOR((" + quote_identifier(x_column) + " - " + std::to_string(min_x) + ") / " + std::to_string(x_range) +
        " * " + std::to_string(x_bins) + ") AS INTEGER))) END";
    const auto y_bin = "CASE WHEN " + std::to_string(y_range) + " = 0 THEN 0 ELSE LEAST(" + std::to_string(y_bins - 1) +
        ", GREATEST(0, CAST(FLOOR((" + quote_identifier(y_column) + " - " + std::to_string(min_y) + ") / " + std::to_string(y_range) +
        " * " + std::to_string(y_bins) + ") AS INTEGER))) END";
    const auto query = grid_sampling
        ? "SELECT " + projection + ", \"row_key\", \"bin_count\" FROM (SELECT " + projection_with_key + ", " + x_bin +
          " AS \"x_bin\", " + y_bin + " AS \"y_bin\", COUNT(*) OVER (PARTITION BY " + x_bin + ", " + y_bin +
          ") AS \"bin_count\", ROW_NUMBER() OVER (PARTITION BY " + x_bin + ", " + y_bin + " ORDER BY rowid) AS \"bin_row\" FROM " +
          quoted_table + sample_where + ") sampled WHERE \"bin_row\" = 1 ORDER BY \"x_bin\", \"y_bin\" LIMIT " + std::to_string(limit)
        : mode == "sample"
        ? "SELECT " + projection + ", \"row_key\" FROM (SELECT " + projection_with_key + ", ROW_NUMBER() OVER (" + order.substr(1) +
          ") AS \"__streamfind_row_number\" FROM " + quoted_table + sample_where + ") sampled WHERE ((\"__streamfind_row_number\" - 1) % " +
          std::to_string(std::max<std::size_t>(1, (total_rows + limit - 1) / limit)) + ") = 0 ORDER BY \"__streamfind_row_number\" LIMIT " +
          std::to_string(limit) + " OFFSET " + std::to_string(offset)
        : "SELECT " + projection_with_key + ", COUNT(*) OVER() AS \"__streamfind_total_rows\" FROM " + quoted_table + where + order +
          " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
    auto rows = iterator->second->query_json(query);
    if (!rows.empty()) {
        if (mode != "sample") total_rows = std::stoull(rows.front().value("__streamfind_total_rows", std::string{"0"}));
        for (auto &row : rows) row.erase("__streamfind_total_rows");
    } else {
        const auto count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + where);
        if (!count.empty()) total_rows = std::stoull(count.front().value("__streamfind_total_rows", std::string{"0"}));
    }
    Json column_json = Json::array();
    for (const auto &column : description)
        if (std::find(selected_columns.begin(), selected_columns.end(), column.value("column_name", std::string{})) != selected_columns.end())
            column_json.push_back({{"name", column.value("column_name", std::string{})}, {"type", column.value("column_type", std::string{})}});
    return {{"artifact_id", artifact_id}, {"columns", column_json}, {"rows", rows},
            {"offset", offset}, {"limit", limit}, {"total_rows", total_rows},
            {"returned_rows", rows.size()}, {"mode", mode}, {"has_more", offset + rows.size() < total_rows}};
}

Json ProjectRuntimeManager::artifact_data(const std::string &session_id, const Json &request) const {
    return artifact_query(session_id, request);
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
