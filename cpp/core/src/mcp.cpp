#include "streamfind/mcp.hpp"
#include "streamfind/api.hpp"
#include "streamfind/catalogue.hpp"
#include "streamfind/version.hpp"
#include "streamfind/service_runtime_client.hpp"
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <utility>

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

std::string operation_instance_id(const Json &value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int>());
    throw Error(ErrorCode::InvalidArgument, "operation_id must be a string or integer");
}

bool matches_search(const Json &entry, const std::string &search) {
    if (search.empty()) return true;
    const auto query = lower(search);
    for (const auto &field : {"canonical_id", "label", "module_id"}) {
        const auto value = lower(entry.value(field, ""));
        if (value.find(query) == std::string::npos) continue;
        if (query.size() >= 4) return true;
        std::string token;
        for (const char character : value + " ") {
            if (std::isalnum(static_cast<unsigned char>(character))) {
                token += character;
            } else {
                if (token == query) return true;
                token.clear();
            }
        }
    }
    return false;
}

Json operation_summary(const Json &entry) {
    return Json{{"canonical_id", entry.value("canonical_id", "")},
                {"label", entry.value("label", "")},
                {"domain", entry.value("domain", "")},
                {"module_id", entry.value("module_id", "")},
                {"definition", entry.value("definition", "")}};
}

