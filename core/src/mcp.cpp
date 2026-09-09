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
    static const std::array<std::string, 30> commands = {
        "create", "describe", "validate", "get_domain", "get_metadata",
        "set_metadata", "get_workflow", "get_workflow_execution", "create_workflow_execution", "get_execution", "list_executions", "transition_execution", "cancel_execution", "set_workflow", "add_method", "remove_method", "validate_workflow",
        "run_workflow", "get_cache", "get_cache_size", "delete_cache",
        "get_audit_trail", "get_available_methods", "run_method", "copy", "close",
        "tools_status", "tools_install", "tools_install_java", "tools_install_metfrag"
    };
    return std::find(commands.begin(), commands.end(), name) == commands.end() ? nullptr : name.c_str();
}

std::string interface_guidance() {
    if (const auto entries = streamfind::catalogue::entries_json()) {
        for (const auto &entry : *entries) {
            const auto guidance = entry.value("interface_guidance", "");
            if (!guidance.empty()) return guidance;
        }
    }
    return "Start with create, then describe the project. Domain operations are stateless and require database_path; connect is only needed for workflow methods.";
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
                    catalogue.push_back(Json{
                        {"name", entry->value("canonical_id", definition.id)},
                        {"description", detail::tool_description(*entry, definition.description)},
                        {"inputSchema", entry->at("mcp").at("input_schema")},
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
            const Json result = project.run_operation(name, arguments, operations_);
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
