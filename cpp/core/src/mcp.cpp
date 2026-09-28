#include "streamfind/mcp.hpp"
#include "streamfind/api.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/version.hpp"
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
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

    // Core project commands are catalogue entries, but their shared project
    // scope is enforced by the MCP dispatcher rather than by an ontology
    // operation schema. Keep tools/list truthful for clients that construct
    // calls from the advertised JSON Schema.
    const auto schema = [](Json properties, Json required) {
        return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
    };
    const Json database_path = {{"type", "string"}, {"description", "Path to the project DuckDB database."}};
    const Json project_path_schema = schema(Json{{"database_path", database_path}}, Json::array({"database_path"}));
    const Json workflow_schema = schema(Json{{"database_path", database_path}, {"workflow", {{"type", "object"}}}},
                                        Json::array({"database_path", "workflow"}));
    const Json add_operation_schema = schema(
        Json{{"database_path", database_path}, {"operation_id", {{"type", "string"}}},
             {"operation", {{"type", "string"}}}, {"parameters", {{"type", "object"}}},
             {"inputs", {{"type", "object"}}}, {"position", {{"type", "object"}}}},
        Json::array({"database_path", "operation_id", "operation"}));
    const Json connect_operations_schema = schema(
        Json{{"database_path", database_path}, {"source_operation", {{"type", "string"}}},
             {"source_port", {{"type", "string"}}}, {"target_operation", {{"type", "string"}}},
             {"target_port", {{"type", "string"}}}},
        Json::array({"database_path", "source_operation", "source_port", "target_operation", "target_port"}));
    for (auto &entry : result) {
        const auto name = entry.value("name", "");
        if (name == "create" || name == "describe" || name == "connect" || name == "validate" ||
            name == "get_project_domains" || name == "get_metadata" || name == "set_metadata" ||
            name == "get_workflow" || name == "get_workflow_execution" || name == "create_workflow_execution" ||
            name == "get_execution" || name == "list_executions" || name == "transition_execution" ||
            name == "cancel_execution" ||
            name == "run_workflow" || name == "get_cache" || name == "get_cache_size" ||
            name == "delete_cache" || name == "get_audit_trail" || name == "get_artifact_inventory" ||
            name == "request_artifact" || name == "resolve_operation_inputs")
            entry["inputSchema"] = project_path_schema;
        else if (name == "set_workflow" || name == "validate_workflow") entry["inputSchema"] = workflow_schema;
        else if (name == "add_operation") entry["inputSchema"] = add_operation_schema;
        else if (name == "connect_operations") entry["inputSchema"] = connect_operations_schema;

        if (name == "create") {
            entry["description"] = "Create a new project database. This is the first step of the operation-graph workflow; then call add_operation for each node.";
            entry["_meta"]["streamfind"]["guidance"] = "Call create once with database_path, then add operation nodes with their parameters.";
        } else if (name == "add_operation") {
            entry["description"] = "Add one configured operation node to the persisted workflow graph. Provide operation_id, canonical operation, and parameters in the same call.";
            entry["_meta"]["streamfind"]["guidance"] = "Add every node with its final parameters before connecting typed ports. Use get_operation to inspect the operation schema.";
        } else if (name == "connect_operations") {
            entry["description"] = "Connect one operation output port to another operation input port in the persisted workflow graph.";
            entry["_meta"]["streamfind"]["guidance"] = "Use exact operation instance ids and semantic port ids. Connect all required inputs before validation.";
        } else if (name == "set_workflow") {
            entry["description"] = "Replace the persisted operation graph atomically with a complete workflow object containing operations and typed connections.";
            entry["_meta"]["streamfind"]["guidance"] = "Use this instead of repeated add_operation/connect_operations calls when submitting the complete graph at once.";
        } else if (name == "validate_workflow") {
            entry["description"] = "Validate the persisted operation graph and its parameters without executing it.";
            entry["_meta"]["streamfind"]["guidance"] = "Call after adding and connecting nodes; fix every validation error before run_workflow.";
        } else if (name == "run_workflow") {
            entry["description"] = "Execute the persisted connected operation graph and publish its table and structured-result artifacts.";
            entry["_meta"]["streamfind"]["guidance"] = "Call only after validate_workflow succeeds. Then inspect get_artifact_inventory and structuredContent.";
        }
    }
    return result;
}

