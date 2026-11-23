/**
 * @file api.cpp
 * @brief JSON command facade over the streamfind Project API.
 */

#include "streamfind/api.hpp"
#include "streamfind/service_runtime_client.hpp"
#include "streamfind/project_layout.hpp"


#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace streamfind::api {
namespace detail {

ProjectOptions options_from_request(const Json &request) {
    if (!request.is_object() || !request.contains("project_path")) {
        throw Error(ErrorCode::InvalidArgument, "Request requires project_path");
    }
    ProjectOptions options;
    options.database_path = ProjectLayout::from_container(
        path_from_utf8(request.at("project_path").get<std::string>())).database();
    if (request.contains("workflow_metadata")) options.workflow_metadata = request.at("workflow_metadata");
    return options;
}

Json project_json_table(const Json &row) {
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

std::int64_t next_workflow_operation_id(const Json &workflow) {
    std::int64_t maximum = 0;
    for (const auto &operation : workflow.value("operations", Json::array())) {
        const auto id = operation.value("id", Json(nullptr));
        try {
            const auto text = id.is_string() ? id.get<std::string>()
                                             : id.is_number_integer() ? id.dump() : std::string{};
            std::size_t consumed = 0;
            const auto parsed = std::stoll(text, &consumed);
            if (consumed == text.size())
                maximum = std::max(maximum, static_cast<std::int64_t>(parsed));
        } catch (const std::exception &) {}
    }
    if (maximum == std::numeric_limits<std::int64_t>::max())
        throw Error(ErrorCode::InvalidArgument, "workflow has no available integer operation id");
    return maximum + 1;
}

Json metadata_table(const Json &metadata) {
    return {{"row_count", 1}, {"columns", {{"metadata", Json::array({metadata.dump()})}}}};
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
    if (name == "get_project_plugins") return ProjectCommand::get_project_plugins;
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
        const auto attachment = service::create_project(
            options.database_path.parent_path(), request.value("metadata", Json::object()),
            options.workflow_metadata);
        return detail::project_json_table(service::get_project(attachment));
    }
    case ProjectCommand::describe: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return detail::project_json_table(service::get_project(attachment));
    }
    case ProjectCommand::get_workflow: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        return service::get_workflow(attachment);
    }
    case ProjectCommand::validate_workflow: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        const auto workflow = request.contains("workflow") ? request.at("workflow") : Json(nullptr);
        return service::validate_workflow(attachment, workflow);
    }
    case ProjectCommand::validate: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::validate_workflow(attachment);
    }
    case ProjectCommand::get_project_plugins: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        return service::get_project(attachment).value("plugins", Json::array());
    }

    case ProjectCommand::copy: {
        if (!request.contains("destination_project_path")) {
            throw Error(ErrorCode::InvalidArgument, "Request requires destination_project_path");
        }
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::copy_project(attachment,
            path_from_utf8(request.at("destination_project_path").get<std::string>()));
    }
    case ProjectCommand::set_workflow: {
        if (!request.contains("workflow")) throw Error(ErrorCode::InvalidArgument, "Request requires workflow");
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        return service::save_workflow(attachment, request.at("workflow"));
    }
    case ProjectCommand::get_workflow_execution:
        return service::workflow_snapshot(service::attach_project(
            detail::options_from_request(request).database_path.parent_path()));
    case ProjectCommand::create_workflow_execution: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::start_workflow(attachment, request.value("execution_mode", std::string{"detailed"}));
    }
    case ProjectCommand::get_execution: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::workflow_snapshot(attachment);
    }
    case ProjectCommand::list_executions: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::workflow_snapshot(attachment);
    }
    case ProjectCommand::transition_execution: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        const auto status = request.at("status").get<std::string>();
        if (status == "running")
            return service::start_workflow(attachment, request.value("execution_mode", std::string{"detailed"}));
        if (status == "cancelling" || status == "cancelled")
            return service::cancel_workflow(attachment);
        throw Error(ErrorCode::InvalidArgument,
                    "transition_execution supports running, cancelling, and cancelled through the service runtime");
    }
    case ProjectCommand::cancel_execution: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::cancel_workflow(attachment);
    }

    case ProjectCommand::add_operation: {
        if (!request.contains("operation"))
            throw Error(ErrorCode::InvalidArgument, "Request requires operation");
        if (request.contains("operation_id"))
            throw Error(ErrorCode::InvalidArgument, "operation_id is assigned automatically; omit it");
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        const auto workflow = service::get_workflow(attachment);
        if (request.contains("operation_id"))
            throw Error(ErrorCode::InvalidArgument, "operation_id is assigned automatically; omit it");
        auto updated = workflow;
        const auto assigned_id = detail::next_workflow_operation_id(updated);
        updated["operations"].push_back({{"id", assigned_id},
            {"operation", request.at("operation")},
            {"parameters", request.value("parameters", Json::object())},
            {"inputs", request.value("inputs", Json::object())},
            {"position", request.value("position", Json::object())}});
        auto saved = service::save_workflow(attachment, updated);
        saved["assigned_operation_id"] = assigned_id;
        return saved;
    }
    case ProjectCommand::connect_operations: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        auto workflow = service::get_workflow(attachment);
        workflow["connections"].push_back({
            {"source_operation", request.at("source_operation")},
            {"source_port", request.at("source_port")},
            {"target_operation", request.at("target_operation")},
            {"target_port", request.at("target_port")}});
        return service::save_workflow(attachment, workflow);
    }
    case ProjectCommand::get_result_inventory: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        auto inventory = service::get_results(attachment);
        for (auto &artifact : inventory) artifact.erase("payload");
        return inventory;
    }
    case ProjectCommand::get_current_result_inventory: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        auto inventory = service::get_current_results(attachment);
        for (auto &artifact : inventory) artifact.erase("payload");
        return inventory;
    }
    case ProjectCommand::request_result: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        const auto inventory = service::get_results(attachment);
        Json result = Json::array();
        for (const auto &artifact : inventory) {
            if (request.contains("result_id") && artifact.value("result_id", "") != request.at("result_id").get<std::string>()) continue;
            if (request.contains("operation") && artifact.value("producer_operation", "") != request.at("operation").get<std::string>()) continue;
            if (request.contains("operation_instance") && artifact.value("producer_instance", "") != request.at("operation_instance").get<std::string>()) continue;
            if (request.contains("output_port") && artifact.value("contract_id", "") != request.at("output_port").get<std::string>()) continue;
            if (request.contains("workflow_revision")) {
                if (!request.at("workflow_revision").is_number_integer())
                    throw Error(ErrorCode::InvalidArgument, "workflow_revision must be an integer");
                if (!artifact.contains("workflow_revision") ||
                    artifact.at("workflow_revision") != request.at("workflow_revision")) continue;
            }
            auto descriptor = artifact;
            descriptor.erase("payload");
            result.push_back(std::move(descriptor));
        }
        if (!request.value("include_data", false)) return result;
        if (result.size() != 1)
            throw Error(ErrorCode::InvalidArgument,
                        "include_data requires exactly one result selector; provide result_id, operation_instance, or output_port");
        auto artifact = result.front();
        if (artifact.value("representation", "") == "table") {
            Json query{{"result_id", artifact.value("result_id", "")},
                       {"mode", "page"},
                       {"limit", std::clamp(request.value("limit", 1000), 1, 10000)},
                       {"offset", std::max(request.value("offset", 0), 0)}};
            if (request.contains("columns")) query["columns"] = request.at("columns");
            if (request.contains("filters")) query["filters"] = request.at("filters");
            const auto data = service::get_result_data(attachment, query);
            artifact["data"] = data.value("rows", Json::array());
            artifact["columns"] = data.value("columns", Json::array());
            artifact["row_count"] = data.value("total_rows", 0);
            artifact["limit"] = data.value("limit", request.value("limit", 1000));
            artifact["offset"] = data.value("offset", request.value("offset", 0));
            artifact["returned_rows"] = data.value("returned_rows", artifact["data"].size());
            artifact["has_more"] = data.value("has_more", false);
        } else if (artifact.value("representation", "") == "json") {
            if (artifact.value("status", "") != "published")
                throw Error(ErrorCode::MethodExecution,
                            artifact.value("availability_error", "Result is unavailable"));
            const auto data = service::get_result_data(
                attachment, Json{{"result_id", artifact.value("result_id", std::string{})}});
            if (!data.is_object() || !data.contains("payload"))
                throw Error(ErrorCode::MethodExecution,
                            "The selected JSON result did not return a payload.");
            artifact["data"] = data.at("payload");
        } else {
            throw Error(ErrorCode::InvalidArgument,
                        "include_data is supported only for JSON and Arrow table results");
        }
        artifact.erase("payload");
        return Json::array({std::move(artifact)});
    }
    case ProjectCommand::resolve_operation_inputs: {
        if (!request.contains("operation_id"))
            throw Error(ErrorCode::InvalidArgument, "Request requires operation_id");
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        return service::resolve_workflow_inputs(attachment, detail::workflow_operation_id(request.at("operation_id")));
    }

    case ProjectCommand::run_workflow: {
        const auto project_path = detail::options_from_request(request).database_path.parent_path();
        const auto attachment = service::attach_project(project_path);
        const auto workflow = service::get_workflow(attachment);
        return {{"result", service::start_workflow(attachment)}, {"workflow", workflow}};
    }
    case ProjectCommand::get_metadata: {
        const auto attachment = service::attach_project(detail::options_from_request(request).database_path.parent_path());
        return detail::metadata_table(service::get_project(attachment).value("metadata", Json::object()));
    }
    case ProjectCommand::set_metadata: {
        if (!request.contains("metadata")) throw Error(ErrorCode::InvalidArgument, "Request requires metadata");
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return detail::metadata_table(service::set_metadata(attachment, request.at("metadata"))
                                          .value("metadata", Json::object()));
    }

    case ProjectCommand::get_audit_trail: {
        const auto attachment = service::attach_project(
            detail::options_from_request(request).database_path.parent_path());
        return service::get_audit_trail(attachment);
    }
    case ProjectCommand::close: {
            const auto attachment = service::attach_project(
                detail::options_from_request(request).database_path.parent_path());
            return service::close_project(attachment);
        }
        }
    throw Error(ErrorCode::InvalidArgument, "Unsupported Project command");
}

} // namespace streamfind::api
