/**
 * @file project.cpp
 * @brief Project persistence, method metadata, workflow execution, and cache storage.
 */

#include "streamfind/project.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <duckdb.h>

namespace streamfind {
namespace detail {

using Statement = duckdb_prepared_statement;

std::string db_error(duckdb_result &result) {
    const char *message = duckdb_result_error(&result);
    return message ? message : "DuckDB operation failed";
}

void check(duckdb_state state, const std::string &context) {
    if (state == DuckDBError) {
        throw Error(ErrorCode::DatabaseError, context);
    }
}

class ResultGuard {
public:
    explicit ResultGuard(duckdb_result &result) : result_(result) {}
    ~ResultGuard() { duckdb_destroy_result(&result_); }
    ResultGuard(const ResultGuard &) = delete;
    ResultGuard &operator=(const ResultGuard &) = delete;

private:
    duckdb_result &result_;
};

class StatementGuard {
public:
    explicit StatementGuard(Statement statement) : statement_(statement) {}
    ~StatementGuard() {
        if (statement_) duckdb_destroy_prepare(&statement_);
    }
    StatementGuard(const StatementGuard &) = delete;
    StatementGuard &operator=(const StatementGuard &) = delete;

private:
    Statement statement_;
};

class AppenderGuard {
public:
    explicit AppenderGuard(duckdb_appender *appender) : appender_(appender) {}
    ~AppenderGuard() {
        if (appender_ && *appender_) duckdb_appender_destroy(appender_);
    }
    AppenderGuard(const AppenderGuard &) = delete;
    AppenderGuard &operator=(const AppenderGuard &) = delete;

private:
    duckdb_appender *appender_;
};

void query(duckdb_connection connection, const std::string &sql,
           const std::string &context) {
    duckdb_result result{};
    if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError) {
        const std::string message = context + ": " + db_error(result);
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    duckdb_destroy_result(&result);
}

template <typename Bind, typename Read>
void prepared(duckdb_connection connection, const std::string &sql,
              const std::string &context, Bind bind, Read read) {
    Statement statement = nullptr;
    if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError) {
        const char *message = statement ? duckdb_prepare_error(statement) : nullptr;
        const std::string error = context + ": " + (message ? message : "prepare failed");
        if (statement) duckdb_destroy_prepare(&statement);
        throw Error(ErrorCode::DatabaseError, error);
    }
    StatementGuard statement_guard(statement);
    bind(statement);
    duckdb_result result{};
    if (duckdb_execute_prepared(statement, &result) == DuckDBError) {
        const std::string message = context + ": " + db_error(result);
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    ResultGuard result_guard(result);
    read(result);
}

std::string value_string(duckdb_result &result, idx_t column, idx_t row) {
    char *value = duckdb_value_varchar(&result, column, row);
    if (!value) return {};
    std::string output(value);
    duckdb_free(value);
    return output;
}

Json parse_json(const std::string &value, const char *context) {
    if (value.empty()) return Json::object();
    try {
        return Json::parse(value);
    } catch (const std::exception &error) {
        throw Error(ErrorCode::SchemaMismatch,
                    std::string(context) + ": invalid JSON: " + error.what());
    }
}

std::string json_text(const Json &value) {
    return value.is_null() ? "null" : value.dump();
}

std::string hash_text(const std::string &value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

void bind_text(Statement statement, idx_t index, const std::string &value) {
    duckdb_bind_varchar(statement, index, value.c_str());
}

bool has_column(duckdb_connection connection, const char *table, const char *column) {
    Statement statement = nullptr;
    const std::string sql = "SELECT 1 FROM information_schema.columns WHERE table_name = ? AND column_name = ? LIMIT 1";
    if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError) {
        if (statement) duckdb_destroy_prepare(&statement);
        throw Error(ErrorCode::DatabaseError, "inspect schema");
    }
    StatementGuard guard(statement);
    bind_text(statement, 1, table);
    bind_text(statement, 2, column);
    duckdb_result result{};
    if (duckdb_execute_prepared(statement, &result) == DuckDBError) {
        const std::string message = db_error(result);
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, "inspect schema: " + message);
    }
    ResultGuard result_guard(result);
    return duckdb_row_count(&result) != 0;
}

bool has_table(duckdb_connection connection, const char *table) {
    Statement statement = nullptr;
    const std::string sql = "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' AND table_name = ? LIMIT 1";
    if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError) {
        if (statement) duckdb_destroy_prepare(&statement);
        throw Error(ErrorCode::DatabaseError, "inspect tables");
    }
    StatementGuard guard(statement);
    bind_text(statement, 1, table);
    duckdb_result result{};
    if (duckdb_execute_prepared(statement, &result) == DuckDBError) {
        const std::string message = db_error(result);
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, "inspect tables: " + message);
    }
    ResultGuard result_guard(result);
    return duckdb_row_count(&result) != 0;
}

idx_t project_row_count(duckdb_connection connection) {
    duckdb_result result{};
    if (duckdb_query(connection, "SELECT COUNT(*) FROM PROJECT", &result) == DuckDBError) {
        const std::string message = db_error(result);
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, "count PROJECT rows: " + message);
    }
    ResultGuard guard(result);
    return static_cast<idx_t>(duckdb_value_int64(&result, 0, 0));
}

const std::vector<std::string> &expected_domain_tables(const std::string &domain) {
    static const std::vector<std::string> none;
    static const std::vector<std::string> mass_spec{
        "MASS_SPEC_ANALYSES", "MASS_SPEC_SPECTRA_HEADERS",
        "MASS_SPEC_CHROMATOGRAMS_HEADERS", "MASS_SPEC_CHROMATOGRAMS",
        "MASS_SPEC_NTA_FEATURES", "MASS_SPEC_NTA_SUSPECTS",
        "MASS_SPEC_NTA_INTERNAL_STANDARDS", "MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS"};
    if (domain == "mass_spec") return mass_spec;
    return none;
}

void ensure_schema(duckdb_connection connection,
                   const ProjectOptions &) {
    query(connection,
          "CREATE TABLE IF NOT EXISTS PROJECT (domain_id VARCHAR NOT NULL, metadata JSON, workflow JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, schema_version INTEGER NOT NULL DEFAULT 1, framework_version VARCHAR NOT NULL DEFAULT '" STREAMFIND_FRAMEWORK_VERSION "')",
          "create PROJECT table");

    query(connection,
          "CREATE TABLE IF NOT EXISTS CACHE (name VARCHAR NOT NULL, description VARCHAR NOT NULL, hash VARCHAR NOT NULL, data BLOB NOT NULL, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(hash))",
          "create CACHE table");
    query(connection,
          "CREATE TABLE IF NOT EXISTS AUDIT_TRAIL (operation_type VARCHAR NOT NULL, object_type VARCHAR NOT NULL, operation_details JSON, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
          "create AUDIT_TRAIL table");
    query(connection,
          "CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION (domain_id VARCHAR NOT NULL DEFAULT '', workflow_revision INTEGER NOT NULL, launch_snapshot JSON NOT NULL DEFAULT '{}', status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{}', result_reference VARCHAR, process_id VARCHAR, server_id VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, error VARCHAR, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)",
          "create WORKFLOW_EXECUTION table");
    query(connection,
          "CREATE TABLE IF NOT EXISTS WORKFLOW_EXECUTION_STEP (workflow_revision INTEGER NOT NULL, step_index INTEGER NOT NULL PRIMARY KEY, method VARCHAR NOT NULL, parameters JSON NOT NULL DEFAULT '{}', parameter_hash VARCHAR NOT NULL, cache_key VARCHAR NOT NULL, status VARCHAR NOT NULL, progress JSON NOT NULL DEFAULT '{}', result_reference VARCHAR, error_code VARCHAR, error_message VARCHAR, started_at TIMESTAMP, completed_at TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)",
          "create WORKFLOW_EXECUTION_STEP table");
}

std::string now_string() {
    return "current";
}

std::string sql_quote(const std::string &value) {
    std::string result = "'";
    for (const char character : value) result += character == '\'' ? "''" : std::string(1, character);
    return result + "'";
}

std::string identifier_quote(const std::string &value) {
    std::string result = "\"";
    for (const char character : value) result += character == '"' ? "\"\"" : std::string(1, character);
    return result + "\"";
}