const char *command(const std::string &name) {
    static const std::array<std::string, 31> commands = {
        "create", "describe", "validate", "get_project_domains", "get_metadata",
        "set_metadata", "get_workflow", "get_workflow_execution", "create_workflow_execution", "get_execution", "list_executions", "transition_execution", "cancel_execution", "set_workflow", "validate_workflow",
        "run_workflow", "get_cache", "get_cache_size", "delete_cache",
        "get_audit_trail", "copy", "close",
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
        "schemas; create or describe a project; discover canonical operations with get_operations "
        "and inspect their full schemas with get_operation; use add_operation and "
        "connect_operations (or set_workflow for an atomic graph replacement); call "
        "validate_workflow; call get_workflow to confirm the saved graph; call run_workflow; "
        "then inspect the returned operation results, get_artifact_inventory, request_artifact, "
        "and resolve_operation_inputs. Table outputs are immutable published artifacts identified "
        "by workflow revision, producer operation instance, and output contract. Use the artifact "
        "inventory rather than execution text as proof that an output is available to downstream "
        "operations. This operation-graph workflow is stateless at the MCP boundary: every "
        "project tool call carries database_path, and no connect or method session is needed.";
}

std::string tool_description(const Json &entry, const std::string &fallback) {
    std::string description = entry.value("label", fallback) + ": " + entry.value("definition", fallback);
    const auto guidance = entry.at("interface").value("guidance", "");
    if (!guidance.empty()) description += " Guidance: " + guidance;
    const auto model = entry.at("interface").value("invocation_model", "");
    if (!model.empty()) description += " Invocation model: " + model + ".";
    return description;
}

Json operation_result(const Json &value) {
    Json result = {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}};
    if (value.is_object() && value.value("schema", "") == "streamfind.visualization/v1") {
        result["structuredContent"] = value;
        result["content"].push_back({{"type", "resource_link"},
                                      {"uri", "ui://streamfind/visualization"},
                                      {"name", value.value("title", "StreamFind visualization")},
                                      {"mimeType", "text/html"}});
    }
    return result;
}

Json workflow_result(const Json &value) {
    Json result = {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}};
    Json visualizations = Json::array();
    for (const auto &operation : value.value("operations", Json::array())) {
        const auto operation_result_value = operation.value("result", Json(nullptr));
        if (operation_result_value.is_object() &&
            operation_result_value.value("schema", "") == "streamfind.visualization/v1")
            visualizations.push_back(operation_result_value);
    }
    if (visualizations.size() == 1) {
        result["structuredContent"] = visualizations.front();
        result["content"].push_back({{"type", "resource_link"},
                                      {"uri", "ui://streamfind/visualization"},
                                      {"name", visualizations.front().value("title", "StreamFind visualization")},
                                      {"mimeType", "text/html"}});
    } else if (!visualizations.empty()) {
        result["structuredContent"] = {{"schema", "streamfind.mcp.workflow-visualizations/v1"},
                                        {"visualizations", visualizations}};
        for (const auto &visualization : visualizations)
            result["content"].push_back({{"type", "resource_link"},
                                          {"uri", "ui://streamfind/visualization"},
                                          {"name", visualization.value("title", "StreamFind visualization")},
                                          {"mimeType", "text/html"}});
    }
    return result;
}

