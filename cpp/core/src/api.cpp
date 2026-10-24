/**
 * @file api.cpp
 * @brief JSON command facade over the streamfind Project API.
 */

#include "streamfind/api.hpp"
#include "streamfind/project_table_store.hpp"
#include "streamfind/service/worker_supervisor.hpp"
#include "streamfind/service_runtime_client.hpp"


#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace streamfind::api {
namespace detail {

ProjectOptions options_from_request(const Json &request, bool read_only = false) {
    if (!request.is_object() || !request.contains("database_path")) {
        throw Error(ErrorCode::InvalidArgument, "Request requires database_path");
    }
    ProjectOptions options;
    options.database_path = request.at("database_path").get<std::string>();
    if (request.contains("workflow_metadata")) options.workflow_metadata = request.at("workflow_metadata");
    return options;
}

Json descriptor(const Project &project) {
    const auto &info = project.info();
    return {
        {"domains", info.domains},
        {"metadata", info.metadata.dump()},
        {"schema_version", info.schema_version},
        {"framework_version", info.framework_version},
        {"created_at", info.created_at},
        {"workflow", project.get_workflow().to_json()}
    };
}

Json descriptor_table(const Project &project) {
    const auto row = descriptor(project);
    Json columns = Json::object();
    for (auto it = row.begin(); it != row.end(); ++it) columns[it.key()] = Json::array({it.value()});
    return {{"row_count", 1}, {"columns", std::move(columns)}};
}

std::string workflow_operation_id(const Json &value) {
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::size_t consumed = 0;
        try {
            const auto number = std::stoll(text, &consumed);
            if (consumed == text.size()) return std::to_string(number);
        } catch (const std::exception &) {
        }
    }
    throw Error(ErrorCode::InvalidArgument, "Workflow operation id must be an integer");
}

Json workflow_execution_table(const Json &rows) {
    const std::vector<std::string> names = {
        "workflow_revision", "step_index", "operation", "parameter_hash",
        "status", "started_at", "completed_at", "error", "cache_key"};
    Json columns = Json::object();
    for (const auto &name : names) columns[name] = Json::array();
    for (const auto &row : rows) for (const auto &name : names) columns[name].push_back(row.value(name, Json(nullptr)));
    return {{"row_count", rows.size()}, {"columns", std::move(columns)}};
}

Json metadata_table(const Json &metadata) {
    return {{"row_count", 1}, {"columns", {{"metadata", Json::array({metadata.dump()})}}}};
}

Json workflow_table(const Workflow &workflow) {
    return workflow.to_json();
}


Json audit_entries(const Project &project) {
    Json output = Json::array();
    for (const auto &entry : project.get_audit_trail()) {
        output.push_back({{"operation_type", entry.operation_type},
                          {"object_type", entry.object_type},
                          {"details", entry.details}, {"created_at", entry.created_at}});
    }
    return output;
}

} // namespace detail

ProjectCommand command_from_string(std::string_view name) {
    if (name == "create") return ProjectCommand::create;
    if (name == "describe") return ProjectCommand::describe;
    if (name == "get_workflow") return ProjectCommand::get_workflow;
    if (name == "get_workflow_execution") return ProjectCommand::get_workflow_execution;
    if (name == "create_workflow_execution") return ProjectCommand::create_workflow_execution;
    if (name == "get_execution") return ProjectCommand::get_execution;
    if (name == "list_executions") return ProjectCommand::list_executions;
    if (name == "transition_execution") return ProjectCommand::transition_execution;
    if (name == "cancel_execution") return ProjectCommand::cancel_execution;
    if (name == "set_workflow") return ProjectCommand::set_workflow;
    if (name == "validate_workflow") return ProjectCommand::validate_workflow;
    if (name == "validate") return ProjectCommand::validate;
    if (name == "get_project_domains") return ProjectCommand::get_project_domains;
    if (name == "copy") return ProjectCommand::copy;
    if (name == "run_workflow") return ProjectCommand::run_workflow;
    if (name == "get_metadata") return ProjectCommand::get_metadata;
    if (name == "set_metadata") return ProjectCommand::set_metadata;

    if (name == "get_audit_trail") return ProjectCommand::get_audit_trail;
    if (name == "close") return ProjectCommand::close;
    if (name == "add_operation") return ProjectCommand::add_operation;
    if (name == "connect_operations") return ProjectCommand::connect_operations;
    if (name == "get_result_inventory") return ProjectCommand::get_result_inventory;
    if (name == "get_current_result_inventory") return ProjectCommand::get_current_result_inventory;
    if (name == "request_result") return ProjectCommand::request_result;
    if (name == "resolve_operation_inputs") return ProjectCommand::resolve_operation_inputs;

    throw Error(ErrorCode::InvalidArgument, "Unknown Project command: " + std::string(name));
}