Json snapshot_tables(duckdb_connection connection, const std::vector<std::string> &tables) {
    Json snapshots = Json::object();
    for (const auto &table : tables) {
        Json rows = Json::array();
        duckdb_result result{};
        const std::string filter;
        const std::string sql = "SELECT to_json(t) FROM " + identifier_quote(table) + " t" + filter;
        if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError) { const std::string message = db_error(result); duckdb_destroy_result(&result); throw Error(ErrorCode::DatabaseError, "snapshot " + table + ": " + message); }
        ResultGuard guard(result);
        for (idx_t row = 0; row < duckdb_row_count(&result); ++row) rows.push_back(parse_json(value_string(result, 0, row), "table snapshot"));
        snapshots[table] = std::move(rows);
    }
    return snapshots;
}

void restore_tables(duckdb_connection connection, const Json &snapshots) {
    for (auto table = snapshots.begin(); table != snapshots.end(); ++table) {
        const std::string table_name = table.key();
        duckdb_result schema{};
        const std::string describe = "DESCRIBE " + identifier_quote(table_name);
        if (duckdb_query(connection, describe.c_str(), &schema) == DuckDBError) { const std::string message = db_error(schema); duckdb_destroy_result(&schema); throw Error(ErrorCode::DatabaseError, "describe cached table: " + message); }
        ResultGuard schema_guard(schema);
        std::vector<std::pair<std::string, std::string>> columns;
        for (idx_t row = 0; row < duckdb_row_count(&schema); ++row) columns.emplace_back(value_string(schema, 0, row), value_string(schema, 1, row));
        query(connection, "DELETE FROM " + identifier_quote(table_name), "clear cached table");
        for (const auto &row : table.value()) {
            std::string sql = "INSERT INTO " + identifier_quote(table_name) + " VALUES (";
            for (std::size_t index = 0; index < columns.size(); ++index) {
                if (index) sql += ", ";
                const auto &value = row.value(columns[index].first, Json(nullptr));
                if (value.is_null()) sql += "NULL";
                else if (value.is_string()) sql += sql_quote(value.get<std::string>());
                else if (value.is_boolean()) sql += value.get<bool>() ? "TRUE" : "FALSE";
                else sql += value.dump();
            }
            sql += ")";
            query(connection, sql, "restore cached table");
        }
    }
}

