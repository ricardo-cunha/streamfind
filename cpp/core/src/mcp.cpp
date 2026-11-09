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
#include <limits>
#include <set>
#include <utility>
#include <vector>

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
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    throw Error(ErrorCode::InvalidArgument, "operation_id must be a string or integer");
}

std::int64_t next_workflow_operation_id(const Json &workflow) {
    std::int64_t maximum = 0;
    const auto operations = workflow.value("operations", Json::array());
    for (const auto &node : operations) {
        if (!node.is_object() || !node.contains("id")) continue;
        try {
            const auto &value = node.at("id");
            std::int64_t id = 0;
            if (value.is_number_integer()) id = value.get<std::int64_t>();
            else if (value.is_string()) {
                const auto text = value.get<std::string>();
                std::size_t consumed = 0;
                id = std::stoll(text, &consumed);
                if (consumed != text.size()) continue;
            } else continue;
            maximum = std::max(maximum, id);
        } catch (const std::exception &) {
            // Existing nonnumeric IDs do not participate in integer allocation.
        }
    }
    maximum = std::max(maximum, static_cast<std::int64_t>(operations.size()));
    if (maximum == std::numeric_limits<std::int64_t>::max())
        throw Error(ErrorCode::InvalidArgument, "workflow has no available integer operation id");
    return maximum + 1;
}

std::string resolve_operation_instance(const Json &workflow, const std::string &operation_id,
                                       const std::string &requested_instance) {
    if (!requested_instance.empty()) return requested_instance;
    if (!workflow.is_object() || !workflow.contains("operations") ||
        !workflow.at("operations").is_array()) return operation_id;

    std::vector<std::string> matching_instances;
    for (const auto &node : workflow.at("operations")) {
        if (!node.is_object() || node.value("operation", std::string{}) != operation_id ||
            !node.contains("id")) continue;
        try {
            matching_instances.push_back(operation_instance_id(node.at("id")));
        } catch (const Error &) {
            continue;
        }
    }
    if (matching_instances.size() == 1) return matching_instances.front();
    if (matching_instances.size() > 1)
        throw Error(ErrorCode::InvalidArgument,
                    "operation_instance is required because the workflow contains multiple nodes for " + operation_id);
    return operation_id;
}