Json workflow_demos(const OperationRegistry &registry) {
    Json result = Json::array();
    std::vector<std::filesystem::path> roots;
    if (const auto *configured = std::getenv("STREAMFIND_WORKFLOW_RESOURCES")) roots.emplace_back(configured);
    roots.push_back(std::filesystem::current_path() / "plugins");
    roots.push_back(std::filesystem::current_path().parent_path() / "plugins");
    std::set<std::filesystem::path> files;
    for (const auto &root : roots) {
        if (!std::filesystem::exists(root)) continue;
        for (const auto &plugin : std::filesystem::directory_iterator(root)) {
            if (!plugin.is_directory()) continue;
            const auto workflows = plugin.path() / "resources" / "workflows";
            if (!std::filesystem::exists(workflows)) continue;
            for (const auto &file : std::filesystem::directory_iterator(workflows))
                if (file.is_regular_file() && file.path().extension() == ".json") files.insert(file.path());
        }
    }
    for (const auto &file : files) {
        try {
            std::ifstream input(file);
            const auto document = Json::parse(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
            const auto workflow_json = document.contains("workflow") ? document.at("workflow") : document;
            auto workflow = Workflow::from_json(workflow_json);
            workflow.validate(registry);
            const auto metadata = document.value("metadata", Json::object());
            const auto id = metadata.value("id", file.stem().string());
            result.push_back({{"id", id}, {"name", metadata.value("name", workflow.name.empty() ? id : workflow.name)},
                              {"description", metadata.value("description", "Workflow demonstration")},
                              {"use_case", metadata.value("use_case", "")}, {"domain", metadata.value("domain", "")},
                              {"workflow", workflow.to_json()}});
        } catch (const std::exception &) {
        }
    }
    return result;
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
    result.push_back(tool("list_workflow_demos", "List validated workflow demos registered by installed plugins, including metadata and portable workflow definitions.", Json::object(), Json::array()));
    result.push_back(tool("run_operation", "Run one canonical domain operation against a project database. Entry operations may run directly; operations with typed inputs require an existing graph binding or explicit inputs.",
                          Json{{"operation", {{"type", "string"}}},
                               {"database_path", {{"type", "string"}}},
                               {"arguments", {{"type", "object"}}},
                               {"operation_instance", {{"type", "string"}}},
                               {"inputs", {{"type", "object"}}}},
                          Json::array({"operation", "database_path"})));
    result.push_back(tool("get_dependencies", "List plugin-provided external dependencies and their current availability.",
                          Json{{"operation", {{"type", "string"}}}}, Json::array()));
    result.push_back(tool("install_dependencies", "Explicitly install selected plugin dependencies after reviewing their network and license requirements.",
                          Json{{"dependency_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                               {"allow_network", {{"type", "boolean"}}}},
                          Json::array({"dependency_ids"})));

    // Core project commands are catalogue entries, but their shared project
    // scope is enforced by the MCP dispatcher rather than by an ontology
    // operation schema. Keep tools/list truthful for clients that construct
    // calls from the advertised JSON Schema.
    const auto schema = [](Json properties, Json required) {
        return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
    };
    const Json database_path = {{"type", "string"}, {"description", "Path to the project DuckDB database."}};
    const Json project_path_schema = schema(Json{{"database_path", database_path}}, Json::array({"database_path"}));
    const Json workflow_metadata_schema = Json{{"type", "object"}, {"additionalProperties", true},
        {"required", Json::array({"name", "description"})},
        {"properties", Json{{"name", Json{{"type", "string"}, {"minLength", 1}}},
                             {"description", Json{{"type", "string"}, {"minLength", 1}}}}},
        {"description", "User-defined workflow metadata. Workflow revision is tracked by the top-level workflow version."}};
    const Json create_schema = schema(Json{{"database_path", database_path}, {"workflow_metadata", workflow_metadata_schema}},
                                      Json::array({"database_path"}));
    const Json workflow_schema = schema(Json{{"database_path", database_path}, {"workflow", {{"type", "object"}}}},
                                        Json::array({"database_path"}));
    const Json set_workflow_schema = schema(Json{{"database_path", database_path}, {"workflow", {{"type", "object"}}}},
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
    const Json request_result_schema = schema(
        Json{{"database_path", database_path}, {"result_id", {{"type", "string"}, {"description", "Exact published result identifier."}}},
             {"operation", {{"type", "string"}, {"description", "Canonical producer operation identifier."}}},
             {"operation_instance", {{"type", "string"}, {"description", "Workflow operation instance that produced the result."}}},
             {"output_port", {{"type", "string"}, {"description", "Output contract or semantic port identifier."}}},
             {"workflow_revision", {{"type", "integer"}}},
             {"include_data", {{"type", "boolean"}, {"description", "Return the JSON payload or bounded table rows for exactly one selected result."}}},
             {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
             {"offset", Json{{"type", "integer"}, {"minimum", 0}}}},
        Json::array({"database_path"}));
    for (auto &entry : result) {
        const auto name = entry.value("name", "");
        if (name == "create") entry["inputSchema"] = create_schema;
        else if (name == "describe" || name == "connect" || name == "validate" ||
            name == "get_project_domains" || name == "get_metadata" || name == "set_metadata" ||
            name == "get_workflow" || name == "get_workflow_execution" || name == "create_workflow_execution" ||
            name == "get_execution" || name == "list_executions" || name == "transition_execution" ||
            name == "cancel_execution" ||
            name == "run_workflow" || name == "get_audit_trail" || name == "get_result_inventory" ||
            name == "get_current_result_inventory" ||
            name == "resolve_operation_inputs")
            entry["inputSchema"] = project_path_schema;
        else if (name == "set_workflow") entry["inputSchema"] = set_workflow_schema;
        else if (name == "validate_workflow") entry["inputSchema"] = workflow_schema;
        else if (name == "add_operation") entry["inputSchema"] = add_operation_schema;
        else if (name == "connect_operations") entry["inputSchema"] = connect_operations_schema;
        else if (name == "request_result") entry["inputSchema"] = request_result_schema;

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
            entry["description"] = "Execute the persisted connected operation graph and publish its table and structured results for inspection in the streamfind web app.";
            entry["_meta"]["streamfind"]["guidance"] = "Call only after validate_workflow succeeds. Then inspect get_result_inventory and request_result; open the saved workflow in the streamfind web app for rich visualization.";
        } else if (name == "request_result") {
            entry["description"] = "Select one published operation output, optionally returning its JSON payload or bounded table rows.";
            entry["_meta"]["streamfind"]["guidance"] = "First call without include_data to discover candidates. Then select exactly one result_id, operation_instance plus output_port, or another unique filter and set include_data=true.";
        }
    }
    return result;
}

const char *command(const std::string &name) {
    static const std::array<std::string, 29> commands = {
        "create", "describe", "validate", "get_project_domains", "get_metadata",
        "set_metadata", "get_workflow", "get_workflow_execution", "create_workflow_execution", "get_execution", "list_executions", "transition_execution", "cancel_execution", "set_workflow", "validate_workflow",
        "run_workflow",
        "get_audit_trail", "copy", "close",
        "add_operation", "connect_operations", "get_result_inventory", "get_current_result_inventory",
        "request_result", "resolve_operation_inputs",

    };
    return std::find(commands.begin(), commands.end(), name) == commands.end() ? nullptr : name.c_str();
}

std::string interface_guidance() {
    return
        "StreamFind is a workflow-centric data-processing framework. A project is a "
        "DuckDB-backed workspace with derived domains, ontology-defined operations, immutable "
        "results, workflow revisions, and execution history. Build processing workflows "
        "as an acyclic graph of operation instances: each operation has a unique instance "
        "id, parameters, typed input/output ports, and an operationSuccessSignal. Connect "
        "output ports to input ports or compatible parameters explicitly with "
        "source_operation, source_port, target_operation, and target_port. Use the "
        "target_port form parameter:<parameter_name> when a preceding result should "
        "supply a dynamic parameter value; this is useful for selectors, branching, "
        "loops, thresholds, and other generic control/dataflow operations. The runtime "
        "merges that result payload into the target operation parameters and validates "
        "the resulting parameter object. Do not infer dependencies from operation order "
        "or create next-operation edges.\n\n"
        "Recommended agent workflow: call tools/list to discover callable MCP tools and "
        "schemas; create or describe a project; discover canonical operations with get_operations "
        "and inspect their full schemas with get_operation; use add_operation and "
        "connect_operations (or set_workflow for an atomic graph replacement); call "
        "validate_workflow; call get_workflow to confirm the saved graph; call run_workflow; "
        "then inspect the returned operation results, get_result_inventory, "
                "request_result, and resolve_operation_inputs. "
                "Visualization results are persisted results: use their result metadata and "
                "bounded text fallback in MCP, then open the saved workflow in the streamfind web app "
                "for rich interactive visualization. Table outputs are immutable published results "
        "by workflow revision, producer operation instance, and output contract. Use the result "
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
    // MCP returns a bounded text/metadata fallback only. The persisted
    // visualization artifact remains the rich-rendering contract owned by the
    // streamfind web app; do not duplicate it in structuredContent or an MCP
    // Apps HTML resource.
    return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}};
}

Json workflow_result(const Json &value) {
    return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}};
}
}