void execution_row(duckdb_connection connection, int revision, std::size_t index, const std::string &method, const std::string &parameter_hash, const std::string &status, const std::string &cache_key, const std::string &launch_snapshot, const std::string &error = {}) {
    duckdb_result parent_result{};
    if (duckdb_query(connection, "SELECT process_id FROM WORKFLOW_EXECUTION LIMIT 1", &parent_result) == DuckDBError) {
        const std::string message = "inspect workflow execution: " + db_error(parent_result);
        duckdb_destroy_result(&parent_result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    const bool has_parent = duckdb_row_count(&parent_result) != 0;
    duckdb_destroy_result(&parent_result);
    if (!has_parent) {
        prepared(connection, "INSERT INTO WORKFLOW_EXECUTION (workflow_revision, launch_snapshot, status, error) VALUES (?, ?, ?, ?)", "write workflow execution", [&](Statement statement) { duckdb_bind_int32(statement, 1, revision); bind_text(statement, 2, launch_snapshot); bind_text(statement, 3, status); bind_text(statement, 4, error); }, [](duckdb_result &) {});
    } else {
        query(connection, "UPDATE WORKFLOW_EXECUTION SET workflow_revision = " + std::to_string(revision) + ", launch_snapshot = " + sql_quote(launch_snapshot) + ", status = CASE WHEN process_id IS NULL OR process_id = '' THEN " + sql_quote(status) + " ELSE status END, error = " + sql_quote(error) + ", updated_at = CURRENT_TIMESTAMP", "update workflow execution");
    }
    prepared(connection, "INSERT INTO WORKFLOW_EXECUTION_STEP (workflow_revision, step_index, method, parameters, parameter_hash, cache_key, status) VALUES (?, ?, ?, '{}', ?, ?, ?) ON CONFLICT(step_index) DO UPDATE SET workflow_revision = excluded.workflow_revision, method = excluded.method, parameter_hash = excluded.parameter_hash, cache_key = excluded.cache_key, status = excluded.status", "write workflow execution step", [&](Statement statement) { duckdb_bind_int32(statement, 1, revision); duckdb_bind_int32(statement, 2, static_cast<int>(index)); bind_text(statement, 3, method); bind_text(statement, 4, parameter_hash); bind_text(statement, 5, cache_key); bind_text(statement, 6, status); }, [](duckdb_result &) {});
}

const char *parameter_type_name(ParameterType type) {
    switch (type) {
    case ParameterType::string: return "string";
    case ParameterType::integer: return "integer";
    case ParameterType::real: return "real";
    case ParameterType::boolean: return "boolean";
    case ParameterType::array: return "array";
    case ParameterType::object: return "object";
    case ParameterType::table: return "table";
    }
    return "unknown";
}

ParameterType parameter_type_from_name(const std::string &name) {
    if (name == "string") return ParameterType::string;
    if (name == "integer") return ParameterType::integer;
    if (name == "real") return ParameterType::real;
    if (name == "boolean") return ParameterType::boolean;
    if (name == "array") return ParameterType::array;
    if (name == "object") return ParameterType::object;
    if (name == "table") return ParameterType::table;
    throw Error(ErrorCode::InvalidArgument, "Unknown parameter type: " + name);
}

bool parameter_type_matches(ParameterType type, const Json &value) {
    switch (type) {
    case ParameterType::string: return value.is_string();
    case ParameterType::integer: return value.is_number_integer();
    case ParameterType::real: return value.is_number();
    case ParameterType::boolean: return value.is_boolean();
    case ParameterType::array: return value.is_array();
    case ParameterType::object: return value.is_object();
    case ParameterType::table: return value.is_object() && value.contains("columns");
    }
    return false;
}

} // namespace detail

using namespace detail;

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode Error::code() const noexcept { return code_; }

bool valid_execution_transition(ExecutionState from, ExecutionState to) noexcept {
    return (from == ExecutionState::queued &&
            (to == ExecutionState::running || to == ExecutionState::cancelled)) ||
           (from == ExecutionState::running &&
            (to == ExecutionState::completed || to == ExecutionState::failed ||
             to == ExecutionState::cancelling || to == ExecutionState::interrupted)) ||
           (from == ExecutionState::cancelling && to == ExecutionState::cancelled);
}

void CancellationToken::cancel() noexcept { cancelled_.store(true); }
bool CancellationToken::is_cancelled() const noexcept { return cancelled_.load(); }

Json ExecutionResult::to_json() const {
    return {{"results", results}, {"cancelled", cancelled}};
}

Json TableColumnDefinition::to_json() const {
    return {{"name", name}, {"description", description},
            {"type", detail::parameter_type_name(type)}, {"required", required}};
}

TableColumnDefinition TableColumnDefinition::from_json(const Json &value) {
    if (!value.is_object() || !value.contains("name") || !value.contains("type")) {
        throw Error(ErrorCode::InvalidArgument, "Table column requires name and type");
    }
    return {value.at("name").get<std::string>(), value.value("description", ""),
            detail::parameter_type_from_name(value.at("type").get<std::string>()),
            value.value("required", true)};
}

Json TableSchema::to_json() const {
    Json output = Json::array();
    for (const auto &column : columns) output.push_back(column.to_json());
    return output;
}

TableSchema TableSchema::from_json(const Json &value) {
    if (!value.is_array()) throw Error(ErrorCode::InvalidArgument, "Table schema must be an array");
    TableSchema schema;
    for (const auto &column : value) schema.columns.push_back(TableColumnDefinition::from_json(column));
    return schema;
}

std::size_t Table::row_count() const {
    if (columns.empty()) return 0;
    return std::visit([](const auto &values) { return values.size(); }, columns.front().values);
}

void Table::validate(const std::optional<TableSchema> &schema) const {
    std::size_t rows = 0;
    bool first = true;
    for (const auto &column : columns) {
        if (column.name.empty()) throw Error(ErrorCode::WorkflowValidation, "Table column name must not be empty");
        const std::size_t length = std::visit([](const auto &values) { return values.size(); }, column.values);
        if (first) { rows = length; first = false; }
        if (length != rows) throw Error(ErrorCode::WorkflowValidation, "Table columns must have equal lengths");
        if (schema) {
            const auto it = std::find_if(schema->columns.begin(), schema->columns.end(),
                                         [&](const auto &definition) { return definition.name == column.name; });
            if (it == schema->columns.end() || it->type != column.type) {
                throw Error(ErrorCode::WorkflowValidation, "Table column does not match its schema: " + column.name);
            }
        }
    }
    if (schema) {
        for (const auto &definition : schema->columns) {
            const auto it = std::find_if(columns.begin(), columns.end(),
                                         [&](const auto &column) { return column.name == definition.name; });
            if (definition.required && it == columns.end()) {
                throw Error(ErrorCode::WorkflowValidation, "Missing required table column: " + definition.name);
            }
        }
    }
}

Json Table::to_json() const {
    Json output = Json::array();
    for (const auto &column : columns) {
        Json values = Json::array();
        std::visit([&](const auto &items) { for (const auto &item : items) values.push_back(item); }, column.values);
        output.push_back({{"name", column.name}, {"type", detail::parameter_type_name(column.type)}, {"values", values}});
    }
    return {{"columns", output}};
}

Table Table::from_json(const Json &value) {
    if (!value.is_object() || !value.at("columns").is_array()) {
        throw Error(ErrorCode::WorkflowValidation, "Table value requires a columns array");
    }
    Table table;
    for (const auto &item : value.at("columns")) {
        const auto type = detail::parameter_type_from_name(item.at("type").get<std::string>());
        const Json &values = item.at("values");
        if (!values.is_array()) throw Error(ErrorCode::WorkflowValidation, "Table column values must be arrays");
        TableColumn column;
        column.name = item.at("name").get<std::string>();
        column.type = type;
        switch (type) {
        case ParameterType::string: column.values = values.get<std::vector<std::string>>(); break;
        case ParameterType::integer: column.values = values.get<std::vector<std::int64_t>>(); break;
        case ParameterType::real: column.values = values.get<std::vector<double>>(); break;
        case ParameterType::boolean: column.values = values.get<std::vector<bool>>(); break;
        default: throw Error(ErrorCode::WorkflowValidation, "Table columns must be scalar types");
        }
        table.columns.push_back(std::move(column));
    }
    table.validate();
    return table;
}

Json TypeDescriptor::to_json() const {
    Json output = {{"type", detail::parameter_type_name(kind)}};
    if (kind == ParameterType::array) {
        if (!items) throw Error(ErrorCode::InvalidArgument, "Array type requires an items type");
        output["items"] = items->to_json();
    } else if (kind == ParameterType::table && table_schema) {
        output["columns"] = table_schema->to_json();
    }
    return output;
}

TypeDescriptor TypeDescriptor::from_json(const Json &value) {
    if (!value.is_object() || !value.contains("type")) {
        throw Error(ErrorCode::InvalidArgument, "Type descriptor requires a type");
    }
    TypeDescriptor descriptor;
    descriptor.kind = detail::parameter_type_from_name(value.at("type").get<std::string>());
    if (descriptor.kind == ParameterType::array) {
        if (!value.contains("items")) throw Error(ErrorCode::InvalidArgument, "Array type requires an items type");
        descriptor.items = std::make_shared<TypeDescriptor>(from_json(value.at("items")));
    } else if (descriptor.kind == ParameterType::table && value.contains("columns")) {
        descriptor.table_schema = TableSchema::from_json(value.at("columns"));
    }
    return descriptor;
}

void TypeDescriptor::validate(const Json &value) const {
    if (kind == ParameterType::array) {
        if (!items || !value.is_array()) throw Error(ErrorCode::WorkflowValidation, "Invalid array parameter");
        for (const auto &item : value) items->validate(item);
        return;
    }
    if (kind == ParameterType::table) {
        Table::from_json(value).validate(table_schema);
        return;
    }
    if (!parameter_type_matches(kind, value)) {
        throw Error(ErrorCode::WorkflowValidation,
                    std::string("Invalid parameter type; expected ") + detail::parameter_type_name(kind));
    }
}

Json ParameterDefinition::to_json() const {
    return {{"name", name}, {"description", description},
            {"type", type.to_json()}, {"default", default_value},
            {"required", required}, {"example", example}, {"constraints", constraints}, {"ui", ui}};
}

ParameterDefinition ParameterDefinition::from_json(const Json &value) {
    if (!value.is_object() || !value.contains("name") || !value.contains("type")) {
        throw Error(ErrorCode::InvalidArgument, "Parameter definition requires name and type");
    }
    ParameterDefinition definition;
    definition.name = value.at("name").get<std::string>();
    definition.description = value.value("description", "");
    definition.type = TypeDescriptor::from_json(value.at("type"));
    definition.default_value = value.value("default", Json(nullptr));
    definition.required = value.value("required", false);
    definition.example = value.value("example", Json(nullptr));
    definition.constraints = value.value("constraints", Json::object());
    definition.ui = value.value("ui", Json::object());
    return definition;
}

Json ParameterSchema::to_json() const {
    Json output = Json::array();
    for (const auto &definition : definitions) output.push_back(definition.to_json());
    return output;
}

ParameterSchema ParameterSchema::from_json(const Json &value) {
    if (!value.is_array()) throw Error(ErrorCode::InvalidArgument, "Parameter schema must be an array");
    ParameterSchema schema;
    for (const auto &item : value) schema.definitions.push_back(ParameterDefinition::from_json(item));
    return schema;
}

Json ParameterSchema::resolve_and_validate(const Json &values) const {
    if (!values.is_null() && !values.is_object()) {
        throw Error(ErrorCode::WorkflowValidation, "Method parameters must be an object");
    }
    Json resolved = Json::object();
    for (const auto &definition : definitions) {
        if (!definition.default_value.is_null()) resolved[definition.name] = definition.default_value;
        if (definition.required && definition.default_value.is_null() &&
            (!values.is_object() || !values.contains(definition.name))) {
            throw Error(ErrorCode::WorkflowValidation,
                        "Missing required parameter: " + definition.name);
        }
    }
    if (values.is_object()) {
        for (const auto &[name, value] : values.items()) {
            const auto it = std::find_if(definitions.begin(), definitions.end(),
                                         [&](const auto &definition) { return definition.name == name; });
            if (it == definitions.end()) {
                throw Error(ErrorCode::WorkflowValidation, "Unknown parameter: " + name);
            }
            resolved[name] = value;
        }
    }
    for (const auto &definition : definitions) {
        if (!resolved.contains(definition.name)) continue;
        definition.type.validate(resolved.at(definition.name));
    }
    return resolved;
}

Json ParameterValues::to_json() const { return values; }

ParameterValues ParameterValues::from_json(const Json &value) {
    if (!value.is_null() && !value.is_object()) {
        throw Error(ErrorCode::WorkflowValidation, "Parameter values must be an object");
    }
    return {value.is_null() ? Json::object() : value};
}

Method::Method(MethodDefinition definition, MethodExecutor executor,
               MethodValidator validator)
    : definition_(std::move(definition)), executor_(std::move(executor)),
      validator_(std::move(validator)) {
    if (definition_.id.empty()) {
        throw Error(ErrorCode::InvalidArgument, "Method id must not be empty");
    }
    std::vector<std::string> parameter_names;
    for (const auto &parameter : definition_.parameters.definitions) {
        if (parameter.name.empty() ||
            std::find(parameter_names.begin(), parameter_names.end(), parameter.name) != parameter_names.end()) {
            throw Error(ErrorCode::InvalidArgument, "Method parameter names must be unique and non-empty");
        }
        parameter_names.push_back(parameter.name);
    }
}

const MethodDefinition &Method::definition() const noexcept { return definition_; }

Json Method::to_json() const {
    return {
        {"id", definition_.id}, {"name", definition_.name},
        {"description", definition_.description},
        {"version", definition_.version}, {"domain", definition_.domain},
        {"required_methods", definition_.required_methods},
        {"single_occurrence", definition_.single_occurrence},
        {"developer", definition_.developer}, {"contact", definition_.contact},
        {"link", definition_.link}, {"doi", definition_.doi},
        {"parameters", definition_.parameters.to_json()},
        {"cacheable", definition_.cacheable}, {"writes", definition_.writes}
    };
}

MethodDefinition Method::definition_from_json(const Json &value) {
    if (!value.is_object() || !value.contains("id")) {
        throw Error(ErrorCode::InvalidArgument, "Method metadata requires an id");
    }
    MethodDefinition definition;
    definition.id = value.at("id").get<std::string>();
    definition.name = value.value("name", definition.id);
    definition.description = value.value("description", "");
    definition.version = value.value("version", "1");
    definition.domain = value.value("domain", "");
    definition.required_methods = value.value("required_methods", std::vector<std::string>{});
    definition.single_occurrence = value.value("single_occurrence", false);
    definition.developer = value.value("developer", "");
    definition.contact = value.value("contact", "");
    definition.link = value.value("link", "");
    definition.doi = value.value("doi", "");
    definition.parameters = ParameterSchema::from_json(value.value("parameters", Json::array()));
    definition.cacheable = value.value("cacheable", false);
    definition.writes = value.value("writes", std::vector<std::string>{});
    return definition;
}

Json Method::resolve_parameters(const Json &value) const {
    Json resolved = definition_.parameters.resolve_and_validate(value);
    validate_parameters(resolved);
    return resolved;
}

void Method::validate_parameters(const Json &value) const {
    if (!validator_) return;
    try {
        validator_(value);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &error) {
        throw Error(ErrorCode::WorkflowValidation,
                    definition_.id + ": invalid parameters: " + error.what());
    }
}

Json Method::run(Project &project, const Json &parameters) const {
    if (!executor_) {
        throw Error(ErrorCode::MethodExecution,
                    "Method has no implementation: " + definition_.id);
    }
    try {
        return executor_(project, resolve_parameters(parameters));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &error) {
        throw Error(ErrorCode::MethodExecution,
                    definition_.id + ": " + error.what());
    }
}

void MethodRegistry::register_method(Method method) {
    if (find(method.definition().id)) {
        throw Error(ErrorCode::InvalidArgument,
                    "Method already registered: " + method.definition().id);
    }
    methods_.push_back(std::move(method));
}

const Method *MethodRegistry::find(const std::string &id) const noexcept {
    const auto it = std::find_if(methods_.begin(), methods_.end(),
                                 [&](const Method &method) { return method.definition().id == id; });
    return it == methods_.end() ? nullptr : &*it;
}

std::vector<MethodDefinition> MethodRegistry::list(const std::string &domain) const {
    std::vector<MethodDefinition> output;
    output.reserve(methods_.size());
    for (const auto &method : methods_) {
        if (domain.empty() || method.definition().domain == domain) output.push_back(method.definition());
    }
    return output;
}

Operation::Operation(OperationDefinition definition, OperationExecutor executor)
    : definition_(std::move(definition)), executor_(std::move(executor)) {
    if (definition_.id.empty()) throw Error(ErrorCode::InvalidArgument, "Operation id must not be empty");
}
const OperationDefinition &Operation::definition() const noexcept { return definition_; }
Json Operation::to_json() const {
    return {{"id", definition_.id}, {"name", definition_.name}, {"description", definition_.description}, {"domain", definition_.domain}, {"parameters", definition_.parameters.to_json()}};
}
Json Operation::resolve_parameters(const Json &value) const { return definition_.parameters.resolve_and_validate(value); }
Json Operation::run(Project &project, const Json &value) const {
    if (!executor_) throw Error(ErrorCode::MethodExecution, "Operation has no implementation: " + definition_.id);
    try { return executor_(project, resolve_parameters(value)); }
    catch (const Error &) { throw; }
    catch (const std::exception &error) { throw Error(ErrorCode::MethodExecution, definition_.id + ": " + error.what()); }
}
void OperationRegistry::register_operation(Operation operation) {
    if (find(operation.definition().id)) throw Error(ErrorCode::InvalidArgument, "Operation already registered: " + operation.definition().id);
    operations_.push_back(std::move(operation));
}
const Operation *OperationRegistry::find(const std::string &id) const noexcept {
    const auto it = std::find_if(operations_.begin(), operations_.end(), [&](const Operation &operation) { return operation.definition().id == id; });
    return it == operations_.end() ? nullptr : &*it;
}
std::vector<OperationDefinition> OperationRegistry::list(const std::string &domain) const {
    std::vector<OperationDefinition> output;
    for (const auto &operation : operations_) if (domain.empty() || operation.definition().domain == domain) output.push_back(operation.definition());
    return output;
}

MethodRegistry &methods() {
    static MethodRegistry registry;
    return registry;
}

Json WorkflowStep::to_json() const {
    return {{"method", method}, {"parameters", parameters.to_json()}};
}

WorkflowStep WorkflowStep::from_json(const Json &value) {
    if (!value.is_object() || (!value.contains("method") && !value.contains("id"))) {
        throw Error(ErrorCode::WorkflowValidation, "Workflow step requires a method");
    }
    WorkflowStep step;
    step.method = value.value("method", value.value("id", ""));
    step.parameters = ParameterValues::from_json(value.value("parameters", Json::object()));
    return step;
}

void Workflow::validate(const MethodRegistry &registry) const {
    std::unordered_map<std::string, std::size_t> counts;
    std::vector<std::string> prior;
    for (const auto &step : steps) {
        const Method *method = registry.find(step.method);
        if (!method) throw Error(ErrorCode::WorkflowValidation,
                                 "Unknown workflow method: " + step.method);
        if (!method->implemented()) {
            throw Error(ErrorCode::WorkflowValidation,
                        "Method is not implemented: " + step.method);
        }
        const auto &definition = method->definition();
        if (!domain.empty() && !definition.domain.empty() && domain != definition.domain) {
            throw Error(ErrorCode::WorkflowValidation,
                        "Method domain does not match workflow domain: " + step.method);
        }
        for (const auto &required : definition.required_methods) {
            if (std::find(prior.begin(), prior.end(), required) == prior.end()) {
                throw Error(ErrorCode::WorkflowValidation,
                            "Required method is not earlier in workflow: " + required);
            }
        }
        if (definition.single_occurrence && ++counts[step.method] > 1) {
            throw Error(ErrorCode::WorkflowValidation,
                        "Method occurs too many times: " + step.method);
        }
        method->resolve_parameters(step.parameters.values);
        prior.push_back(step.method);
    }
}

Json Workflow::to_json() const {
    Json serialized_steps = Json::array();
    for (const auto &step : steps) serialized_steps.push_back(step.to_json());
    return serialized_steps;
}

bool Method::implemented() const noexcept { return static_cast<bool>(executor_); }

Json Workflow::to_json(const MethodRegistry &registry) const {
    Json serialized = Json::object({
        {"name", name},
        {"version", version},
        {"domain", domain},
        {"steps", Json::array()}
    });
    for (const auto &step : steps) {
        const auto *method = registry.find(step.method);
        if (!method) throw Error(ErrorCode::WorkflowValidation, "Unknown workflow method: " + step.method);
        auto value = method->to_json();
        value["parameters"] = step.parameters.to_json();
        serialized["steps"].push_back(std::move(value));
    }
    return serialized;
}

Workflow Workflow::from_json(const Json &value) {
    if (value.is_null()) return {};
    Workflow workflow;
    if (value.is_array()) {
        for (const auto &item : value) workflow.steps.push_back(WorkflowStep::from_json(item));
        return workflow;
    }
    if (!value.is_object()) throw Error(ErrorCode::WorkflowValidation, "Workflow must be an object or array");
    workflow.name = value.value("name", "");
    workflow.version = value.value("version", 1);
    workflow.domain = value.value("domain", "");
    for (const auto &item : value.value("steps", Json::array())) {
        workflow.steps.push_back(WorkflowStep::from_json(item));
    }
    return workflow;
}

struct Project::Impl {
    ProjectOptions options;
    ProjectInfo info;
    mutable std::mutex mutex;
    bool closed{false};
};

class Connection {
public:
    Connection(const Project::Impl &impl) {
        duckdb_config config = nullptr;
        if (duckdb_create_config(&config) == DuckDBError) {
            throw Error(ErrorCode::DatabaseError, "create DuckDB config failed");
        }
        char *error = nullptr;
        if (duckdb_open_ext(impl.options.database_path.string().c_str(), &database_, config, &error) != DuckDBSuccess) {
            const std::string message = error ? error : "open DuckDB database failed";
            if (error) duckdb_free(error);
            duckdb_destroy_config(&config);
            throw Error(ErrorCode::DatabaseError, message);
        }
        duckdb_destroy_config(&config);
        if (duckdb_connect(database_, &connection_) != DuckDBSuccess) {
            duckdb_close(&database_);
            throw Error(ErrorCode::DatabaseError, "connect DuckDB database failed");
        }
    }
    ~Connection() {
        if (connection_) duckdb_disconnect(&connection_);
        if (database_) duckdb_close(&database_);
    }
    duckdb_connection get() const noexcept { return connection_; }

private:
    duckdb_database database_{nullptr};
    duckdb_connection connection_{nullptr};
};

void ensure_active(const Project::Impl &impl) {
    if (impl.closed) throw Error(ErrorCode::InvalidArgument, "Project is closed");
}

ProjectInfo read_info(duckdb_connection connection) {
    ProjectInfo info;
    prepared(connection,
             "SELECT domain_id, metadata, schema_version, framework_version, created_at FROM PROJECT LIMIT 1",
             "read PROJECT row",
             [](Statement) {},
             [&](duckdb_result &result) {
                 if (duckdb_row_count(&result) == 0) throw Error(ErrorCode::ProjectNotFound, "Project row not found");
                 info.domain = value_string(result, 0, 0);
                 info.metadata = parse_json(value_string(result, 1, 0), "PROJECT metadata");
                 info.schema_version = duckdb_value_int32(&result, 2, 0);
                 info.framework_version = value_string(result, 3, 0);
                 info.created_at = value_string(result, 4, 0);
             });
    return info;
}

void audit(duckdb_connection connection, const std::string &operation,
           const std::string &object, const Json &details) {
    prepared(connection,
             "INSERT INTO AUDIT_TRAIL (operation_type, object_type, operation_details) VALUES (?, ?, ?)",
             "write audit trail",
             [&](Statement statement) {
                 bind_text(statement, 1, operation);
                 bind_text(statement, 2, object);
                 bind_text(statement, 3, json_text(details));
             }, [](duckdb_result &) {});
}

Project project_from_options(const ProjectOptions &options, bool creating) {
    if (options.database_path.empty()) throw Error(ErrorCode::InvalidArgument, "Project database path is required");
    const bool exists = options.database_path != ":memory:" && std::filesystem::exists(options.database_path);
    if (!creating && !exists) throw Error(ErrorCode::ProjectNotFound, "Project database does not exist");
    if (!exists && options.database_path != ":memory:") {
        const auto parent = options.database_path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
    }
    auto impl = std::make_shared<Project::Impl>();
    impl->options = options;
    Connection connection(*impl);
    if (has_table(connection.get(), "PROJECTS")) throw Error(ErrorCode::SchemaMismatch, "legacy PROJECTS registry is not supported; expected PROJECT");
    ensure_schema(connection.get(), options);
    const idx_t existing_projects = project_row_count(connection.get());
    if (!creating && existing_projects != 1) throw Error(ErrorCode::SchemaMismatch, "project database must contain exactly one PROJECT row");
    if (creating && existing_projects != 0) throw Error(ErrorCode::ProjectAlreadyExists, "DuckDB file already contains a project");
    if (creating) {
        prepared(connection.get(), "INSERT INTO PROJECT (domain_id, metadata, workflow) VALUES (?, ?, '[]')",
                  "create PROJECT row",
                  [&](Statement statement) { bind_text(statement, 1, options.domain); bind_text(statement, 2, json_text(options.metadata)); }, [](duckdb_result &) {});
    }
    impl->info = read_info(connection.get());
    if (!options.domain.empty() && impl->info.domain != options.domain) throw Error(ErrorCode::SchemaMismatch, "Project domain mismatch");
    audit(connection.get(), creating ? "create" : "open", "project", Json::object());
    if (!creating) query(connection.get(), "UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'", "recover workflow executions");
    return Project(std::move(impl));
}

Project Project::create(const ProjectOptions &options) { return project_from_options(options, true); }
Project Project::open(const ProjectOptions &options) { return project_from_options(options, false); }

Project::Project(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Project::Project(Project &&other) noexcept = default;
Project &Project::operator=(Project &&other) noexcept = default;
Project::~Project() { close(); }

const ProjectInfo &Project::info() const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    return impl_->info;
}

Json Project::get_metadata() const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    return impl_->info.metadata;
}

