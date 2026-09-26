#include "streamfind/mcp.hpp"
#include "streamfind/api.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/version.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <set>

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

bool is_executable_operation(const Json &entry) {
    return entry.value("kind", "") == "operation" && entry.value("exposed", false) &&
           entry.value("executable", false);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool matches_search(const Json &entry, const std::string &search) {
    if (search.empty()) return true;
    const auto query = lower(search);
    for (const auto &field : {"canonical_id", "label", "definition", "module_id"})
        if (lower(entry.value(field, "")).find(query) != std::string::npos) return true;
    return false;
}

Json operation_summary(const Json &entry) {
    return Json{{"canonical_id", entry.value("canonical_id", "")},
                {"label", entry.value("label", "")},
                {"domain", entry.value("domain", "")},
                {"module_id", entry.value("module_id", "")},
                {"definition", entry.value("definition", "")}};
}

Json tools() {
    // Keep the initial MCP surface stable and small. Domain operations are
    // discovered through the query tools below and invoked through
    // run_operation rather than being registered as one tool per operation.
    const auto catalogue = streamfind::catalogue::tools_json();
    Json result = catalogue ? *catalogue : Json::array();
    result.push_back(tool("get_domains", "List domains that provide exposed operations.", Json::object(), Json::array()));
    result.push_back(tool("get_modules", "List operation modules available in a domain.",
                          Json{{"domain", {{"type", "string"}}}}, Json::array({"domain"})));
    result.push_back(tool("get_operations", "List exposed executable operations for a domain and optional module, with optional search.",
                          Json{{"domain", {{"type", "string"}}}, {"module", {{"type", "string"}}},
                               {"search", {{"type", "string"}}}},
                          Json::array({"domain"})));
    result.push_back(tool("get_operation", "Return the full catalogue definition for one operation.",
                          Json{{"operation", {{"type", "string"}}}}, Json::array({"operation"})));
    result.push_back(tool("run_operation", "Run one canonical domain operation against a project database.",
                          Json{{"operation", {{"type", "string"}}},
                               {"database_path", {{"type", "string"}}},
                               {"arguments", {{"type", "object"}}}},
                          Json::array({"operation", "database_path"})));
    return result;
}

const char *command(const std::string &name) {
    static const std::array<std::string, 35> commands = {
        "create", "describe", "validate", "get_project_domains", "get_metadata",
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
        "DuckDB-backed workspace with derived domains, ontology-defined operations, immutable "
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
        "database_path. connect is only needed for workflow methods that require session context.";
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
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", catalogue}}}};
        }
    if (method != "tools/call") return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Unsupported MCP method"}}}};
    const auto name = request.at("params").value("name", "");
    if (name == "connect") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            ProjectOptions options;
            options.database_path = arguments.at("database_path").get<std::string>();
            Project::open(options);
            project_ = arguments;
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", Json{{{"status", "finished"}, {"info", "Project connected successfully."}}}.dump()}}})}}}};
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "get_domains" || name == "get_modules" || name == "get_operations" || name == "get_operation" || name == "run_operation") {
        const auto arguments = request.at("params").value("arguments", Json::object());
        const auto entries = streamfind::catalogue::entries_json();
        try {
            if (name == "run_operation") {
                const auto operation_id = arguments.at("operation").get<std::string>();
                if (!operations_.find(operation_id)) throw Error(ErrorCode::InvalidArgument, "operation not found: " + operation_id);
                ProjectOptions options;
                options.database_path = arguments.at("database_path").get<std::string>();
                const auto operation_arguments = arguments.value("arguments", Json::object());
                const auto result = Project::open(options).run_operation(operation_id, operation_arguments, operations_);
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
            }
            Json result = Json::array();
            std::set<std::string> values;
            if (entries) for (const auto &entry : *entries) {
                if (!detail::is_executable_operation(entry)) continue;
                const auto domain = entry.value("domain", "");
                if (name != "get_domains" && name != "get_operation" && domain != arguments.value("domain", "")) continue;
                if (name == "get_domains") values.insert(domain);
                else if (name == "get_modules") values.insert(entry.value("module_id", ""));
                else if (name == "get_operations" &&
                         (arguments.value("module", "").empty() || entry.value("module_id", "") == arguments.value("module", "")) &&
                         detail::matches_search(entry, arguments.value("search", "")))
                    result.push_back(detail::operation_summary(entry));
                else if (name == "get_operation" && entry.value("canonical_id", "") == arguments.value("operation", ""))
                    result.push_back(entry);
            }
            if (name == "get_domains" || name == "get_modules")
                for (const auto &value : values) result.push_back(value);
            if (name == "get_operation" && result.empty())
                throw Error(ErrorCode::InvalidArgument,
                            "operation not found: " + arguments.value("operation", ""));
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
        } catch (const std::exception &error) {
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
            auto project = Project::open(options);
            Json parameters = arguments;
            parameters.erase("database_path");
            parameters.erase("domain");
            const Json result = project.run_operation(name, parameters, operations_);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
        } catch (const Error &error) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}}; }
    }
    if (!command && dynamic) {
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
