/**
 * @file api.cpp
 * @brief JSON command facade over the streamfind Project API.
 */

#include "streamfind/api.hpp"


#include <algorithm>
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
    options.domain = request.value("domain", "");
    return options;
}

Json descriptor(const Project &project, const MethodRegistry &registry) {
    const auto &info = project.info();
    return {
        {"domain", info.domain},
        {"metadata", info.metadata.dump()},
        {"schema_version", info.schema_version},
        {"framework_version", info.framework_version},
        {"created_at", info.created_at},
        {"workflow", project.get_workflow().to_json(registry)}
    };
}

Json descriptor_table(const Project &project, const MethodRegistry &registry) {
    const auto row = descriptor(project, registry);
    Json columns = Json::object();
    for (auto it = row.begin(); it != row.end(); ++it) columns[it.key()] = Json::array({it.value()});
    return {{"row_count", 1}, {"columns", std::move(columns)}};
}

Json workflow_execution_table(const Json &rows) {
    const std::vector<std::string> names = {
        "workflow_revision", "step_index", "method", "parameter_hash",
        "status", "started_at", "completed_at", "error", "cache_key"};
    Json columns = Json::object();
    for (const auto &name : names) columns[name] = Json::array();
    for (const auto &row : rows) for (const auto &name : names) columns[name].push_back(row.value(name, Json(nullptr)));
    return {{"row_count", rows.size()}, {"columns", std::move(columns)}};
}

Json metadata_table(const Json &metadata) {
    return {{"row_count", 1}, {"columns", {{"metadata", Json::array({metadata.dump()})}}}};
}

Json workflow_table(const Workflow &workflow, const MethodRegistry &registry) {
    return workflow.to_json(registry);
}