const std::filesystem::path &Project::get_database_path() const noexcept { return impl_->options.database_path; }

std::string Project::get_domain() const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    return impl_->info.domain;
}

void Project::validate() const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    Connection connection(*impl_);
    query(connection.get(), "SELECT domain_id, metadata, workflow, schema_version, framework_version FROM PROJECT LIMIT 0", "validate PROJECT schema");
    read_info(connection.get());
    for (const auto &table : detail::expected_domain_tables(impl_->info.domain)) {
        if (!detail::has_table(connection.get(), table.c_str()))
            throw Error(ErrorCode::SchemaMismatch, "missing expected domain table: " + table);
    }
    query(connection.get(), "SELECT name, description, hash, data, created_at FROM CACHE LIMIT 0", "validate CACHE schema");
    query(connection.get(), "SELECT operation_type, object_type, operation_details, created_at FROM AUDIT_TRAIL LIMIT 0", "validate AUDIT_TRAIL schema");
    query(connection.get(), "SELECT workflow_revision, launch_snapshot, status, progress, result_reference, started_at, completed_at, error, created_at, updated_at FROM WORKFLOW_EXECUTION LIMIT 0", "validate WORKFLOW_EXECUTION schema");
    query(connection.get(), "SELECT workflow_revision, step_index, method, parameters, parameter_hash, cache_key, status, progress, result_reference, error_code, error_message, started_at, completed_at, updated_at FROM WORKFLOW_EXECUTION_STEP LIMIT 0", "validate WORKFLOW_EXECUTION_STEP schema");
}