std::string requested_execution_mode(const Json &arguments) {
    if (!arguments.contains("execution_mode")) return "detailed";
    const auto &value = arguments.at("execution_mode");
    if (!value.is_string())
        throw Error(ErrorCode::InvalidArgument,
                    "execution_mode must be a string: detailed or pipeline");
    const auto mode = value.get<std::string>();
    if (mode != "detailed" && mode != "pipeline")
        throw Error(ErrorCode::InvalidArgument,
                    "execution_mode must be detailed or pipeline");
    return mode;
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

Json workflow_demos(const OperationRegistry &registry,
                    const std::vector<std::filesystem::path> &directories) {
    Json workflows = Json::array();
    Json diagnostics = Json::array();
    std::set<std::filesystem::path> files;
    for (const auto &directory : directories) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            diagnostics.push_back({{"path", directory.string()},
                                   {"message", error ? error.message()
                                                     : "workflow resources directory does not exist"}});
            continue;
        }
        if (!std::filesystem::is_directory(directory, error)) {
            diagnostics.push_back({{"path", directory.string()},
                                  {"message", error ? error.message() : "workflow resources path is not a directory"}});
            continue;
        }
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (!iterator->is_regular_file(error) || iterator->path().extension() != ".json") continue;
            files.insert(iterator->path());
        }
        if (error) diagnostics.push_back({{"path", directory.string()}, {"message", error.message()}});
    }
    for (const auto &file : files) {
        try {
            std::ifstream input(file);
            if (!input) throw std::runtime_error("unable to open workflow demo file");
            const auto document = Json::parse(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
            const auto workflow_json = document.contains("workflow") ? document.at("workflow") : document;
            auto workflow = Workflow::from_json(workflow_json);
            workflow.validate(registry);
            const auto metadata = document.value("metadata", Json::object());
            const auto id = metadata.value("id", file.stem().string());
            workflows.push_back({{"id", id}, {"name", metadata.value("name", workflow.name.empty() ? id : workflow.name)},
                                 {"description", metadata.value("description", "Workflow demonstration")},
                                 {"use_case", metadata.value("use_case", "")}, {"domain", metadata.value("domain", "")},
                                 {"workflow", workflow.to_json()}});
        } catch (const std::exception &error) {
            diagnostics.push_back({{"path", file.string()}, {"message", error.what()}});
        }
    }
    return Json{{"workflows", workflows}, {"diagnostics", diagnostics}};
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
    result.push_back(tool("list_workflow_demos", "List validated workflow demos from enabled package plugins. Demo resources are resolved relative to the MCP executable configuration; malformed or unreadable demo files are returned in diagnostics.", Json::object(), Json::array()));
    result.push_back(tool("run_operation", "Run one canonical domain operation against a project database. If exactly one matching workflow node exists, its parameters, inputs, and result-port identity are used automatically. Supply operation_instance when multiple matching nodes exist. Entry operations may run directly; operations with typed inputs require an existing graph binding or explicit inputs.",
                          Json{{"operation", {{"type", "string"}}},
                               {"database_path", {{"type", "string"}}},
                               {"arguments", {{"type", "object"}}},
                               {"operation_instance", {{"type", "integer"}, {"description", "Integer ID of an existing workflow node."}}},
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
    const Json operation_instance_reference = {
        {"type", "integer"},
        {"description", "Integer identifier of a workflow operation node."}};
    const Json project_path_schema = schema(Json{{"database_path", database_path}}, Json::array({"database_path"}));
    const Json workflow_execution_schema = schema(
        Json{{"database_path", database_path},
             {"operation_instance", {{"type", "integer"},
                                     {"description", "When supplied, return the durable status for this integer workflow node ID; otherwise return workflow status."}}}},
        Json::array({"database_path"}));
    const Json execution_mode = {{"type", "string"}, {"enum", {"detailed", "pipeline"}},
                                {"default", "detailed"},
                                {"description", "Result publication mode. detailed publishes each node result immediately; pipeline retains node results during execution and publishes them together when the run ends. Both modes retain results for inspection."}};
    const Json run_workflow_schema = schema(
        Json{{"database_path", database_path}, {"execution_mode", execution_mode}},
        Json::array({"database_path"}));
    const Json transition_execution_schema = schema(
        Json{{"database_path", database_path},
             {"status", {{"type", "string"}, {"enum", {"running", "cancelling", "cancelled"}}}},
             {"execution_mode", execution_mode}},
        Json::array({"database_path", "status"}));
    const Json workflow_metadata_schema = Json{{"type", "object"}, {"additionalProperties", true},
        {"required", Json::array({"name", "description"})},
        {"properties", Json{{"name", Json{{"type", "string"}, {"minLength", 1}}},
                             {"description", Json{{"type", "string"}, {"minLength", 1}}}}},
        {"description", "User-defined workflow metadata. Definition concurrency is tracked by definition_revision; workflow version tracks result provenance."}};
    const Json create_schema = schema(Json{{"database_path", database_path}, {"workflow_metadata", workflow_metadata_schema}},
                                      Json::array({"database_path"}));
    const Json workflow_schema = schema(Json{{"database_path", database_path}, {"workflow", {{"type", "object"}}}},
                                        Json::array({"database_path"}));
    const Json set_workflow_schema = schema(Json{{"database_path", database_path},
        {"workflow", Json{{"type", "object"},
            {"required", Json::array({"schema_version", "definition_revision", "operations", "connections"})},
            {"properties", Json{{"definition_revision", Json{{"type", "integer"},
                {"minimum", 1}, {"description", "Concurrency revision returned by get_workflow. Saving fails if another client has changed the workflow since this revision."}}}}},
            {"additionalProperties", true}}}},
                                            Json::array({"database_path", "workflow"}));
    const Json add_operation_schema = schema(
        Json{{"database_path", database_path},
             {"operation", {{"type", "string"}}}, {"parameters", {{"type", "object"}}},
             {"inputs", {{"type", "object"}}}, {"position", {{"type", "object"}}}},
        Json::array({"database_path", "operation"}));
    const Json connect_operations_schema = schema(
        Json{{"database_path", database_path}, {"source_operation", operation_instance_reference},
             {"source_port", {{"type", "string"}}}, {"target_operation", operation_instance_reference},
             {"target_port", {{"type", "string"}}}},
        Json::array({"database_path", "source_operation", "source_port", "target_operation", "target_port"}));
    const Json result_filter_schema = {
        {"type", "object"},
        {"properties", Json{
            {"column", {{"type", "string"}}},
            {"operator", {{"type", "string"},
                          {"enum", {"eq", "neq", "gt", "gte", "lt", "lte", "between", "in", "is_null", "is_not_null"}}}},
            {"value", Json::object()}, {"min", Json::object()}, {"max", Json::object()},
            {"values", {{"type", "array"}}}}},
        {"required", Json::array({"column"})}};
    const Json request_result_schema = schema(
        Json{{"database_path", database_path}, {"result_id", {{"type", "string"}, {"description", "Exact published result identifier."}}},
             {"operation", {{"type", "string"}, {"description", "Canonical producer operation identifier."}}},
             {"operation_instance", {{"type", "integer"}, {"description", "Integer workflow node ID that produced the result."}}},
             {"output_port", {{"type", "string"}, {"description", "Output contract or semantic port identifier."}}},
             {"workflow_revision", {{"type", "integer"}}},
             {"include_data", {{"type", "boolean"}, {"description", "Return the JSON payload or bounded table rows for exactly one selected result."}}},
             {"columns", {{"type", "array"}, {"items", {{"type", "string"}}},
                          {"minItems", 1}, {"description", "Physical table columns to return. Required when include_data is true for a table result."}}},
             {"filters", {{"type", "array"}, {"items", result_filter_schema}}},
             {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
             {"offset", Json{{"type", "integer"}, {"minimum", 0}}}},
        Json::array({"database_path"}));
    for (auto &entry : result) {
        const auto name = entry.value("name", "");
        if (name == "create") entry["inputSchema"] = create_schema;
        else if (name == "describe" || name == "connect" || name == "validate" ||
            name == "get_project_domains" || name == "get_metadata" || name == "set_metadata" ||
            name == "get_workflow" || name == "create_workflow_execution" ||
            name == "get_execution" || name == "list_executions" ||
            name == "cancel_execution" ||
            name == "get_audit_trail" || name == "get_result_inventory" ||
            name == "get_current_result_inventory" ||
            name == "resolve_operation_inputs")
            entry["inputSchema"] = project_path_schema;
        else if (name == "set_workflow") entry["inputSchema"] = set_workflow_schema;
        else if (name == "validate_workflow") entry["inputSchema"] = workflow_schema;
        else if (name == "add_operation") entry["inputSchema"] = add_operation_schema;
        else if (name == "connect_operations") entry["inputSchema"] = connect_operations_schema;
        else if (name == "request_result") entry["inputSchema"] = request_result_schema;
        else if (name == "run_workflow") entry["inputSchema"] = run_workflow_schema;
        else if (name == "transition_execution") entry["inputSchema"] = transition_execution_schema;
        else if (name == "get_workflow_execution") entry["inputSchema"] = workflow_execution_schema;

        if (name == "create") {
            entry["description"] = "Create a new project database. This is the first step of the operation-graph workflow; then call add_operation for each node.";
            entry["_meta"]["streamfind"]["guidance"] = "Call create once with database_path, then add operation nodes with their parameters.";
        } else if (name == "add_operation") {
            entry["description"] = "Add one configured operation node to the persisted workflow graph. The service assigns the next integer node ID automatically.";
            entry["_meta"]["streamfind"]["guidance"] = "The node ID is assigned as one greater than the highest integer ID already in the workflow. Add every node with its final parameters before connecting typed ports. Use get_operation to inspect the operation schema.";
        } else if (name == "connect_operations") {
            entry["description"] = "Connect one operation output port to another operation input port in the persisted workflow graph.";
            entry["_meta"]["streamfind"]["guidance"] = "Use exact operation instance ids and semantic port ids. Connect all required inputs before validation.";
        } else if (name == "set_workflow") {
            entry["description"] = "Replace the persisted operation graph atomically with a complete workflow object containing operations and typed connections.";
            entry["_meta"]["streamfind"]["guidance"] = "Read the latest workflow with get_workflow and preserve its definition_revision in the submitted workflow. A stale revision is rejected; reload and reconcile before retrying. Use this instead of repeated add_operation/connect_operations calls when submitting the complete graph at once.";
        } else if (name == "validate_workflow") {
            entry["description"] = "Validate the persisted operation graph and its parameters without executing it.";
            entry["_meta"]["streamfind"]["guidance"] = "Call after adding and connecting nodes; fix every validation error before run_workflow.";
        } else if (name == "run_workflow") {
            entry["description"] = "Execute the persisted connected operation graph. Both execution modes retain every completed node result for inspection after the run.";
            entry["_meta"]["streamfind"]["guidance"] = "Call only after validate_workflow succeeds. Use execution_mode=detailed to publish results as each node completes, or execution_mode=pipeline to publish retained results together when the run ends. Omit execution_mode to use detailed. Then inspect get_result_inventory and request_result.";
        } else if (name == "run_operation") {
            entry["description"] = "Submit one operation to the service runtime and return immediately with a run ID. The service owns execution and result publication after the MCP call returns.";
            entry["_meta"]["streamfind"]["guidance"] = "Use the returned operation_instance with get_workflow_execution to poll queued, running, cancelling, completed, failed, or cancelled state. Then inspect get_result_inventory for published outputs.";
        } else if (name == "get_workflow_execution") {
            entry["description"] = "Get service runtime status. Supply operation_instance to inspect a direct operation run; omit it to inspect the workflow run.";
            entry["_meta"]["streamfind"]["guidance"] = "The service retains the latest operation status for the project session and returns worker metadata when available. Poll until a terminal state, then inspect results.";
        } else if (name == "request_result") {
            entry["description"] = "Select one published operation output, optionally returning its JSON payload or bounded table rows.";
            entry["_meta"]["streamfind"]["guidance"] = "First call without include_data to discover candidates and inspect the schema. Then select one result. For table results, specify the columns you need; optional filters use the shared service query operators and are independent of the table's domain.";
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
                 std::filesystem::path worker_configuration,
                 WorkflowDemoDirectories workflow_demo_directories)
    : operations_(operations), dependencies_(std::move(dependencies)), installer_(std::move(installer)),
      worker_executable_(std::move(worker_executable)), worker_configuration_(std::move(worker_configuration)),
      workflow_demo_directories_(std::move(workflow_demo_directories)) {}

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
        const auto directories = workflow_demo_directories_ ? workflow_demo_directories_()
                                                             : std::vector<std::filesystem::path>{};
        return {{"jsonrpc", "2.0"}, {"id", id},
                {"result", detail::operation_result(detail::workflow_demos(operations_, directories))}};
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
        } catch (const std::exception &error) {
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
                const auto operation_instance = arguments.contains("operation_instance")
                    ? detail::operation_instance_id(arguments.at("operation_instance")) : std::string{};
                const auto inputs = arguments.value("inputs", Json(nullptr));
                const auto attachment = service::attach_project(options.database_path);
                const auto workflow = service::get_workflow(attachment);
                const auto effective_instance = detail::resolve_operation_instance(
                    workflow, operation_id, operation_instance);
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
                auto accepted = service::start_operation(attachment, operation_id,
                                                         operation_arguments,
                                                         effective_instance, inputs);
                accepted["message"] = "Operation accepted by the service. Poll get_workflow_execution with this operation_instance for status, then inspect get_result_inventory.";
                return {{"jsonrpc", "2.0"}, {"id", id},
                        {"result", detail::operation_result(std::move(accepted))}};
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
            const auto operation_instance = parameters.contains("operation_instance")
                ? detail::operation_instance_id(parameters.at("operation_instance")) : std::string{};
            parameters.erase("inputs");
            parameters.erase("operation_instance");
            const auto effective_instance = detail::resolve_operation_instance(
                workflow, name, operation_instance);
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
            auto accepted = service::start_operation(attachment, name, parameters,
                                                     effective_instance, inputs);
            accepted["message"] = "Operation accepted by the service. Poll get_workflow_execution with this operation_instance for status, then inspect get_result_inventory.";
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", detail::operation_result(std::move(accepted))}};
        } catch (const std::exception &error) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isError", true}, {"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}}}}; }
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
        } catch (const std::exception &error) {
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
                attachment, detail::requested_execution_mode(arguments));
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
                if (arguments.contains("operation_id"))
                    throw Error(ErrorCode::InvalidArgument, "operation_id is assigned automatically; omit it");
                const auto operation_id = detail::next_workflow_operation_id(workflow);
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
            auto saved = service::save_workflow(attachment, workflow);
            if (name == "add_operation" && saved.is_object())
                saved["assigned_operation_id"] = workflow.at("operations").back().at("id");
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
                if (status == "running")
                    service::start_workflow(attachment,
                                            detail::requested_execution_mode(arguments));
                else if (status == "cancelling" || status == "cancelled") service::cancel_workflow(attachment);
            }
            // The old execution rows are replaced by the service-owned
            // workflow snapshot, which includes run, worker, progress, and
            // cancellation state without reopening the project database.
            auto snapshot = name == "get_workflow_execution" && arguments.contains("operation_instance")
                ? service::operation_snapshot(attachment,
                    detail::operation_instance_id(arguments.at("operation_instance")))
                : service::workflow_snapshot(attachment);
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
                    result.value("producer_instance", "") != detail::operation_instance_id(arguments.at("operation_instance"))) continue;
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
                if (!arguments.contains("columns") || !arguments.at("columns").is_array() ||
                    arguments.at("columns").empty())
                    throw Error(ErrorCode::InvalidArgument,
                                "table include_data requires an explicit non-empty columns array");
                for (const auto &column : arguments.at("columns"))
                    if (!column.is_string() || column.get<std::string>().empty())
                        throw Error(ErrorCode::InvalidArgument,
                                    "table columns must contain non-empty strings");
                Json query{{"result_id", result.value("result_id", "")},
                           {"mode", "page"},
                           {"columns", arguments.at("columns")},
                           {"limit", std::clamp(arguments.value("limit", 1000), 1, 10000)},
                           {"offset", std::max(arguments.value("offset", 0), 0)}};
                if (arguments.contains("filters")) query["filters"] = arguments.at("filters");
                const auto data = service::get_result_data(attachment, query);
                result["data"] = data.value("rows", Json::array());
                result["columns"] = data.value("columns", Json::array());
                result["row_count"] = data.value("total_rows", 0);
                result["limit"] = data.value("limit", arguments.value("limit", 1000));
                result["offset"] = data.value("offset", arguments.value("offset", 0));
                result["returned_rows"] = data.value("returned_rows", result["data"].size());
                result["has_more"] = data.value("has_more", false);
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
