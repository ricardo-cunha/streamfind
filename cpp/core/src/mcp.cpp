#include "streamfind/mcp.hpp"
#include "streamfind/api.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/version.hpp"
#include <algorithm>
#include <array>

namespace streamfind::mcp {

const OperationRegistry &operations() {
    static const OperationRegistry registry;
    return registry;
}

namespace detail {
Json tool(const char *name, const char *description, Json properties, Json required) {
    return {{"name", name}, {"description", description},
            {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}}};
}

Json tools() {
    // Catalogue-backed tool definitions; on a catalogue miss degrade to a
    // minimal toolset (the registry-derived tools are appended by tools/list).
    const auto catalogue = streamfind::catalogue::tools_json();
    return catalogue ? *catalogue : Json::array();
}

const char *command(const std::string &name) {
    static const std::array<std::string, 35> commands = {
        "create", "describe", "validate", "get_domain", "get_metadata",
        "set_metadata", "get_workflow", "get_workflow_execution", "create_workflow_execution", "get_execution", "list_executions", "transition_execution", "cancel_execution", "set_workflow", "add_method", "remove_method", "validate_workflow",
        "run_workflow", "get_cache", "get_cache_size", "delete_cache",
        "get_audit_trail", "get_available_methods", "run_method", "copy", "close",
        "add_operation", "connect_operations", "get_artifact_inventory",
        "request_artifact", "resolve_operation_inputs",

    };
    return std::find(commands.begin(), commands.end(), name) == commands.end() ? nullptr : name.c_str();
}

std::string interface_guidance() {
    return
        "StreamFind is a workflow-centric data-processing framework. A project is a "
        "DuckDB-backed workspace with a domain, ontology-defined operations, immutable "
        "artifacts, workflow revisions, and execution history. Build processing workflows "
        "as an acyclic graph of operation instances: each operation has a unique instance "
        "id, parameters, typed input/output ports, and an operationSuccessSignal. Connect "
        "output ports to input ports or compatible parameters explicitly with "
        "source_operation, source_port, target_operation, and target_port. Use the "
        "target_port form parameter:<parameter_name> when a preceding result should "
        "supply a dynamic parameter value; this is useful for selectors, branching, "
        "loops, thresholds, and other generic control/dataflow operations. The runtime "
        "merges that artifact payload into the target operation parameters and validates "
        "the resulting parameter object. Do not infer dependencies from operation order "
        "or create next-operation edges.\n\n"
        "Recommended agent workflow: call tools/list to discover callable MCP tools and "
        "schemas; create or describe a project; inspect domain operations and their ontology "
        "guidance; use add_operation and connect_operations (or set_workflow for an atomic "
        "graph replacement); call validate_workflow; call get_workflow to confirm the saved "
        "graph; call run_workflow; then inspect the returned operation results, get_artifact_inventory, "
        "request_artifact, and resolve_operation_inputs. Table outputs are immutable published "
        "artifacts identified by workflow revision, producer operation instance, and output "
        "contract. Use the artifact inventory rather than execution text as proof that an "
        "output is available to downstream operations. Stateless domain operations require "
        "database_path. connect is only needed for legacy workflow methods and session context.";
}

std::string tool_description(const Json &entry, const std::string &fallback) {
    std::string description = entry.value("label", fallback) + ": " + entry.value("definition", fallback);
    const auto guidance = entry.at("interface").value("guidance", "");
    if (!guidance.empty()) description += " Guidance: " + guidance;
    const auto model = entry.at("interface").value("invocation_model", "");
    if (!model.empty()) description += " Invocation model: " + model + ".";
    return description;
}
}

Session::Session(const MethodRegistry &registry, const OperationRegistry &operations) : registry_(registry), operations_(operations) {}

