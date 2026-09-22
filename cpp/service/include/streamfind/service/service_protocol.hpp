#pragma once

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"

#include <string>
#include <set>
#include <vector>

namespace streamfind::service {

inline constexpr const char *kServiceName = "streamfind_service";
inline constexpr const char *kProtocolVersion = "1.0";

struct SessionDto {
    std::string service{kServiceName};
    std::string protocol_version{kProtocolVersion};
    std::string state{"ready"};
    std::string backend_version{"development"};
};

struct ProjectSessionDto {
    std::string session_id;
    std::string database_path;
    std::uintmax_t database_size_bytes{0};
    std::string domain;
    Json metadata{Json::object()};
};

inline void to_json(Json &json, const SessionDto &value) {
    json = Json{{"service", value.service}, {"protocol_version", value.protocol_version},
                {"state", value.state}, {"backend_version", value.backend_version}};
}

inline void to_json(Json &json, const ProjectSessionDto &value) {
    json = Json{{"session_id", value.session_id}, {"database_path", value.database_path},
                {"database_size_bytes", value.database_size_bytes},
                {"domain", value.domain}, {"metadata", value.metadata}};
}

inline Json capabilities_json() {
    const auto entries = catalogue::entries_json();
    std::set<std::string> domains;
    Json projected_entries = Json::array();
    Json methods = Json::array();
    if (entries) for (const auto &raw_entry : *entries) {
        Json entry = raw_entry;
        if (entry.contains("domain") && entry.at("domain").is_string()) domains.insert(entry.at("domain").get<std::string>());
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
                                              {"table", Json{{"table_name", result_id},
                                                               {"domain", entry.value("domain", "")},
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
                {"domains", std::vector<std::string>(domains.begin(), domains.end())},
                {"endpoints", Json::array({ "/session", "/capabilities", "/projects", "/projects/<session_id>", "/projects/<session_id>/workflow", "/projects/<session_id>/workflow/validate", "/projects/<session_id>/workflow/run", "/projects/<session_id>/workflow/pause", "/projects/<session_id>/workflow/cancel", "/events" })}};
}

inline Json project_initialization_json(const std::string &domain) {
    const auto entries = catalogue::entries_json();
    if (entries) for (const auto &entry : *entries) {
        if (entry.value("kind", "") == "operation" && entry.value("domain", "") == domain && entry.value("project_entry", false))
            return Json{{"required", true}, {"state", "awaiting_input"}, {"operation_id", entry.value("canonical_id", "")}, {"operation", entry}};
    }
    return Json{{"required", false}, {"state", "not_required"}};
}

}  // namespace streamfind::service