Json run(ProjectCommand command, const Json &request, const OperationRegistry &registry) {
    switch (command) {
    case ProjectCommand::create: {
        auto options = detail::options_from_request(request);
        auto project = Project::create(options);
        if (request.contains("metadata")) project.set_metadata(request.at("metadata"));
        return detail::descriptor_table(project);
    }
    case ProjectCommand::describe:
        return detail::descriptor_table(Project::open(detail::options_from_request(request, true)));
    case ProjectCommand::get_workflow: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        return service::get_workflow(attachment);
    }
    case ProjectCommand::validate_workflow: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        const auto workflow = request.contains("workflow") ? request.at("workflow") : Json(nullptr);
        return service::validate_workflow(attachment, workflow);
    }
    case ProjectCommand::validate: {
        auto project = Project::open(detail::options_from_request(request, true));
        project.validate();
        return {{"valid", true}, {"info", "Project validation finished successfully."}};
    }
    case ProjectCommand::get_project_domains: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        return service::get_project(attachment).value("domains", Json::array());
    }

    case ProjectCommand::copy: {
        if (!request.contains("destination_database_path")) {
            throw Error(ErrorCode::InvalidArgument, "Request requires destination_database_path");
        }
        auto source = Project::open(detail::options_from_request(request, true));
        ProjectOptions destination_options;
        destination_options.database_path = request.at("destination_database_path").get<std::string>();
        auto destination = source.copy(destination_options);
        return detail::descriptor_table(destination);
    }
    case ProjectCommand::set_workflow: {
        if (!request.contains("workflow")) throw Error(ErrorCode::InvalidArgument, "Request requires workflow");
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path);
        return service::save_workflow(attachment, request.at("workflow"));
    }
    case ProjectCommand::get_workflow_execution:
        return detail::workflow_execution_table(Project::open(detail::options_from_request(request, true)).get_workflow_execution());
    case ProjectCommand::create_workflow_execution: {
        auto project = Project::open(detail::options_from_request(request));
        return WorkflowExecutionManager(project).create(request);
    }
    case ProjectCommand::get_execution: {
        auto project = Project::open(detail::options_from_request(request, true));
        return WorkflowExecutionManager(project).current();
    }
    case ProjectCommand::list_executions: {
        auto project = Project::open(detail::options_from_request(request, true));
        return WorkflowExecutionManager(project).list();
    }
    case ProjectCommand::transition_execution: {
        auto project = Project::open(detail::options_from_request(request));
        const auto status = request.at("status").get<std::string>();
        ExecutionState state;
        if (status == "queued") state = ExecutionState::queued;
        else if (status == "running") state = ExecutionState::running;
        else if (status == "cancelling") state = ExecutionState::cancelling;
        else if (status == "cancelled") state = ExecutionState::cancelled;
        else if (status == "completed") state = ExecutionState::completed;
        else if (status == "failed") state = ExecutionState::failed;
        else if (status == "interrupted") state = ExecutionState::interrupted;
        else throw Error(ErrorCode::InvalidArgument, "unknown execution state");
        return WorkflowExecutionManager(project).transition(state);
    }
    case ProjectCommand::cancel_execution: {
        auto project = Project::open(detail::options_from_request(request));
        return WorkflowExecutionManager(project).cancel();
    }

    case ProjectCommand::add_operation: {
        if (!request.contains("operation_id") || !request.contains("operation"))
            throw Error(ErrorCode::InvalidArgument, "Request requires operation_id and operation");
        auto project = Project::open(detail::options_from_request(request));
        auto workflow = project.get_workflow();
        WorkflowOperation operation;
        operation.id = detail::workflow_operation_id(request.at("operation_id"));
        operation.operation = request.at("operation").get<std::string>();
        operation.parameters = ParameterValues::from_json(request.value("parameters", Json::object()));
        operation.inputs = request.value("inputs", Json::object());
        operation.position = request.value("position", Json::object());
        if (!operation.inputs.is_object())
            throw Error(ErrorCode::InvalidArgument, "Request inputs must be an object");
        if (!operation.position.is_object())
            throw Error(ErrorCode::InvalidArgument, "Request position must be an object");
        for (const auto &existing : workflow.operations)
            if (existing.id == operation.id)
                throw Error(ErrorCode::InvalidArgument, "Workflow operation id already exists: " + operation.id);
        workflow.operations.push_back(std::move(operation));
        // Incremental graph construction may temporarily contain unconnected
        // required inputs. validate_workflow remains the explicit validation gate.
        project.set_workflow(std::move(workflow));
        return {{"updated", true}, {"workflow", project.get_workflow().to_json()}};
    }
    case ProjectCommand::connect_operations: {
        auto project = Project::open(detail::options_from_request(request));
        auto workflow = project.get_workflow();
        workflow.connections.push_back(WorkflowConnection::from_json(request));
        // Connections and nodes are persisted incrementally; validation is
        // intentionally deferred until validate_workflow or run_workflow.
        project.set_workflow(std::move(workflow));
        return {{"updated", true}, {"workflow", project.get_workflow().to_json()}};
    }
    case ProjectCommand::get_result_inventory: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        auto inventory = service::get_results(attachment);
        for (auto &artifact : inventory) artifact.erase("payload");
        return inventory;
    }
    case ProjectCommand::get_current_result_inventory: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        auto inventory = service::get_current_results(attachment);
        for (auto &artifact : inventory) artifact.erase("payload");
        return inventory;
    }
    case ProjectCommand::request_result: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        const auto inventory = service::get_results(attachment);
        Json result = Json::array();
        for (const auto &artifact : inventory) {
            if (request.contains("result_id") && artifact.value("result_id", "") != request.at("result_id").get<std::string>()) continue;
            if (request.contains("operation") && artifact.value("producer_operation", "") != request.at("operation").get<std::string>()) continue;
            if (request.contains("operation_instance") && artifact.value("producer_instance", "") != request.at("operation_instance").get<std::string>()) continue;
            if (request.contains("output_port") && artifact.value("contract_id", "") != request.at("output_port").get<std::string>()) continue;
            if (request.contains("workflow_revision") && artifact.value("workflow_revision", "") !=
                    std::to_string(request.at("workflow_revision").get<int>())) continue;
            auto descriptor = artifact;
            descriptor.erase("payload");
            result.push_back(std::move(descriptor));
        }
        if (!request.value("include_data", false)) return result;
        if (result.size() != 1)
            throw Error(ErrorCode::InvalidArgument,
                        "include_data requires exactly one artifact selector; provide result_id, operation_instance, or output_port");
        auto artifact = result.front();
        if (artifact.value("representation", "") == "table") {
            const auto data = service::get_result_data(
                attachment, Json{{"result_id", artifact.value("result_id", "")},
                                 {"mode", "page"},
                                 {"limit", std::clamp(request.value("limit", 1000), 1, 10000)},
                                 {"offset", std::max(request.value("offset", 0), 0)}});
            Json rows = Json::array();
            const auto columns = data.value("columns", Json::array());
            for (const auto &row : data.value("rows", Json::array())) {
                Json object = Json::object();
                for (std::size_t index = 0; index < columns.size() && index < row.size(); ++index)
                    object[columns.at(index).value("name", std::to_string(index))] = row.at(index);
                rows.push_back(std::move(object));
            }
            artifact["data"] = std::move(rows);
            artifact["row_count"] = data.value("total_rows", 0);
            artifact["limit"] = data.value("limit", request.value("limit", 1000));
            artifact["offset"] = data.value("offset", request.value("offset", 0));
        } else if (artifact.contains("payload") && !artifact.at("payload").is_null()) {
            const auto &payload = artifact.at("payload");
            if (payload.is_string()) {
                const auto parsed = Json::parse(payload.get<std::string>(), nullptr, false);
                artifact["data"] = parsed.is_discarded() ? Json(payload.get<std::string>()) : parsed;
            } else {
                artifact["data"] = payload;
            }
        }
        artifact.erase("payload");
        return Json::array({std::move(artifact)});
    }
    case ProjectCommand::resolve_operation_inputs: {
        if (!request.contains("operation_id"))
            throw Error(ErrorCode::InvalidArgument, "Request requires operation_id");
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        return service::resolve_workflow_inputs(attachment, detail::workflow_operation_id(request.at("operation_id")));
    }

    case ProjectCommand::run_workflow: {
        const auto database_path = detail::options_from_request(request).database_path;
        const auto attachment = service::attach_project(database_path);
        const auto workflow = service::get_workflow(attachment);
        return {{"result", service::start_workflow(attachment)}, {"workflow", workflow}};
    }
    case ProjectCommand::get_metadata: {
        const auto attachment = service::attach_project(detail::options_from_request(request, true).database_path);
        return detail::metadata_table(service::get_project(attachment).value("metadata", Json::object()));
    }
    case ProjectCommand::set_metadata: {
        if (!request.contains("metadata")) throw Error(ErrorCode::InvalidArgument, "Request requires metadata");
        auto project = Project::open(detail::options_from_request(request));
        project.set_metadata(request.at("metadata"));
        return detail::metadata_table(project.get_metadata());
    }

    case ProjectCommand::get_audit_trail:
        return detail::audit_entries(Project::open(detail::options_from_request(request, true)));
    case ProjectCommand::close: {
            auto project = Project::open(detail::options_from_request(request, true));
            project.close();
            return Json{{"status", "finished"}, {"info", "Project closed successfully."}};
        }
        }
    throw Error(ErrorCode::InvalidArgument, "Unsupported Project command");
}

} // namespace streamfind::api
