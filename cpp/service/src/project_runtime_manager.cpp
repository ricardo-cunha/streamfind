#include "streamfind/service/project_runtime_manager.hpp"
#include "streamfind/fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <vector>

namespace streamfind::service {

namespace detail {

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

std::vector<unsigned char> transient_slice(const ProjectRuntimeManager::TransientResult &result,
                                           std::uintmax_t offset, std::uintmax_t length) {
    if (offset > result.bytes.size()) return {};
    const auto available = result.bytes.size() - static_cast<std::size_t>(offset);
    const auto count = std::min<std::size_t>(available, static_cast<std::size_t>(length));
    return std::vector<unsigned char>(result.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      result.bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

Json transient_arrow_batch(const ProjectRuntimeManager::TransientResult &result,
                           std::uintmax_t batch_index) {
    std::size_t cursor = 0;
    std::uintmax_t index = 0;
    while (cursor + sizeof(std::uint64_t) <= result.bytes.size()) {
        std::uint64_t size = 0;
        for (std::size_t byte = 0; byte < sizeof(size); ++byte)
            size |= static_cast<std::uint64_t>(result.bytes[cursor + byte]) << (byte * 8);
        cursor += sizeof(size);
        if (size > result.bytes.size() - cursor) throw std::runtime_error("transient Arrow result frame is truncated");
        std::vector<unsigned char> frame(result.bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                          result.bytes.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += static_cast<std::size_t>(size);
        if (index++ == batch_index)
            return Json{{"rows", data_proxy::arrow_ipc_decode_rows(frame)},
                        {"offset", batch_index}, {"limit", 1}};
    }
    return Json{{"rows", Json::array()}, {"offset", batch_index}, {"limit", 1}};
}

Json transient_arrow_rows(const ProjectRuntimeManager::TransientResult &result,
                          std::uintmax_t offset, std::uintmax_t limit) {
    Json rows = Json::array();
    std::size_t cursor = 0;
    while (cursor + sizeof(std::uint64_t) <= result.bytes.size()) {
        std::uint64_t size = 0;
        for (std::size_t byte = 0; byte < sizeof(size); ++byte)
            size |= static_cast<std::uint64_t>(result.bytes[cursor + byte]) << (byte * 8);
        cursor += sizeof(size);
        if (size > result.bytes.size() - cursor)
            throw std::runtime_error("transient Arrow result frame is truncated");
        const std::vector<unsigned char> frame(
            result.bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            result.bytes.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += static_cast<std::size_t>(size);
        const auto frame_rows = data_proxy::arrow_ipc_decode_rows(frame);
        if (!frame_rows.is_array()) throw std::runtime_error("transient Arrow result is not a row array");
        for (const auto &row : frame_rows) rows.push_back(row);
    }
    if (offset >= rows.size()) return Json::array();
    const auto begin = static_cast<std::size_t>(offset);
    const auto end = std::min(rows.size(), begin + static_cast<std::size_t>(limit));
    return Json(rows.begin() + static_cast<std::ptrdiff_t>(begin),
                rows.begin() + static_cast<std::ptrdiff_t>(end));
}

Json table_parameter_value(const ResultStore &store, const Json &descriptor) {
    const auto result_id = descriptor.value("result_id", std::string{});
    if (result_id.empty()) throw std::invalid_argument("table result has no result_id");
    const auto format = descriptor.value("format", std::string{});
    Json rows = Json::array();
    std::uintmax_t offset = 0;
    constexpr std::uintmax_t page_size = 65536;
    for (;;) {
        const auto page = format == "arrow-ipc-batches"
            ? store.read_arrow_ipc_batch(result_id, offset, page_size)
            : store.read_jsonl_batch(result_id, offset, page_size);
        const auto page_rows = page.value("rows", Json::array());
        for (const auto &row : page_rows) rows.push_back(row);
        offset += page_rows.size();
        if (!page.value("has_more", false) || page_rows.empty()) break;
    }

    struct ColumnInfo { std::string name; std::string type{"string"}; };
    std::vector<ColumnInfo> columns;
    for (const auto &row : rows) {
        if (!row.is_object()) continue;
        for (const auto &[name, value] : row.items()) {
            const auto found = std::find_if(columns.begin(), columns.end(),
                                            [&](const ColumnInfo &column) { return column.name == name; });
            if (found == columns.end()) {
                std::string type = "string";
                if (value.is_boolean()) type = "boolean";
                else if (value.is_number_integer()) type = "integer";
                else if (value.is_number()) type = "real";
                columns.push_back({name, type});
            } else if (found->type == "integer" && value.is_number_float()) {
                found->type = "real";
            }
        }
    }

    Json output = Json{{"columns", Json::array()}};
    for (const auto &column : columns) {
        Json values = Json::array();
        for (const auto &row : rows) {
            const auto value = row.is_object() && row.contains(column.name) ? row.at(column.name) : Json{};
            if (column.type == "boolean") values.push_back(value.is_boolean() ? Json(value.get<bool>()) : Json(false));
            else if (column.type == "integer") values.push_back(value.is_number_integer() ? Json(value.get<std::int64_t>()) : Json(std::int64_t{0}));
            else if (column.type == "real") values.push_back(value.is_number() ? Json(value.get<double>()) : Json(0.0));
            else if (value.is_null()) values.push_back("");
            else values.push_back(value.is_string() ? Json(value.get<std::string>()) : Json(value.dump()));
        }
        output["columns"].push_back({{"name", column.name}, {"type", column.type}, {"values", values}});
    }
    return output;
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
    }
    for (const auto &worker : workers) if (worker) worker->cancel();
    for (auto &thread : threads) if (thread.joinable()) thread.join();
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
    return ProjectSessionDto{session_id,
                             project.get_database_path().string(),
                             error ? 0 : size,
                             project.get_domains(),
                             project.get_metadata(),
                             result_store.inspect().to_json()};
}

ProjectSessionDto ProjectRuntimeManager::create(const std::string &session_id,
                                                 const ProjectOptions &options) {
    std::lock_guard lock(mutex_);
    if (projects_.contains(session_id)) throw std::invalid_argument("project session already exists");
    ensure_database_is_not_open(options.database_path);
    auto project = std::make_unique<Project>(Project::create(options));
    ResultStore(project->get_database_path()).initialize();
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
    ResultStore(project->get_database_path()).initialize();
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
    ProjectSessionDto result;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        if (operation_cancellations_.contains(session_id))
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
    }
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(mutex_);
        projects_.erase(session_id);
        workflow_definitions_.erase(session_id);
        workflow_states_.erase(session_id);
        workflow_cancellations_.erase(session_id);
        workflow_progress_.erase(session_id);
        workflow_execution_modes_.erase(session_id);
        workflow_operation_steps_.erase(session_id);
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
    auto workflow = Workflow::from_json(definition);
    const auto current = workflow_definitions_.at(session_id);
    if (workflow.workflow_id.empty()) workflow.workflow_id = current.workflow_id.empty() ? "workflow" : current.workflow_id;
    const bool layout_only = detail::workflow_without_positions(workflow.to_json()) ==
                             detail::workflow_without_positions(current.to_json());
    workflow.version = layout_only ? current.version : std::max(current.version + 1, workflow.version);
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
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_all_results(const std::string &session_id) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_all_results();
    return Json{{"workflow", iterator->second->get_workflow().to_json()}, {"cleared", true}};
}

Json ProjectRuntimeManager::clear_node_results(const std::string &session_id, const std::string &producer_instance) {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    iterator->second->clear_results_for_producer_instance(producer_instance);
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
        if (!workflow_active) {
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

Json ProjectRuntimeManager::operation_snapshot(const std::string &session_id,
                                               const std::string &operation_instance) const {
    std::lock_guard lock(mutex_);
    if (!projects_.contains(session_id)) throw std::invalid_argument("project session not found");
    const auto worker = active_workers_.find(session_id);
    if (worker == active_workers_.end())
        return Json{{"session_id", session_id}, {"operation_id", operation_instance}, {"state", "idle"}, {"worker", nullptr}};
    auto metadata = worker->second->metadata();
    return Json{{"session_id", session_id}, {"operation_id", operation_instance},
                {"state", metadata.state}, {"worker", metadata.to_json()}};
}

Json ProjectRuntimeManager::result_inventory(const std::string &session_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    auto results = iterator->second->get_result_inventory();
    for (auto &artifact : results) {
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
        if (table.rfind("result:", 0) == 0) continue;
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
    const ResultStore store(iterator->second->get_database_path());
    for (const auto &manifest : store.published_results()) {
        auto result = manifest;
        result["result_id"] = manifest.value("result_id", std::string{});
        result["status"] = "published";
        result["representation"] = manifest.value("kind", std::string{"binary"});
        if (result.value("representation", std::string{}) == "table" &&
            result.contains("schema") && result.at("schema").is_array())
            result["columns"] = result["schema"];
        if (result.value("representation", std::string{}) == "table")
            result["physical_table"] = "result:" + result.value("result_id", std::string{});
        if (manifest.contains("contract_id")) result["contract_id"] = manifest.at("contract_id");
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
    std::lock_guard lock(mutex_);
    const auto iterator = projects_.find(session_id);
    if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
    const auto result_id = request.value("result_id", std::string{});
    if (result_id.empty()) throw std::invalid_argument("result_id is required");
    const ResultStore store(iterator->second->get_database_path());
    try {
        const auto descriptor = store.describe_result(result_id);
        if (descriptor.value("kind", std::string{}) == "table" &&
            (descriptor.value("format", std::string{}) == "jsonl" ||
             descriptor.value("format", std::string{}) == "arrow-ipc-batches")) {
            const auto mode = request.value("mode", std::string{"page"});
            if (mode != "page" && mode != "sample" && mode != "detail")
                throw std::invalid_argument("result table query mode must be page, sample, or detail");
            const auto offset = request.value("offset", std::uintmax_t{0});
            const auto limit = request.value("limit", std::uintmax_t{1000});
            const auto batch = descriptor.value("format", std::string{}) == "arrow-ipc-batches"
                ? store.read_arrow_ipc_batch(result_id, offset, limit)
                : store.read_jsonl_batch(result_id, offset, limit);
            Json columns = descriptor.value("schema", Json::array());
            Json rows = batch.value("rows", Json::array());
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
            return Json{{"result_id", result_id},
                        {"columns", columns}, {"rows", rows}, {"offset", offset},
                        {"limit", limit}, {"returned_rows", rows.size()},
                        {"mode", mode}, {"has_more", batch.value("has_more", false)}};
        }
    } catch (const std::exception &) {
        // A non-result-store identifier is handled by the legacy metadata path below.
    }
    const auto quote = [](const std::string &value) {
        std::string output = "'";
        for (const char character : value) output += character == '\'' ? "''" : std::string(1, character);
        return output + "'";
    };
    const auto metadata = iterator->second->query_json(
        "SELECT physical_table, representation FROM RESULT_REGISTRY WHERE result_id = " + quote(result_id));
    if (metadata.empty()) throw std::invalid_argument("result not found");
    if (metadata.front().value("representation", std::string{}) != "table")
        throw std::invalid_argument("result is not a table");
    const auto table = metadata.front().value("physical_table", std::string{});
    if (table.empty()) throw std::invalid_argument("result has no physical table");
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
    if (mode != "page" && mode != "sample" && mode != "detail" && mode != "eic" && mode != "visualization")
        throw std::invalid_argument("result query mode must be page, sample, detail, eic, or visualization");
    std::vector<std::string> selected_columns;
    if (request.contains("columns")) {
        if (!request.at("columns").is_array()) throw std::invalid_argument("result query columns must be an array");
        for (const auto &column : request.at("columns")) {
            if (!column.is_string()) throw std::invalid_argument("result query column names must be strings");
            const auto name = column.get<std::string>();
            if (std::find(columns.begin(), columns.end(), name) == columns.end())
                throw std::invalid_argument("result query column not found: " + name);
            selected_columns.push_back(name);
        }
    }
    if (selected_columns.empty()) selected_columns = columns;
    if (selected_columns.size() > 128) throw std::invalid_argument("result query selects too many columns");
    std::vector<std::string> trace_columns;
    if (mode == "visualization") {
        if (!request.contains("trace_columns") || !request.at("trace_columns").is_array() || request.at("trace_columns").empty())
            throw std::invalid_argument("visualization mode requires non-empty trace_columns");
        for (const auto &column : request.at("trace_columns")) {
            if (!column.is_string()) throw std::invalid_argument("visualization trace column names must be strings");
            const auto name = column.get<std::string>();
            if (std::find(columns.begin(), columns.end(), name) == columns.end())
                throw std::invalid_argument("visualization trace column not found: " + name);
            trace_columns.push_back(name);
        }
    }
    std::string x_column;
    std::string y_column;
    if (mode == "visualization") {
        x_column = request.value("x_column", std::string{});
        y_column = request.value("y_column", std::string{});
        if (std::find(columns.begin(), columns.end(), x_column) == columns.end() ||
            std::find(columns.begin(), columns.end(), y_column) == columns.end())
            throw std::invalid_argument("visualization mode requires valid x_column and y_column");
        if (std::find(selected_columns.begin(), selected_columns.end(), x_column) == selected_columns.end())
            selected_columns.push_back(x_column);
        if (std::find(selected_columns.begin(), selected_columns.end(), y_column) == selected_columns.end())
            selected_columns.push_back(y_column);
    }
    if (selected_columns.size() > 128) throw std::invalid_argument("result query selects too many columns");
    std::string projection;
    for (const auto &column : selected_columns) {
        if (!projection.empty()) projection += ", ";
        projection += quote_identifier(column);
    }
    if (std::find(columns.begin(), columns.end(), "row_key") != columns.end())
        throw std::invalid_argument("result query cannot expose a source column named row_key");
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
    if (!filters.is_array()) throw std::invalid_argument("result query filters must be an array");
    const auto scalar_sql = [](const Json &value) {
        if (value.is_null()) return std::string("NULL");
        if (value.is_boolean()) return value.get<bool>() ? std::string("TRUE") : std::string("FALSE");
        if (value.is_number()) return value.dump();
        if (value.is_string()) {
            std::string escaped = "'";
            for (const char character : value.get<std::string>()) escaped += character == '\'' ? "''" : std::string(1, character);
            return escaped + "'";
        }
        throw std::invalid_argument("result query filter value must be scalar");
    };
    for (const auto &filter : filters) {
        if (!filter.is_object() || !filter.contains("column") || !filter.at("column").is_string())
            throw std::invalid_argument("result query filter requires a column");
        const auto column = filter.at("column").get<std::string>();
        if (std::find(columns.begin(), columns.end(), column) == columns.end())
            throw std::invalid_argument("result query filter column not found: " + column);
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
                throw std::invalid_argument("unsupported result query filter");
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
    if (mode == "visualization" && (request.contains("x_min") || request.contains("x_max"))) {
        if (!request.contains("x_min") || !request.contains("x_max") ||
            !request.at("x_min").is_number() || !request.at("x_max").is_number())
            throw std::invalid_argument("visualization x_min and x_max must be numeric and supplied together");
        const auto x_min = request.at("x_min").get<double>();
        const auto x_max = request.at("x_max").get<double>();
        if (!std::isfinite(x_min) || !std::isfinite(x_max) || x_min > x_max)
            throw std::invalid_argument("visualization x_min and x_max must be finite with x_min <= x_max");
        append_condition(quote_identifier(x_column) + " BETWEEN " + std::to_string(x_min) + " AND " + std::to_string(x_max));
    }
    const auto sort = request.value("sort_column", std::string{});
    const bool valid_sort = std::find(columns.begin(), columns.end(), sort) != columns.end();
    std::string order = valid_sort ? (" ORDER BY " + quote_identifier(sort) + " " + (request.value("descending", false) ? "DESC" : "ASC")) : std::string{};
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
    const auto limit = std::clamp(request.value("limit", mode == "sample" ? 50000 : mode == "eic" ? 250000 :
                                           mode == "visualization" ? 1000000 : 100), 1,
                                  mode == "sample" ? 50000 : mode == "eic" ? 250000 : mode == "visualization" ? 1000000 : 1000);
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
    std::string trace_expression;
    if (mode == "visualization") {
        for (const auto &column : trace_columns) {
            if (!trace_expression.empty()) trace_expression += ", ";
            trace_expression += "COALESCE(CAST(" + quote_identifier(column) + " AS VARCHAR), '')";
        }
        trace_expression = "concat_ws('\\x1f', " + trace_expression + ")";
    }
    std::size_t visualization_trace_count = 0;
    if (mode == "visualization") {
        const auto trace_count = iterator->second->query_json(
            "SELECT COUNT(DISTINCT " + trace_expression + ") AS \"__streamfind_trace_count\" FROM " + quoted_table + where);
        if (!trace_count.empty())
            visualization_trace_count = std::stoull(trace_count.front().value("__streamfind_trace_count", std::string{"0"}));
    }
    const auto requested_points_per_trace = std::max(1, request.value("max_points_per_trace", 2000));
    const auto balanced_points_per_trace = mode == "visualization" && visualization_trace_count > 0
        ? std::min<std::size_t>(static_cast<std::size_t>(requested_points_per_trace),
                                std::max<std::size_t>(1, static_cast<std::size_t>(limit) / visualization_trace_count))
        : static_cast<std::size_t>(requested_points_per_trace);
    const auto visualization_query = mode == "visualization"
        ? "SELECT " + projection + ", \"row_key\", \"__streamfind_trace_key\" FROM (SELECT " + projection_with_key +
          ", " + trace_expression + " AS \"__streamfind_trace_key\", ROW_NUMBER() OVER (PARTITION BY " + trace_expression +
          " ORDER BY rowid) AS \"__streamfind_trace_row\", COUNT(*) OVER (PARTITION BY " + trace_expression +
          ") AS \"__streamfind_trace_count\" FROM " + quoted_table + where +
          ") sampled WHERE ((\"__streamfind_trace_row\" - 1) % GREATEST(1, CAST(CEIL(\"__streamfind_trace_count\" / " +
          std::to_string(balanced_points_per_trace) + ") AS BIGINT))) = 0 LIMIT " +
          std::to_string(limit)
        : std::string{};
    const auto query = mode == "visualization"
        ? visualization_query
        : grid_sampling
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
        if (mode != "sample" && mode != "visualization") total_rows = std::stoull(rows.front().value("__streamfind_total_rows", std::string{"0"}));
        if (mode == "visualization") {
            const auto count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + where);
            if (!count.empty()) total_rows = std::stoull(count.front().value("__streamfind_total_rows", std::string{"0"}));
        }
        for (auto &row : rows) row.erase("__streamfind_total_rows");
    } else {
        const auto count = iterator->second->query_json("SELECT COUNT(*) AS \"__streamfind_total_rows\" FROM " + quoted_table + where);
        if (!count.empty()) total_rows = std::stoull(count.front().value("__streamfind_total_rows", std::string{"0"}));
    }
    Json column_json = Json::array();
    for (const auto &column : description)
        if (std::find(selected_columns.begin(), selected_columns.end(), column.value("column_name", std::string{})) != selected_columns.end())
            column_json.push_back({{"name", column.value("column_name", std::string{})}, {"type", column.value("column_type", std::string{})}});
    if (mode == "visualization") {
        column_json.push_back({{"name", "__streamfind_trace_key"}, {"type", "VARCHAR"}});
    }
    return {{"result_id", result_id}, {"columns", column_json}, {"rows", rows},
            {"offset", offset}, {"limit", limit}, {"total_rows", total_rows},
            {"returned_rows", rows.size()}, {"mode", mode}, {"has_more", mode == "visualization" ? false : offset + rows.size() < total_rows}};
}

Json ProjectRuntimeManager::result_data(const std::string &session_id, const Json &request) const {
    return result_query(session_id, request);
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
                            resolved[connection.target_port] = detail::table_parameter_value(store, descriptor);
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
                resolved[connection.target_port] = detail::table_parameter_value(store, *selected);
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
        if (const auto transient = transient_results_.find(session_id + ":" + result_id);
            transient != transient_results_.end() && transient->second.published) {
            auto descriptor = transient->second.descriptor;
            descriptor["result_id"] = result_id;
            descriptor["format"] = transient->second.format;
            descriptor["state"] = "published";
            descriptor["size"] = transient->second.bytes.size();
            return data_proxy::response(request, std::move(descriptor)).to_json();
        }
        return data_proxy::response(request, store.describe_result(result_id)).to_json();
    }
    if (method == "read_result_range") {
        const auto result_id = request.payload.value("result_id", std::string{});
        if (result_id.empty()) throw std::invalid_argument("read_result_range requires result_id");
        const auto offset = request.payload.value("offset", std::uintmax_t{0});
        const auto length = request.payload.value("length", std::uintmax_t{0});
        if (const auto transient = transient_results_.find(session_id + ":" + result_id);
            transient != transient_results_.end() && transient->second.published) {
            return data_proxy::response(request, Json{
                {"data", data_proxy::base64_encode(detail::transient_slice(transient->second, offset, length))},
                {"offset", offset}, {"length", length}}).to_json();
        }
        return data_proxy::response(request, store.read_result_range(result_id, offset, length)).to_json();
    }
    const auto lease_key = session_id + ":" + request.lease_id;
    if (method == "allocate_result") {
        const auto result_id = request.payload.value("result_id", std::string{});
        const auto run_id = request.payload.value("run_id", request.run_id);
        const auto format = request.payload.value("format", std::string{});
        const auto extension = request.payload.value("extension", format);
        const auto storage = request.payload.value("storage", std::string{"published"});
        if (result_id.empty() || run_id.empty() || format.empty())
            throw std::invalid_argument("allocate_result requires result_id, run_id, and format");
        const auto lease_id = detail::create_data_proxy_lease_id();
        ResultLease lease;
        if (storage == "transient") {
            lease.result_id = result_id;
            lease.run_id = run_id;
            lease.format = "transient:" + format;
            transient_results_[session_id + ":" + result_id] = TransientResult{
                result_id, run_id, format, request.payload, {}, false};
        } else {
            lease = store.allocate(run_id, result_id, format, extension);
        }
        data_proxy_leases_[session_id + ":" + lease_id] = lease;
        return data_proxy::response(request, { {"lease_id", lease_id}, {"result_id", result_id},
                                                {"run_id", run_id}, {"format", format},
                                                {"storage", storage}, {"max_chunk_size", 1024 * 1024},
                                                {"capabilities", {"write_chunks", "publish"}} }).to_json();
    }
    if (method == "write_result_chunk") {
        if (request.lease_id.empty()) throw std::invalid_argument("write_result_chunk requires lease_id");
        const auto lease_iterator = data_proxy_leases_.find(lease_key);
        if (lease_iterator == data_proxy_leases_.end()) throw std::invalid_argument("result lease not found");
        const auto offset = request.payload.value("offset", std::uintmax_t{0});
        const auto encoded = request.payload.value("data", std::string{});
        if (!request.payload.value("lease_id", request.lease_id).empty() &&
            request.payload.value("lease_id", request.lease_id) != request.lease_id)
            throw std::invalid_argument("result lease does not match the request");
        const auto bytes = detail::decode_base64(encoded);
        if (lease_iterator->second.format.rfind("transient:", 0) == 0) {
            auto transient = transient_results_.find(session_id + ":" + lease_iterator->second.result_id);
            if (transient == transient_results_.end()) throw std::invalid_argument("transient result not found");
            if (offset != transient->second.bytes.size())
                throw std::invalid_argument("transient result chunks must be sequential");
            transient->second.bytes.insert(transient->second.bytes.end(), bytes.begin(), bytes.end());
        } else {
            store.append_staging_chunk(lease_iterator->second, offset, bytes);
        }
        return data_proxy::response(request, { {"lease_id", request.lease_id},
                                                {"result_id", lease_iterator->second.result_id},
                                                {"offset", offset}, {"length", bytes.size()} }).to_json();
    }
    if (method == "publish_result") {
        if (request.lease_id.empty()) throw std::invalid_argument("publish_result requires lease_id");
        const auto lease_iterator = data_proxy_leases_.find(lease_key);
        if (lease_iterator == data_proxy_leases_.end()) throw std::invalid_argument("result lease not found");
        const auto metadata = request.payload.value("metadata", Json::object());
        if (!metadata.is_object()) throw std::invalid_argument("result metadata must be an object");
        Json manifest;
        if (lease_iterator->second.format.rfind("transient:", 0) == 0) {
            auto transient = transient_results_.find(session_id + ":" + lease_iterator->second.result_id);
            if (transient == transient_results_.end()) throw std::invalid_argument("transient result not found");
            transient->second.published = true;
            transient->second.descriptor["metadata"] = metadata;
            manifest = transient->second.descriptor;
            manifest["result_id"] = transient->second.result_id;
            manifest["format"] = transient->second.format;
            manifest["state"] = "published";
            manifest["storage"] = "transient";
        } else {
            manifest = store.publish(lease_iterator->second, metadata);
        }
        data_proxy_leases_.erase(lease_iterator);
        return data_proxy::response(request, manifest).to_json();
    }
    if (method == "abort_result") {
        if (request.lease_id.empty()) throw std::invalid_argument("abort_result requires lease_id");
        const auto lease_iterator = data_proxy_leases_.find(lease_key);
        if (lease_iterator == data_proxy_leases_.end()) throw std::invalid_argument("result lease not found");
        if (lease_iterator->second.format.rfind("transient:", 0) == 0)
            transient_results_.erase(session_id + ":" + lease_iterator->second.result_id);
        else
            store.abort(lease_iterator->second);
        data_proxy_leases_.erase(lease_iterator);
        return data_proxy::response(request, {{"lease_id", request.lease_id}, {"state", "aborted"}}).to_json();
    }
    const auto table = request.payload.value("table", std::string{});
    if (table.empty()) throw std::invalid_argument("data-proxy request requires table");
    if (method == "read_batch" && table.rfind("result:", 0) == 0) {
        const auto result_id = table.substr(std::string("result:").size());
        if (result_id.empty()) throw std::invalid_argument("result reference is empty");
        const auto offset = request.payload.value("offset", std::uintmax_t{0});
        const auto limit = std::min<std::uintmax_t>(request.payload.value("limit", std::uintmax_t{4096}), 65536);
        const auto transient = transient_results_.find(session_id + ":" + result_id);
        if (transient != transient_results_.end() && transient->second.published) {
            auto result = Json{{"rows", detail::transient_arrow_rows(transient->second, offset, limit)},
                               {"offset", offset}, {"limit", limit}};
            result["result_id"] = result_id;
            return data_proxy::response(request, std::move(result)).to_json();
        }
        const auto descriptor = store.describe_result(result_id);
        auto result = descriptor.value("format", std::string{}) == "arrow-ipc-batches"
            ? store.read_arrow_ipc_batch(result_id, offset, limit)
            : store.read_jsonl_batch(result_id, offset, limit);
        result["result_id"] = result_id;
        return data_proxy::response(request, std::move(result)).to_json();
    }
    if (method == "read_batch_binary" && table.rfind("result:", 0) == 0) {
        const auto result_id = table.substr(std::string("result:").size());
        if (result_id.empty()) throw std::invalid_argument("result reference is empty");
        const auto offset = request.payload.value("offset", std::uintmax_t{0});
        const auto limit = std::min<std::uintmax_t>(request.payload.value("limit", std::uintmax_t{4096}), 16384);
        if (const auto transient = transient_results_.find(session_id + ":" + result_id);
            transient != transient_results_.end() && transient->second.published) {
            const auto rows = detail::transient_arrow_rows(transient->second, offset, limit);
            const auto encoded = data_proxy::arrow_ipc_encode_rows(rows);
            return data_proxy::response(request, Json{
                {"encoding", "arrow-ipc"}, {"media_type", "application/vnd.apache.arrow.stream"},
                {"data", data_proxy::base64_encode(encoded)}, {"offset", offset},
                {"limit", limit}, {"row_count", rows.size()}}).to_json();
        }
        const auto descriptor = store.describe_result(result_id);
        const auto rows = (descriptor.value("format", std::string{}) == "arrow-ipc-batches"
            ? store.read_arrow_ipc_batch(result_id, offset, limit)
            : store.read_jsonl_batch(result_id, offset, limit)).value("rows", Json::array());
        const auto encoded = data_proxy::arrow_ipc_encode_rows(rows);
        return data_proxy::response(request, {
            {"encoding", "arrow-ipc"}, {"media_type", "application/vnd.apache.arrow.stream"},
            {"data", data_proxy::base64_encode(encoded)}, {"offset", offset},
            {"limit", limit}, {"row_count", rows.size()}}).to_json();
    }
    if (method == "count_rows" && table.rfind("result:", 0) == 0) {
        const auto result_id = table.substr(std::string("result:").size());
        if (result_id.empty()) throw std::invalid_argument("result reference is empty");
        if (const auto transient = transient_results_.find(session_id + ":" + result_id);
            transient != transient_results_.end() && transient->second.published) {
            return data_proxy::response(
                request, {{"row_count", transient->second.descriptor.value("row_count", std::uint64_t{0})}})
                .to_json();
        }
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
    if (method == "read_batch_binary") {
        const auto description = iterator->second->query_json("DESCRIBE " + quoted_table);
        std::vector<std::string> available;
        for (const auto &column : description) available.push_back(column.value("column_name", std::string{}));
        const auto requested = request.payload.value("columns", Json::array());
        if (!requested.is_array() || requested.empty())
            throw std::invalid_argument("data-proxy read_batch_binary requires columns");
        if (requested.size() > 128) throw std::invalid_argument("data-proxy read_batch_binary selects too many columns");
        std::string projection;
        for (const auto &value : requested) {
            if (!value.is_string()) throw std::invalid_argument("data-proxy columns must be strings");
            const auto column = value.get<std::string>();
            if (std::find(available.begin(), available.end(), column) == available.end())
                throw std::invalid_argument("data-proxy column not found: " + column);
            if (!projection.empty()) projection += ", ";
            projection += detail::quoted_identifier(column);
        }
        const auto offset = request.payload.value("offset", std::uint64_t{0});
        const auto limit = request.payload.value("limit", std::uint64_t{4096});
        if (limit > 16384) throw std::invalid_argument("data-proxy binary batch limit is too large");
        const auto rows = iterator->second->query_json("SELECT " + projection + " FROM " + quoted_table +
                                                       " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));
        const auto encoded = data_proxy::arrow_ipc_encode_rows(rows);
        return data_proxy::response(request, {
            {"encoding", "arrow-ipc"}, {"media_type", "application/vnd.apache.arrow.stream"},
            {"data", data_proxy::base64_encode(encoded)}, {"offset", offset},
            {"limit", limit}, {"row_count", rows.size()}}).to_json();
    }
    if (method == "read_batch") {
        const auto description = iterator->second->query_json("DESCRIBE " + quoted_table);
        std::vector<std::string> available;
        for (const auto &column : description) available.push_back(column.value("column_name", std::string{}));
        const auto requested = request.payload.value("columns", Json::array());
        if (!requested.is_array() || requested.empty())
            throw std::invalid_argument("data-proxy read_batch requires columns");
        if (requested.size() > 128) throw std::invalid_argument("data-proxy read_batch selects too many columns");
        std::string projection;
        for (const auto &value : requested) {
            if (!value.is_string()) throw std::invalid_argument("data-proxy columns must be strings");
            const auto column = value.get<std::string>();
            if (std::find(available.begin(), available.end(), column) == available.end())
                throw std::invalid_argument("data-proxy column not found: " + column);
            if (!projection.empty()) projection += ", ";
            projection += detail::quoted_identifier(column);
        }
        const auto offset = request.payload.value("offset", std::uint64_t{0});
        const auto limit = request.payload.value("limit", std::uint64_t{4096});
        if (limit > 65536) throw std::invalid_argument("data-proxy batch limit is too large");
        const auto rows = iterator->second->query_json("SELECT " + projection + " FROM " + quoted_table +
                                                       " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));
        return data_proxy::response(request, {{"rows", rows}, {"offset", offset}, {"limit", limit}}).to_json();
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
        if (const auto worker = workflow_workers_.find(session_id); worker != workflow_workers_.end() && worker->second.joinable()) {
            const auto state = workflow_states_.find(session_id);
            if (state != workflow_states_.end() &&
                (state->second == "queued" || state->second == "running" || state->second == "cancelling"))
                throw std::invalid_argument("workflow is already running");
            completed_worker = std::move(worker->second);
            workflow_workers_.erase(worker);
        }
        workflow_states_[session_id] = "queued";
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
            const auto clear_transient_results = [this, session_id] {
                std::lock_guard lock(mutex_);
                for (auto iterator = transient_results_.begin(); iterator != transient_results_.end();) {
                    if (!iterator->second.published && iterator->first.rfind(session_id + ":", 0) == 0)
                        iterator = transient_results_.erase(iterator);
                    else
                        ++iterator;
                }
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
                OperationRunner runner = [this, session_id, project, workflow, execution_mode](const std::string &operation,
                                                                        const Json &parameters,
                                                                        const std::string &instance,
                                                                        const Json &inputs,
                                                                        const std::string &computation_key,
                                                                        bool terminal_output) {
                    WorkerRequest request;
                    request.executable = configuration_path_.parent_path() /
#ifdef _WIN32
                        "streamfind-worker.exe";
#else
                        "streamfind-worker";
#endif
                    request.configuration = configuration_path_;
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
                    request.persist_outputs = execution_mode == "detailed" || terminal_output;
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
                    const auto worker_result = worker->run();
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
                    return worker_result.result;
                };
                const auto result = project->run_worker(session_id, *operations_, std::move(runner));
                clear_transient_results();
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
                clear_transient_results();
                try { set_workflow_state(session_id, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, (error.code() == ErrorCode::Cancelled || cancellation->load()) ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
            } catch (const std::exception &error) {
                clear_transient_results();
                try { set_workflow_state(session_id, cancellation->load() ? "cancelled" : "failed"); } catch (...) {}
                if (operation_event_callback_)
                    operation_event_callback_(session_id, {}, cancellation->load() ? "workflow.cancelled" : "workflow.failed", Json{{"message", error.what()}});
                std::lock_guard lock(mutex_);
                workflow_cancellations_.erase(session_id);
                workflow_progress_[session_id]["error"] = error.what();
            } catch (...) {
                clear_transient_results();
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
    const auto cancellation = operation_cancellations_.find(session_id);
    if (cancellation == operation_cancellations_.end())
        throw std::invalid_argument("operation is not running");
    (void)operation_instance;
    cancellation->second->store(true);
    if (const auto worker = active_workers_.find(session_id); worker != active_workers_.end())
        worker->second->cancel();
    return "cancelling";
}

Json ProjectRuntimeManager::run_operation(const std::string &session_id,
                                          const std::string &operation_id,
                                          const Json &parameters,
                                          const std::string &operation_instance,
                                          const Json &inputs) {
    const auto effective_instance = operation_instance.empty() ? operation_id : operation_instance;
    Project *project = nullptr;
    const Operation *operation = nullptr;
    Json effective_inputs = inputs;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        const auto iterator = projects_.find(session_id);
        if (iterator == projects_.end()) throw std::invalid_argument("project session not found");
        operation = operations_->find(operation_id);
        if (!operation) throw std::invalid_argument("operation not found: " + operation_id);
        if (workflow_cancellations_.contains(session_id))
            throw std::invalid_argument("workflow is already running");
        if (operation_cancellations_.contains(session_id))
            throw std::invalid_argument("an operation is already running for this project");
        project = iterator->second.get();
    }

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
            if (port.optional) continue;
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

    {
        std::lock_guard lock(mutex_);
        if (workflow_cancellations_.contains(session_id))
            throw std::invalid_argument("workflow is already running");
        if (operation_cancellations_.contains(session_id))
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
    request.service_port = service_port_;
    request.session_id = session_id;
    request.run_id = effective_instance;
    request.operation_id = operation_id;
    request.operation_instance = effective_instance;
    request.operation = operations_->find(operation_id)->definition().id;
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
        const auto worker_result = worker->run();
        if (operation_event_callback_) {
            const auto worker_event = worker_result.cancelled
                ? "worker.cancelled"
                : worker_result.error.empty() && worker_result.exit_code == 0
                    ? "worker.completed"
                    : "worker.failed";
            operation_event_callback_(session_id, worker_result.worker.operation_id,
                                      worker_event, worker_result.worker.to_json());
        }
        std::lock_guard lock(mutex_);
        operation_cancellations_.erase(session_id);
        active_workers_.erase(session_id);
        if (worker_result.cancelled) throw Error(ErrorCode::Cancelled, "Operation cancelled");
        if (!worker_result.error.empty()) throw Error(ErrorCode::MethodExecution, worker_result.error);
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