Json visualization_resource() {
    const auto app_directory_value = std::getenv("STREAMFIND_MCP_APP_DIR");
    const auto app_directory = app_directory_value == nullptr
                                    ? std::filesystem::path{}
                                    : std::filesystem::path(app_directory_value);
    const auto app_file = app_directory / "mcp-visualization.html";
    if (!app_directory.empty() && std::filesystem::exists(app_file)) {
        std::ifstream input(app_file, std::ios::binary);
        std::string html((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::string asset_prefix = "ui://streamfind/visualization/assets/";
        std::size_t position = 0;
        while ((position = html.find("/assets/", position)) != std::string::npos) {
            html.replace(position, 8, asset_prefix);
            position += asset_prefix.size();
        }
        return {{"uri", "ui://streamfind/visualization"},
                {"name", "StreamFind visualization"},
                {"description", "Frontend visualization app for StreamFind visualization specifications."},
                {"mimeType", "text/html"}, {"text", html}};
    }
    const auto configured_url = std::getenv("STREAMFIND_MCP_VISUALIZATION_URL");
    const std::string app_url = configured_url == nullptr
                                    ? "http://127.0.0.1:5173/mcp-visualization.html"
                                    : configured_url;
    return {{"uri", "ui://streamfind/visualization"},
            {"name", "StreamFind visualization"},
            {"description", "Frontend visualization app for StreamFind visualization specifications."},
            {"mimeType", "text/html"},
            {"text", "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>StreamFind visualization</title></head><body><div id=\"root\"></div><script type=\"module\" src=\"" + app_url + "\"></script></body></html>"}};
}
}

Session::Session(const OperationRegistry &operations) : operations_(operations) {}

Json Session::handle(const Json &request) {
    const auto id = request.value("id", Json(nullptr));
    const auto method = request.value("method", "");
    if (method == "initialize") return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", "2025-03-26"}, {"capabilities", {{"tools", Json::object()}, {"resources", {{"subscribe", false}, {"listChanged", false}}}}}, {"serverInfo", {{"name", "streamfind-cpp"}, {"version", std::string(streamfind::version())}}}, {"instructions", detail::interface_guidance()}}}};
    if (method == "tools/list") {
            auto catalogue = detail::tools();
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", catalogue}}}};
        }
    if (method == "resources/list") {
        const auto resource = detail::visualization_resource();
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"resources", Json::array({{{"uri", resource.at("uri")}, {"name", resource.at("name")}, {"description", resource.at("description")}, {"mimeType", resource.at("mimeType")}}})}}}};
    }
    if (method == "resources/read") {
        const auto uri = request.at("params").value("uri", "");
        const std::string asset_prefix = "ui://streamfind/visualization/assets/";
        if (uri.rfind(asset_prefix, 0) == 0) {
            const auto app_directory_value = std::getenv("STREAMFIND_MCP_APP_DIR");
            const auto asset_name = uri.substr(asset_prefix.size());
            const auto app_directory = app_directory_value == nullptr
                                            ? std::filesystem::path{}
                                            : std::filesystem::path(app_directory_value);
            const auto asset_path = app_directory / "assets" / asset_name;
            if (asset_name.empty() || asset_name.find("..") != std::string::npos ||
                !std::filesystem::exists(asset_path))
                return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32002}, {"message", "Unknown MCP resource asset"}}}};
            std::ifstream input(asset_path, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            const std::string asset_uri_prefix = "ui://streamfind/visualization/assets/";
            std::size_t import_position = 0;
            while ((import_position = content.find("\"./", import_position)) != std::string::npos) {
                content.replace(import_position + 1, 2, asset_uri_prefix);
                import_position += asset_uri_prefix.size() + 1;
            }
            import_position = 0;
            while ((import_position = content.find("'./", import_position)) != std::string::npos) {
                content.replace(import_position + 1, 2, asset_uri_prefix);
                import_position += asset_uri_prefix.size() + 1;
            }
            const auto extension = asset_path.extension().string();
            const auto mime_type = extension == ".css" ? "text/css" :
                                   extension == ".js" ? "text/javascript" : "application/octet-stream";
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", Json::array({{{"uri", uri}, {"mimeType", mime_type}, {"text", content}}})}}}};
        }
        if (uri != "ui://streamfind/visualization")
            return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32002}, {"message", "Unknown MCP resource"}}}};
        const auto resource = detail::visualization_resource();
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", Json::array({{{"uri", resource.at("uri")}, {"mimeType", resource.at("mimeType")}, {"text", resource.at("text")}}})}}}};
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
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(result)}};
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
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(result)}};
        } catch (const Error &error) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}}; }
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
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::workflow_result(result)}};
            }
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (!command) return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "Unknown MCP tool"}}}};
    try {
        Json result = api::run(api::command_from_string(command), request.at("params").value("arguments", Json::object()), operations_);

        if (name == "close") {
                            project_ = Json::object();

                        }
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}}}};
    } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }

    Json handle(const Json &request, const OperationRegistry &operations) {
        return Session(operations).handle(request);
    }
    }
