#include "streamfind/project_table_store.hpp"
#include "streamfind/catalogue.hpp"

#include <algorithm>
#include <sstream>

namespace streamfind {

ProjectTableStore::ProjectTableStore(Project &project) noexcept : project_(&project) {}

bool ProjectTableStore::has_table(const std::string &table_name) const {
    const auto tables = project_->list_tables();
    return std::find(tables.begin(), tables.end(), table_name) != tables.end();
}

void ProjectTableStore::require(const std::vector<TableRequirement> &requirements) const {
    for (const auto &requirement : requirements) {
        if (!has_table(requirement.name))
            throw Error(ErrorCode::SchemaMismatch, "missing domain table: " + requirement.name);
        if (requirement.required_columns.empty() && requirement.required_column_types.empty()) continue;
        const auto rows = project_->query_json("DESCRIBE \"" + requirement.name + "\"");
        for (const auto &column : requirement.required_columns) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&column](const auto &row) {
                return row.value("column_name", "") == column;
            });
            if (found == rows.end())
                throw Error(ErrorCode::SchemaMismatch,
                            "missing column " + column + " in domain table " + requirement.name);
        }
        for (const auto &[column, semantic_type] : requirement.required_column_types) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&column](const auto &row) {
                return row.value("column_name", "") == column;
            });
            if (found == rows.end())
                throw Error(ErrorCode::SchemaMismatch,
                            "missing column " + column + " in domain table " + requirement.name);
            const std::string actual = found->value("column_type", "");
            const auto hash = semantic_type.rfind('#');
            const std::string expected = hash == std::string::npos ? semantic_type : semantic_type.substr(hash + 1);
            const bool compatible = expected == "string" ? (actual == "VARCHAR" || actual == "TEXT")
                : expected == "integer" ? actual.find("INT") != std::string::npos
                : expected == "real" ? (actual == "DOUBLE" || actual == "FLOAT" || actual == "DECIMAL")
                : expected == "boolean" ? actual == "BOOLEAN"
                : expected == "timestamp" ? actual.find("TIMESTAMP") != std::string::npos
                : true;
            if (!compatible)
                throw Error(ErrorCode::SchemaMismatch, "incompatible type for " + column + " in domain table " +
                                                        requirement.name + ": expected " + expected + ", got " + actual);
        }
    }
}

void ProjectTableStore::require_manifest(const std::string &domain) const {
    const auto manifest = catalogue::table_manifest_json(domain);
    if (!manifest) throw Error(ErrorCode::DatabaseError, "generated table manifest unavailable");
    std::vector<TableRequirement> requirements;
    for (const auto &table : *manifest) {
        TableRequirement requirement;
        requirement.name = table.value("table_name", "");
        for (const auto &column : table.value("columns", Json::array()))
            requirement.required_column_types.emplace_back(column.value("name", ""), column.value("type", ""));
        requirements.push_back(std::move(requirement));
    }
    require(requirements);
}

void ProjectTableStore::ensure_table(const std::string &, const std::string &ddl) const {
    project_->execute_sql(ddl);
}

Json ProjectTableStore::query(const std::string &sql) const { return project_->query_json(sql); }

void ProjectTableStore::execute(const std::string &sql) const { project_->execute_sql(sql); }

void ProjectTableStore::append(
    const std::string &table_name, const std::vector<std::string> &column_names,
    const std::vector<std::vector<std::optional<std::string>>> &rows) const {
    project_->append_rows(table_name, column_names, rows);
}

}  // namespace streamfind