Session::Session(const OperationRegistry &operations, DependencyList dependencies,
                 DependencyInstaller installer, std::filesystem::path worker_executable,
                 std::filesystem::path worker_configuration)
    : operations_(operations), dependencies_(std::move(dependencies)), installer_(std::move(installer)),
      worker_executable_(std::move(worker_executable)), worker_configuration_(std::move(worker_configuration)) {}

Json Session::handle(const Json &request) {
    const auto id = request.value("id", Json(nullptr));
    const auto method = request.value("method", "");
    if (method == "initialized" && !request.contains("id")) return Json(nullptr);
    if (method == "initialize") return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", "2025-03-26"}, {"capabilities", {{"tools", Json::object()}}}, {"serverInfo", {{"name", "streamfind-cpp"}, {"version", std::string(streamfind::version())}}}, {"instructions", detail::interface_guidance()}}}};
    if (method == "tools/list") {
            auto catalogue = detail::tools();
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", catalogue}}}};
        }

    if (method != "tools/call") return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Unsupported MCP method"}}}};
    const auto name = request.at("params").value("name", "");
    if (name == "list_workflow_demos") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(detail::workflow_demos(operations_))}};
    }
    if (name == "get_dependencies") {
        if (!dependencies_) return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(Json{{"dependencies", Json::array()}})}};
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(dependencies_())}};
    }
    if (name == "install_dependencies") {
        if (!installer_) throw Error(ErrorCode::MethodExecution, "dependency installation is unavailable");
        const auto arguments = request.at("params").value("arguments", Json::object());
        if (!arguments.value("allow_network", false))
            throw Error(ErrorCode::InvalidArgument, "install_dependencies requires allow_network=true");
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(installer_(arguments))}};
    }
    if (name == "connect") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto database_path = arguments.at("database_path").get<std::string>();
            const auto attachment = service::attach_project(database_path);
            project_ = Json{{"database_path", database_path},
                            {"service_instance_id", attachment.service_instance_id},
                            {"session_id", attachment.session_id},
                            {"endpoint", attachment.endpoint}};
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
                auto operation_arguments = arguments.value("arguments", Json::object());
                const auto operation_instance = arguments.value("operation_instance", std::string{});
                const auto inputs = arguments.value("inputs", Json(nullptr));
                const auto attachment = service::attach_project(options.database_path);
                const auto workflow = service::get_workflow(attachment);
                const auto effective_instance = operation_instance.empty() ? operation_id : operation_instance;
                if (operation_arguments.empty() && workflow.is_object()) {
                    for (const auto &node : workflow.value("operations", Json::array())) {
                        const auto node_id = node.value("id", Json(nullptr));
                        const auto matches_instance = node_id.is_string()
                            ? node_id.get<std::string>() == effective_instance
                            : node_id.is_number_integer() &&
                              std::to_string(node_id.get<int>()) == effective_instance;
                        if (matches_instance) {
                            operation_arguments = node.value("parameters", Json::object());
                            break;
                        }
                    }
                }
                // Do not hold the MCP stdio request loop open for the worker.
                // Long operations otherwise make MCP clients report timeout
                // 32001 even though the service continues and publishes the
                // result.  The service remains the owner of execution and
                // publication; this thread only waits for its HTTP request to
                // complete while subsequent MCP status calls remain usable.
                std::thread([attachment, operation_id, operation_arguments,
                             effective_instance, inputs]() mutable {
                    try {
                        (void)service::run_operation(attachment, operation_id,
                                                     operation_arguments,
                                                     effective_instance, inputs);
                    } catch (...) {
                        // The service emits the failure event and exposes it
                        // through the runtime snapshot.  There is no pending
                        // MCP request to which a late exception can be sent.
                    }
                }).detach();
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(
                    Json{{"accepted", true}, {"state", "queued"},
                         {"session_id", attachment.session_id},
                         {"operation_id", operation_id},
                         {"operation_instance", effective_instance},
                         {"message", "Operation accepted; poll get_workflow_execution or get_execution for runtime state, then inspect get_result_inventory."}})}};
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
            const auto attachment = service::attach_project(options.database_path);
            const auto workflow = service::get_workflow(attachment);
            Json parameters = arguments;
            parameters.erase("database_path");
            parameters.erase("domain");
            const auto inputs = parameters.value("inputs", Json(nullptr));
            const auto operation_instance = parameters.value("operation_instance", std::string{});
            parameters.erase("inputs");
            parameters.erase("operation_instance");
            const auto effective_instance = operation_instance.empty() ? name : operation_instance;
            if (parameters.empty() && workflow.is_object()) {
                for (const auto &node : workflow.value("operations", Json::array())) {
                    const auto node_id = node.value("id", Json(nullptr));
                    const auto matches_instance = node_id.is_string()
                        ? node_id.get<std::string>() == effective_instance
                        : node_id.is_number_integer() &&
                          std::to_string(node_id.get<int>()) == effective_instance;
                    if (matches_instance) {
                        parameters = node.value("parameters", Json::object());
                        break;
                    }
                }
            }
            const Json result = service::run_operation(attachment, name, parameters,
                                                       effective_instance, inputs);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(result)}};
        } catch (const Error &error) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}}; }
    }

    if (name == "validate_workflow" || name == "set_workflow") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            if (!arguments.contains("database_path") ||
                (name == "set_workflow" && !arguments.contains("workflow")))
                throw Error(ErrorCode::InvalidArgument,
                            name == "set_workflow"
                                ? "Workflow update requires database_path and workflow"
                                : "Workflow validation requires database_path");
            const auto database_path = arguments.at("database_path").get<std::string>();
            const auto attachment = service::attach_project(database_path);
            const auto workflow = arguments.contains("workflow")
                ? arguments.at("workflow") : Json(nullptr);
            const Json result = name == "set_workflow"
                ? service::save_workflow(attachment, workflow)
                : service::validate_workflow(attachment, workflow);
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
            const auto database_path = arguments.at("database_path").get<std::string>();
            const auto attachment = service::attach_project(database_path);
            Json response = service::start_workflow(
                attachment, arguments.value("execution_mode", std::string{"detailed"}));
            response["workflow"] = service::get_workflow(attachment);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::workflow_result(response)}};
        } catch (const Error &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "create" || name == "describe" || name == "validate" ||
        name == "get_project_domains" || name == "get_metadata" ||
        name == "set_metadata" || name == "get_audit_trail") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto database_path = arguments.at("database_path").get<std::string>();
            if (name == "create") {
                const auto attachment = service::create_project(
                    database_path, arguments.value("metadata", Json::object()));
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::get_project(attachment))}};
            }
            const auto attachment = service::attach_project(database_path);
            if (name == "describe")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::get_project(attachment))}};
            if (name == "get_project_domains")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::get_project(attachment).value("domains", Json::array()))}};
            if (name == "get_metadata")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::get_project(attachment).value("metadata", Json::object()))}};
            if (name == "set_metadata")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::set_metadata(attachment, arguments.value("metadata", Json::object())))}};
            if (name == "get_audit_trail")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::get_audit_trail(attachment))}};
            if (name == "validate")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::validate_workflow(attachment))}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "add_operation" || name == "connect_operations") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto attachment = service::attach_project(arguments.at("database_path").get<std::string>());
            auto workflow = service::get_workflow(attachment);
            if (name == "add_operation") {
                const auto operation_id = arguments.at("operation_id");
                for (const auto &existing : workflow.value("operations", Json::array()))
                    if (existing.value("id", Json(nullptr)) == operation_id)
                        throw Error(ErrorCode::InvalidArgument, "Workflow operation id already exists");
                workflow["operations"].push_back({
                    {"id", operation_id},
                    {"operation", arguments.at("operation")},
                    {"parameters", arguments.value("parameters", Json::object())},
                    {"inputs", arguments.value("inputs", Json::object())},
                    {"position", arguments.value("position", Json::object())}});
            } else {
                workflow["connections"].push_back({
                    {"source_operation", arguments.at("source_operation")},
                    {"source_port", arguments.at("source_port")},
                    {"target_operation", arguments.at("target_operation")},
                    {"target_port", arguments.at("target_port")}});
            }
            const auto saved = service::save_workflow(attachment, workflow);
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(saved)}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "copy") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto attachment = service::attach_project(arguments.at("database_path").get<std::string>());
            const auto result = service::copy_project(
                attachment, arguments.at("destination_database_path").get<std::string>());
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(result)}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "get_workflow_execution" || name == "create_workflow_execution" ||
        name == "get_execution" || name == "list_executions" ||
        name == "transition_execution" || name == "cancel_execution") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto database_path = arguments.at("database_path").get<std::string>();
            const auto attachment = service::attach_project(database_path);
            if (name == "cancel_execution")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::cancel_workflow(attachment))}};
            if (name == "transition_execution") {
                const auto status = arguments.value("status", std::string{});
                if (status == "running") service::start_workflow(attachment, arguments.value("execution_mode", std::string{"detailed"}));
                else if (status == "cancelling" || status == "cancelled") service::cancel_workflow(attachment);
            }
            // The old execution rows are replaced by the service-owned
            // workflow snapshot, which includes run, worker, progress, and
            // cancellation state without reopening the project database.
            auto snapshot = service::workflow_snapshot(attachment);
            snapshot["execution_model"] = "service_runtime";
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(snapshot)}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "close") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto attachment = service::attach_project(arguments.at("database_path").get<std::string>());
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(service::close_project(attachment))}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (name == "get_workflow" || name == "get_result_inventory" ||
        name == "get_current_result_inventory" || name == "request_result" ||
        name == "resolve_operation_inputs") {
        try {
            const auto arguments = request.at("params").value("arguments", Json::object());
            const auto attachment = service::attach_project(arguments.at("database_path").get<std::string>());
            if (name == "get_workflow")
                return {{"jsonrpc", "2.0"}, {"id", id},
                        {"result", detail::operation_result(service::get_workflow(attachment))}};
            if (name == "resolve_operation_inputs")
                return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(
                    service::resolve_workflow_inputs(attachment,
                        detail::operation_instance_id(arguments.at("operation_id"))))}};

            auto inventory = name == "get_current_result_inventory"
                ? service::get_current_results(attachment)
                : service::get_results(attachment);
            if (name == "get_result_inventory" || name == "get_current_result_inventory") {
                for (auto &result : inventory) result.erase("payload");
                return {{"jsonrpc", "2.0"}, {"id", id},
                        {"result", detail::operation_result(inventory)}};
            }

            Json selected = Json::array();
            for (const auto &result : inventory) {
                if (arguments.contains("result_id") &&
                    result.value("result_id", "") != arguments.at("result_id").get<std::string>()) continue;
                if (arguments.contains("operation") &&
                    result.value("producer_operation", "") != arguments.at("operation").get<std::string>()) continue;
                if (arguments.contains("operation_instance") &&
                    result.value("producer_instance", "") != arguments.at("operation_instance").get<std::string>()) continue;
                if (arguments.contains("output_port") &&
                    result.value("contract_id", "") != arguments.at("output_port").get<std::string>()) continue;
                if (arguments.contains("workflow_revision") &&
                    result.value("workflow_revision", "") !=
                        std::to_string(arguments.at("workflow_revision").get<int>())) continue;
                auto descriptor = result;
                descriptor.erase("payload");
                selected.push_back(std::move(descriptor));
            }
            if (!arguments.value("include_data", false))
                return {{"jsonrpc", "2.0"}, {"id", id},
                        {"result", detail::operation_result(selected)}};
            if (selected.size() != 1)
                throw Error(ErrorCode::InvalidArgument,
                            "include_data requires exactly one result selector; provide result_id, operation_instance, or output_port");
            auto result = selected.front();
            if (result.value("representation", "") == "table") {
                const auto data = service::get_result_data(
                    attachment, Json{{"result_id", result.value("result_id", "")},
                                     {"mode", "page"},
                                     {"limit", std::clamp(arguments.value("limit", 1000), 1, 10000)},
                                     {"offset", std::max(arguments.value("offset", 0), 0)}});
                Json rows = Json::array();
                const auto columns = data.value("columns", Json::array());
                for (const auto &row : data.value("rows", Json::array())) {
                    Json object = Json::object();
                    for (std::size_t index = 0; index < columns.size() && index < row.size(); ++index)
                        object[columns.at(index).value("name", std::to_string(index))] = row.at(index);
                    rows.push_back(std::move(object));
                }
                result["data"] = std::move(rows);
                result["row_count"] = data.value("total_rows", 0);
                result["limit"] = data.value("limit", arguments.value("limit", 1000));
                result["offset"] = data.value("offset", arguments.value("offset", 0));
            } else if (result.contains("payload") && !result.at("payload").is_null()) {
                const auto &payload = result.at("payload");
                if (payload.is_string()) {
                    const auto parsed = Json::parse(payload.get<std::string>(), nullptr, false);
                    result["data"] = parsed.is_discarded() ? Json(payload.get<std::string>()) : parsed;
                } else result["data"] = payload;
            }
            result.erase("payload");
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", detail::operation_result(Json::array({std::move(result)}))}};
        } catch (const std::exception &error) {
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}};
        }
    }
    if (!command) return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "Unknown MCP tool"}}}};
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", -32602}, {"message", "MCP tool is not available through the service runtime"}}}};
    }

    Json handle(const Json &request, const OperationRegistry &operations) {
        return Session(operations).handle(request);
    }
    }
