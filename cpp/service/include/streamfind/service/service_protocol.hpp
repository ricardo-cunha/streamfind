#pragma once

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"

#include <string>
#include <set>
#include <vector>
#include <algorithm>

namespace streamfind::service {

inline constexpr const char *kServiceName = "streamfind-server";
inline constexpr const char *kProtocolVersion = "1.0";

struct SessionDto {
    std::string service{kServiceName};
    std::string protocol_version{kProtocolVersion};
    std::string state{"ready"};
    std::string backend_version{"development"};
};

struct ProjectSessionDto {
    std::string session_id;
    std::string project_path;
    std::uintmax_t database_size_bytes{0};
    std::vector<std::string> plugins;
    Json metadata = Json::object();
    Json result_store = Json::object();
    Json diagnostics = Json::array();
};

inline void to_json(Json &json, const SessionDto &value) {
    json = Json{{"service", value.service}, {"protocol_version", value.protocol_version},
                {"state", value.state}, {"backend_version", value.backend_version}};
}

inline void to_json(Json &json, const ProjectSessionDto &value) {
    json = Json{{"session_id", value.session_id}, {"project_path", value.project_path},
                {"database_size_bytes", value.database_size_bytes},
                {"plugins", value.plugins}, {"metadata", value.metadata},
                {"result_store", value.result_store}, {"diagnostics", value.diagnostics}};
}

inline Json capability_operations_json(const std::string &plugin,
                                       const std::string &module = {},
                                       const std::string &search = {},
                                       bool include_schema = false) {
    Json operations = Json::array();
    const auto entries = catalogue::entries_json();
    if (!entries) return operations;
    for (const auto &entry : *entries) {
        if (entry.value("kind", "") != "operation" || !entry.value("exposed", false) ||
            !entry.value("executable", false) || entry.value("plugin", "") != plugin)
            continue;
        if (!module.empty() && entry.value("module_id", "") != module) continue;
        const auto canonical_id = entry.value("canonical_id", "");
        const auto label = entry.value("label", canonical_id);
        if (!search.empty() && canonical_id.find(search) == std::string::npos && label.find(search) == std::string::npos)
            continue;
        if (!include_schema) {
            operations.push_back(Json{{"canonical_id", canonical_id}, {"label", label},
                                      {"plugin", plugin}, {"module_id", entry.value("module_id", "")},
                                      {"definition", entry.value("definition", "")}});
        } else {
            operations.push_back(entry);
        }
    }
    return operations;
}

inline Json capability_index_json() {
    std::set<std::string> plugins;
    std::set<std::pair<std::string, std::string>> modules;
    const auto entries = catalogue::entries_json();
    if (entries) for (const auto &entry : *entries) {
        if (entry.value("kind", "") != "operation" || !entry.value("exposed", false) ||
            !entry.value("executable", false))
            continue;
        const auto plugin = entry.value("plugin", "");
        plugins.insert(plugin);
        modules.emplace(plugin, entry.value("module_id", ""));
    }
    Json module_values = Json::array();
    for (const auto &[plugin, module] : modules)
        module_values.push_back(Json{{"plugin", plugin}, {"module_id", module}});
    return Json{{"protocol_version", kProtocolVersion},
                {"plugins", std::vector<std::string>(plugins.begin(), plugins.end())},
                {"modules", module_values}};
}

inline Json capabilities_json() {
    const auto entries = catalogue::entries_json();
    std::set<std::string> plugins;
    Json projected_entries = Json::array();
    Json methods = Json::array();
    if (entries) for (const auto &raw_entry : *entries) {
        Json entry = raw_entry;
        if (entry.contains("plugin") && entry.at("plugin").is_string()) plugins.insert(entry.at("plugin").get<std::string>());
        if (entry.value("kind", "") == "method") {
            entry["canvas"] = Json{{"node_kind", "method"}, {"outputs_to_canvas", false}, {"chainable", true}};
            methods.push_back(entry);
        } else if (entry.value("kind", "") == "operation") {
            Json output_results = Json::array();
            const auto result = entry.value("result", Json::object());
            const auto result_id = result.value("id", "");
            if (!result_id.empty()) {
                output_results.push_back(Json{{"canonical_id", result_id},
                                              {"label", result_id},
                                              {"definition", result.value("schema", Json::object()).value("description", "")},
                                              {"table", Json{{"table_contract_name", result_id},
                                                               {"plugin", entry.value("plugin", "")},
                                                               {"module_id", entry.value("module_id", "")},
                                                               {"columns", Json::array()}}},
                                              {"schema", result.value("schema", Json::object())}});
            }
            entry["canvas"] = Json{{"node_kind", "operation"},
                                    {"outputs_to_canvas", true},
                                    {"output_results", output_results}};
        }
        projected_entries.push_back(std::move(entry));
    }
    return Json{{"protocol_version", kProtocolVersion},
                {"operations", projected_entries},
                {"methods", methods},
                {"plugins", std::vector<std::string>(plugins.begin(), plugins.end())},
                                              {"endpoints", Json::array({ "/health", "/session", "/service/stop", "/capabilities", "/capabilities/index", "/capabilities/plugins/<plugin>/modules", "/capabilities/operations?plugin=...&module=...", "/capabilities/operations/<canonical_id>", "/workflow-demos", "/chemistry/structure-svg", "/chemistry/isotope-pattern", "/projects", "/projects/<session_id>", "/projects/<session_id>/workflow", "/projects/<session_id>/workflow/validate", "/projects/<session_id>/workflow/run", "/projects/<session_id>/workflow/cancel", "/projects/<session_id>/results/cache/lookup", "/projects/<session_id>/results/trace", "/events" })}};
}

inline Json project_initialization_json(const std::vector<std::string> &plugins) {
    const auto entries = catalogue::entries_json();
    Json operations = Json::array();
    if (entries) for (const auto &entry : *entries) {
        if (entry.value("kind", "") != "operation" || !entry.value("project_entry", false)) continue;
        if (!plugins.empty() && std::find(plugins.begin(), plugins.end(), entry.value("plugin", "")) == plugins.end()) continue;
        operations.push_back(entry);
    }
    return Json{{"required", plugins.empty()}, {"state", plugins.empty() ? "awaiting_input" : "ready"}, {"operations", operations}};
}

}  // namespace streamfind::service
