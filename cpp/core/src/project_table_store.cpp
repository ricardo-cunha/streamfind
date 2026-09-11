#include "streamfind/project_table_store.hpp"
#include "streamfind/catalogue.hpp"

#include <algorithm>
#include <cctype>
#include <duckdb.h>
#include <sstream>

namespace streamfind {

struct ProjectTableStore::Impl {
    duckdb_database database{nullptr};
    duckdb_connection connection{nullptr};
    std::vector<std::string> owned_tables;
};

namespace detail {

void close_store_connection(ProjectTableStore::Impl &impl) noexcept {
    if (impl.connection) duckdb_disconnect(&impl.connection);
    if (impl.database) duckdb_close(&impl.database);
}

void store_query(ProjectTableStore::Impl &impl, const std::string &sql) {
    duckdb_result result{};
    if (duckdb_query(impl.connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "DuckDB query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    duckdb_destroy_result(&result);
}

Json store_query_json(ProjectTableStore::Impl &impl, const std::string &sql) {
    duckdb_result result{};
    if (duckdb_query(impl.connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "DuckDB query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    Json rows = Json::array();
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        Json object = Json::object();
        for (idx_t column = 0; column < duckdb_column_count(&result); ++column) {
            const auto name = duckdb_column_name(&result, column);
            if (duckdb_value_is_null(&result, column, row)) object[name] = nullptr;
            else {
                char *value = duckdb_value_varchar(&result, column, row);
                object[name] = value ? value : "";
                if (value) duckdb_free(value);
            }
        }
        rows.push_back(std::move(object));
    }
    duckdb_destroy_result(&result);
    return rows;
}

bool owns(const ProjectTableStore::Impl &impl, const std::string &table_name) {
    return impl.owned_tables.empty() ||
           std::find(impl.owned_tables.begin(), impl.owned_tables.end(), table_name) != impl.owned_tables.end();
}

void require_owned(const ProjectTableStore::Impl &impl, const std::string &table_name) {
    if (!owns(impl, table_name))
        throw Error(ErrorCode::SchemaMismatch, "table is not owned by module: " + table_name);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

void require_sql_ownership(const ProjectTableStore::Impl &impl, const std::string &sql) {
    if (impl.owned_tables.empty()) return;
    const auto statement = lower_copy(sql);
    if (statement.find("begin ") == 0 || statement.find("commit") == 0 ||
        statement.find("rollback") == 0 || statement.find("pragma") == 0 ||
        statement.find("module_schema") != std::string::npos) return;
    for (const auto &table : impl.owned_tables) {
        if (statement.find(lower_copy(table)) != std::string::npos) return;
    }
    throw Error(ErrorCode::SchemaMismatch, "SQL does not reference a table owned by module");
}

}  // namespace detail

ProjectTableStore::ProjectTableStore(Project &project) noexcept : project_(&project) {}

ProjectTableStore::ProjectTableStore(Project &project, std::vector<std::string> owned_tables)
    : project_(&project), impl_(std::make_unique<Impl>()) {
    impl_->owned_tables = std::move(owned_tables);
    if (duckdb_open(project.get_database_path().string().c_str(), &impl_->database) == DuckDBError)
        throw Error(ErrorCode::DatabaseError, "open transaction project database failed");
    if (duckdb_connect(impl_->database, &impl_->connection) == DuckDBError) {
        duckdb_close(&impl_->database);
        throw Error(ErrorCode::DatabaseError, "connect transaction project database failed");
    }

}

ProjectTableStore::~ProjectTableStore() {
    if (impl_) detail::close_store_connection(*impl_);
}

void ProjectTableStore::transaction(Project &project,
                                    const std::vector<std::string> &owned_tables,
                                    const std::function<void(ProjectTableStore &)> &callback) {
    ProjectTableStore store(project, owned_tables);
    detail::store_query(*store.impl_, "BEGIN TRANSACTION");
    try {
        callback(store);
        detail::store_query(*store.impl_, "COMMIT");
    } catch (...) {
        try { detail::store_query(*store.impl_, "ROLLBACK"); } catch (...) {}
        throw;
    }
}

bool ProjectTableStore::has_table(const std::string &table_name) const {
    if (impl_) {
        duckdb_result result{};
        const std::string sql = "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' AND table_name = '" +
                                table_name + "' LIMIT 1";
        if (duckdb_query(impl_->connection, sql.c_str(), &result) == DuckDBError) {
            const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "inspect tables failed";
            duckdb_destroy_result(&result);
            throw Error(ErrorCode::DatabaseError, message);
        }
        const bool found = duckdb_row_count(&result) != 0;
        duckdb_destroy_result(&result);
        return found;
    }
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

void ProjectTableStore::require_manifest(const std::string &domain, const std::string &module_id) const {
    const auto manifest = catalogue::table_manifest_json(domain, module_id);
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

void ProjectTableStore::require_installed_manifest(const std::string &domain,
                                                   const std::string &module_id,
                                                   int minimum_version) const {
    try {
        const auto rows = project_->query_json("SELECT schema_version FROM MODULE_SCHEMA WHERE domain_id = '" +
                                               domain + "' AND module_id = '" + module_id + "' LIMIT 1");
        if (rows.empty() || rows.at(0).value("schema_version", "") < std::to_string(minimum_version))
            throw Error(ErrorCode::SchemaMismatch, "module schema is not installed: " + module_id);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &) {
        throw Error(ErrorCode::SchemaMismatch, "module schema is not installed: " + module_id);
    }
    require_manifest(domain, module_id);
}

void ProjectTableStore::ensure_table(const std::string &table_name, const std::string &ddl) const {
    if (impl_) {
        detail::require_owned(*impl_, table_name);
        detail::store_query(*impl_, ddl);
        return;
    }
    project_->execute_sql(ddl);
}

Json ProjectTableStore::query(const std::string &sql) const {
    if (impl_) {
        detail::require_sql_ownership(*impl_, sql);
        return detail::store_query_json(*impl_, sql);
    }
    return project_->query_json(sql);
}

std::optional<std::string> ProjectTableStore::scalar(const std::string &sql) const {
    if (impl_) detail::require_sql_ownership(*impl_, sql);
    if (!impl_) {
        const auto rows = project_->query_json(sql);
        if (rows.empty() || rows.at(0).empty()) return std::nullopt;
        return rows.at(0).begin().value().dump();
    }
    duckdb_result result{};
    if (duckdb_query(impl_->connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "scalar query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    if (duckdb_row_count(&result) == 0) {
        duckdb_destroy_result(&result);
        return std::nullopt;
    }
    char *value = duckdb_value_varchar(&result, 0, 0);
    std::optional<std::string> output = value ? std::optional<std::string>(value) : std::nullopt;
    if (value) duckdb_free(value);
    duckdb_destroy_result(&result);
    return output;
}

void ProjectTableStore::execute(const std::string &sql) const {
    if (impl_) {
        detail::require_sql_ownership(*impl_, sql);
        detail::store_query(*impl_, sql);
        return;
    }
    project_->execute_sql(sql);
}

void ProjectTableStore::append(
    const std::string &table_name, const std::vector<std::string> &column_names,
    const std::vector<std::vector<std::optional<std::string>>> &rows) const {
    if (impl_) detail::require_owned(*impl_, table_name);
    project_->append_rows(table_name, column_names, rows);
}

}  // namespace streamfind