void Project::set_metadata(Json metadata) {
    if (!metadata.is_object()) throw Error(ErrorCode::InvalidArgument, "Project metadata must be an object");
    for (const auto &[key, value] : metadata.items())
        if (value.is_object() || value.is_array())
            throw Error(ErrorCode::InvalidArgument, "Project metadata values must be scalar: " + key);
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    Connection connection(*impl_);
    prepared(connection.get(), "UPDATE PROJECT SET metadata = ?, updated_at = CURRENT_TIMESTAMP", "update metadata",
             [&](Statement statement) { bind_text(statement, 1, json_text(metadata)); },
             [](duckdb_result &) {});
    audit(connection.get(), "update", "metadata", metadata);
    impl_->info.metadata = std::move(metadata);
}

Project Project::copy(const ProjectOptions &options) const {
    const auto workflow_value = get_workflow();
    ProjectOptions destination_options = options;
    destination_options.domain = impl_->info.domain;
    Project destination = Project::create(destination_options);
    destination.set_metadata(impl_->info.metadata);
    destination.set_workflow(workflow_value);
    for (const auto &entry : get_cache()) {
        destination.set_cache(entry.name, entry.description, entry.hash,
                              parse_json(std::string(entry.data.begin(), entry.data.end()), "cache entry"));
    }
    return destination;
}

Workflow Project::get_workflow() const {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    Connection connection(*impl_);
    Json value;
    prepared(connection.get(), "SELECT workflow FROM PROJECT", "read workflow",
             [&](Statement statement) {  },
             [&](duckdb_result &result) { if (duckdb_row_count(&result)) value = parse_json(value_string(result, 0, 0), "workflow"); });
    return Workflow::from_json(value);
}

void Project::set_workflow(Workflow workflow_value, const MethodRegistry &registry) {
    workflow_value.domain = workflow_value.domain.empty() ? impl_->info.domain : workflow_value.domain;
    workflow_value.validate(registry);
    const auto execution = query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
    if (!execution.empty()) {
        const auto status = execution.at(0).value("status", "");
        if (status == "queued" || status == "running" || status == "cancelling") {
            throw Error(ErrorCode::InvalidArgument,
                        "workflow mutation is blocked while an execution is active");
        }
    }
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    Connection connection(*impl_);
    prepared(connection.get(), "UPDATE PROJECT SET workflow = ?, updated_at = CURRENT_TIMESTAMP", "update workflow",
             [&](Statement statement) { bind_text(statement, 1, json_text(workflow_value.to_json(registry))); },
             [](duckdb_result &) {});
    audit(connection.get(), "update", "workflow", workflow_value.to_json(registry));
}

