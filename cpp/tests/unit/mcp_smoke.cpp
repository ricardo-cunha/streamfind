#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "streamfind/catalogue.hpp"
#include "streamfind/mcp.hpp"
#include "streamfind/mass_spec/register.hpp"
#include "streamfind/raman/register.hpp"
#include "streamfind/sensors/register.hpp"
#include "../tmp_projects.hpp"

#ifndef STREAMFIND_CATALOGUE_PATH
#error STREAMFIND_CATALOGUE_PATH is required
#endif

namespace {

/// Core tool names `tools/list` must advertise, derived from the committed
/// semantic catalogue (the same file the MCP servers serve from): exposed
/// operations of the `streamfind` domain. Tests adapt automatically when
/// operations are added or removed — no hardcoded counts.
std::vector<std::string> expected_core_tools(const streamfind::Json &entries) {
    std::vector<std::string> names;
    for (const auto &entry : entries) {
        if (entry.value("kind", "") == "operation" &&
            entry.value("domain", "") == "streamfind" &&
            entry.value("exposed", false)) {
            names.push_back(entry.at("mcp").at("name").get<std::string>());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// The full expected tool set: core tools + all registered domain operations
/// (methods are never tools). Operations are always advertised because they
/// are stateless and carry their own database_path.
std::vector<std::string> expected_all_tools(const streamfind::Json &entries,
                                             const streamfind::OperationRegistry &operations) {
    auto names = expected_core_tools(entries);
    for (const auto &definition : operations.list("")) names.push_back(definition.id);
    std::sort(names.begin(), names.end());
    return names;
}

/// Assert a tools/list payload advertises exactly the expected tool names.
void assert_advertises(const streamfind::Json &tools, const std::vector<std::string> &expected) {
    std::vector<std::string> actual;
    for (const auto &tool : tools) actual.push_back(tool.at("name").get<std::string>());
    std::sort(actual.begin(), actual.end());
    assert(actual == expected);
    assert(tools.size() == expected.size());
    for (const auto &tool : tools) assert(tool.at("inputSchema").at("type") == "object");
}

}  // namespace

int main() {
    const char *phase = "startup";
    try {
    // Load the catalogue explicitly so the test exercises the same artifact the
    // MCP servers query at runtime.
    const auto entries = streamfind::catalogue::load(STREAMFIND_CATALOGUE_PATH);
    assert(entries && "catalogue load failed");
    const auto expected_core = expected_core_tools(*entries);

    phase = "global tools/list";
    const auto response = streamfind::mcp::handle({{"id", 1}, {"method", "tools/list"}});

    const auto tools = response.at("result").at("tools");
    assert_advertises(tools, expected_core);
    phase = "initialize";
    const auto initialize = streamfind::mcp::handle({{"id", 0}, {"method", "initialize"}});
    const auto instructions = initialize.at("result").at("instructions").get<std::string>();
    for (const auto &term : {"create", "describe", "tools/list", "get_available_methods", "connect"}) {
        assert(instructions.find(term) != std::string::npos);
    }
    // A well-known core tool keeps a behavioural contract check: its required
    // parameters are statically meaningful and change only with that operation.
    const auto create = std::find_if(tools.begin(), tools.end(), [](const auto &tool) {
        return tool.at("name") == "create";
    });
    assert(create != tools.end());
    const auto required = create->at("inputSchema").at("required");
    assert(std::find(required.begin(), required.end(), "database_path") != required.end());

    const auto path = streamfind::test::tmp_projects_dir() / "streamfind-mcp-lifecycle.duckdb";
    std::error_code error;
    std::filesystem::remove(path, error);
    streamfind::MethodRegistry registry;
    streamfind::OperationRegistry operations;
    streamfind::mass_spec::register_plugin(*entries, registry, operations);
    streamfind::raman::register_plugin(*entries, registry, operations);
    streamfind::sensors::register_plugin(*entries, registry, operations);
    const auto expected_all = expected_all_tools(*entries, operations);
    streamfind::mcp::Session session(registry, operations);
    phase = "get_available_methods";
    const auto methods_call = session.handle({{"id", 9}, {"method", "tools/call"}, {"params", {
        {"name", "get_available_methods"}, {"arguments", {{"domain", "mass_spec"}}}
    }}});
    const auto methods = streamfind::Json::parse(methods_call.at("result").at("content").at(0).at("text").get<std::string>());
    const auto find_features = std::find_if(methods.begin(), methods.end(), [](const auto &method) {
        return method.at("id") == "mass_spec.find_features";
    });
    assert(find_features != methods.end());
    assert(find_features->at("inputSchema").at("type") == "object");
    assert(find_features->at("inputSchema").at("properties").is_object());

    phase = "create";
    const auto create_call = session.handle({{"id", 2}, {"method", "tools/call"}, {"params", {
        {"name", "create"}, {"arguments", {{"database_path", path.string()}, {"domain", "mass_spec"}}}
    }}});
    if (create_call.at("result").value("isError", false)) {
        std::cerr << "create failed\n";
        return 1;
    }
    const auto before_connect = session.handle({{"id", 3}, {"method", "tools/list"}}).at("result").at("tools");
    assert_advertises(before_connect, expected_all);
    for (const auto &operation : {"raman.add_analyses", "raman.remove_analyses"}) {
        const auto advertised = std::find_if(before_connect.begin(), before_connect.end(),
                                             [operation](const auto &tool) {
                                                 return tool.at("name") == operation;
                                             });
        assert(advertised != before_connect.end());
        assert(advertised->at("inputSchema").at("type") == "object");
    }
    const auto sensors_tool = std::find_if(before_connect.begin(), before_connect.end(), [](const auto &tool) {
        return tool.at("name").template get<std::string>().rfind("sensors.", 0) == 0;
    });
    assert(sensors_tool == before_connect.end());
    const auto eic_before_connect = std::find_if(before_connect.begin(), before_connect.end(), [](const auto &tool) {
        return tool.at("name") == "mass_spec.get_raw_spectra_eic";
    });
    assert(eic_before_connect != before_connect.end());
    assert(eic_before_connect->at("inputSchema").at("properties").at("targets").at("items").contains("properties"));
    const auto pre_connect_info = session.handle({{"id", 10}, {"method", "tools/call"}, {"params", {
        {"name", "mass_spec.get_analyses_info"}, {"arguments", {{"database_path", path.string()}}}
    }}});
    assert(!pre_connect_info.at("result").value("isError", false));
    phase = "connect";
    const auto connect = session.handle({{"id", 4}, {"method", "tools/call"}, {"params", {
        {"name", "connect"}, {"arguments", {{"database_path", path.string()}}}
    }}});
    if (connect.at("result").value("isError", false)) {
        std::cerr << "connect failed\n";
        return 1;
    }
    const auto connected_tools = session.handle({{"id", 5}, {"method", "tools/list"}}).at("result").at("tools");
        assert_advertises(connected_tools, expected_all);
    const auto eic_tool = std::find_if(connected_tools.begin(), connected_tools.end(), [](const auto &tool) {
        return tool.at("name") == "mass_spec.get_raw_spectra_eic";
    });
    if (eic_tool == connected_tools.end()) {
        std::cerr << "mass-spec target schema mismatch: EIC tool absent\n";
        return 1;
    }
    const auto &eic_schema = eic_tool->at("inputSchema");
    const auto &targets_schema = eic_schema.at("properties").at("targets");
    const auto &target_item_schema = targets_schema.at("items");
    const bool schema_ok = targets_schema.at("type") == "array" &&
                           target_item_schema.at("type") == "object" &&
                           eic_schema.at("properties").at("rt_tolerance").at("type") == "number" &&
                           !targets_schema.at("examples").empty() &&
                           targets_schema.at("examples")[0][0].at("id") == "caffeine" &&
                           target_item_schema.contains("properties") &&
                           target_item_schema.at("properties").contains("mz");
    if (!schema_ok) {
        std::cerr << "mass-spec target schema mismatch: " << eic_schema.dump() << '\n';
        return 1;
    }
    const auto info = session.handle({{"id", 5}, {"method", "tools/call"}, {"params", {
        {"name", "mass_spec.get_analyses_info"}, {"arguments", {{"database_path", path.string()}}}
    }}});
    if (info.at("result").value("isError", false) ||
        streamfind::Json::parse(info.at("result").at("content").at(0).at("text").get<std::string>()).at("row_count") != 0) {
        std::cerr << "mass_spec.get_analyses_info failed\n";
        return 1;
    }
    const auto close = session.handle({{"id", 6}, {"method", "tools/call"}, {"params", {
        {"name", "close"}, {"arguments", {{"database_path", path.string()}}}
    }}});
    if (close.at("result").value("isError", false)) {
        std::cerr << "close failed\n";
        return 1;
    }
    assert_advertises(session.handle({{"id", 8}, {"method", "tools/list"}}).at("result").at("tools"), expected_all);
    std::filesystem::remove(path, error);
    } catch (const std::exception &error) {
        std::cerr << "mcp_smoke exception during " << phase << ": " << error.what() << '\n';
        return 1;
    }
}