Json Session::handle(const Json &request) {
    const auto id = request.value("id", Json(nullptr));
    const auto method = request.value("method", "");
    if (method == "initialize") return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", "2025-03-26"}, {"capabilities", {{"tools", Json::object()}}}, {"serverInfo", {{"name", "streamfind-cpp"}, {"version", std::string(streamfind::version())}}}, {"instructions", detail::interface_guidance()}}}};
    if (method == "tools/list") {
            auto catalogue = detail::tools();
            // Methods (kind='method') are NEVER tools: they are referenced by the
            // workflow operations and discovered via get_available_methods.
            // All exposed domain operations are always advertised. They are
            // stateless and carry database_path, so discovery and
            // invocation do not depend on connect.
            const auto entries = streamfind::catalogue::entries_json();
            for (const auto &definition : operations_.list("")) {
                const Json *entry = nullptr;
                if (entries) {
                    for (const auto &candidate : *entries) {
                        if (candidate.value("canonical_id", "") == definition.id) {
                            entry = &candidate;
                            break;
                        }
                    }
                }
                if (entry) {
                    const auto mcp = entry->value("mcp", Json::object());
                    const auto input_schema = mcp.value(
                        "input_schema",
                        Json{{"type", "object"}, {"properties", Json::object()}, {"required", Json::array()}});
                    catalogue.push_back(Json{
                        {"name", entry->value("canonical_id", definition.id)},
                        {"description", detail::tool_description(*entry, definition.description)},
                        {"inputSchema", input_schema},
                        {"annotations", {{"title", entry->value("label", definition.name)},
                                          {"readOnlyHint", !entry->at("effects").value("mutates_project", false)},
                                          {"destructiveHint", entry->at("effects").value("mutates_project", false)}}},
                        {"_meta", {{"streamfind", entry->at("interface")}}},
                        {"effects", entry->value("effects", Json::array())},
                    });
                } else {
                    // Keep the registered-operation intersection guard: a registry
                    // entry without a catalogue entry is not advertised.
                    continue;
                }
            }
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", catalogue}}}};
        }
    if (method != "tools/call") return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Unsupported MCP method"}}}};
    const auto name = request.at("params").value("name", "");
    if (name == "connect") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto domain = api::run(api::ProjectCommand::get_domain, arguments, registry_).get<std::string>();
            domain_ = domain;
                        project_ = arguments;
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", Json{{{"status", "finished"}, {"info", "Project connected successfully."}}}.dump()}}})}}}};
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    const auto command = detail::command(name);
    const auto dynamic = registry_.find(name);
    const auto operation = operations_.find(name);
    if (!command && operation) {
        const auto arguments = request.at("params").value("arguments", Json::object());
        try {
            if (!arguments.contains("database_path")) {
                throw Error(ErrorCode::InvalidArgument, "Domain operations require database_path");
            }
            ProjectOptions options;
            options.database_path = arguments.at("database_path").get<std::string>();
            options.domain = operation->definition().domain;
            auto project = Project::open(options);
            Json parameters = arguments;
            parameters.erase("database_path");
            parameters.erase("domain");
            const Json result = project.run_operation(name, parameters, operations_);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
        } catch (const Error &error) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}}; }
    }
    if (!command && dynamic && !domain_.empty() && dynamic->definition().domain == domain_) {
        if (project_.empty()) return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32000}, {"message", "No project connected"}}}};
        Json arguments = project_;
        arguments["method"] = name;
        arguments["parameters"] = request.at("params").value("arguments", Json::object());
        try {
            const Json result = api::run(api::ProjectCommand::run_method, arguments, registry_);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "validate_workflow" || name == "set_workflow") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            if (!arguments.contains("database_path") || !arguments.contains("workflow"))
                throw Error(ErrorCode::InvalidArgument, "Workflow validation requires database_path and workflow");
            ProjectOptions options;
            options.database_path = arguments.at("database_path").get<std::string>();
            auto project = Project::open(options);
            auto workflow = Workflow::from_json(arguments.at("workflow"));
            workflow.domain = workflow.domain.empty() ? project.get_domain() : workflow.domain;
            workflow.validate(operations_);
            if (name == "set_workflow") project.set_workflow(workflow, operations_);
            const Json result = {{"valid", true}, {"workflow", workflow.to_json()}};
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "run_workflow") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            if (!arguments.contains("database_path"))
                throw Error(ErrorCode::InvalidArgument, "Workflow execution requires database_path");
            ProjectOptions options;
            options.database_path = arguments.at("database_path").get<std::string>();
            auto project = Project::open(options);
            const auto workflow = project.get_workflow();
            if (!workflow.operations.empty() || !workflow.connections.empty()) {
                const Json result = project.run_operation_graph(operations_);
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
            }
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (!command) return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "Unknown MCP tool"}}}};
    try {
        Json result = api::run(api::command_from_string(command), request.at("params").value("arguments", Json::object()), registry_);
        if (name == "get_available_methods" && result.is_array()) {
            if (const auto entries = streamfind::catalogue::entries_json()) {
                for (auto &method : result) {
                    for (const auto &entry : *entries) {
                        if (entry.value("canonical_id", "") == method.value("id", "")) {
                            method["inputSchema"] = entry.value("method_schema", Json::object());
                            method["interface"] = entry.value("interface", Json::object());
                            break;
                        }
                    }
                }
            }
        }
        if (name == "close") {
                            project_ = Json::object();
                            domain_.clear();
                        }
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
    } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }

    Json handle(const Json &request, const MethodRegistry &registry) {
        return Session(registry).handle(request);
    }
    }