std::vector<std::string> Project::list_tables() const {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    Connection connection(*impl_);
    std::vector<std::string> output;
    query(connection.get(), "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' ORDER BY table_name", "list tables");
    duckdb_result result{};
    if (duckdb_query(connection.get(), "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' ORDER BY table_name", &result) == DuckDBError) {
        const std::string message = db_error(result); duckdb_destroy_result(&result); throw Error(ErrorCode::DatabaseError, message);
    }
    ResultGuard guard(result);
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) output.push_back(value_string(result, 0, row));
    return output;
}

void Project::execute_sql(const std::string &sql) const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    Connection connection(*impl_);
    query(connection.get(), sql, "execute project SQL");
}

void Project::append_rows(const std::string &table_name,
                          const std::vector<std::string> &column_names,
                          const std::vector<std::vector<std::optional<std::string>>> &rows) const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    Connection connection(*impl_);

    duckdb_appender appender = nullptr;
    if (duckdb_appender_create(connection.get(), nullptr, table_name.c_str(), &appender) == DuckDBError) {
        const char *message = appender ? duckdb_appender_error(appender) : nullptr;
        throw Error(ErrorCode::DatabaseError,
                    message ? message : ("create DuckDB appender failed for " + table_name));
    }
    AppenderGuard appender_guard(&appender);

    // Select only the supplied columns as the appender's active column list; every
    // other table column (e.g. created_at) is filled with its DEFAULT value.
    for (const auto &name : column_names) {
        if (duckdb_appender_add_column(appender, name.c_str()) == DuckDBError) {
            const char *message = duckdb_appender_error(appender);
            throw Error(ErrorCode::DatabaseError,
                        message ? message : ("add appender column " + name));
        }
    }

    // Reflect each active column's DuckDB type so numeric values are appended as
    // numbers (not text), matching the R bindings' typed append behaviour.
    const idx_t active_columns = duckdb_appender_column_count(appender);
    std::vector<duckdb_type> types(static_cast<std::size_t>(active_columns));
    for (idx_t col = 0; col < active_columns; ++col) {
        duckdb_logical_type logical = duckdb_appender_column_type(appender, col);
        types[static_cast<std::size_t>(col)] = duckdb_get_type_id(logical);
        duckdb_destroy_logical_type(&logical);
    }

    for (const auto &row : rows) {
        if (row.size() != column_names.size()) {
            throw Error(ErrorCode::InvalidArgument,
                        "append_rows row/column count mismatch for " + table_name);
        }
        if (duckdb_appender_begin_row(appender) == DuckDBError) {
            const char *message = duckdb_appender_error(appender);
            throw Error(ErrorCode::DatabaseError, message ? message : "begin appender row");
        }
        for (idx_t col = 0; col < active_columns; ++col) {
            const auto &cell = row[static_cast<std::size_t>(col)];
            duckdb_state state = DuckDBSuccess;
            if (!cell) {
                state = duckdb_append_null(appender);
            } else {
                switch (types[static_cast<std::size_t>(col)]) {
                    case DUCKDB_TYPE_VARCHAR: state = duckdb_append_varchar(appender, cell->c_str()); break;
                    case DUCKDB_TYPE_DOUBLE: state = duckdb_append_double(appender, std::stod(*cell)); break;
                    case DUCKDB_TYPE_FLOAT: state = duckdb_append_float(appender, std::stof(*cell)); break;
                    case DUCKDB_TYPE_INTEGER: state = duckdb_append_int32(appender, static_cast<std::int32_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_BIGINT: state = duckdb_append_int64(appender, std::stoll(*cell)); break;
                    case DUCKDB_TYPE_SMALLINT: state = duckdb_append_int16(appender, static_cast<std::int16_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_TINYINT: state = duckdb_append_int8(appender, static_cast<std::int8_t>(std::stoll(*cell))); break;
                    case DUCKDB_TYPE_BOOLEAN: state = duckdb_append_bool(appender, *cell == "true" || *cell == "TRUE" || *cell == "1"); break;
                    default:
                        throw Error(ErrorCode::DatabaseError,
                                    "append_rows unsupported column type for " + table_name + " col " + std::to_string(col));
                }
            }
            if (state == DuckDBError) {
                const char *message = duckdb_appender_error(appender);
                throw Error(ErrorCode::DatabaseError, message ? message : "append appender value");
            }
        }
        if (duckdb_appender_end_row(appender) == DuckDBError) {
            const char *message = duckdb_appender_error(appender);
            throw Error(ErrorCode::DatabaseError, message ? message : "end appender row");
        }
    }
    if (duckdb_appender_close(appender) == DuckDBError) {
        const char *message = duckdb_appender_error(appender);
        throw Error(ErrorCode::DatabaseError, message ? message : "close appender");
    }
}

Json Project::query_json(const std::string &sql) const {
    std::lock_guard lock(impl_->mutex);
    ensure_active(*impl_);
    Connection connection(*impl_);
    Json rows = Json::array();
    duckdb_result result;
    if (duckdb_query(connection.get(), sql.c_str(), &result) == DuckDBError) {
        const std::string message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "query failed";
        duckdb_destroy_result(&result);
        throw Error(ErrorCode::DatabaseError, message);
    }
    const auto columns = duckdb_column_count(&result);
    const auto count = duckdb_row_count(&result);
    for (idx_t row = 0; row < count; ++row) {
        Json object = Json::object();
        for (idx_t column = 0; column < columns; ++column) {
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

std::vector<CacheEntry> Project::get_cache() const {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_);
    std::vector<CacheEntry> output;
    prepared(connection.get(), "SELECT name, description, hash, data, created_at FROM CACHE ORDER BY created_at DESC", "read cache",
             [&](Statement statement) {  },
             [&](duckdb_result &result) {
                 for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
                     CacheEntry entry{value_string(result, 0, row), value_string(result, 1, row), value_string(result, 2, row), {}, value_string(result, 4, row)};
                     duckdb_blob blob = duckdb_value_blob(&result, 3, row);
                     if (blob.data && blob.size) entry.data.assign(static_cast<std::uint8_t *>(blob.data), static_cast<std::uint8_t *>(blob.data) + blob.size);
                     if (blob.data) duckdb_free(blob.data);
                     output.push_back(std::move(entry));
                 }
             });
    return output;
}

std::size_t Project::get_cache_size() const { return get_cache().size(); }

std::optional<CacheEntry> Project::get_cache_entry(const std::string &hash) const {
    for (auto &entry : get_cache()) if (entry.hash == hash) return entry;
    return std::nullopt;
}

void Project::set_cache(std::string name, std::string description, std::string hash, const Json &value) {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    const std::string payload = json_text(value);
    Connection connection(*impl_);
    prepared(connection.get(), "INSERT INTO CACHE (name, description, hash, data) VALUES (?, ?, ?, ?) ON CONFLICT(hash) DO UPDATE SET name = excluded.name, description = excluded.description, data = excluded.data",
             "write cache",
              [&](Statement statement) { bind_text(statement, 1, name); bind_text(statement, 2, description); bind_text(statement, 3, hash); duckdb_bind_blob(statement, 4, payload.data(), payload.size()); },
             [](duckdb_result &) {});
}

void Project::delete_cache() {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_);
    Connection connection(*impl_);
    prepared(connection.get(), "DELETE FROM CACHE", "delete cache",
             [&](Statement statement) {  }, [](duckdb_result &) {});
    audit(connection.get(), "delete", "cache", Json::object());
}

std::vector<AuditEntry> Project::get_audit_trail() const {
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_);
    std::vector<AuditEntry> output;
    prepared(connection.get(), "SELECT operation_type, object_type, operation_details, created_at FROM AUDIT_TRAIL ORDER BY created_at ASC", "read audit trail",
              [&](Statement statement) {  },
             [&](duckdb_result &result) { for (idx_t row = 0; row < duckdb_row_count(&result); ++row) output.push_back({value_string(result, 0, row), value_string(result, 1, row), parse_json(value_string(result, 2, row), "audit details"), value_string(result, 3, row)}); });
    return output;
}

