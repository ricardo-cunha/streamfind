#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "streamfind/mcp.hpp"

namespace streamfind::mcp_schema_contract {

const Json &tool_named(const Json &tools, const std::string &name) {
    for (const auto &tool : tools) {
        if (tool.value("name", "") == name) return tool;
    }
    std::string available;
    for (const auto &tool : tools) available += tool.value("name", "") + " ";
    throw std::runtime_error("MCP schema contract: missing tool " + name +
                             "; available: " + available);
}

void require_node_reference(const Json &schema, const std::string &field) {
    const auto &type = schema.at("properties").at(field).at("type");
    if (type != "integer") {
        throw std::runtime_error("MCP schema contract: " + field +
                                 " must advertise an integer workflow node ID");
    }
}

void run() {
    mcp::Session session;
    const auto initialized = session.handle({{"jsonrpc", "2.0"},
                                             {"method", "initialized"}});
    if (!initialized.is_null()) {
        throw std::runtime_error("MCP schema contract: initialized notification must not return a response");
    }

    const auto response = session.handle({{"jsonrpc", "2.0"}, {"id", 1},
                                          {"method", "tools/list"}});
    const auto &tools = response.at("result").at("tools");
    const auto &run_workflow = tool_named(tools, "run_workflow").at("inputSchema");
    const auto &mode = run_workflow.at("properties").at("execution_mode");
    if (mode.at("enum") != Json::array({"detailed", "pipeline"}) ||
        mode.at("default") != "detailed" ||
        run_workflow.at("required") != Json::array({"database_path"})) {
        throw std::runtime_error("MCP schema contract: run_workflow execution mode schema is misaligned");
    }

    const auto &transition = tool_named(tools, "transition_execution").at("inputSchema");
    if (!transition.at("properties").contains("execution_mode") ||
        transition.at("required") != Json::array({"database_path", "status"})) {
        throw std::runtime_error("MCP schema contract: transition_execution mode must be optional and advertised");
    }

    const auto &workflow_execution =
        tool_named(tools, "get_workflow_execution").at("inputSchema");
    if (!workflow_execution.at("properties").contains("operation_instance") ||
        workflow_execution.at("properties").at("operation_instance").at("type") != "integer" ||
        workflow_execution.at("required") != Json::array({"database_path"})) {
        throw std::runtime_error(
            "MCP schema contract: operation status polling must accept an optional operation_instance");
    }

    const auto &set_workflow = tool_named(tools, "set_workflow").at("inputSchema");
    const auto &submitted_workflow = set_workflow.at("properties").at("workflow");
    if (submitted_workflow.at("required") !=
            Json::array({"schema_version", "definition_revision", "operations", "connections"}) ||
        submitted_workflow.at("properties").at("definition_revision").at("type") != "integer") {
        throw std::runtime_error(
            "MCP schema contract: set_workflow must require the latest definition_revision");
    }

    const auto &add = tool_named(tools, "add_operation").at("inputSchema");
    if (add.at("properties").contains("operation_id") ||
        add.at("required") != Json::array({"database_path", "operation"})) {
        throw std::runtime_error(
            "MCP schema contract: add_operation must omit operation_id and assign it automatically");
    }
    const auto &connect = tool_named(tools, "connect_operations").at("inputSchema");
    require_node_reference(connect, "source_operation");
    require_node_reference(connect, "target_operation");
    for (const auto &port : {"source_port", "target_port"}) {
        if (connect.at("properties").at(port).at("type") != "string") {
            throw std::runtime_error("MCP schema contract: semantic port IDs must remain strings");
        }
    }

    const auto missing_resources =
        std::filesystem::current_path() / "tmp" / "projects" / "mcp-missing-workflow-resources";
    mcp::Session resource_session(
        mcp::operations(), {}, {}, {}, {},
        [missing_resources] { return std::vector<std::filesystem::path>{missing_resources}; });
    const auto demos = resource_session.handle(
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
         {"params", {{"name", "list_workflow_demos"}, {"arguments", Json::object()}}}});
    const auto demo_result = demos.at("result").at("content").at(0).at("text").get<std::string>();
    const auto demo_payload = Json::parse(demo_result);
    if (!demo_payload.contains("diagnostics") || demo_payload.at("diagnostics").size() != 1 ||
        demo_payload.at("diagnostics").at(0).value("path", "") != missing_resources.string() ||
        demo_payload.at("diagnostics").at(0).value("message", "") !=
            "workflow resources directory does not exist") {
        throw std::runtime_error(
            "MCP resource contract: missing workflow demo directory must be reported in diagnostics");
    }
}

} // namespace streamfind::mcp_schema_contract

int main() {
    try {
        streamfind::mcp_schema_contract::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
