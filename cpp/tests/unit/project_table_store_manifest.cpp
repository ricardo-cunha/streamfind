#include <cassert>
#include <array>
#include <filesystem>
#include <string>
#include <tuple>
#include <iostream>

#include "streamfind/catalogue.hpp"
#include "streamfind/project.hpp"
#include "streamfind/project_table_store.hpp"
#include "../tmp_projects.hpp"

namespace streamfind::test::manifest_proof {
std::string sql_type(const std::string &kind) {
    const auto hash = kind.rfind('#');
    const auto name = kind.substr(hash == std::string::npos ? 0 : hash + 1);
    if (name == "boolean") return "BOOLEAN";
    if (name == "integer") return "INTEGER";
    if (name == "real") return "DOUBLE";
    if (name == "timestamp") return "TIMESTAMP";
    return "VARCHAR";
}

void install(streamfind::Project &project, bool omit_table, bool omit_column, bool wrong_type) {
    const auto manifest = streamfind::catalogue::table_manifest_json("mass_spec", "mass_spec.base");
    assert(manifest);
    std::string first_table;
    std::string first_column;
    for (const auto &table : *manifest) {
        if (table.at("table_name").get<std::string>().rfind("MASS_SPEC_", 0) == 0) {
            first_table = table.at("table_name");
            if (!table.at("columns").empty()) first_column = table.at("columns").front().at("name");
            break;
        }
    }
    for (const auto &table : *manifest) {
        const auto name = table.at("table_name").get<std::string>();
        if (omit_table && name == first_table) continue;
        std::string ddl = "CREATE TABLE IF NOT EXISTS \"" + name + "\" (";
        bool first = true;
        for (const auto &column : table.at("columns")) {
            const auto column_name = column.at("name").get<std::string>();
            if (omit_column && column_name == first_column && name == first_table) continue;
            if (!first) ddl += ", ";
            first = false;
            const auto type = wrong_type && column_name == first_column && name == first_table
                ? "INTEGER" : sql_type(column.at("type").get<std::string>());
            ddl += "\"" + column_name + "\" " + type;
        }
        ddl += ")";
        project.execute_sql(ddl);
    }
}

int run() {
    const auto base = streamfind::test::tmp_projects_dir();
    const std::array cases{
        std::tuple{"valid", false, false, false},
        std::tuple{"missing-table", true, false, false},
        std::tuple{"missing-column", false, true, false},
        std::tuple{"wrong-type", false, false, true},
    };
    for (const auto &[suffix, omit_table, omit_column, wrong_type] : cases) {
        const auto path = base / (std::string("manifest-proof-") + suffix + ".duckdb");
        std::error_code error;
        std::filesystem::remove(path, error);
        auto project = streamfind::Project::create({path, "mass_spec", {}});
        install(project, omit_table, omit_column, wrong_type);
        bool passed = true;
        try {
            streamfind::ProjectTableStore(project).require_manifest("mass_spec", "mass_spec.base");
        } catch (const streamfind::Error &exception) {
            std::cerr << suffix << ": " << exception.what() << '\n';
            passed = false;
        }
        if (suffix == std::string("valid") ? !passed : passed) return 1;
        project.close();
        std::filesystem::remove(path, error);
    }
    return 0;
}
} // namespace streamfind::test::manifest_proof

int main() { return streamfind::test::manifest_proof::run(); }
