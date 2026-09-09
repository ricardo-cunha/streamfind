#pragma once

#include <string_view>

#include "streamfind/project.hpp"

namespace streamfind::api {

/** @brief Project operations exposed to command-line and language adapters. */
enum class ProjectCommand {
    create,
    describe,
    get_workflow,
    get_workflow_execution,
    create_workflow_execution,
    get_execution,
    list_executions,
    transition_execution,
    cancel_execution,
    set_workflow,
    add_method,
    remove_method,
    validate_workflow,
    validate,
    get_domain,
    get_available_methods,
    run_method,
    copy,
    run_workflow,
    get_metadata,
    set_metadata,
    get_cache,
    delete_cache,
    get_cache_size,
    get_audit_trail,
    close,
    tools_status,
    tools_install,
    tools_install_java,
    tools_install_metfrag,
};

/** @brief Convert a command name to a ProjectCommand. */
STREAMFIND_CORE_API ProjectCommand command_from_string(std::string_view name);

/**
 * @brief Execute one JSON request against a Project.
 *
 * Requests use `database_path` to select a project. Commands
 * that modify a workflow additionally require a `workflow` JSON value.
 * Results are JSON objects suitable for direct CLI output.
 *
 * @param command Project operation to execute.
 * @param request JSON request object.
 * @param registry Registry used for workflow validation and execution.
 * @return JSON command result.
 * @throws Error if the request or Project operation is invalid.
 */
STREAMFIND_CORE_API Json run(ProjectCommand command, const Json &request,
                             const MethodRegistry &registry = methods());

}