Json Project::get_workflow_execution() const {
    return query_json("SELECT workflow_revision, step_index, method, parameter_hash, status, started_at, completed_at, error_message AS error, cache_key FROM WORKFLOW_EXECUTION_STEP ORDER BY workflow_revision, step_index");
}

ExecutionResult Project::run_workflow(const MethodRegistry &registry, CancellationToken *cancellation, ProgressCallback progress) {
    Workflow current = get_workflow(); current.validate(registry);
    const std::string launch_snapshot = current.to_json(registry).dump();
    { std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_); query(connection.get(), "DELETE FROM WORKFLOW_EXECUTION_STEP", "reset workflow execution steps"); }
    Json results = Json::array();
    std::size_t completed = 0;
    std::string previous_hash = "initial";
    const auto publish_progress = [&](std::size_t done) {
        const Json value = Json{{"completed", done}, {"total", current.steps.size()}, {"current_step", done == 0 ? 0 : done - 1}};
        execute_sql("UPDATE WORKFLOW_EXECUTION SET progress = " + detail::sql_quote(value.dump()) + ", updated_at = CURRENT_TIMESTAMP");
        if (progress) progress({"workflow", done, current.steps.size()});
    };
    for (std::size_t index = 0; index < current.steps.size(); ++index) {
        const auto &step = current.steps[index];
        if (cancellation && cancellation->is_cancelled()) return {results, true};
        if (cancellation) {
            const auto state = query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
            if (!state.empty() && state.at(0).value("status", "") == "cancelling") return {results, true};
        }
        const Method *method = registry.find(step.method);
        const Json parameters = method->resolve_parameters(step.parameters.values);
        const auto &definition = method->definition();
        const std::string parameter_hash = hash_text(parameters.dump());
        const std::string key = hash_text(previous_hash + "\n" + definition.id + "\n" + definition.version + "\n" + parameters.dump());
        { std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_); execution_row(connection.get(), current.version, index, definition.id, parameter_hash, "pending", key, launch_snapshot); }
        if (definition.cacheable) {
            if (auto cached = get_cache_entry(key)) {
                const Json payload = parse_json(std::string(cached->data.begin(), cached->data.end()), "cached result");
                if (payload.is_object() && payload.contains("result") && payload.contains("tables") && payload.at("tables").is_object()) {
                    { std::lock_guard lock(impl_->mutex); Connection connection(*impl_); restore_tables(connection.get(), payload.at("tables"));
                    results.push_back(payload.at("result"));
                    execution_row(connection.get(), current.version, index, definition.id, parameter_hash, "completed", key, launch_snapshot);
                    audit(connection.get(), "cache_hit", "workflow_step", Json{{"method", definition.id}, {"cache_key", key}});
                    }
                    previous_hash = key;
                    publish_progress(++completed);
                    continue;
                }
            }
            std::lock_guard lock(impl_->mutex); Connection connection(*impl_);
            audit(connection.get(), "cache_miss", "workflow_step", Json{{"method", definition.id}, {"cache_key", key}});
        }
        {
            std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_);
            audit(connection.get(), "start", "workflow_step", Json{{"method", definition.id}, {"cache_key", key}, {"parameters", parameters}});
            execution_row(connection.get(), current.version, index, definition.id, parameter_hash, "running", key, launch_snapshot);
        }
        try {
            Json result = method->run(*this, parameters);
            results.push_back(result);
             if (definition.cacheable) {
                 Json snapshots;
                 { std::lock_guard lock(impl_->mutex); Connection connection(*impl_); snapshots = snapshot_tables(connection.get(), definition.writes); }
                  set_cache(definition.id, "workflow result", key, Json{{"result", result}, {"tables", std::move(snapshots)}});
             }
             previous_hash = key;
            publish_progress(++completed);
            std::lock_guard lock(impl_->mutex); Connection connection(*impl_); audit(connection.get(), "complete", "workflow_step", Json{{"method", definition.id}, {"cache_key", key}});
            execution_row(connection.get(), current.version, index, definition.id, parameter_hash, "completed", key, launch_snapshot);
        } catch (const std::exception &error) {
            std::lock_guard lock(impl_->mutex); Connection connection(*impl_); audit(connection.get(), "failed", "workflow_step", Json{{"method", definition.id}, {"error", error.what()}}); execution_row(connection.get(), current.version, index, definition.id, parameter_hash, "failed", key, launch_snapshot, error.what());
            throw;
        }
    }
    return {results, false};
}

Json Project::run_worker(const std::string &worker_id, const MethodRegistry &registry) {
    WorkflowExecutionManager manager(*this);
    manager.scheduler_tick(worker_id);
    CancellationToken cancellation;
    try {
        const auto result = run_workflow(registry, &cancellation);
        manager.release_worker(worker_id, result.cancelled ? ExecutionState::cancelled : ExecutionState::completed);
    } catch (const std::exception &error) {
        try {
            execute_sql("UPDATE WORKFLOW_EXECUTION SET error = " + detail::sql_quote(error.what()) + ", updated_at = CURRENT_TIMESTAMP");
            manager.release_worker(worker_id, ExecutionState::failed);
        } catch (...) { }
        throw;
    }
    return manager.current();
}

Json Project::run_method(const std::string &method_id, const Json &parameters,
                         const MethodRegistry &registry) {
    const Method *method = registry.find(method_id);
    if (!method) throw Error(ErrorCode::WorkflowValidation, "Unknown method: " + method_id);
    Workflow workflow = get_workflow();
    workflow.domain = workflow.domain.empty() ? get_domain() : workflow.domain;
    const Json resolved = method->resolve_parameters(parameters);
    workflow.steps.push_back({method_id, ParameterValues{resolved}});
    set_workflow(workflow, registry);
    workflow = get_workflow();
    workflow.validate(registry);
    const std::string launch_snapshot = workflow.to_json(registry).dump();
    const std::size_t index = workflow.steps.size() - 1;
    bool preceding_step_matches = index == 0;
    if (index > 0) {
        const auto parent = query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
        const bool parent_completed = !parent.empty() && parent.at(0).value("status", "") == "completed";
        for (const auto &row : parent_completed ? get_workflow_execution() : Json::array()) {
            if (row.value("workflow_revision", "") == std::to_string(workflow.version) &&
                row.value("step_index", "") == std::to_string(index - 1) &&
                row.value("method", "") == workflow.steps[index - 1].method &&
                row.value("status", "") == "completed" &&
                !row.value("cache_key", "").empty()) {
                preceding_step_matches = true;
                break;
            }
        }
    }
    if (!preceding_step_matches) {
        const auto execution = run_workflow(registry);
        if (execution.results.empty()) throw Error(ErrorCode::WorkflowValidation, "Workflow has no steps: " + method_id);
        return execution.results.back();
    }
    if (workflow.steps[index].method != method_id) throw Error(ErrorCode::WorkflowValidation, "Method is not the appended workflow step: " + method_id);
    if (workflow.steps[index].parameters.values != resolved) throw Error(ErrorCode::WorkflowValidation, "Parameters do not match the planned workflow step");
    std::string previous_hash = "initial";
    if (index > 0) {
        const auto execution = get_workflow_execution();
        for (const auto &row : execution) {
            if (row.value("workflow_revision", "") == std::to_string(workflow.version) && row.value("step_index", "") == std::to_string(index - 1) && row.value("status", "") == "completed") {
                previous_hash = row.value("cache_key", "initial");
                break;
            }
        }
        if (previous_hash == "initial") throw Error(ErrorCode::WorkflowValidation, "Previous workflow step has not completed");
    }
    const auto &definition = method->definition();
    const std::string parameter_hash = hash_text(resolved.dump());
    const std::string key = hash_text(previous_hash + "\n" + definition.id + "\n" + definition.version + "\n" + resolved.dump());
    { std::lock_guard lock(impl_->mutex); Connection connection(*impl_); execution_row(connection.get(), workflow.version, index, method_id, parameter_hash, "running", key, launch_snapshot); }
    {
        std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_);
        audit(connection.get(), "start", "method", Json{{"method", method_id}, {"parameters", resolved}});
    }
    Json result;
    bool cache_hit = false;
    try {
        if (definition.cacheable) {
            if (auto cached = get_cache_entry(key)) {
                const Json payload = parse_json(std::string(cached->data.begin(), cached->data.end()), "cached result");
                if (payload.is_object() && payload.contains("result") && payload.contains("tables") && payload.at("tables").is_object()) {
                    std::lock_guard lock(impl_->mutex); Connection connection(*impl_); restore_tables(connection.get(), payload.at("tables"));
                    result = payload.at("result");
                    execution_row(connection.get(), workflow.version, index, method_id, parameter_hash, "completed", key, launch_snapshot);
                    cache_hit = true;
                }
            }
        }
        if (!cache_hit) result = method->run(*this, resolved);
        if (!cache_hit && definition.cacheable) {
            Json snapshots;
            { std::lock_guard lock(impl_->mutex); Connection connection(*impl_); snapshots = snapshot_tables(connection.get(), definition.writes); }
            set_cache(definition.id, "workflow result", key, Json{{"result", result}, {"tables", std::move(snapshots)}});
        }
        if (!cache_hit) { std::lock_guard lock(impl_->mutex); Connection connection(*impl_); execution_row(connection.get(), workflow.version, index, method_id, parameter_hash, "completed", key, launch_snapshot); }
    } catch (const std::exception &error) {
        std::lock_guard lock(impl_->mutex); Connection connection(*impl_); execution_row(connection.get(), workflow.version, index, method_id, parameter_hash, "failed", key, launch_snapshot, error.what());
        throw;
    }
    {
        std::lock_guard lock(impl_->mutex); Connection connection(*impl_);
        audit(connection.get(), "complete", "method", Json{{"method", method_id}});
    }
    return result;
}

