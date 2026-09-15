#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../tmp_projects.hpp"

int main() {
    try {
    const auto path = streamfind::test::tmp_projects_dir() / "dynamic_schema_install_test.duckdb";
    std::filesystem::create_directories(path.parent_path());
    std::error_code error;
    std::filesystem::remove(path, error);

    streamfind::catalogue::set_runtime_document({
        {"version", 2},
        {"entries", streamfind::Json::array()},
        {"tables", streamfind::Json::array({streamfind::Json{
            {"table_name", "DYNAMIC_SCHEMA_TABLE"},
            {"domain", "test"},
            {"module_id", "test.module"},
            {"columns", streamfind::Json::array({
                streamfind::Json{{"name", "name"}, {"type", "string"}},
                streamfind::Json{{"name", "count"}, {"type", "integer"}},
                streamfind::Json{{"name", "ratio"}, {"type", "real"}},
                streamfind::Json{{"name", "created"}, {"type", "timestamp"}},
                streamfind::Json{{"name", "amount"}, {"type", "decimal"}},
                streamfind::Json{{"name", "payload"}, {"type", "binary"}},
            })}}})}
    });

    streamfind::ProjectOptions options;
    options.database_path = path;
    options.domain = "test";
    auto project = streamfind::Project::create(options);
    streamfind::ProjectTableStore::install_manifest_schema(project, "test", "test.module");

    streamfind::ProjectTableStore tables(project);
    if (!tables.has_table("DYNAMIC_SCHEMA_TABLE"))
        throw std::runtime_error("manifest table was not installed");
    const auto state = project.query_json(
        "SELECT schema_version FROM MODULE_SCHEMA WHERE domain_id = 'test' AND module_id = 'test.module'");
    if (state.size() != 1 || state.at(0).value("schema_version", std::string{}) != "1")
        throw std::runtime_error("manifest schema state was not persisted");

    streamfind::ProjectTableStore::transaction(project, {"DYNAMIC_SCHEMA_TABLE"},
        [](streamfind::ProjectTableStore &tables) {
            tables.append("DYNAMIC_SCHEMA_TABLE",
                          {"name", "count", "ratio", "created", "amount", "payload"},
                          {{std::string("native"), std::string("7"), std::string("1.25"),
                            std::string("2026-09-12 19:45:01.123456"), std::string("123456789012345.678"),
                            std::string({'A', '\0', 'B'})},
                           {std::string("nullable"), std::string("8"), std::string("2.5"),
                            std::nullopt, std::nullopt, std::nullopt}});
        });
    const auto values = project.query_json(
        "SELECT name, CAST(created AS VARCHAR) AS created, CAST(amount AS VARCHAR) AS amount, "
        "octet_length(payload) AS payload_size, hex(payload) AS payload_hex FROM DYNAMIC_SCHEMA_TABLE ORDER BY count");
    if (values.size() != 2 || values.at(0).value("created", "") != "2026-09-12 19:45:01.123456" ||
        values.at(0).value("amount", "") != "123456789012345.678" ||
        values.at(0).value("payload_size", "") != "3" || values.at(0).value("payload_hex", "") != "410042")
        throw std::runtime_error("native timestamp, decimal, or blob append failed");
    return 0;
    } catch (const std::exception &error) {
        std::cerr << "failure: " << error.what() << '\n';
        return 3;
    }
}
