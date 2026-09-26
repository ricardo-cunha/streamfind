#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "streamfind/mcp.hpp"

namespace {

streamfind::Json call(streamfind::mcp::Session &session, int id, const std::string &name,
                      streamfind::Json arguments = streamfind::Json::object()) {
    return session.handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                           {"params", {{"name", name}, {"arguments", arguments}}}});
}

streamfind::Json text_json(const streamfind::Json &response) {
    const auto text = response.at("result").at("content").at(0).at("text").get<std::string>();
    return streamfind::Json::parse(text);
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        streamfind::mcp::Session session;
        const auto listed = session.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
        const auto tools = listed.at("result").at("tools");
        bool has_domains = false;
        bool has_modules = false;
        bool has_operations = false;
        bool has_operation = false;
        bool has_run_operation = false;
        for (const auto &tool : tools) {
            const auto name = tool.value("name", "");
            require(name.rfind("mass_spec.", 0) != 0, "operation-specific MCP tool leaked into tools/list");
            has_domains = has_domains || name == "get_domains";
            has_modules = has_modules || name == "get_modules";
            has_operations = has_operations || name == "get_operations";
            has_operation = has_operation || name == "get_operation";
            has_run_operation = has_run_operation || name == "run_operation";
        }
        require(has_domains && has_modules && has_operations && has_operation && has_run_operation,
                "stable MCP discovery tools are incomplete");

        const auto domains = text_json(call(session, 2, "get_domains"));
        require(domains.is_array() && std::find(domains.begin(), domains.end(), "mass_spec") != domains.end(),
                "mass_spec was not discoverable");

        const auto operations = text_json(call(session, 3, "get_operations",
                                                {{"domain", "mass_spec"}, {"module", "mass_spec.nta"}}));
        require(operations.is_array() && !operations.empty(), "mass_spec.nta operations were not discoverable");
        const auto first = operations.at(0);
        const std::set<std::string> summary_fields = {"canonical_id", "label", "domain", "module_id", "definition"};
        require(first.size() == summary_fields.size(), "operation summary has unexpected fields");
        for (const auto &field : summary_fields)
            require(first.contains(field) && first.at(field).is_string(), "operation summary field is missing or not a string");
        require(first.value("canonical_id", "").rfind("mass_spec.", 0) == 0 &&
                    first.value("module_id", "") == "mass_spec.nta",
                "filtered operation summary is not canonical");
        require(!first.contains("parameters") && !first.contains("input_ports") && !first.contains("executable"),
                "operation summary leaked full catalogue detail");
        for (const auto &operation : operations)
            require(operation.value("domain", "") == "mass_spec" &&
                        operation.value("module_id", "") == "mass_spec.nta",
                    "operation filtering returned the wrong module");

        const auto searched = text_json(call(session, 4, "get_operations",
                                             {{"domain", "mass_spec"}, {"search", "nta"}}));
        require(searched.is_array() && !searched.empty() && searched.size() <= operations.size(),
                "operation search returned an invalid result");
        for (const auto &operation : searched)
            require(operation.value("canonical_id", "").find("nta") != std::string::npos ||
                        operation.value("label", "").find("nta") != std::string::npos ||
                        operation.value("definition", "").find("nta") != std::string::npos ||
                        operation.value("module_id", "").find("nta") != std::string::npos,
                    "operation search returned a non-matching summary");

        const auto detail = text_json(call(session, 5, "get_operation",
                                           {{"operation", first.at("canonical_id")}}));
        require(detail.is_array() && detail.size() == 1, "operation detail lookup returned the wrong shape");
        require(detail.at(0).value("canonical_id", "") == first.at("canonical_id").get<std::string>(),
                "operation detail does not match the requested operation");
        require(detail.at(0).value("exposed", false) && detail.at(0).value("executable", false),
                "operation detail is not an exposed executable operation");
        require(detail.at(0).contains("input_ports") || detail.at(0).contains("parameters"),
                "operation detail omitted its schema");

        const auto missing = call(session, 5, "get_operation", {{"operation", "mass_spec.no_such_operation"}});
        require(missing.contains("result") && missing.at("result").value("isError", false),
                "unknown operation did not return an MCP error result");
        std::cout << "MCP discovery contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