Json Project::run_operation(const std::string &operation_id, const Json &parameters,
                            const OperationRegistry &registry) const {
    const Operation *operation = registry.find(operation_id);
    if (!operation) throw Error(ErrorCode::InvalidArgument, "Unknown operation: " + operation_id);
    Json input = parameters;
    input["database_path"] = get_database_path().string();
        const Json result = operation->run(const_cast<Project &>(*this), input);
    std::lock_guard lock(impl_->mutex); ensure_active(*impl_); Connection connection(*impl_);
    audit(connection.get(), "complete", "operation", Json{{"operation", operation_id}});
    return result;
}

void Project::close() noexcept {
    if (impl_) {
        std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
    }
}

namespace detail {
const char *execution_state_name(ExecutionState state) {
    switch (state) {
    case ExecutionState::queued: return "queued";
    case ExecutionState::running: return "running";
    case ExecutionState::cancelling: return "cancelling";
    case ExecutionState::cancelled: return "cancelled";
    case ExecutionState::completed: return "completed";
    case ExecutionState::failed: return "failed";
    case ExecutionState::interrupted: return "interrupted";
    }
    throw Error(ErrorCode::InvalidArgument, "unknown execution state");
}

ExecutionState execution_state(const std::string &value) {
    if (value == "queued") return ExecutionState::queued;
    if (value == "running") return ExecutionState::running;
    if (value == "cancelling") return ExecutionState::cancelling;
    if (value == "cancelled") return ExecutionState::cancelled;
    if (value == "completed") return ExecutionState::completed;
    if (value == "failed") return ExecutionState::failed;
    if (value == "interrupted") return ExecutionState::interrupted;
    throw Error(ErrorCode::InvalidArgument, "unknown execution state: " + value);
}
}

WorkflowExecutionManager::WorkflowExecutionManager(Project &project) noexcept : project_(&project) {}

Json WorkflowExecutionManager::create(const Json &request) {
    const auto existing = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
    if (!existing.empty()) {
        const auto status = existing.at(0).value("status", "");
        if (status == "queued" || status == "running" || status == "cancelling") {
            throw Error(ErrorCode::InvalidArgument,
                        "an active workflow execution already owns this project");
        }
    }
    const auto revision = request.value("workflow_revision", 0);
    const auto progress = request.value("progress", Json::object());
    const std::string columns = "domain_id, workflow_revision, launch_snapshot, status, progress";
    const std::string values = detail::sql_quote(project_->get_domain()) + ", " + std::to_string(revision) + ", " + detail::sql_quote(project_->get_workflow().to_json().dump()) + ", 'queued', " + detail::sql_quote(progress.dump());
    project_->execute_sql("DELETE FROM WORKFLOW_EXECUTION");
    project_->execute_sql("INSERT INTO WORKFLOW_EXECUTION (" + columns + ") VALUES (" + values + ")");
    return current();
}

Json WorkflowExecutionManager::current() const {
    auto rows = project_->query_json("SELECT domain_id, workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION LIMIT 1");
    if (rows.empty()) throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
    return rows.at(0);
}

Json WorkflowExecutionManager::list() const {
    auto rows = project_->query_json("SELECT domain_id, workflow_revision, status, progress, result_reference, error FROM WORKFLOW_EXECUTION");
    return rows;
}

Json WorkflowExecutionManager::transition(ExecutionState state) {
    const auto rows = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
    if (rows.empty()) throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
    const auto row = rows.at(0);
    const auto current_state = detail::execution_state(row.value("status", ""));
    if (!valid_execution_transition(current_state, state)) throw Error(ErrorCode::InvalidArgument, "invalid execution transition");
    const auto target = detail::execution_state_name(state);
    project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = " + detail::sql_quote(target) + ", completed_at = CASE WHEN " + detail::sql_quote(target) + " IN ('completed', 'failed', 'cancelled', 'interrupted') THEN CURRENT_TIMESTAMP ELSE completed_at END, updated_at = CURRENT_TIMESTAMP");
    return current();
}

Json WorkflowExecutionManager::cancel() {
    const auto rows = project_->query_json("SELECT status FROM WORKFLOW_EXECUTION LIMIT 1");
    if (rows.empty()) throw Error(ErrorCode::InvalidArgument, "workflow execution not found");
    const auto current_state = detail::execution_state(rows.at(0).at("status").get<std::string>());
    if (current_state == ExecutionState::queued) return transition(ExecutionState::cancelled);
    if (current_state == ExecutionState::running) return transition(ExecutionState::cancelling);
    throw Error(ErrorCode::InvalidArgument, "execution cannot be cancelled");
}

Json WorkflowExecutionManager::scheduler_tick(const std::string &worker_id) {
    if (worker_id.empty()) throw Error(ErrorCode::InvalidArgument, "worker_id must not be empty");
    project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'running', process_id = " + detail::sql_quote(worker_id) + ", server_id = " + detail::sql_quote(worker_id) + ", started_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'queued' AND (process_id IS NULL OR process_id = '')");
    const auto rows = project_->query_json("SELECT status, process_id FROM WORKFLOW_EXECUTION LIMIT 1");
    if (rows.empty() || rows.at(0).value("status", "") != "running" || rows.at(0).value("process_id", "") != worker_id) {
        throw Error(ErrorCode::InvalidArgument, "stale or conflicting workflow worker claim");
    }
    return current();
}

Json WorkflowExecutionManager::release_worker(const std::string &worker_id, ExecutionState state) {
    if (worker_id.empty() || (state != ExecutionState::completed && state != ExecutionState::failed && state != ExecutionState::cancelled && state != ExecutionState::interrupted)) {
        throw Error(ErrorCode::InvalidArgument, "invalid workflow worker release");
    }
    const auto target = detail::execution_state_name(state);
    project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = " + detail::sql_quote(target) + ", completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE process_id = " + detail::sql_quote(worker_id) + " AND status IN ('running', 'cancelling')");
    const auto rows = project_->query_json("SELECT status, process_id FROM WORKFLOW_EXECUTION LIMIT 1");
    if (rows.empty() || rows.at(0).value("status", "") != target || rows.at(0).value("process_id", "") != worker_id) {
        throw Error(ErrorCode::InvalidArgument, "stale workflow worker cannot release execution");
    }
    return current();
}

std::size_t WorkflowExecutionManager::recover_interrupted() {
    const auto rows = list();
    std::size_t running = 0;
    for (const auto &row : rows) if (row.value("status", "") == "running") ++running;
    project_->execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'interrupted', completed_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE status = 'running'");
    return running;
}

} // namespace streamfind