Json cache_entries(const Project &project) {
    Json output = Json::array();
    for (const auto &entry : project.get_cache()) {
        output.push_back({{"name", entry.name}, {"description", entry.description},
                          {"hash", entry.hash}, {"created_at", entry.created_at},
                          {"size", entry.data.size()}});
    }
    return output;
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

Json method_entries(const MethodRegistry &registry, const std::string &domain) {
    Json output = Json::array();
    for (const auto &definition : registry.list(domain)) {
        if (const auto *method = registry.find(definition.id)) output.push_back(method->to_json());
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
    if (name == "add_method") return ProjectCommand::add_method;
    if (name == "remove_method") return ProjectCommand::remove_method;
    if (name == "validate_workflow") return ProjectCommand::validate_workflow;
    if (name == "validate") return ProjectCommand::validate;
    if (name == "get_domain") return ProjectCommand::get_domain;
    if (name == "get_available_methods") return ProjectCommand::get_available_methods;
    if (name == "run_method") return ProjectCommand::run_method;
    if (name == "copy") return ProjectCommand::copy;
    if (name == "run_workflow") return ProjectCommand::run_workflow;
    if (name == "get_metadata") return ProjectCommand::get_metadata;
    if (name == "set_metadata") return ProjectCommand::set_metadata;
    if (name == "get_cache") return ProjectCommand::get_cache;
    if (name == "delete_cache") return ProjectCommand::delete_cache;
    if (name == "get_cache_size") return ProjectCommand::get_cache_size;
    if (name == "get_audit_trail") return ProjectCommand::get_audit_trail;
        if (name == "close") return ProjectCommand::close;

    throw Error(ErrorCode::InvalidArgument, "Unknown Project command: " + std::string(name));
}

Json run(ProjectCommand command, const Json &request, const MethodRegistry &registry) {
    switch (command) {
    case ProjectCommand::create: {
        auto options = detail::options_from_request(request);
        auto project = Project::create(options);
        if (request.contains("metadata")) project.set_metadata(request.at("metadata"));
        return detail::descriptor_table(project, registry);
    }
    case ProjectCommand::describe:
        return detail::descriptor_table(Project::open(detail::options_from_request(request, true)), registry);
    case ProjectCommand::get_workflow: {
        auto project = Project::open(detail::options_from_request(request, true));
        return detail::workflow_table(project.get_workflow(), registry);
    }
    case ProjectCommand::validate_workflow: {
        if (!request.contains("workflow")) throw Error(ErrorCode::InvalidArgument, "Request requires workflow");
        const auto workflow = Workflow::from_json(request.at("workflow"));
        workflow.validate(registry);
        return {{"valid", true}, {"info", "Workflow validation finished successfully."}};
    }
    case ProjectCommand::validate: {
        auto project = Project::open(detail::options_from_request(request, true));
        project.validate();
        return {{"valid", true}, {"info", "Project validation finished successfully."}};
    }
    case ProjectCommand::get_domain:
        return Project::open(detail::options_from_request(request, true)).get_domain();
    case ProjectCommand::get_available_methods:
        return detail::method_entries(registry, request.value("domain", ""));
    case ProjectCommand::run_method: {
        if (!request.contains("method")) throw Error(ErrorCode::InvalidArgument, "Request requires method");
        auto project = Project::open(detail::options_from_request(request));
        return project.run_method(request.at("method").get<std::string>(), request.value("parameters", Json::object()), registry);
    }
    case ProjectCommand::copy: {
        if (!request.contains("destination_database_path")) {
            throw Error(ErrorCode::InvalidArgument, "Request requires destination_database_path");
        }
        auto source = Project::open(detail::options_from_request(request, true));
        ProjectOptions destination_options;
        destination_options.database_path = request.at("destination_database_path").get<std::string>();
        auto destination = source.copy(destination_options);
        return detail::descriptor_table(destination, registry);
    }
    case ProjectCommand::set_workflow: {
        if (!request.contains("workflow")) throw Error(ErrorCode::InvalidArgument, "Request requires workflow");
        auto project = Project::open(detail::options_from_request(request));
        auto workflow = Workflow::from_json(request.at("workflow"));
        workflow.validate(registry);
        project.set_workflow(std::move(workflow), registry);
        return detail::workflow_table(project.get_workflow(), registry);
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
    case ProjectCommand::add_method: {
        if (!request.contains("method")) throw Error(ErrorCode::InvalidArgument, "Request requires method");
        auto project = Project::open(detail::options_from_request(request));
        auto workflow = project.get_workflow();
        workflow.steps.push_back({request.at("method").get<std::string>(), request.value("parameters", Json::object())});
        project.set_workflow(std::move(workflow), registry);
        return detail::workflow_table(project.get_workflow(), registry);
    }
    case ProjectCommand::remove_method: {
        if (!request.contains("method")) throw Error(ErrorCode::InvalidArgument, "Request requires method");
        auto project = Project::open(detail::options_from_request(request));
        auto workflow = project.get_workflow();
        const auto method = request.at("method").get<std::string>();
        const auto step = std::find_if(workflow.steps.begin(), workflow.steps.end(),
                                       [&method](const auto &candidate) { return candidate.method == method; });
        if (step == workflow.steps.end()) throw Error(ErrorCode::InvalidArgument, "Method is not in workflow: " + method);
        workflow.steps.erase(step);
        project.set_workflow(std::move(workflow), registry);
        return detail::workflow_table(project.get_workflow(), registry);
    }
    case ProjectCommand::run_workflow: {
        auto project = Project::open(detail::options_from_request(request));
        const Json result = request.contains("worker_id")
            ? project.run_worker(request.at("worker_id").get<std::string>(), registry)
            : project.run_workflow(registry).to_json();
        return {{"result", result}, {"workflow", detail::workflow_table(project.get_workflow(), registry)}};
    }
    case ProjectCommand::get_metadata:
        return detail::metadata_table(Project::open(detail::options_from_request(request, true)).get_metadata());
    case ProjectCommand::set_metadata: {
        if (!request.contains("metadata")) throw Error(ErrorCode::InvalidArgument, "Request requires metadata");
        auto project = Project::open(detail::options_from_request(request));
        project.set_metadata(request.at("metadata"));
        return detail::metadata_table(project.get_metadata());
    }
    case ProjectCommand::get_cache:
        return detail::cache_entries(Project::open(detail::options_from_request(request, true)));
    case ProjectCommand::delete_cache: {
        auto project = Project::open(detail::options_from_request(request));
        project.delete_cache();
        return Json{{"status", "finished"}, {"info", "Cache deleted successfully."}};
    }
    case ProjectCommand::get_cache_size:
        return Project::open(detail::options_from_request(request, true)).get_cache_size();
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
